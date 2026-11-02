#pragma once
#include "core/diagnostics/latency_histogram.hpp"
#include "core/diagnostics/thread_observation.hpp"
#include "features/streaming/infrastructure/control_queue.hpp"
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace stream {
// One transport generation. Only the worker invokes RTC operations. All state
// publication is bounded and never waits for transport work or acknowledgment.
class AsyncControlSender {
public:
    using Mode = ControlQueue::Mode;
    enum class Failure { None, Transport, Unexpected };
    struct Snapshot {
        std::size_t pending = 0, bytes = 0;
        std::uint64_t rejected = 0, sent = 0, ageMs = 0, ageMaxMs = 0;
        std::uint64_t sendCalls = 0, sendTotalUs = 0, sendMaxUs = 0;
        std::uint64_t waitCalls = 0, waitTotalUs = 0, waitMaxUs = 0, inFlightMs = 0, pongCoalesced = 0;
        std::uint64_t psFrames = 0, psAccepted = 0, psRejected = 0, gamepadCoalesced = 0;
        std::uint64_t buttonQueuedEdges = 0, buttonRejectedEdges = 0;
        std::uint64_t touchQueuedDown = 0, touchQueuedUp = 0, touchRejectedDown = 0, touchRejectedUp = 0;
        std::uint64_t touchAttemptDown = 0, touchAttemptUp = 0, touchSentDown = 0, touchSentUp = 0;
        std::uint64_t touchFailedDown = 0, touchFailedUp = 0, touchDropped = 0;
        std::uint64_t lastQueuedSeq = 0, lastAttemptSeq = 0, lastSentSeq = 0;
        std::array<std::uint64_t, 6> nativeQueued{}, nativeRejected{}, nativeAttempt{}, nativeSent{}, nativeFailed{}, nativeDropped{}, nativeCoalesced{};
        bool stalled = false, backpressure = false, operationStalled = false, inFlight = false, transportActive = false;
        std::uint64_t operationMs = 0, queueResets = 0;
        Failure failure = Failure::None;
    };
    // available/send must own their transport handles, never an application Impl.
    // send's bool result is deliberately ignored: false still means RTC accepted it.
    AsyncControlSender(std::function<bool()> available, std::function<void(const std::string&)> send,
                       std::function<void(bool)> pongResult = {}, std::string neutral = {},
                       std::vector<ControlQueue::Message> releases = {}, bool reset = false)
        : available_(std::move(available)), send_(std::move(send)), pongResult_(std::move(pongResult)),
          neutral_(std::move(neutral)) {
        for (auto& release : releases) {
            release.enqueuedMs = now();
            if (release.trace.playStation) release.trace.sequence = ++psSequence_;
            held_[release.input] = std::move(release);
        }
        initialized_ = !reset;
        recoveryProgressAt_ = now();
        for (const auto& entry : held_) recovery_.push_back(entry.second);
        if (reset && !neutral_.empty()) recovery_.push_back({neutral_, {}, Mode::GamepadNeutral, now(), {}, {}});
        worker_ = std::thread([this] { run(); });
    }
    ~AsyncControlSender() { stop(); }
    AsyncControlSender(const AsyncControlSender&) = delete;
    AsyncControlSender& operator=(const AsyncControlSender&) = delete;
    bool enqueue(std::string payload, Mode mode = Mode::Ordered, std::string input = {},
                 std::string release = {}) {
        std::lock_guard<std::mutex> lock(mutex_);
        return enqueueLocked(std::move(payload), mode, std::move(input), std::move(release));
    }
    // The Switch has digital triggers. Bits 0..17 identify buttons, 18/19 the
    // pressed triggers; sticks may change freely while this signature is equal.
    bool enqueueGamepad(std::string payload, std::uint32_t buttons, bool playStation = false, std::uint64_t sampledAtUs = 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        if ((buttons & ~std::uint32_t{0xfffff}) || payload.size() > ControlQueue::GamepadBytes) return false;
        if (playStation) ++stats_.psFrames;
        const bool touchDown = playStation && (buttons & (std::uint32_t{1} << 17));
        const bool touchEdge = playStation && touchDown != acceptedTouchDown_;
        const bool edge = buttons != acceptedButtons_;
        const auto sequence = playStation ? ++psSequence_ : 0;
        const bool releases = (acceptedButtons_ & ~buttons) != 0;
        const auto mode = !edge ? Mode::Gamepad : buttons == 0 ? Mode::GamepadNeutral :
            releases ? Mode::GamepadRelease : Mode::GamepadEdge;
        const bool accepted = enqueueLocked(std::move(payload), mode, {}, {}, {playStation, touchDown, touchEdge, sequence, 0, sampledAtUs});
        if (accepted) {
            acceptedButtons_ = buttons;
            if (edge) ++stats_.buttonQueuedEdges;
            if (playStation) {
                ++stats_.psAccepted; stats_.lastQueuedSeq = sequence; psKnown_ = true;
                if (touchEdge) { if (touchDown) ++stats_.touchQueuedDown; else ++stats_.touchQueuedUp; }
                acceptedTouchDown_ = touchDown;
            }
        } else {
            if (edge) {
                ++stats_.buttonRejectedEdges;
                // An unqueueable edge cannot be replaced by a later physical
                // release. Publish failure to the owner instead of losing a tap.
                if (!stopping_ && failure_ == Failure::None) backpressure_ = true;
            }
            if (playStation) {
                ++stats_.psRejected;
                if (touchEdge) { if (touchDown) ++stats_.touchRejectedDown; else ++stats_.touchRejectedUp; }
            }
        }
        return accepted;
    }
    bool enqueueGamepad(std::string payload, bool touchDown) {
        return enqueueGamepad(std::move(payload), touchDown ? (std::uint32_t{1} << 17) : 0, true);
    }
    bool enqueueNative(std::string payload, Mode mode, std::string input, std::string release, unsigned kind) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (kind == 0 || kind >= stats_.nativeQueued.size()) return false;
        ControlQueue::InputTrace trace{true, false, false, ++psSequence_, kind};
        const bool accepted = enqueueLocked(std::move(payload), mode, std::move(input), std::move(release), trace);
        if (accepted) { ++stats_.nativeQueued[kind]; stats_.lastQueuedSeq = trace.sequence; }
        else ++stats_.nativeRejected[kind];
        return accepted;
    }
private:
    bool enqueueLocked(std::string payload, Mode mode, std::string input, std::string release,
                       ControlQueue::InputTrace trace = {false, false, false, 0, 0, 0}) {
        if (payload.size() > 64 * 1024 || release.size() > 64 * 1024 || input.size() > 1024) return false;
        refreshLocked();
        if (stopping_ || operationStalled_ || failure_ != Failure::None) return false;
        if (backpressure_) {
            // Retain explicit releases even after overload sealed ordinary input.
            // They are exported to the replacement generation, never stale presses.
            if (mode == Mode::Release && !input.empty()) return rememberRelease(payload, input, trace);
            return false;
        }
        if (mode == Mode::Pong) {
            if (payload.size() > 64 * 1024) { ++pongRejected_; return false; }
            // Preserve ordinary ping bursts, independently of ordered input.
            // Under overload keep the latest timestamp in the final bounded slot.
            if (pongs_.size() == MaxPongs) {
                const auto began = pongs_.back().enqueuedMs;
                pongs_.back() = ControlQueue::Message{std::move(payload), {}, mode, began, {}};
                ++stats_.pongCoalesced;
            } else pongs_.push_back({std::move(payload), {}, mode, now(), {}});
        } else {
            if (!queue_.push(payload, mode, input, now(), release, trace)) {
                const bool edge = mode == Mode::GamepadEdge || mode == Mode::GamepadRelease || mode == Mode::GamepadNeutral;
                if (mode == Mode::Release || edge || !release.empty()) {
                    // Under overload taps may be lost, but no accepted held input
                    // may remain pressed. Drop history and preserve only release
                    // obligations, neutral, and the latest gamepad sample.
                    stats_.touchDropped += queue_.touchEdges();
                    const auto dropped = queue_.nativePending();
                    for (std::size_t n = 1; n < dropped.size(); ++n) stats_.nativeDropped[n] += dropped[n];
                    queue_.clear(); recovery_.clear();
                    for (const auto& entry : held_) recovery_.push_back(entry.second);
                    if (!neutral_.empty()) queue_.push(neutral_, Mode::GamepadNeutral, {}, now());
                    if (edge && payload != neutral_) queue_.push(payload, mode, {}, now(), {}, trace);
                    backpressure_ = true; ++stats_.queueResets;
                    if (mode == Mode::Release && !input.empty()) {
                        if (!rememberRelease(payload, input, trace)) return false;
                        recovery_.clear();
                        for (const auto& entry : held_) recovery_.push_back(entry.second);
                    }
                    condition_.notify_all();
                    return mode == Mode::Release || edge;
                }
                return false;
            }
        }
        condition_.notify_one();
        return true;
    }
public:
    // RTC's buffered-amount-low callback publishes readiness. A revision also
    // retains a callback that races the unlocked availability probe.
    void notifyWritable() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++writableRevision_; condition_.notify_one();
    }
    void allow(bool allowed) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (allowed_ != allowed) ++writableRevision_;
        allowed_ = allowed; condition_.notify_one();
    }
    std::string inputLatencyDiagnostics() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return "provider=MTS scope=rolling-5s boundary=pad-to-send-return accepted-only=1 " +
            inputLatency_.text(fog::metrics::steadyNowUs());
    }
    Snapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto out = stats_;
        const auto ms = now();
        out.pending = queue_.size() + pongs_.size() + recovery_.size() + (inFlight_ ? 1 : 0);
        out.bytes = queue_.bytes() + inFlightBytes_;
        for (const auto& pong : pongs_) out.bytes += pong.payload.size();
        for (const auto& release : recovery_) out.bytes += release.payload.size();
        out.rejected = queue_.rejected() + pongRejected_;
        out.gamepadCoalesced = queue_.coalesced();
        out.nativeCoalesced = queue_.nativeCoalesced();
        const auto oldest = queue_.empty() ? ms : queue_.front().enqueuedMs;
        out.ageMs = ms >= oldest ? ms - oldest : 0;
        if (!recovery_.empty() && ms >= recovery_.front().enqueuedMs) out.ageMs = std::max(out.ageMs, ms - recovery_.front().enqueuedMs);
        if (!pongs_.empty() && ms >= pongs_.front().enqueuedMs) out.ageMs = std::max(out.ageMs, ms - pongs_.front().enqueuedMs);
        out.inFlight = inFlight_;
        out.transportActive = transportActive_;
        if (inFlight_ && ms >= inFlightAt_) out.inFlightMs = ms - inFlightAt_;
        out.ageMs = std::max(out.ageMs, out.inFlightMs);
        out.ageMaxMs = std::max(out.ageMaxMs, out.ageMs);
        out.operationMs = transportActive_ && ms >= operationAt_ ? ms - operationAt_ : 0;
        out.backpressure = backpressure_;
        out.operationStalled = operationStalled_;
        out.stalled = backpressure_ || operationStalled_;
        out.failure = failure_;
        return out;
    }
    // Deadline publication is explicit. Diagnostics never latch a failure.
    Snapshot inspect() {
        { std::lock_guard<std::mutex> lock(mutex_); refreshLocked(); }
        return snapshot();
    }
    std::vector<ControlQueue::Message> releasesForRetry() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<ControlQueue::Message> releases;
        for (const auto& entry : held_) releases.push_back(entry.second);
        return releases;
    }
    // Invalidate before joining and before destroying any RTC handle. Pending
    // old-generation presses never drain into a closing/replacement transport.
    // A synchronous RTC send cannot be cancelled; join waits for its return,
    // never for a peer ACK. Caller must not hold any RTC/callback mutex here.
    void requestStop(std::string neutral = {}, bool releaseInputs = true) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stopping_) {
            stopping_ = true; shutdownAllowed_ = allowed_ && !blocked_;
            if (!neutral.empty()) neutral_ = std::move(neutral);
            const auto ms = now();
            if (!queue_.empty() && ms >= queue_.front().enqueuedMs)
                stats_.ageMaxMs = std::max(stats_.ageMaxMs, ms - queue_.front().enqueuedMs);
            if (inFlight_ && ms >= inFlightAt_) stats_.ageMaxMs = std::max(stats_.ageMaxMs, ms - inFlightAt_);
            stats_.touchDropped += queue_.touchEdges();
            const auto dropped = queue_.nativePending();
            for (std::size_t n = 1; n < dropped.size(); ++n) stats_.nativeDropped[n] += dropped[n];
            queue_.clear(); pongs_.clear(); recovery_.clear();
        }
        if (!releaseInputs) shutdownAllowed_ = false;
        condition_.notify_all();
    }
    void stop(std::string neutral = {}) {
        requestStop(std::move(neutral));
        if (worker_.joinable()) worker_.join();
    }
#ifdef FOG_STREAM_TESTING
    void block(bool blocked) {
        std::lock_guard<std::mutex> lock(mutex_);
        blocked_ = blocked; condition_.notify_one();
    }
    void advance(unsigned milliseconds, bool publish = true) {
        std::lock_guard<std::mutex> lock(mutex_);
        offsetMs_ += milliseconds; if (publish) { refreshLocked(); condition_.notify_one(); }
    }
#endif
private:
    bool rememberRelease(const std::string& payload, const std::string& input, ControlQueue::InputTrace trace) {
        if (held_.find(input) == held_.end() && held_.size() >= 256) { failure_ = Failure::Unexpected; return false; }
        held_[input] = {payload, input, Mode::Release, now(), {}, trace};
        return true;
    }
    void refreshLocked() {
        const auto ms = now();
        if (transportActive_ && ms >= operationAt_ && ms - operationAt_ >= ControlQueue::StallMs)
            operationStalled_ = true;
        if ((!recovery_.empty() && ms >= recoveryProgressAt_ && ms - recoveryProgressAt_ >= ControlQueue::StallMs) ||
            queue_.stalled(ms) || (!pongs_.empty() && ms >= pongs_.front().enqueuedMs &&
            ms - pongs_.front().enqueuedMs >= ControlQueue::StallMs)) backpressure_ = true;
    }
    using Clock = std::chrono::steady_clock;
    static std::uint64_t micros(Clock::duration duration) {
        return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    }
    std::uint64_t now() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count() + offsetMs_;
    }
    void transmit(const ControlQueue::Message& message) {
        { std::lock_guard<std::mutex> lock(mutex_); recordInputAttempt(message.trace); }
        const auto began = Clock::now();
        Failure failure = Failure::None;
        try { send_(message.payload); }
        catch (const std::runtime_error&) { failure = Failure::Transport; }
        catch (...) { failure = Failure::Unexpected; }
        const auto completedUs = fog::metrics::steadyNowUs();
        const auto elapsed = micros(Clock::now() - began);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Completion must retain an exceeded deadline even if no owner
            // inspected the operation while its callback was blocked.
            refreshLocked();
            ++stats_.sendCalls; stats_.sendTotalUs += elapsed;
            recordInputSend(message.trace, failure == Failure::None);
            if (failure == Failure::None && message.trace.sampledAtUs && completedUs >= message.trace.sampledAtUs)
                inputLatency_.record(completedUs - message.trace.sampledAtUs, completedUs);
            stats_.sendMaxUs = std::max(stats_.sendMaxUs, elapsed);
            if (failure == Failure::None) {
                ++stats_.sent; recoveryProgressAt_ = now();
                if (message.mode != Mode::Pong) queue_.progress(now());
            }
            else if (failure_ == Failure::None) failure_ = failure;
            inFlight_ = false; inFlightBytes_ = 0; transportActive_ = false;
            if (failure == Failure::None && message.mode == Mode::Release) held_.erase(message.input);
        }
        if (message.mode == Mode::Pong && pongResult_) {
            try { pongResult_(failure == Failure::None); } catch (...) {}
        }
    }
    void shutdown() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!shutdownAllowed_ || failure_ != Failure::None) return;
            transportActive_ = true; operationAt_ = now();
        }
        bool available = false;
        try { available = available_(); } catch (...) {}
        {
            std::lock_guard<std::mutex> lock(mutex_);
            refreshLocked(); transportActive_ = false;
        }
        if (!available) return;
        // Held inputs include an ambiguous in-flight press. Shutdown never
        // drains queued presses, and stops after the first failed release.
        for (const auto& entry : held_) {
            auto trace = entry.second.trace;
            { std::lock_guard<std::mutex> lock(mutex_); if (trace.playStation) trace.sequence = ++psSequence_; }
            if (!shutdownSend(entry.second.payload, trace)) return;
        }
        if (!neutral_.empty()) {
            ControlQueue::InputTrace trace;
            { std::lock_guard<std::mutex> lock(mutex_); trace = {psKnown_, false, sentTouchDown_, psKnown_ ? ++psSequence_ : 0}; }
            shutdownSend(neutral_, trace);
        }
    }
    bool shutdownSend(const std::string& payload, ControlQueue::InputTrace trace = {false, false, false, 0, 0, 0}) noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // A concurrent retry seal may revoke best-effort release permission.
            if (!shutdownAllowed_) return false;
            inFlight_ = true; transportActive_ = true; operationAt_ = now(); inFlightAt_ = now(); inFlightBytes_ = payload.size();
            recordInputAttempt(trace);
        }
        const auto began = Clock::now();
        bool sent = true;
        try { send_(payload); } catch (...) { sent = false; }
        std::lock_guard<std::mutex> lock(mutex_);
        const auto elapsed = micros(Clock::now() - began);
        refreshLocked();
        ++stats_.sendCalls; stats_.sendTotalUs += elapsed;
        recordInputSend(trace, sent);
        stats_.sendMaxUs = std::max(stats_.sendMaxUs, elapsed);
        if (sent) ++stats_.sent;
        inFlight_ = false; transportActive_ = false; inFlightBytes_ = 0;
        return sent;
    }
    // Called under mutex_ after RTC send returned. Success means local transport
    // acceptance only; the Fog Play input protocol has no remote input ACK.
    void recordInputSend(const ControlQueue::InputTrace& trace, bool sent) {
        if (!trace.playStation) return;
        if (trace.nativeInput != 0) {
            if (sent) ++stats_.nativeSent[trace.nativeInput]; else ++stats_.nativeFailed[trace.nativeInput];
        }
        if (trace.touchEdge) {
            if (trace.touchDown) {
                if (sent) ++stats_.touchSentDown; else ++stats_.touchFailedDown;
            } else {
                if (sent) ++stats_.touchSentUp; else ++stats_.touchFailedUp;
            }
        }
        if (sent) {
            stats_.lastSentSeq = trace.sequence;
            if (trace.nativeInput == 0) sentTouchDown_ = trace.touchDown;
        }
    }
    void recordInputAttempt(const ControlQueue::InputTrace& trace) {
        if (!trace.playStation) return;
        stats_.lastAttemptSeq = trace.sequence;
        if (trace.nativeInput != 0) ++stats_.nativeAttempt[trace.nativeInput];
        if (trace.touchEdge) {
            if (trace.touchDown) ++stats_.touchAttemptDown; else ++stats_.touchAttemptUp;
        }
    }
    void run() noexcept {
        fog::diagnostics::observeThread("control-sender");
        try {
            std::unique_lock<std::mutex> lock(mutex_);
            while (!stopping_) {
                const auto waited = Clock::now();
                condition_.wait(lock, [this] {
                    return stopping_ || (allowed_ && !blocked_ && !backpressure_ && !operationStalled_ && failure_ == Failure::None &&
                        (initialized_ || (!queue_.empty() && queue_.front().mode == Mode::Initialize)) &&
                        (!queue_.empty() || !pongs_.empty() || !recovery_.empty()));
                });
                refreshLocked();
                const auto elapsed = micros(Clock::now() - waited);
                ++stats_.waitCalls; stats_.waitTotalUs += elapsed;
                stats_.waitMaxUs = std::max(stats_.waitMaxUs, elapsed);
                if (stopping_) break;
                if (!allowed_ || blocked_ || backpressure_ || operationStalled_ || failure_ != Failure::None ||
                    (queue_.empty() && pongs_.empty() && recovery_.empty())) continue;
                transportActive_ = true; operationAt_ = now();
                const auto readiness = writableRevision_;
                lock.unlock();
                bool available = false;
                Failure failure = Failure::None;
                try { available = available_(); }
                catch (const std::runtime_error&) { failure = Failure::Transport; }
                catch (...) { failure = Failure::Unexpected; }
                lock.lock();
                refreshLocked();
                transportActive_ = false;
                if (failure != Failure::None) { failure_ = failure; continue; }
                if (stopping_ || !allowed_ || blocked_ || backpressure_ || operationStalled_) continue;
                if (!available) {
                    // Wait for buffer/route readiness, not a 5-ms retry loop.
                    // Only the oldest local obligation's deadline is timed.
                    const auto ms = now();
                    auto oldest = ms;
                    if (!queue_.empty()) oldest = std::min(oldest, queue_.front().enqueuedMs);
                    if (!pongs_.empty()) oldest = std::min(oldest, pongs_.front().enqueuedMs);
                    if (!recovery_.empty()) oldest = std::min(oldest, recoveryProgressAt_);
                    const auto age = ms - oldest;
                    const auto remaining = age < ControlQueue::StallMs ? ControlQueue::StallMs - age : 0;
                    condition_.wait_for(lock, std::chrono::milliseconds(remaining), [&] {
                        return stopping_ || !allowed_ || blocked_ || backpressure_ || operationStalled_ ||
                            failure_ != Failure::None || readiness != writableRevision_;
                    });
                    continue;
                }
                ControlQueue::Message message;
                // Complete protocol initialization before releasing persistent
                // host inputs; every fresh input still follows the reset prefix.
                if (!queue_.empty() && queue_.front().mode == Mode::Initialize) {
                    message = queue_.front(); queue_.pop(now()); initialized_ = true;
                }
                else if (!recovery_.empty()) { message = std::move(recovery_.front()); recovery_.pop_front(); }
                else if (!pongs_.empty() && (pongStreak_ < 4 || queue_.empty())) {
                    message = std::move(pongs_.front()); pongs_.pop_front(); ++pongStreak_;
                }
                else { message = queue_.front(); queue_.pop(now()); pongStreak_ = 0; }
                const auto ms = now();
                if (ms >= message.enqueuedMs) stats_.ageMaxMs = std::max(stats_.ageMaxMs, ms - message.enqueuedMs);
                inFlight_ = true; transportActive_ = true; operationAt_ = now(); inFlightAt_ = ms; inFlightBytes_ = message.payload.size();
                if (!message.release.empty()) {
                    if (held_.size() >= 256 && held_.find(message.input) == held_.end()) {
                        failure_ = Failure::Unexpected; inFlight_ = false; inFlightBytes_ = 0; transportActive_ = false; continue;
                    }
                    ControlQueue::Message released{std::move(message.release), message.input, Mode::Release,
                                                   message.enqueuedMs, {}, message.trace};
                    if (released.trace.nativeInput == 1 || released.trace.nativeInput == 2) released.trace.nativeInput = 3;
                    held_[message.input] = std::move(released);
                }
                lock.unlock(); transmit(message); lock.lock();
            }
            lock.unlock(); shutdown();
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            failure_ = Failure::Unexpected; inFlight_ = false; inFlightBytes_ = 0; transportActive_ = false;
        }
    }
    std::function<bool()> available_;
    std::function<void(const std::string&)> send_;
    std::function<void(bool)> pongResult_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    ControlQueue queue_;
    static constexpr std::size_t MaxPongs = 16;
    std::deque<ControlQueue::Message> pongs_, recovery_;
    std::map<std::string, ControlQueue::Message> held_;
    Snapshot stats_;
    fog::metrics::LatencyHistogram inputLatency_;
    Failure failure_ = Failure::None;
    bool backpressure_ = false, operationStalled_ = false;
    bool transportActive_ = false;
    bool allowed_ = false, blocked_ = false, stopping_ = false, shutdownAllowed_ = false, inFlight_ = false;
    bool initialized_ = true;
    bool acceptedTouchDown_ = false, sentTouchDown_ = false, psKnown_ = false;
    std::uint32_t acceptedButtons_ = 0;
    std::uint64_t psSequence_ = 0;
    std::uint64_t writableRevision_ = 0;
    std::uint64_t offsetMs_ = 0, inFlightAt_ = 0, operationAt_ = 0, recoveryProgressAt_ = 0, pongRejected_ = 0;
    std::size_t inFlightBytes_ = 0, pongStreak_ = 0;
    std::string neutral_;
    std::thread worker_;
};
}
