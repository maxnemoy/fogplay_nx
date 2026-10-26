#pragma once
#include "features/streaming/infrastructure/control_queue.hpp"
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

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
        std::uint64_t touchQueuedDown = 0, touchQueuedUp = 0, touchRejectedDown = 0, touchRejectedUp = 0;
        std::uint64_t touchAttemptDown = 0, touchAttemptUp = 0, touchSentDown = 0, touchSentUp = 0;
        std::uint64_t touchFailedDown = 0, touchFailedUp = 0, touchDropped = 0;
        std::uint64_t lastQueuedSeq = 0, lastAttemptSeq = 0, lastSentSeq = 0;
        std::array<std::uint64_t, 6> nativeQueued{}, nativeRejected{}, nativeAttempt{}, nativeSent{}, nativeFailed{}, nativeDropped{}, nativeCoalesced{};
        bool stalled = false, inFlight = false, transportActive = false;
        Failure failure = Failure::None;
    };
    // available/send must own their transport handles, never an application Impl.
    // send's bool result is deliberately ignored: false still means RTC accepted it.
    AsyncControlSender(std::function<bool()> available, std::function<void(const std::string&)> send,
                       std::function<void(bool)> pongResult = {})
        : available_(std::move(available)), send_(std::move(send)), pongResult_(std::move(pongResult)),
          worker_([this] { run(); }) {}
    ~AsyncControlSender() { stop(); }
    AsyncControlSender(const AsyncControlSender&) = delete;
    AsyncControlSender& operator=(const AsyncControlSender&) = delete;
    bool enqueue(std::string payload, Mode mode = Mode::Ordered, std::string input = {},
                 std::string release = {}) {
        std::lock_guard<std::mutex> lock(mutex_);
        return enqueueLocked(std::move(payload), mode, std::move(input), std::move(release));
    }
    bool enqueueGamepad(std::string payload, bool touchDown) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.psFrames;
        const bool edge = touchDown != acceptedTouchDown_;
        const auto sequence = ++psSequence_;
        const auto mode = edge ? (touchDown ? Mode::GamepadEdge : Mode::GamepadRelease) : Mode::Gamepad;
        const bool accepted = enqueueLocked(std::move(payload), mode, {}, {}, {true, touchDown, edge, sequence});
        if (accepted) {
            ++stats_.psAccepted; stats_.lastQueuedSeq = sequence; psKnown_ = true;
            if (edge) { if (touchDown) ++stats_.touchQueuedDown; else ++stats_.touchQueuedUp; }
            acceptedTouchDown_ = touchDown;
        } else {
            ++stats_.psRejected;
            if (edge) { if (touchDown) ++stats_.touchRejectedDown; else ++stats_.touchRejectedUp; }
        }
        return accepted;
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
                       ControlQueue::InputTrace trace = {false, false, false, 0, 0}) {
        if (stopping_ || stalled_ || failure_ != Failure::None) return false;
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
            if (release.size() > 64 * 1024 || input.size() > 1024) return false;
            if (!queue_.push(std::move(payload), mode, std::move(input), now(), std::move(release), trace)) return false;
        }
        condition_.notify_one();
        return true;
    }
public:
    void allow(bool allowed) {
        std::lock_guard<std::mutex> lock(mutex_);
        allowed_ = allowed; condition_.notify_one();
    }
    Snapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto out = stats_;
        const auto ms = now();
        out.pending = queue_.size() + pongs_.size() + (inFlight_ ? 1 : 0);
        out.bytes = queue_.bytes() + inFlightBytes_;
        for (const auto& pong : pongs_) out.bytes += pong.payload.size();
        out.rejected = queue_.rejected() + pongRejected_;
        out.gamepadCoalesced = queue_.coalesced();
        out.nativeCoalesced = queue_.nativeCoalesced();
        const auto oldest = queue_.empty() ? ms : queue_.front().enqueuedMs;
        out.ageMs = ms >= oldest ? ms - oldest : 0;
        if (!pongs_.empty() && ms >= pongs_.front().enqueuedMs) out.ageMs = std::max(out.ageMs, ms - pongs_.front().enqueuedMs);
        out.inFlight = inFlight_;
        out.transportActive = transportActive_;
        if (inFlight_ && ms >= inFlightAt_) out.inFlightMs = ms - inFlightAt_;
        out.ageMs = std::max(out.ageMs, out.inFlightMs);
        out.ageMaxMs = std::max(out.ageMaxMs, out.ageMs);
        stalled_ = stalled_ || queue_.stalled(ms) || out.inFlightMs >= ControlQueue::StallMs ||
            (!pongs_.empty() && ms >= pongs_.front().enqueuedMs && ms - pongs_.front().enqueuedMs >= ControlQueue::StallMs);
        out.stalled = stalled_;
        out.failure = failure_;
        return out;
    }
    // Invalidate before joining and before destroying any RTC handle. Pending
    // old-generation presses never drain into a closing/replacement transport.
    // A synchronous RTC send cannot be cancelled; join waits for its return,
    // never for a peer ACK. Caller must not hold any RTC/callback mutex here.
    void requestStop(std::string neutral = {}, bool releaseInputs = true) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stopping_) {
            stopping_ = true; shutdownAllowed_ = allowed_ && !blocked_;
            neutral_ = std::move(neutral);
            const auto ms = now();
            if (!queue_.empty() && ms >= queue_.front().enqueuedMs)
                stats_.ageMaxMs = std::max(stats_.ageMaxMs, ms - queue_.front().enqueuedMs);
            if (inFlight_ && ms >= inFlightAt_) stats_.ageMaxMs = std::max(stats_.ageMaxMs, ms - inFlightAt_);
            stats_.touchDropped += queue_.touchEdges();
            const auto dropped = queue_.nativePending();
            for (std::size_t n = 1; n < dropped.size(); ++n) stats_.nativeDropped[n] += dropped[n];
            queue_.clear(); pongs_.clear();
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
    void advance(unsigned milliseconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        offsetMs_ += milliseconds; condition_.notify_one();
    }
#endif
private:
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
        const auto elapsed = micros(Clock::now() - began);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++stats_.sendCalls; stats_.sendTotalUs += elapsed;
            recordInputSend(message.trace, failure == Failure::None);
            stats_.sendMaxUs = std::max(stats_.sendMaxUs, elapsed);
            if (failure == Failure::None) {
                ++stats_.sent;
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
            transportActive_ = true;
        }
        bool available = false;
        try { available = available_(); } catch (...) {}
        {
            std::lock_guard<std::mutex> lock(mutex_);
            transportActive_ = false;
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
    bool shutdownSend(const std::string& payload, ControlQueue::InputTrace trace = {false, false, false, 0, 0}) noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // A concurrent retry seal may revoke best-effort release permission.
            if (!shutdownAllowed_) return false;
            inFlight_ = true; transportActive_ = true; inFlightAt_ = now(); inFlightBytes_ = payload.size();
            recordInputAttempt(trace);
        }
        const auto began = Clock::now();
        bool sent = true;
        try { send_(payload); } catch (...) { sent = false; }
        std::lock_guard<std::mutex> lock(mutex_);
        const auto elapsed = micros(Clock::now() - began);
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
        try {
            std::unique_lock<std::mutex> lock(mutex_);
            while (!stopping_) {
                const auto waited = Clock::now();
                condition_.wait(lock, [this] {
                    return stopping_ || (allowed_ && !blocked_ && failure_ == Failure::None && (!queue_.empty() || !pongs_.empty()));
                });
                const auto elapsed = micros(Clock::now() - waited);
                ++stats_.waitCalls; stats_.waitTotalUs += elapsed;
                stats_.waitMaxUs = std::max(stats_.waitMaxUs, elapsed);
                if (stopping_) break;
                if (!allowed_ || blocked_ || failure_ != Failure::None || (queue_.empty() && pongs_.empty())) continue;
                transportActive_ = true;
                lock.unlock();
                bool available = false;
                Failure failure = Failure::None;
                try { available = available_(); }
                catch (const std::runtime_error&) { failure = Failure::Transport; }
                catch (...) { failure = Failure::Unexpected; }
                lock.lock();
                transportActive_ = false;
                if (failure != Failure::None) { failure_ = failure; continue; }
                if (stopping_ || !allowed_ || blocked_) continue;
                if (!available) {
                    // Avoid spinning when RTC's bounded buffer is full; route or
                    // shutdown publication still wakes this wait immediately.
                    condition_.wait_for(lock, std::chrono::milliseconds(5)); continue;
                }
                ControlQueue::Message message;
                if (!pongs_.empty() && (pongStreak_ < 4 || queue_.empty())) {
                    message = std::move(pongs_.front()); pongs_.pop_front(); ++pongStreak_;
                }
                else { message = queue_.front(); queue_.pop(now()); pongStreak_ = 0; }
                const auto ms = now();
                if (ms >= message.enqueuedMs) stats_.ageMaxMs = std::max(stats_.ageMaxMs, ms - message.enqueuedMs);
                inFlight_ = true; transportActive_ = true; inFlightAt_ = ms; inFlightBytes_ = message.payload.size();
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
    std::deque<ControlQueue::Message> pongs_;
    std::map<std::string, ControlQueue::Message> held_;
    Snapshot stats_;
    Failure failure_ = Failure::None;
    mutable bool stalled_ = false;
    bool transportActive_ = false;
    bool allowed_ = false, blocked_ = false, stopping_ = false, shutdownAllowed_ = false, inFlight_ = false;
    bool acceptedTouchDown_ = false, sentTouchDown_ = false, psKnown_ = false;
    std::uint64_t psSequence_ = 0;
    std::uint64_t offsetMs_ = 0, inFlightAt_ = 0, pongRejected_ = 0;
    std::size_t inFlightBytes_ = 0, pongStreak_ = 0;
    std::string neutral_;
    std::thread worker_;
};
}
