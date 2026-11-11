#include "features/streaming/infrastructure/network_telemetry_capture.hpp"
#include "features/streaming/infrastructure/bounded_network_events.hpp"
#include "features/streaming/application/network_diagnostics_formatter.hpp"
#include "features/streaming/infrastructure/telemetry_capture.hpp"
#include "features/streaming/application/stream_diagnostics_formatter.hpp"
#include "core/diagnostics/latency_histogram.hpp"
#include "core/constants/stage1_options.hpp"
#include "moonlight_session.hpp"
#include "app_selection.hpp"
#include "frame_diagnostics.hpp"
#include "resume_retry.hpp"
#include "transport_retry.hpp"
#include "controller_input.hpp"
#include "video_input_gate.hpp"
#include "encryption_policy.hpp"
#include "features/streaming/domain/video_limits.hpp"
#include "core/diagnostics/thread_observation.hpp"
#include "features/streaming/application/remote_input.hpp"
#include "features/streaming/infrastructure/rumble_mailbox.hpp"
#include <Settings.hpp>
#include <Diagnostics.hpp>
#include <FogMoonlightDiagnostics.h>
#include <FogMoonlightRecovery.h>
#include <FogMoonlightTermination.h>
#include <gamestream/client.h>
#include <gamestream/errors.h>
#include <switch.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <type_traits>
#include <string_view>
#include <utility>
#include <arpa/inet.h>
#include <sys/socket.h>

namespace moonlight {
namespace {
using Clock = std::chrono::steady_clock;
std::atomic<bool> protocolClaimed{false};
// The core event namespace outlives a Client. Serialize its cursor and legacy /
// typed consumer handoff; receive threads never acquire this adapter mutex.
std::mutex coreEventsMutex;
std::uint64_t coreEventsCursor=0;
std::uint64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}
std::string randomSuffix() {
    unsigned char bytes[16]; randomGet(bytes, sizeof(bytes));
    static constexpr char hex[] = "0123456789abcdef";
    std::string result; result.reserve(32);
    for (auto value : bytes) { result += hex[value >> 4]; result += hex[value & 15]; }
    return result;
}
bool validHost(const std::string& host) {
    if (host.empty() || host.size() > 253) return false;
    if (host.find(':') != std::string::npos) {
        unsigned char binary[16]; return inet_pton(AF_INET6, host.c_str(), binary) == 1;
    }
    if (host.front() == '.' || host.front() == '-' || host.back() == '.' || host.back() == '-') return false;
    return host.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") == std::string::npos;
}
struct State {
    bool ownsLease = false;
    SERVER_DATA host{};
    STREAM_CONFIGURATION config{};
    fog::EntityId rental;
    std::string identity, endpoint;
    std::atomic<bool> valid{true}, paired{false}, pairDone{false}, pairOK{false};
    std::atomic<bool> stop{false}, running{false}, starting{false}, streamInUse{false};
    std::atomic<bool> workerFinished{true};
    std::atomic<bool> terminated{false}, failed{false}, decoderWaiting{true}, assemblyWaiting{true};
    std::atomic<bool> fatalFailure{false}, retryableFailure{false};
    std::atomic<bool> coreDiagnosticsReady{false};
    std::atomic<bool> typedNetworkEvents{false};
    bool coreEventsReadable=false; // coreEventsMutex; excludes core initialization.
    std::atomic<std::uint64_t> coreSourceMissed{0}; // diagnostic State lifetime, not per-video generation
    stream::telemetry::network::EventJournal<stream::telemetry::network::NetworkEvent> networkEvents;
    std::uint64_t telemetryCoreClockUs=0,telemetryCoreSteadyUs=0; // mutex; numeric final source time
    FogMoonlightDiagnostics encryptionResult{}; // mutex; retained across stop, cleared on start.
    std::atomic<int> code{0};
    std::atomic<int> terminationCode{0};
    std::atomic<int> terminationOrigin{FOG_TERMINATION_TRANSPORT};
    std::atomic<int> application{0};
    std::atomic<std::uint64_t> transportClosedMs{0};
    std::atomic<std::uint64_t> lastVideoMs{0};
    std::atomic<stream::VideoCodec> selectedCodec{stream::VideoCodec::H264};
    std::atomic<int> negotiatedWidth{0}, negotiatedHeight{0}, negotiatedFps{0};
    std::atomic<std::uint64_t> frames{0}, audioFrames{0}, videoDrops{0}, audioDrops{0}, inputAccepted{0}, inputRejected{0};
    std::atomic<std::uint64_t> audioOverflowDrops{0}, audioInvalidDrops{0};
    std::atomic<std::uint64_t> audioDropsVideoWaiting{0}, audioDropsVideoStalled{0};
    std::atomic<std::uint64_t> audioDropLastMs{0}, audioDropLastQueueAgeMs{0}, audioDropLastVideoAgeMs{0};
    std::atomic<std::uint64_t> inputIgnored{0};
    std::atomic<unsigned short> accelRateHz{0}, gyroRateHz{0};
    std::atomic<std::uint64_t> videoInvalid{0}, videoWaitDrops{0}, videoResyncs{0};
    detail::FrameDiagnostics trace;
    stream::RumbleMailbox rumble;
    mutable std::mutex mutex, apiMutex;
    std::condition_variable changed;
    std::shared_ptr<stream::MediaSignals> mediaSignals = std::make_shared<stream::MediaSignals>();
    // The media-video owner pulls directly from the core. Teardown closes this
    // gate, wakes the core wait and joins all in-flight frame scopes before
    // destroying the core's queue. No SDK call is made while holding mutex.
    mutable std::mutex pullMutex;
    std::condition_variable pullChanged;
    bool pullOpen = false, pullActive = false;
    std::deque<stream::Packet> audio;
    std::string message = "Moonlight ready for pairing", error;
    std::uint64_t generation = 1;
    const std::uint64_t telemetryStreamEpoch = stream::telemetry::nextStreamEpoch();
    std::uint64_t telemetryEpoch = 0; // State::mutex, transport starts only (not video resync).
    moonlight_diagnostics::CommandSnapshot command;
    moonlight_diagnostics::CommandSnapshot firstCommandFailure;
    unsigned commandAttempt = 0;
    unsigned commandFailures = 0;
    std::uint64_t closedToResumeMs = 0;
    Clock::time_point pairingDeadline;
    State() { LiInitializeServerInformation(&host.serverInfo); LiInitializeStreamConfiguration(&config); }
    ~State() { delete[] host.serverInfo.rtspSessionUrl; releaseLease(); }
    bool claimLease() { bool expected = false; ownsLease = protocolClaimed.compare_exchange_strong(expected, true); return ownsLease; }
    void releaseLease() {
        // Re-pairing may retain the old Client after its worker joined. Close
        // its global-core read gate before another State can claim the lease.
        std::lock_guard<std::mutex> consumer(coreEventsMutex);
        coreEventsReadable=false;
        if (ownsLease) { ownsLease = false; protocolClaimed = false; }
    }
    void status(const char* value) { std::lock_guard<std::mutex> lock(mutex); message = value; }
    void fail(const char* value, int failure = -1, bool retryable = false) {
        { std::lock_guard<std::mutex> lock(mutex); error = value; message = value;
            if (!terminated) code = failure;
            retryableFailure = retryable;
            if (!retryable) fatalFailure = true;
            failed = true; rumble.clear(); }
        changed.notify_all();
        mediaSignals->notifyAll();
    }
};
// Core callbacks are process-global. An exclusive protocol lease prevents two
// contexts, and the worker clears this pointer only after LiStopConnection joins
// every callback thread (including the prepared termination callback shim).
std::atomic<State*> active{nullptr};
void stageStarting(int) {}
void stageFailed(int, int code) { if (auto* state = active.load()) state->code = code; }
void terminated(int code) {
    if (auto* state = active.load()) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            const auto origin = FogMoonlightGetTerminationOrigin();
            // Retain terminal evidence even if another callback races teardown.
            if (state->terminated && state->fatalFailure && origin != FOG_TERMINATION_HOST) return;
            state->code = code; state->terminationCode = code;
            state->terminationOrigin = origin;
            if (state->terminationOrigin == FOG_TERMINATION_HOST || !detail::retryableTransportError(code)) state->fatalFailure = true;
            state->terminated = true; state->rumble.clear();
        }
        state->changed.notify_all();
        state->mediaSignals->notifyAll();
    }
}
void rumble(unsigned short controller, unsigned short lowFrequency, unsigned short highFrequency) {
    if (auto* state = active.load()) {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->valid || state->stop || state->failed || state->terminated || !state->streamInUse) return;
        state->rumble.put({controller, lowFrequency, highFrequency, 0, nowMs()});
    }
}
void motionEventState(unsigned short controller, unsigned char type, unsigned short rateHz) {
    if (controller != 0) return;
    if (auto* state = active.load()) {
        if (type == LI_MOTION_TYPE_ACCEL) state->accelRateHz = rateHz;
        else if (type == LI_MOTION_TYPE_GYRO) state->gyroRateHz = rateHz;
    }
}
void ignoreLog(const char*, ...) {}
int videoSetup(int format, int width, int height, int fps, void*, int) {
    auto* state = active.load();
    if (!state || (format != VIDEO_FORMAT_H264 && format != VIDEO_FORMAT_H265) ||
        width <= 0 || width > 1920 || height <= 0 || height > 1080) return -1;
    state->selectedCodec = format == VIDEO_FORMAT_H265 ? stream::VideoCodec::H265 : stream::VideoCodec::H264;
    state->negotiatedWidth = width; state->negotiatedHeight = height; state->negotiatedFps = fps;
    return 0;
}
int audioInit(int config, OPUS_MULTISTREAM_CONFIGURATION* opus, void*, int) {
    return opus && config == AUDIO_CONFIGURATION_STEREO && opus->sampleRate == 48000 &&
        opus->channelCount == 2 && opus->streams == 1 && opus->coupledStreams == 1 &&
        opus->mapping[0] == 0 && opus->mapping[1] == 1 &&
        (opus->samplesPerFrame == 240 || opus->samplesPerFrame == 480) ? 0 : -1;
}
void audioSample(char* bytes, int size) {
    auto* state = active.load();
    if (!state || state->stop) return;
    try {
        if (!bytes || size <= 0 || size > 4096) { ++state->audioDrops; ++state->audioInvalidDrops; return; }
        const auto now = nowMs();
        stream::Packet packet; packet.kind = stream::Kind::Audio; packet.enqueueMs = now;
        packet.data.resize(static_cast<std::size_t>(size)); std::memcpy(packet.data.data(), bytes, size);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->audio.size() >= 20) {
                ++state->audioDrops; ++state->audioOverflowDrops;
                if (state->assemblyWaiting || state->decoderWaiting) ++state->audioDropsVideoWaiting;
                const auto lastVideo = state->lastVideoMs.load();
                if (!lastVideo || now < lastVideo || now - lastVideo >= 150) ++state->audioDropsVideoStalled;
                state->audioDropLastMs = now;
                state->audioDropLastQueueAgeMs = now >= state->audio.front().enqueueMs ? now - state->audio.front().enqueueMs : 0;
                state->audioDropLastVideoAgeMs = lastVideo && now >= lastVideo ? now - lastVideo : 0;
                state->audio.pop_front();
            }
            state->audio.push_back(std::move(packet)); ++state->audioFrames;
        }
        state->mediaSignals->audio.notify();
    } catch (...) {
        state->fatalFailure = true; state->failed = true; state->terminated = true; state->code = -900;
        state->changed.notify_all(); state->mediaSignals->notifyAll();
    }
}
bool copyFrame(const DECODE_UNIT& unit, std::vector<std::byte>& target) {
    if (unit.fullLength <= 0 || static_cast<std::size_t>(unit.fullLength) > stream::MaxVideoAccessUnitBytes) return false;
    target.reserve(unit.fullLength);
    std::size_t entries = 0;
    for (auto* item = unit.bufferList; item; item = item->next) {
        if (++entries > 4096 || !item->data || item->length <= 0 ||
            static_cast<std::size_t>(item->length) > static_cast<std::size_t>(unit.fullLength) - target.size()) return false;
        const auto position = target.size(); target.resize(position + item->length);
        std::memcpy(target.data() + position, item->data, item->length);
    }
    return target.size() == static_cast<std::size_t>(unit.fullLength);
}
stream::telemetry::network::CoreRole networkRole(std::string_view value) {
    using Role=stream::telemetry::network::CoreRole;
    static constexpr std::pair<std::string_view,Role> roles[]={{"VideoRecv",Role::VideoRecv},{"VideoDec",Role::VideoDecoder},{"AudioRecv",Role::AudioRecv},
        {"AudioDec",Role::AudioDecoder},{"ControlRecv",Role::ControlRecv},{"ControlSend",Role::ControlSend},{"Input",Role::Input},{"VideoPing",Role::VideoPing},
        {"AudioPing",Role::AudioPing},{"LossStats",Role::LossStats},{"ReqIdrFrame",Role::ReqIdrFrame},{"CtrlAsyncCb",Role::CtrlAsyncCb},{"InvRefFrames",Role::InvRefFrames}};
    for(const auto& role:roles)if(role.first==value)return role.second;
    return Role::Unknown;
}
void drainCoreEvents(State& state,bool typedConsumer=false) noexcept {
    try {
        using namespace stream::telemetry::network;
        std::lock_guard<std::mutex> consumer(coreEventsMutex);
        const bool typed=state.typedNetworkEvents.load();
        if(typed!=typedConsumer||!state.coreEventsReadable)return;
        if(!typed&&!moonlight_diagnostics::sink.load())return;
        // Demand transfer, never overwrite the adapter's unread prefix. Reserve
        // one slot for the source-loss record that any read may discover.
        const auto free=decltype(state.networkEvents)::Capacity-state.networkEvents.size();
        const auto limit=typed?(free>1?free-1:0):FOG_MOONLIGHT_EVENT_CAPACITY;
        if(!limit)return;
        std::uint64_t missed=0;
        NetworkEvent context;context.provider=Provider::FogHub;
        {std::lock_guard<std::mutex> lock(state.mutex);context.streamEpoch=state.telemetryStreamEpoch;context.transportEpoch=state.telemetryEpoch;context.videoGeneration=state.generation;}
        // Core connectionId is the authoritative event namespace. The adapter's
        // current generation is only drain context, never a guessed source binding.
        context.transportBindingKnown=false;
        FogMoonlightEvent event;
        for(unsigned budget=0;budget<limit&&FogMoonlightReadEvent(&coreEventsCursor,&event,&missed);++budget){
            auto value=context;value.kind=EventKind::Core;
            std::size_t roleBytes=0;while(roleBytes<sizeof(event.role)&&event.role[roleBytes])++roleBytes;
            value.core={event.sequence,event.connectionId,event.waitId,event.timeUs,event.kind,event.frameNumber,event.value,event.requests,event.intactFrames,event.fecFailures,event.totalsValid,
                event.threadCore,event.threadIdealCore,event.threadPriority,event.threadQueryErrors,event.threadAffinityMask,networkRole(std::string_view(event.role,roleBytes))};
            if(typed)state.networkEvents.push(value);else moonlight_diagnostics::emit(coreEventText(value));
        }
        if(missed){state.coreSourceMissed.fetch_add(missed);auto value=context;value.kind=EventKind::CoreMissed;value.core.sequence=coreEventsCursor;value.core.value=missed;
            if(typed)state.networkEvents.push(value);else moonlight_diagnostics::emit(coreEventText(value));}
    }catch(...){/* Optional numeric journal cannot terminate the session worker. */}
}
class CoreFrameCompletion final : public stream::VideoFrameCompletion {
public:
    CoreFrameCompletion(std::shared_ptr<State> state, VIDEO_FRAME_HANDLE handle, std::uint64_t generation)
        : state_(std::move(state)),handle_(handle),generation_(generation) {}
    ~CoreFrameCompletion() override { complete(false); }
    void complete(bool decoded) noexcept override {
        if (completed_.exchange(true)) return;
        bool accepted;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            accepted = decoded && !state_->stop && !state_->failed && !state_->terminated &&
                generation_ == state_->generation;
            if (!accepted) {
                ++state_->videoDrops;
                if (!state_->assemblyWaiting) { ++state_->generation; ++state_->videoResyncs; }
                state_->assemblyWaiting = true;
            }
        }
        const auto began = detail::steadyUs();
        LiCompleteVideoFrame(handle_, accepted ? DR_OK : DR_NEED_IDR);
        state_->trace.completion.add(detail::steadyUs() - began);
        if (!accepted) state_->mediaSignals->video.notify();
        { std::lock_guard<std::mutex> lock(state_->pullMutex); state_->pullActive = false; }
        state_->pullChanged.notify_all();
    }
private:
    std::shared_ptr<State> state_;
    VIDEO_FRAME_HANDLE handle_;
    std::uint64_t generation_;
    std::atomic<bool> completed_{false};
};
bool pullVideo(State& state, stream::Packet& packet, bool wait, std::uint64_t observedWake,
               const std::shared_ptr<State>& owner = nullptr) {
    {
        std::lock_guard<std::mutex> lock(state.pullMutex);
        if (!state.pullOpen || state.pullActive || state.stop || state.failed || state.terminated ||
            (wait && state.mediaSignals->video.sequence() != observedWake)) return false;
        state.pullActive = true;
    }
    // This scope includes both the blocking pull and completion. A terminal
    // callback may close admission concurrently, but cannot destroy the queue
    // or decoder unit until completion releases this scope.
    struct PullScope {
        State& state;
        bool owns = true;
        ~PullScope() {
            if (!owns) return;
            { std::lock_guard<std::mutex> lock(state.pullMutex); state.pullActive = false; }
            state.pullChanged.notify_all();
        }
    } scope{state};
    VIDEO_FRAME_HANDLE handle{}; PDECODE_UNIT unit{};
    state.trace.worker(detail::steadyUs());
    const bool received = wait ? LiWaitForNextVideoFrame(&handle, &unit) : LiPollNextVideoFrame(&handle, &unit);
    if (!received) {
        if (!wait) ++state.trace.emptyPolls;
        else {
            // A sticky core wake can outlive the receipt being decoded when
            // stop/reset signalled it. Re-enter LiWait on the next iteration,
            // rather than sleeping on the unrelated application signal for 1s.
            state.mediaSignals->video.notify();
        }
        return false;
    }
    const auto processingStarted = detail::steadyUs();
    int result = DR_NEED_IDR;
    // Every successfully pulled handle is completed exactly once, including
    // allocation failures and shutdown racing the copy.
    struct FrameScope {
        State& state;
        VIDEO_FRAME_HANDLE handle;
        int& result;
        bool owns = true;
        ~FrameScope() {
            if (!owns) return;
            const auto completeStarted = detail::steadyUs();
            LiCompleteVideoFrame(handle, result);
            state.trace.completion.add(detail::steadyUs() - completeStarted);
        }
    } frame{state,handle,result};
    if (state.stop || state.failed || state.terminated) return false;
    if (unit) state.trace.frame(static_cast<std::uint32_t>(unit->frameNumber), unit->frameType == FRAME_TYPE_IDR,
        unit->fullLength, unit->receiveTimeUs, unit->enqueueTimeUs, LiGetMicroseconds(), unit->frameHostProcessingLatency);
    std::uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        generation = state.generation;
    }
    try {
        if (unit && (!state.assemblyWaiting || unit->frameType == FRAME_TYPE_IDR)) {
            stream::Packet output;
            const auto copyStarted = detail::steadyUs();
            const bool copied = copyFrame(*unit, output.data);
            state.trace.copy.add(detail::steadyUs() - copyStarted);
            if (copied) {
                output.timestamp = unit->rtpTimestamp; output.enqueueMs = nowMs();
                // Core timestamps can use a different epoch. Preserve elapsed
                // receipt age rather than copying an absolute core timestamp.
                const auto coreNowUs = LiGetMicroseconds();
                if (unit->receiveTimeUs && coreNowUs >= unit->receiveTimeUs) {
                    const auto ageMs = (coreNowUs - unit->receiveTimeUs) / 1000;
                    if (output.enqueueMs >= ageMs) output.firstPacketMs = output.enqueueMs - ageMs;
                }
                const auto lockStarted = detail::steadyUs();
                std::lock_guard<std::mutex> lock(state.mutex);
                state.trace.queueLock.add(detail::steadyUs() - lockStarted);
                // A decoder command issued during the copy invalidates this
                // frame; an old IDR must not be labelled as the new generation.
                if (generation == state.generation && !state.stop && !state.failed && !state.terminated) {
                    state.assemblyWaiting = false;
                    output.videoGeneration = generation;
                    if (wait) {
                        if (!owner) throw std::logic_error("Blocking pull requires retained session state");
                        output.completion = std::make_shared<CoreFrameCompletion>(owner,handle,generation);
                    }
                    packet = std::move(output); ++state.frames; result = DR_OK;
                    if (wait) { frame.owns = false; scope.owns = false; }
                    state.lastVideoMs = nowMs();
                }
            } else ++state.videoInvalid;
        } else if (!unit) ++state.videoInvalid;
        else ++state.videoWaitDrops;
    } catch (...) {
        state.fatalFailure = true; state.failed = true; state.terminated = true; state.code = -901;
        state.changed.notify_all(); state.mediaSignals->notifyAll();
    }
    if (result != DR_OK) {
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            ++state.videoDrops;
            if (!state.assemblyWaiting) { ++state.generation; ++state.videoResyncs; }
            state.assemblyWaiting = true;
        }
        // Media's generic empty-queue wait must immediately re-enter the core
        // pull after a rejected AU. The replacement IDR wakes the core queue,
        // not an application producer queue (which no longer exists).
        state.mediaSignals->video.notify();
    }
    state.trace.drain.add(detail::steadyUs() - processingStarted);
    return result == DR_OK;
}
void freeApps(APP_LIST* app) {
    while (app) { auto* next = app->next; std::free(app->name); std::free(app); app = next; }
}
bool applicationRequestAborted(State& state) {
    if (!state.stop && state.valid && !Settings::instance().request_cancelled()) return false;
    if (!state.stop) state.fail("Moonlight application request cancelled or timed out");
    return true;
}
bool launch(State& state, bool resumeOnly) {
    using Retry = detail::ResumeRetry;
    const auto deadline = Clock::now() + std::chrono::milliseconds(Retry::BudgetMs);
    const unsigned attempts = resumeOnly ? Retry::MaxAttempts : 1;
    for (unsigned attempt = 1; attempt <= attempts; ++attempt) {
        if (applicationRequestAborted(state)) return false;
        // Every resume attempt refreshes this exact pinned host, without pairing
        // or changing the retained rental/app identity. Old RTSP URLs are private.
        delete[] state.host.serverInfo.rtspSessionUrl; state.host.serverInfo.rtspSessionUrl = nullptr;
        moonlight_diagnostics::beginCommand(moonlight_diagnostics::CommandOperation::None);
        int code = gs_init(&state.host, state.endpoint);
        moonlight_diagnostics::finishCommand(code, Settings::instance().request_cancelled());
        const auto hostRead = moonlight_diagnostics::commandSnapshot();
        if (code != GS_OK || !state.host.paired) {
            state.fail("Moonlight authenticated host unavailable", code, resumeOnly && code != GS_OK &&
                detail::retryableHostRead(hostRead.curl,hostRead.http,hostRead.cancelled)); return false;
        }
        APP_LIST* raw = nullptr;
        moonlight_diagnostics::beginCommand(moonlight_diagnostics::CommandOperation::None);
        code = gs_applist(&state.host, &raw);
        moonlight_diagnostics::finishCommand(code, Settings::instance().request_cancelled());
        const auto appRead = moonlight_diagnostics::commandSnapshot();
        std::unique_ptr<APP_LIST, decltype(&freeApps)> apps(raw, freeApps);
        if (code != GS_OK) {
            state.fail("Moonlight application list unavailable", code, resumeOnly &&
                detail::retryableHostRead(appRead.curl,appRead.http,appRead.cancelled)); return false;
        }
        std::vector<Application> applications;
        for (auto* app = raw; app; app = app->next) {
            if (app->id <= 0 || !app->name) continue;
            applications.push_back({app->id, app->name});
        }
        const auto selected = selectApplication(applications);
        if (!selected) { state.fail("Moonlight host has no unambiguous Desktop, Playnite or Steam launcher"); return false; }
        if (state.host.currentGame != 0 && state.host.currentGame != *selected) {
            state.fail("Moonlight host has another running application"); return false;
        }
        if (resumeOnly && (state.host.currentGame != *selected || state.application != *selected)) {
            state.fail("Moonlight quality reconnect refused: previous application is no longer active"); return false;
        }
        if (applicationRequestAborted(state)) return false;
        // Clear even an early SDK setup failure; a stale 503 must never permit retry.
        moonlight_diagnostics::beginCommand(resumeOnly ? moonlight_diagnostics::CommandOperation::Resume :
            (state.host.currentGame ? moonlight_diagnostics::CommandOperation::Resume : moonlight_diagnostics::CommandOperation::Launch));
        const auto closed = state.transportClosedMs.load();
        const auto requestedAt = nowMs();
        const auto closedToResume = resumeOnly && closed && requestedAt >= closed ? requestedAt - closed : 0;
        code = gs_start_app(&state.host, &state.config, *selected, false, false, 1);
        const auto command = moonlight_diagnostics::commandSnapshot();
        { std::lock_guard<std::mutex> lock(state.mutex);
            state.command = command; state.commandAttempt = attempt; state.closedToResumeMs = closedToResume;
            if (code != GS_OK) {
                if (state.commandFailures == 0) state.firstCommandFailure = command;
                ++state.commandFailures;
            }
        }
        if (code == GS_OK) {
            // An acknowledged command owns this app even if its HTTP budget
            // just expired. Start a distinct transport stage; never clear a
            // manual cancellation or replay /launch because the reply was late.
            state.application = *selected;
            if (state.stop || !state.valid || Settings::instance().cancel_requested()) {
                if (!state.stop) state.fail("Moonlight application request cancelled");
                return false;
            }
            Settings::instance().set_timeout_ms(15000);
            return true;
        }
        if (applicationRequestAborted(state)) return false;
        if (resumeOnly && Clock::now() >= deadline) {
            state.fail("Moonlight application resume deadline exceeded"); return false;
        }
        const bool retry = resumeOnly && command.operation == moonlight_diagnostics::CommandOperation::Resume &&
            Retry::retryable(command.curl,command.http,command.xmlStatus,command.xmlResult,
                command.dispatched,command.cancelled);
        const auto delay = std::chrono::milliseconds(Retry::backoffMs(attempt));
        if (!retry || attempt == attempts || Clock::now() + delay >= deadline ||
            state.stop || !state.valid || Settings::instance().request_cancelled()) {
            if (!state.stop) state.fail(resumeOnly ? "Moonlight application resume failed" : "Moonlight application start failed", code, retry);
            return false;
        }
        state.status("Moonlight resume retry pending");
        const auto retryAt = Clock::now() + delay;
        while (Clock::now() < retryAt) {
            if (applicationRequestAborted(state)) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    return false;
}
class Client final : public stream::StreamingClient {
public:
    explicit Client(std::shared_ptr<State> state) : state_(std::move(state)) {}
    ~Client() override { close(); }
    bool start(const stream::StreamConnection& connection) override {
        close();
        if (!state_ || !state_->valid || !state_->paired || connection.rental.empty() ||
            connection.rental != state_->rental || state_->streamInUse.exchange(true)) return false;
        resumeOnly_ = connection.resumeOnly;
        state_->rumble.clear();
        state_->stop = false; state_->failed = false; state_->terminated = false;
        state_->workerFinished = false;
        state_->fatalFailure = false; state_->retryableFailure = false;
        state_->code = 0; state_->terminationCode = 0;
        state_->terminationOrigin = FOG_TERMINATION_TRANSPORT;
        state_->frames = 0; state_->audioFrames = 0; state_->videoDrops = 0; state_->audioDrops = 0;
        state_->audioOverflowDrops = 0; state_->audioInvalidDrops = 0;
        state_->audioDropsVideoWaiting = 0; state_->audioDropsVideoStalled = 0;
        state_->audioDropLastMs = 0; state_->audioDropLastQueueAgeMs = 0; state_->audioDropLastVideoAgeMs = 0;
        state_->inputAccepted = 0; state_->inputRejected = 0; state_->inputIgnored = 0;
        state_->accelRateHz = 0; state_->gyroRateHz = 0; lastMotionMs_ = {};
        state_->videoInvalid = 0; state_->videoWaitDrops = 0; state_->videoResyncs = 0;
        state_->trace.reset();
        {std::lock_guard<std::mutex> consumer(coreEventsMutex);
            state_->coreEventsReadable=false;state_->networkEvents.discardPending();}
        state_->coreDiagnosticsReady = false;
        state_->selectedCodec = stream::VideoCodec::H264;
        state_->negotiatedWidth = 0; state_->negotiatedHeight = 0; state_->negotiatedFps = 0;
        state_->decoderWaiting = true; state_->assemblyWaiting = true;
        state_->lastVideoMs = 0;
        // No callbacks or previous pull/consumer threads remain at this point.
        state_->mediaSignals = mediaSignals();
        { std::lock_guard<std::mutex> lock(state_->mutex); state_->audio.clear(); state_->error.clear();
            state_->encryptionResult = {};
            state_->telemetryCoreClockUs=state_->telemetryCoreSteadyUs=0;
            ++state_->telemetryEpoch;
            state_->command = {}; state_->firstCommandFailure = {}; state_->commandFailures = 0;
            state_->commandAttempt = 0; state_->closedToResumeMs = 0; ++state_->generation; }
        state_->config.width = 1280; state_->config.height = 720; state_->config.fps = settings_.framesPerSecond;
        state_->config.bitrate = settings_.bitrateMbps * 1000;
        state_->config.packetSize = 1024; state_->config.streamingRemotely = STREAM_CFG_REMOTE;
        state_->config.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
        state_->config.encryptionFlags = detail::encryptionFlags();
        state_->config.supportedVideoFormats = VIDEO_FORMAT_H264;
        if (settings_.codec == stream::VideoCodec::H265) state_->config.supportedVideoFormats |= VIDEO_FORMAT_H265;
        state_->config.clientRefreshRateX100 = 6000; state_->config.colorSpace = COLORSPACE_REC_709;
        state_->config.colorRange = COLOR_RANGE_LIMITED;
        state_->status(("Connecting Moonlight 720p" + std::to_string(settings_.framesPerSecond)).c_str());
        Settings::instance().reset_cancel();
        Settings::instance().set_timeout_ms(resumeOnly_ ? detail::ResumeRetry::BudgetMs : 25000);
        try { worker_ = std::thread(&Client::run, this); }
        catch (...) { state_->workerFinished = true; state_->streamInUse = false; state_->fail("Cannot start Moonlight worker"); return false; }
        return true;
    }
    bool configureVideo(const fog::video::Settings& value) override {
        if (!fog::video::valid(value) || (state_ && state_->streamInUse)) return false;
        // Codec preference and bitrate apply at connection setup; RTC presets and
        // periodic PLI are not part of the Moonlight protocol.
        settings_ = value; return true;
    }
    bool configureRelay(bool enabled) override { return !enabled; }
    bool configureController(stream::ControllerProfile profile) override {
        if (!stream::validControllerProfile(profile) || (state_ && state_->streamInUse)) return false;
        profile_ = profile; return true;
    }
    bool updateVideoSettings(const fog::video::Settings& value) override {
        return value == settings_; // Live bitrate/quality/keyframe changes unsupported.
    }
    stream::VideoCodec selectedVideoCodec() const override { return state_->selectedCodec.load(); }
    std::string qualityStatus() const override {
        return "FogHub 720p" + std::to_string(settings_.framesPerSecond) + " / " + std::string(stream::videoCodecName(selectedVideoCodec())) +
            " / " + std::to_string(settings_.bitrateMbps) + " Мбит/с (запрос) / " + detail::encryptionSummary(encryptionSnapshot());
    }
    void pump() override {
        // The application keeps pumping while LiStartConnection runs. Its
        // transport-stage deadline is independent of the acknowledged HTTP
        // command and can interrupt a blocked core handshake.
        if (state_->starting && Settings::instance().request_cancelled()) {
            std::lock_guard<std::mutex> lock(state_->apiMutex);
            if (state_->starting) LiInterruptConnection();
        }
    }
    void observePlayback(const stream::PlaybackObservation&) override {}
    bool poll(stream::Packet& packet) override { return pullVideo(*state_, packet, false, 0); }
    bool pollVideo(stream::Packet& packet) override {
        return pollVideo(packet, state_->mediaSignals->video.sequence());
    }
    bool pollVideo(stream::Packet& packet, std::uint64_t observedWake) override {
        return pullVideo(*state_, packet, true, observedWake, state_);
    }
    bool mediaAdmissionAllowed() const override {
        return state_->valid && state_->running && !state_->stop && !state_->failed && !state_->terminated && !state_->assemblyWaiting;
    }
    std::uint64_t currentVideoGeneration() const override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->generation;
    }
    void wakeMediaWaiters() override {
        // Increment before taking the pull gate. A caller that has sampled the
        // old sequence but has not entered the core yet must refuse to block.
        state_->mediaSignals->notifyAll();
        std::lock_guard<std::mutex> lock(state_->pullMutex);
        if (state_->pullOpen && state_->pullActive) LiWakeWaitForVideoFrame();
    }
    bool pollAudio(stream::Packet& packet) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->audio.empty() || state_->stop || state_->failed || state_->terminated) return false;
        packet = std::move(state_->audio.front()); state_->audio.pop_front(); return true;
    }
    bool pollRumble(stream::Rumble& value) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->valid || !state_->running || state_->stop || state_->failed || state_->terminated) {
            state_->rumble.clear(); return false;
        }
        return state_->rumble.take(value);
    }
    // There is no intermediate assembled-video queue. Core pending depth is
    // available in the synchronized diagnostic snapshot; frame age is measured
    // from the decode unit carried directly to Media.
    std::uint64_t queuedVideoAgeMs() const override { return 0; }
    std::size_t queuedVideoPackets() const override { return coreSnapshot().decodeQueuePending; }
    bool ready() const override { return state_ && state_->running && !state_->stop && !state_->failed && !state_->terminated; }
    std::string status() const override { std::lock_guard<std::mutex> lock(state_->mutex); return state_->message; }
    std::string error() const override { std::lock_guard<std::mutex> lock(state_->mutex); return state_->error; }
    bool retryableError() const override {
        return state_ && !state_->stop && retryIdentityValid() && retryableFailure();
    }
    bool disconnected() const override { return state_->terminated; }
    stream::RecoveryState recoveryState() const override {
        // A host callback is authoritative even before the worker publishes its
        // error text. Terminal closure must never enter transient ICE grace.
        if (state_->failed || state_->terminated)
            return retryableError() ? stream::RecoveryState::RetryableFailure : stream::RecoveryState::TerminalFailure;
        return stream::RecoveryState::Connected;
    }
    bool disconnectForRetry() override {
        // Joining consumes this transport's single retry permission. The app
        // must inspect the exact retained rental/endpoint before starting again.
        const bool eligible = worker_.joinable() && state_ && !state_->stop && retryIdentityValid() &&
            ((!state_->failed && !state_->terminated && state_->running) || retryableFailure());
        close();
        // A terminal callback may arrive during LiStopConnection(). Re-read its
        // evidence after every core callback thread has joined.
        return eligible && retryIdentityValid() && !state_->fatalFailure &&
            ((!state_->failed && !state_->terminated) || retryableFailure());
    }
    stream::telemetry::protocol::ProtocolSnapshot captureProtocolTelemetry()const noexcept override {
        namespace p=stream::telemetry::protocol;p::ProtocolSnapshot out;out.provider=p::Provider::FogHub;out.capturedAtSteadyUs=fog::metrics::steadyNowUs();
        try{auto& h=out.fh;FogMoonlightDiagnostics encryption{};
            {std::lock_guard<std::mutex> lock(state_->mutex);out.streamEpoch=state_->telemetryStreamEpoch;out.transportEpoch=state_->telemetryEpoch;out.videoGeneration=state_->generation;out.sourceCapturedAtSteadyUs=state_->workerFinished?state_->telemetryCoreSteadyUs:out.capturedAtSteadyUs;
                out.state=out.transportEpoch?(state_->workerFinished?p::State::FinalRetained:p::State::Live):p::State::NotPublished;
                const auto& c=state_->command;h.command={static_cast<unsigned>(c.operation),c.curl,c.xmlStatus,c.xmlResult,c.gameSession,c.http,c.dispatched,c.cancelled,c.elapsedMs,c.httpElapsedMs};
                const auto& first=state_->firstCommandFailure;h.firstFailure={static_cast<unsigned>(first.operation),first.curl,first.xmlStatus,first.xmlResult,first.gameSession,first.http,first.dispatched,first.cancelled,first.elapsedMs,first.httpElapsedMs};
                h.attempt=state_->commandAttempt;h.failures=state_->commandFailures;h.closedToResumeMs=state_->closedToResumeMs;encryption=state_->encryptionResult;h.encryptionCapturedAtUs=state_->telemetryCoreSteadyUs;h.encryptionKnown=state_->telemetryCoreSteadyUs!=0;}
            h.video=state_->frames.load(std::memory_order_relaxed);
            h.audio=state_->audioFrames.load(std::memory_order_relaxed);
            h.width=state_->negotiatedWidth.load(std::memory_order_relaxed);
            h.height=state_->negotiatedHeight.load(std::memory_order_relaxed);
            h.fps=state_->negotiatedFps.load(std::memory_order_relaxed);
            h.code=state_->code.load(std::memory_order_relaxed);
            h.terminationCode=state_->terminationCode.load(std::memory_order_relaxed);
            h.assemblyWaiting=state_->assemblyWaiting.load(std::memory_order_relaxed);
            h.decoderWaiting=state_->decoderWaiting.load(std::memory_order_relaxed);
            h.retryableFailure=state_->retryableFailure.load(std::memory_order_relaxed);
            h.fatalFailure=state_->fatalFailure.load(std::memory_order_relaxed);
            h.inputAccepted=state_->inputAccepted.load(std::memory_order_relaxed);
            h.inputRejected=state_->inputRejected.load(std::memory_order_relaxed);
            h.inputIgnored=state_->inputIgnored.load(std::memory_order_relaxed);
            h.accelRateHz=state_->accelRateHz.load(std::memory_order_relaxed);
            h.gyroRateHz=state_->gyroRateHz.load(std::memory_order_relaxed);
            h.codecH265=selectedVideoCodec()==stream::VideoCodec::H265;h.requestedH265=settings_.codec==stream::VideoCodec::H265;h.requestedFps=settings_.framesPerSecond;h.requestedMbps=settings_.bitrateMbps;
            h.hostTermination=state_->terminationOrigin==FOG_TERMINATION_HOST;h.dualSense=profile_==stream::ControllerProfile::DualSense;
            const auto& q=state_->trace.queueLock;h.queueLock={q.count.load(),q.totalUs.load(),q.maxUs.load(),q.over50ms.load()};
            h.encryption.requested=detail::encryptionFlags();
            h.encryption.encryptionClientFlags=encryption.encryptionClientFlags;
            h.encryption.encryptionState=encryption.encryptionState;
            h.encryption.encryptionFailure=encryption.encryptionFailure;
            h.encryption.encryptionHostSupported=encryption.encryptionHostSupported;
            h.encryption.encryptionHostRequired=encryption.encryptionHostRequired;
            h.encryption.encryptionSelected=encryption.encryptionSelected;
            h.encryption.encryptionAudio=encryption.encryptionAudio;
            h.encryption.encryptionControl=encryption.encryptionControl;
            h.encryption.encryptionRtsp=encryption.encryptionRtsp;
            h.encryption.videoDecryptSuccesses=encryption.videoDecryptSuccesses;
            h.encryption.audioDecryptSuccesses=encryption.audioDecryptSuccesses;
            h.encryption.audioDecryptFailures=encryption.audioDecryptFailures;
        }catch(...){out.state=p::State::CaptureFailed;}out.completedAtSteadyUs=fog::metrics::steadyNowUs();return out;
    }
    std::string diagnostics() const override {
        moonlight_diagnostics::CommandSnapshot command;
        moonlight_diagnostics::CommandSnapshot firstFailure;
        unsigned attempt, failures;
        std::uint64_t closedToResume;
        { std::lock_guard<std::mutex> lock(state_->mutex); command = state_->command; attempt = state_->commandAttempt;
            firstFailure = state_->firstCommandFailure; failures = state_->commandFailures; closedToResume = state_->closedToResumeMs; }
        const char* operation = command.operation == moonlight_diagnostics::CommandOperation::Resume ? "resume" :
            command.operation == moonlight_diagnostics::CommandOperation::Launch ? "launch" : "none";
        return "Moonlight code=" + std::to_string(state_->code.load()) +
            " command=" + operation + " attempt=" + std::to_string(attempt) +
            " curl=" + std::to_string(command.curl) + " http=" + std::to_string(command.http) +
            " xml=" + std::to_string(command.xmlStatus) + " xmlResult=" + std::to_string(command.xmlResult) +
            " ackField=" + (command.operation == moonlight_diagnostics::CommandOperation::Resume ? "resume" : "gamesession") +
            " ack=" + std::to_string(command.gameSession) +
            " dispatched=" + std::to_string(command.dispatched) + " cancelled=" + std::to_string(command.cancelled) +
            " commandElapsedMs=" + std::to_string(command.elapsedMs) + " httpElapsedMs=" + std::to_string(command.httpElapsedMs) +
            " commandFailures=" + std::to_string(failures) + " firstFailureCurl=" + std::to_string(firstFailure.curl) +
            " firstFailureHttp=" + std::to_string(firstFailure.http) + " firstFailureXml=" + std::to_string(firstFailure.xmlStatus) +
            " firstFailureAck=" + std::to_string(firstFailure.gameSession) +
            " closedToResumeMs=" + std::to_string(closedToResume) +
            " video=" + std::to_string(state_->frames.load()) + " audio=" + std::to_string(state_->audioFrames.load()) +
            " assemblyWaiting=" + std::to_string(state_->assemblyWaiting.load()) +
            " decoderWaiting=" + std::to_string(state_->decoderWaiting.load()) +
            " codec=" + std::string(stream::videoCodecName(selectedVideoCodec())) +
            " requestedCodec=" + std::string(stream::videoCodecName(settings_.codec)) +
            " width=" + std::to_string(state_->negotiatedWidth.load()) +
            " height=" + std::to_string(state_->negotiatedHeight.load()) +
            " fps=" + std::to_string(state_->negotiatedFps.load()) +
            " requestedFps=" + std::to_string(settings_.framesPerSecond) +
            " requestedMbps=" + std::to_string(settings_.bitrateMbps) +
            " inputAccepted=" + std::to_string(state_->inputAccepted.load()) +
            " inputRejected=" + std::to_string(state_->inputRejected.load()) +
            " terminationCode=" + std::to_string(state_->terminationCode.load()) +
            " terminationOrigin=" + (state_->terminationOrigin == FOG_TERMINATION_HOST ? "host" : "transport") +
            " retryableFailure=" + std::to_string(state_->retryableFailure.load()) +
            " fatalFailure=" + std::to_string(state_->fatalFailure.load()) + detail::encryptionDiagnostics(encryptionSnapshot());
    }
    stream::telemetry::Snapshot captureTelemetry(stream::telemetry::Capture mask) const noexcept override {
        using namespace stream::telemetry;
        Snapshot out;out.provider=Provider::FogHub;out.capturedAtSteadyUs=fog::metrics::steadyNowUs();
        out.input.scope=Scope::ClientRolling5s;
        try {
            {std::lock_guard<std::mutex> lock(state_->mutex);
                out.streamEpoch=state_->telemetryStreamEpoch;out.transportEpoch=state_->telemetryEpoch;out.videoGeneration=state_->generation;
                if(includes(mask,Capture::Control)){
                    const auto& c=state_->command;auto& f=out.fhControl;
                    f.accepted=state_->inputAccepted.load();f.rejected=state_->inputRejected.load();f.ignored=state_->inputIgnored.load();
                    f.operation=static_cast<int>(c.operation);f.curl=c.curl;f.http=c.http;f.xmlStatus=c.xmlStatus;
                    f.xmlResult=c.xmlResult;f.ack=c.gameSession;f.result=c.result;f.elapsedMs=c.elapsedMs;f.httpElapsedMs=c.httpElapsedMs;
                    f.dispatched=c.dispatched;f.cancelled=c.cancelled;f.attempt=state_->commandAttempt;
                    f.failures=state_->commandFailures;f.closedToResumeMs=state_->closedToResumeMs;
                    f.command={state_->commandAttempt?Availability::Available:Availability::NotStarted,
                        state_->commandAttempt?Reason::None:Reason::NotYetPublished,Scope::TransportLifetime};
                }
                if(includes(mask,Capture::Transport)&&!state_->running&&state_->workerFinished&&state_->coreDiagnosticsReady){
                    captureFhCore(out.fh,state_->encryptionResult);
                    out.fh.core={Availability::FinalRetained,Reason::None,Scope::TransportLifetime};
                }
            }
            out.sequence=++telemetrySequence_;
            const Status status{out.transportEpoch?(state_->workerFinished?Availability::FinalRetained:Availability::Available):Availability::NotStarted,
                out.transportEpoch?Reason::None:Reason::NotYetPublished,Scope::TransportLifetime};
            if(includes(mask,Capture::Control))out.control=status;
            if(includes(mask,Capture::Transport)){
                out.transport=status;
                const auto& t=state_->trace;
                out.fh.adapter={captureAtomicTiming(t.workerGap),captureAtomicTiming(t.drain),captureAtomicTiming(t.copy),captureAtomicTiming(t.completion)};
                out.fh.emptyPolls=t.emptyPolls.load(std::memory_order_relaxed);
                if(state_->running&&state_->coreDiagnosticsReady){
                    // Atomics-only diagnostic API for this State's retained core lease.
                    // No SDK network/decoder query and no provider pointer leaves capture.
                    captureFhCore(out.fh,coreSnapshot());out.fh.core={Availability::Available,Reason::None,Scope::TransportLifetime};
                }else if(!available(out.fh.core))out.fh.core={Availability::TemporarilyUnavailable,Reason::NotYetPublished,Scope::TransportLifetime};
            }
            if(includes(mask,Capture::Input)){
                out.input={Availability::Available,Reason::None,Scope::ClientRolling5s};
                out.inputLatency=capturePercentiles(inputLatency_.snapshot(out.capturedAtSteadyUs));
            }
        }catch(...){
            const Status failed{Availability::TemporarilyUnavailable,Reason::CaptureFailed,Scope::TransportLifetime};
            if(includes(mask,Capture::Transport))out.transport=failed;
            if(includes(mask,Capture::Control))out.control=failed;
            if(includes(mask,Capture::Input)){out.input=failed;out.input.scope=Scope::ClientRolling5s;}
        }
        out.captureCompletedAtSteadyUs=fog::metrics::steadyNowUs();return out;
    }
    stream::telemetry::network::NetworkSnapshot captureNetworkTelemetry() const noexcept override {
        namespace net=stream::telemetry::network;
        net::NetworkSnapshot out;out.provider=net::Provider::FogHub;out.capturedAtSteadyUs=fog::metrics::steadyNowUs();
        try{
            FogMoonlightDiagnostics core{};
            {std::lock_guard<std::mutex> lock(state_->mutex);
                out.streamEpoch=state_->telemetryStreamEpoch;out.transportEpoch=state_->telemetryEpoch;out.videoGeneration=state_->generation;
                if(state_->workerFinished&&state_->coreDiagnosticsReady){core=state_->encryptionResult;out.core.state=net::State::FinalRetained;out.core.hasValue=true;
                    out.core.capturedAtUs=state_->telemetryCoreSteadyUs;out.fh.coreNowUs=state_->telemetryCoreClockUs;}
            }
            if(state_->running&&state_->coreDiagnosticsReady){core=coreSnapshot();out.core.state=net::State::Live;out.core.hasValue=true;
                out.core.capturedAtUs=fog::metrics::steadyNowUs();out.fh.coreNowUs=LiGetMicroseconds();}
            out.core.epoch=out.transportEpoch;out.core.generation=out.videoGeneration;out.core.ageKnown=out.core.hasValue&&out.capturedAtSteadyUs>=out.core.capturedAtUs;
            out.core.ageUs=out.core.ageKnown?out.capturedAtSteadyUs-out.core.capturedAtUs:0;out.core.stale=!out.core.hasValue||!out.core.ageKnown||out.core.ageUs>1000000;
            if(out.core.hasValue){out.fh.core=net::captureCore(core);out.fh.coreClockKnown=out.fh.coreNowUs!=0;
                const std::uint64_t stamps[]={core.lastVideoPacketUs,core.idrWaitStartedUs,core.lastIdrRequestUs,core.lastIdrDispatchUs};
                for(unsigned i=0;i<4;++i)if(stamps[i]&&out.fh.coreClockKnown&&out.fh.coreNowUs>=stamps[i])out.fh.coreAgeKnownMask|=1u<<i;}
            out.fh.codecKnown=out.core.hasValue;out.fh.codecH265=state_->selectedCodec.load()==stream::VideoCodec::H265;
            const auto stage=[](const detail::TimingSamples& t){return net::StageTiming{t.count.load(),t.totalUs.load(),t.maxUs.load(),t.over50ms.load()};};
            const auto& t=state_->trace;out.fh.videoTimings={stage(t.assembly),stage(t.coreAge),stage(t.arrivalGap),stage(t.hostProcessing)};
            out.fh.units=t.units.load();out.fh.payloadBytes=t.payloadBytes.load();out.fh.idrs=t.idrs.load();out.fh.skippedFrameNumbers=t.skippedFrameNumbers.load();
            out.fh.repeatedOrReordered=t.repeatedOrReordered.load();out.fh.invalidTimes=t.invalidTimes.load();const auto last=t.lastUnitUs.load();
            out.fh.lastUnitAgeKnown=last&&out.fh.coreClockKnown&&out.fh.coreNowUs>=last;out.fh.lastUnitAgeUs=out.fh.lastUnitAgeKnown?out.fh.coreNowUs-last:0;
            out.fh.videoDrops=state_->videoDrops.load();out.fh.audioDrops=state_->audioDrops.load();out.fh.audioOverflowDrops=state_->audioOverflowDrops.load();out.fh.audioInvalidDrops=state_->audioInvalidDrops.load();
            out.fh.audioDropsVideoWaiting=state_->audioDropsVideoWaiting.load();out.fh.audioDropsVideoStalled=state_->audioDropsVideoStalled.load();out.fh.audioDropLastMs=state_->audioDropLastMs.load();
            out.fh.audioDropLastQueueAgeMs=state_->audioDropLastQueueAgeMs.load();out.fh.audioDropLastVideoAgeMs=state_->audioDropLastVideoAgeMs.load();
            out.fh.videoInvalid=state_->videoInvalid.load();out.fh.videoWaitDrops=state_->videoWaitDrops.load();out.fh.videoResyncs=state_->videoResyncs.load();
            out.rtcp.state=out.control.state=out.history.state=net::State::NotSupported;
            out.eventMissed=state_->networkEvents.lost();out.eventCursor=state_->networkEvents.cursor();out.coreSourceMissed=state_->coreSourceMissed.load();
        }catch(...){out.core.state=net::State::CaptureFailed;out.core.hasValue=false;}
        out.completedAtSteadyUs=fog::metrics::steadyNowUs();
        out.core.ageKnown=out.core.hasValue&&out.completedAtSteadyUs>=out.core.capturedAtUs;
        out.core.ageUs=out.core.ageKnown?out.completedAtSteadyUs-out.core.capturedAtUs:0;
        out.core.stale=!out.core.ageKnown||out.core.ageUs>1000000||out.core.state==net::State::CaptureFailed;return out;
    }
    void enableNetworkEvents(bool enabled) noexcept override {
        try{std::lock_guard<std::mutex> consumer(coreEventsMutex);
            state_->typedNetworkEvents=enabled;if(!enabled)state_->networkEvents.discardPending();
        }catch(...){}
    }
    bool peekNetworkEvent(stream::telemetry::network::NetworkEvent& out)const noexcept override {
        try{drainCoreEvents(*state_,true);
            stream::telemetry::network::EventJournal<stream::telemetry::network::NetworkEvent>::Entry entry;
            if(!state_->networkEvents.peek(entry))return false;
        out=entry.value;out.journalSequence=entry.sequence;out.journalMissed=entry.missed;return true;
        }catch(...){return false;}
    }
    std::size_t peekNetworkEvents(stream::telemetry::network::NetworkEvent* out,std::size_t capacity)const noexcept override {
        if(!out||!capacity)return 0;
        try{
            drainCoreEvents(*state_,true);
            using Journal=stream::telemetry::network::EventJournal<stream::telemetry::network::NetworkEvent>;
            std::array<Journal::Entry,Journal::Capacity> entries{};
            const auto count=state_->networkEvents.peekBatch(entries.data(),std::min(capacity,entries.size()));
            for(std::size_t i=0;i<count;++i){out[i]=entries[i].value;out[i].journalSequence=entries[i].sequence;out[i].journalMissed=entries[i].missed;}
            return count;
        }catch(...){return 0;}
    }
    bool retireNetworkEvent(std::uint64_t sequence,bool admitted)noexcept override {
        try{return state_->networkEvents.retire(sequence,admitted);}catch(...){return false;}
    }
    unsigned networkEventFinalDiscardBudget()const noexcept override {
        constexpr auto capacity=decltype(state_->networkEvents)::Capacity;
        static_assert(capacity>1);
        // Retained adapter prefix + core ring + at most one loss marker for
        // each refill of the core ring (one adapter slot is reserved for it).
        return capacity+FOG_MOONLIGHT_EVENT_CAPACITY+
            (FOG_MOONLIGHT_EVENT_CAPACITY+capacity-2)/(capacity-1);
    }
    std::string transportTimings() const override {
        return stream::telemetry::transportText(captureTelemetry(stream::telemetry::Capture::Transport));
    }
    std::string videoDiagnostics() const override {
        const auto& t = state_->trace;
        const auto now = state_->coreDiagnosticsReady ? LiGetMicroseconds() : 0;
        return "Moonlight " + std::string(stream::videoCodecName(selectedVideoCodec())) + " cumulative units=" + std::to_string(t.units.load()) +
            " encoded_payload_bytes=" + std::to_string(t.payloadBytes.load()) + " idr=" + std::to_string(t.idrs.load()) +
            " skipped_frame_numbers=" + std::to_string(t.skippedFrameNumbers.load()) +
            " repeated_or_reordered=" + std::to_string(t.repeatedOrReordered.load()) +
            " invalid_times=" + std::to_string(t.invalidTimes.load()) +
            " last_unit_age_us=" + std::to_string(t.ageUs(now)) +
            " | " + t.assembly.text("assembly") + " | " + t.coreAge.text("core_queue_age") +
            " | " + t.arrivalGap.text("unit_gap") + " | " + t.hostProcessing.text("host_processing");
    }
    std::string gapDiagnostics() const override {
        const auto core = coreSnapshot();
        const auto now = state_->coreDiagnosticsReady ? LiGetMicroseconds() : 0;
        return "Moonlight core cumulative available=" + std::to_string(state_->coreDiagnosticsReady.load()) +
            " udp_video_packets=" + std::to_string(core.videoPacketsReceived) +
            " udp_video_bytes=" + std::to_string(core.videoBytesReceived) +
            " receive_errors=" + std::to_string(core.videoReceiveErrors) +
            " decrypt_failures=" + std::to_string(core.videoDecryptFailures) +
            " runt_packets=" + std::to_string(core.videoRuntPackets) +
            " last_packet_age_us=" + std::to_string(core.lastVideoPacketUs && now >= core.lastVideoPacketUs ? now - core.lastVideoPacketUs : 0) +
            " packet_gap_max_us=" + std::to_string(core.maxVideoPacketGapUs) +
            " rtp_processing_n=" + std::to_string(core.videoPacketProcessingCount) +
            " rtp_processing_avg_us=" + std::to_string(core.videoPacketProcessingCount ? core.videoPacketProcessingTotalUs / core.videoPacketProcessingCount : 0) +
            " rtp_processing_max_us=" + std::to_string(core.videoPacketProcessingMaxUs) +
            " rtp_processing_invalid_times=" + std::to_string(core.videoPacketProcessingInvalidTimes) +
            " fec_blocks_complete=" + std::to_string(core.fecBlocksCompleted) +
            " fec_blocks_failed=" + std::to_string(core.fecBlocksFailed) +
            " fec_packets_recovered=" + std::to_string(core.fecPacketsRecovered) +
            " fec_reconstruction_failures=" + std::to_string(core.fecReconstructionFailures) +
            " rtp_reject_seq=" + std::to_string(core.videoPacketsRejectedSequence) +
            " rtp_reject_frame=" + std::to_string(core.videoPacketsRejectedFrame) +
            " rtp_reject_block=" + std::to_string(core.videoPacketsRejectedBlock) +
            " rtp_reject_header=" + std::to_string(core.videoPacketsRejectedHeader) +
            " rtp_reject_range=" + std::to_string(core.videoPacketsRejectedRange) +
            " rtp_reject_duplicate=" + std::to_string(core.videoPacketsRejectedDuplicate) +
            " rtp_reject_transition=" + std::to_string(core.videoPacketsRejectedTransition) +
            " rtp_out_of_order=" + std::to_string(core.videoPacketsOutOfOrder) +
            " rtp_gap_fill=" + std::to_string(core.videoPacketsGapFill) +
            " rtp_observed_gap_packets=" + std::to_string(core.videoSequenceGapPackets) +
            " fec_loss_predictions=" + std::to_string(core.fecSpeculativeLossPredictions) +
            " fec_loss_mispredictions=" + std::to_string(core.fecSpeculativeLossMispredictions) +
            " fec_speculation_resumes=" + std::to_string(core.fecSpeculativeModeResumes) +
            " fec_oos_mode=" + std::to_string(core.fecOosMode) +
            " fec_abandoned_current=" + std::to_string(core.fecAbandonedCurrentBlocks) +
            " fec_abandoned_queued=" + std::to_string(core.fecAbandonedPacketsQueued) +
            " fec_abandoned_data_expected=" + std::to_string(core.fecAbandonedDataExpected) +
            " fec_abandoned_data_received=" + std::to_string(core.fecAbandonedDataReceived) +
            " fec_abandoned_data_missing=" + std::to_string(core.fecAbandonedDataMissing) +
            " fec_abandoned_parity_expected=" + std::to_string(core.fecAbandonedParityExpected) +
            " fec_abandoned_parity_received=" + std::to_string(core.fecAbandonedParityReceived) +
            " fec_abandoned_parity_missing=" + std::to_string(core.fecAbandonedParityMissing) +
            " fec_abandoned_data_missing_max=" + std::to_string(core.fecAbandonedMissingDataMax) +
            " fec_abandoned_parity_missing_max=" + std::to_string(core.fecAbandonedMissingParityMax) +
            " core_idr_requests=" + std::to_string(core.idrRequests) +
            " core_waiting_idr=" + std::to_string(core.waitingForIdr) +
            " core_waiting_idr_drops=" + std::to_string(core.waitingForIdrDrops) +
            " core_idr_wait_entries=" + std::to_string(core.idrWaitEntries) +
            " core_idr_wait_reason=" + std::to_string(core.idrWaitReason) +
            " core_idr_wait_age_us=" + std::to_string(core.waitingForIdr && core.idrWaitStartedUs && now >= core.idrWaitStartedUs ? now - core.idrWaitStartedUs : 0) +
            " core_idr_wait_releases=" + std::to_string(core.idrWaitReleases) +
            " core_idr_wait_last_us=" + std::to_string(core.idrWaitLastUs) +
            " core_idr_wait_max_us=" + std::to_string(core.idrWaitMaxUs) +
            " core_idr_wait_retries=" + std::to_string(core.idrWaitRetryRequests) +
            " core_idr_drop_limit_requests=" + std::to_string(core.idrDropLimitRequests) +
            " core_idr_request_age_us=" + std::to_string(core.lastIdrRequestUs && now >= core.lastIdrRequestUs ? now - core.lastIdrRequestUs : 0) +
            " core_idr_dispatches=" + std::to_string(core.idrDispatches) +
            " core_idr_dispatch_ok=" + std::to_string(core.idrDispatchSuccesses) +
            " core_idr_dispatch_failed=" + std::to_string(core.idrDispatchFailures) +
            " core_idr_dispatch_age_us=" + std::to_string(core.lastIdrDispatchUs && now >= core.lastIdrDispatchUs ? now - core.lastIdrDispatchUs : 0) +
            " core_idr_frames_completed=" + std::to_string(core.idrFramesCompleted);
    }
    std::string queueDiagnostics() const override {
        const auto core = coreSnapshot();
        return "Moonlight video_drop=" + std::to_string(state_->videoDrops.load()) + " audio_drop=" + std::to_string(state_->audioDrops.load()) +
            " audio_overflow_drop=" + std::to_string(state_->audioOverflowDrops.load()) +
            " audio_invalid_drop=" + std::to_string(state_->audioInvalidDrops.load()) +
            " audio_drop_video_waiting=" + std::to_string(state_->audioDropsVideoWaiting.load()) +
            " audio_drop_video_stalled=" + std::to_string(state_->audioDropsVideoStalled.load()) +
            " audio_drop_last_ms=" + std::to_string(state_->audioDropLastMs.load()) +
            " audio_drop_last_queue_age_ms=" + std::to_string(state_->audioDropLastQueueAgeMs.load()) +
            " audio_drop_last_video_age_ms=" + std::to_string(state_->audioDropLastVideoAgeMs.load()) +
            " video_pull=1 application_video_queue=0" +
            " invalid_frames=" + std::to_string(state_->videoInvalid.load()) + " waiting_idr_drops=" + std::to_string(state_->videoWaitDrops.load()) +
            " invalid_resyncs=" + std::to_string(state_->videoResyncs.load()) +
            " core_pending=" + std::to_string(core.decodeQueuePending) +
            " core_high=" + std::to_string(core.decodeQueueHigh) +
            " core_overflows=" + std::to_string(core.decodeQueueOverflows) +
            " core_flushes=" + std::to_string(core.decodeQueueFlushes) +
            " core_flushed_frames=" + std::to_string(core.decodeQueueFlushedFrames);
    }
    std::string mutexDiagnostics() const override {
        return "Moonlight cumulative " + state_->trace.queueLock.text("worker_queue_lock");
    }
    std::string dtlsDiagnostics() const override { return "Moonlight transport has no DTLS"; }
    std::string rtcpDiagnostics() const override { return "Moonlight feedback managed by core"; }
    std::string inputDiagnostics() const override {
        return std::string("Moonlight input profile=") + (profile_ == stream::ControllerProfile::DualSense ? "dualsense" : "xbox") +
            " accepted=" + std::to_string(state_->inputAccepted.load()) + " rejected=" + std::to_string(state_->inputRejected.load()) +
            " optionalIgnored=" + std::to_string(state_->inputIgnored.load()) +
            " accelRateHz=" + std::to_string(state_->accelRateHz.load()) + " gyroRateHz=" + std::to_string(state_->gyroRateHz.load());
    }
    bool pollVideoDiagnostic(std::string&) override { return false; }
    bool waitingForVideoKeyframe() const override {
        return state_->assemblyWaiting || state_->decoderWaiting;
    }
    bool supportsInputSuspension() const noexcept override { return true; }
    stream::InputSuspension inputSuspension() const override {
        const auto core = coreSnapshot();
        const auto last = state_->lastVideoMs.load();
        const auto now = nowMs();
        return {core.waitingForIdr != 0, state_->assemblyWaiting || state_->decoderWaiting,
            !last || (now >= last && now - last >= 150)};
    }
    bool requestVideoKeyframe() override {
        std::lock_guard<std::mutex> lock(state_->apiMutex);
        if (!state_->running || state_->stop) return false;
        if (fog::stage1::localIdrRetry && state_->decoderWaiting) {
            // Hand local decode failures to the existing core wait/retry path.
            // The receive thread owns depacketizer state and consumes this command.
            FogIdrRecoveryRequestDecoderRefresh();
            std::lock_guard<std::mutex> queueLock(state_->mutex);
            state_->assemblyWaiting = true; ++state_->generation;
        } else LiRequestIdrFrame();
        return true;
    }
    void setDecoderWaitingForKeyframe(bool waiting) override { state_->decoderWaiting = waiting; }
    std::string inputLatencyDiagnostics() const override {
        return stream::telemetry::inputText(captureTelemetry(stream::telemetry::Capture::Input));
    }
    bool sendGamepad(const stream::Gamepad& gamepad) override {
        std::lock_guard<std::mutex> lock(state_->apiMutex);
        if (!state_->running || state_->stop) return rejected();
        const int flags = detail::controllerButtonFlags(gamepad.buttons, profile_);
        const int result = LiSendMultiControllerEvent(0, 1, flags, trigger(gamepad.leftTrigger), trigger(gamepad.rightTrigger),
            axis(gamepad.axes[0]), axis(-gamepad.axes[1]), axis(gamepad.axes[2]), axis(-gamepad.axes[3]));
        const auto completedUs = fog::metrics::steadyNowUs();
        if (result == 0 && gamepad.sampledAtUs && completedUs >= gamepad.sampledAtUs)
            inputLatency_.record(completedUs - gamepad.sampledAtUs, completedUs);
        return accepted(result);
    }
    bool sendInput(const fog::remote::Input& input) override {
        std::lock_guard<std::mutex> lock(state_->apiMutex);
        if (!state_->running || state_->stop) return rejected();
        return std::visit([this](const auto& value) { return send(value); }, input);
    }
    void close() override {
        if (!worker_.joinable()) return;
        { std::lock_guard<std::mutex> lock(state_->mutex); state_->stop = true; }
        state_->rumble.clear();
        state_->changed.notify_all();
        state_->mediaSignals->notifyAll();
        // LiStartConnection resets its interrupt flag during initialization.
        // A cancellation delivered after starting=true but before that reset
        // must be repeated. Use a condition deadline, preserving prompt final
        // completion notifications and never holding mutex over core APIs.
        {
            std::unique_lock<std::mutex> lock(state_->mutex);
            while (!state_->workerFinished) {
                lock.unlock();
                Settings::instance().request_cancel();
                { std::lock_guard<std::mutex> api(state_->apiMutex); if (state_->starting) LiInterruptConnection(); }
                lock.lock();
                state_->changed.wait_for(lock, std::chrono::milliseconds(10), [&] { return state_->workerFinished.load(); });
            }
        }
        worker_.join();
        // Pairing and other clients cannot replace the authenticated State or
        // its process-global protocol lease until every worker has really joined.
        state_->streamInUse = false;
        state_->rumble.clear();
    }
private:
    std::shared_ptr<State> state_;
    fog::video::Settings settings_;
    stream::ControllerProfile profile_ = stream::ControllerProfile::Xbox;
    std::array<std::uint64_t, 2> lastMotionMs_{};
    fog::metrics::LatencyHistogram inputLatency_; // Application thread only.
    mutable std::uint64_t telemetrySequence_ = 0; // Application-owner capture only.
    bool resumeOnly_ = false;
    std::thread worker_;
    bool retryIdentityValid() const {
        return state_->valid && state_->paired && state_->application > 0;
    }
    bool retryableFailure() const {
        if (state_->fatalFailure) return false;
        return state_->terminated ? state_->terminationOrigin != FOG_TERMINATION_HOST && detail::retryableTransportError(state_->terminationCode) :
            state_->failed && state_->retryableFailure;
    }
    FogMoonlightDiagnostics coreSnapshot() const {
        FogMoonlightDiagnostics result{};
        if (state_->coreDiagnosticsReady) FogMoonlightGetDiagnostics(&result);
        return result;
    }
    FogMoonlightDiagnostics encryptionSnapshot() const {
        if (state_->running && state_->coreDiagnosticsReady) return coreSnapshot();
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->encryptionResult;
    }
    void captureEncryption() {
        FogMoonlightDiagnostics result{};
        FogMoonlightGetDiagnostics(&result);
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->encryptionResult = result;
        state_->telemetryCoreClockUs=LiGetMicroseconds();state_->telemetryCoreSteadyUs=fog::metrics::steadyNowUs();
    }
    bool accepted(int result) { if (result == 0) { ++state_->inputAccepted; return true; } return rejected(); }
    bool rejected() { ++state_->inputRejected; return false; }
    bool ignored() { ++state_->inputIgnored; return true; }
    static short axis(float value) { return static_cast<short>(std::isfinite(value) ? std::clamp(value, -1.f, 1.f) * 32767.f : 0); }
    static unsigned char trigger(float value) { return static_cast<unsigned char>(std::isfinite(value) ? std::clamp(value, 0.f, 1.f) * 255.f : 0); }
    bool send(const fog::remote::AbsoluteMouse& value) {
        if (value.width <= 0 || value.width > 32767 || value.height <= 0 || value.height > 32767) return rejected();
        return accepted(LiSendMousePositionEvent(static_cast<short>(std::clamp(value.x, 0, value.width - 1)),
            static_cast<short>(std::clamp(value.y, 0, value.height - 1)), value.width, value.height));
    }
    bool send(const fog::remote::MouseButton& value) {
        if (value.button != 0 && value.button != 2) return rejected();
        return accepted(LiSendMouseButtonEvent(value.down ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE,
            value.button == 0 ? BUTTON_MOUSE_LEFT : BUTTON_MOUSE_RIGHT));
    }
    bool send(const fog::remote::Wheel& value) {
        // App input uses DOM deltaY (positive down), while Windows wheel units
        // are positive up. Preserve the small +/-32 deltas rather than turning
        // each into 32 full 120-unit clicks.
        const auto amount = std::clamp<std::int64_t>(-static_cast<std::int64_t>(value.dy), -32767, 32767);
        return accepted(LiSendHighResScrollEvent(static_cast<short>(amount)));
    }
    bool send(const fog::remote::Key& value) {
        short key = value.code == "Enter" ? 0x0d : value.code == "Backspace" ? 0x08 :
            value.code == "Tab" ? 0x09 : value.code == "Escape" ? 0x1b : 0;
        if (!key) return rejected();
        return accepted(LiSendKeyboardEvent(key, value.down ? KEY_ACTION_DOWN : KEY_ACTION_UP, 0));
    }
    bool send(const fog::remote::UnicodeText& value) {
        try { fog::remote::validateText(value.utf8); } catch (...) { return rejected(); }
        return accepted(LiSendUtf8TextEvent(value.utf8.data(), static_cast<unsigned int>(value.utf8.size())));
    }
    bool send(const fog::remote::Touch& value) {
        const int type = detail::controllerTouchType(value);
        if (profile_ != stream::ControllerProfile::DualSense || type < 0) return rejected();
        // Unsupported optional input must not hold up contact initialization and
        // consequently block all ordinary controller buttons on older hosts.
        if (!(LiGetHostFeatureFlags() & LI_FF_CONTROLLER_TOUCH_EVENTS)) return ignored();
        const int result = LiSendControllerTouchEvent(0, static_cast<std::uint8_t>(type),
            static_cast<std::uint32_t>(value.finger), value.x, value.y, type == LI_TOUCH_EVENT_UP ? 0.f : 1.f);
        return result == LI_ERR_UNSUPPORTED ? ignored() : accepted(result);
    }
    bool send(const fog::remote::Sensor& value) {
        std::array<float, 3> xyz{};
        const int type = detail::controllerMotion(value, xyz);
        if (profile_ != stream::ControllerProfile::DualSense || type < 0) return rejected();
        if (!(LiGetHostFeatureFlags() & LI_FF_CONTROLLER_TOUCH_EVENTS)) return ignored();
        const unsigned rate = type == LI_MOTION_TYPE_ACCEL ? state_->accelRateHz.load() : state_->gyroRateHz.load();
        if (!rate) return ignored();
        const auto now = nowMs();
        auto& last = lastMotionMs_[static_cast<std::size_t>(type - 1)];
        const auto period = (1000u + rate - 1u) / rate;
        // Focus/overlay loss must stop motion immediately even if the previous
        // sample was sent within this host's requested reporting period.
        const bool neutral = xyz[0] == 0.f && xyz[2] == 0.f &&
            xyz[1] == (type == LI_MOTION_TYPE_ACCEL ? 9.80665f : 0.f);
        if (!neutral && last && now >= last && now - last < period) return ignored();
        const int result = LiSendControllerMotionEvent(0, static_cast<std::uint8_t>(type), xyz[0], xyz[1], xyz[2]);
        if (result == LI_ERR_UNSUPPORTED) return ignored();
        if (result == 0) last = now;
        return accepted(result);
    }
    void run() noexcept {
        fog::diagnostics::observeThread("moonlight-session");
        bool started = false;
        try {
            if (!state_->stop && launch(*state_, resumeOnly_) && !state_->stop) {
                DECODER_RENDERER_CALLBACKS video{}; LiInitializeVideoCallbacks(&video);
                video.setup = videoSetup; video.capabilities = CAPABILITY_PULL_RENDERER;
                AUDIO_RENDERER_CALLBACKS audio{}; LiInitializeAudioCallbacks(&audio);
                audio.init = audioInit; audio.decodeAndPlaySample = audioSample; audio.capabilities = CAPABILITY_SLOW_OPUS_DECODER;
                CONNECTION_LISTENER_CALLBACKS listeners{}; LiInitializeConnectionCallbacks(&listeners);
                listeners.stageStarting = stageStarting; listeners.stageFailed = stageFailed;
                listeners.connectionTerminated = terminated; listeners.logMessage = ignoreLog;
                listeners.rumble = rumble;
                listeners.setMotionEventState = motionEventState;
                active = state_.get();
                { std::lock_guard<std::mutex> lock(state_->apiMutex);
                    if (!state_->stop && state_->valid && !Settings::instance().request_cancelled()) {
                        state_->starting = true;
                    }
                }
                int result = -1;
                if (state_->starting) {
                    result = LiStartConnection(&state_->host.serverInfo, &state_->config, &listeners, &video, &audio, nullptr, 0, nullptr, 0);
                    captureEncryption();
                    {std::lock_guard<std::mutex> consumer(coreEventsMutex);state_->coreEventsReadable=true;}
                }
                { std::lock_guard<std::mutex> lock(state_->apiMutex); state_->starting = false; }
                if (result != 0) {
                    if (!state_->stop) state_->fail("Moonlight connection failed", result,
                        result != -1 && detail::retryableTransportError(result));
                }
                else {
                    started = true; state_->coreDiagnosticsReady = true;
                    { std::lock_guard<std::mutex> lock(state_->pullMutex); state_->pullOpen = true; }
                    { std::lock_guard<std::mutex> lock(state_->apiMutex);
                        LiSendControllerArrivalEvent(0, 1, detail::controllerType(profile_),
                            detail::controllerButtons(profile_), detail::controllerCapabilities(profile_));
                        state_->running = true;
                    }
                    state_->mediaSignals->notifyAll();
                    state_->status(("Moonlight stream active: 720p" + std::to_string(settings_.framesPerSecond)).c_str());
                    while (!state_->stop && !state_->failed && !state_->terminated) {
                        captureEncryption();
                        drainCoreEvents(*state_);
                        // Journaling remains live even during an indefinite core
                        // frame wait. This is a sparse diagnostic deadline, not
                        // a video/audio readiness poll.
                        std::unique_lock<std::mutex> lock(state_->mutex);
                        state_->changed.wait_for(lock, std::chrono::seconds(1), [&] {
                            return state_->stop || state_->failed || state_->terminated;
                        });
                    }
                    if (state_->terminated) state_->fail("Moonlight host disconnected", state_->terminationCode,
                        state_->terminationOrigin != FOG_TERMINATION_HOST && detail::retryableTransportError(state_->terminationCode));
                }
            }
        } catch (...) { state_->fail("Moonlight worker failed"); }
        {
            std::lock_guard<std::mutex> lock(state_->apiMutex);
            state_->running = false; state_->starting = false;
        }
        {
            std::unique_lock<std::mutex> lock(state_->pullMutex);
            state_->pullOpen = false;
            if (started && state_->pullActive) LiWakeWaitForVideoFrame();
            state_->pullChanged.wait(lock, [&] { return !state_->pullActive; });
        }
        state_->changed.notify_all();
        state_->mediaSignals->notifyAll();
        // UI input only takes apiMutex long enough to observe running=false;
        // no caller can enter a core API while stop joins its internal threads.
        if (started) {
            LiSendMultiControllerEvent(0, 0, 0, 0, 0, 0, 0, 0, 0);
            LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_MOUSE_LEFT);
            LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_MOUSE_RIGHT);
            for (short key : {short(0x0d), short(0x08), short(0x09), short(0x1b)}) LiSendKeyboardEvent(key, KEY_ACTION_UP, 0);
            LiStopConnection();
            captureEncryption();
            state_->transportClosedMs = nowMs();
        }
        active = nullptr;
        state_->rumble.clear();
        drainCoreEvents(*state_);
        { std::lock_guard<std::mutex> lock(state_->mutex); state_->workerFinished = true; }
        state_->changed.notify_all();
    }
};
}

struct MoonlightSession::Impl {
    std::string directory;
    std::shared_ptr<State> state;
    std::thread pairing;
    explicit Impl(std::string value) : directory(std::move(value)) {}
};
MoonlightSession::MoonlightSession(std::string directory) : impl_(std::make_unique<Impl>(std::move(directory))) {}
MoonlightSession::~MoonlightSession() { cancelAndJoin(); }
std::string MoonlightSession::generate() {
    // Rejection sampling avoids modulo bias in the four-digit PIN.
    std::uint32_t value;
    do { randomGet(&value, sizeof(value)); } while (value >= 4294960000u);
    char digits[5]; std::snprintf(digits, sizeof(digits), "%04u", value % 10000); return digits;
}
bool MoonlightSession::begin(const fog::PairedEndpoint& endpoint, const std::string& pin) {
    if (!validHost(endpoint.host) || !endpoint.httpPort || pin.size() != 4 || pin.find_first_not_of("0123456789") != std::string::npos) return false;
    if (impl_->state && impl_->state->streamInUse) return false;
    cancelAndJoin();
    if (impl_->state) { impl_->state->valid = false; impl_->state->releaseLease(); }
    auto state = std::make_shared<State>();
    if (!state->claimLease()) return false;
    impl_->state = state;
    state->pairingDeadline = Clock::now() + std::chrono::seconds(60);
    state->identity = impl_->directory + "/" + randomSuffix();
    state->endpoint = (endpoint.host.find(':') == std::string::npos ? endpoint.host : "[" + endpoint.host + "]") + ":" + std::to_string(endpoint.httpPort);
    Settings::instance().set_production(true);
    Settings::instance().set_key_dir(state->identity);
    Settings::instance().reset_cancel(); Settings::instance().set_timeout_ms(60000);
    try {
        impl_->pairing = std::thread([state, pin = std::string(pin)]() mutable {
            fog::diagnostics::observeThread("moonlight-pairing");
            bool success = false;
            try {
                std::error_code error; std::filesystem::create_directories(state->identity, error);
                if (error) state->fail("Cannot create private Moonlight identity directory");
                else {
                    int result = gs_init(&state->host, state->endpoint);
                    if (result != GS_OK) state->fail("Moonlight host discovery failed", result);
                    else if (state->host.paired) state->fail("Fresh Moonlight identity unexpectedly already paired");
                    else {
                        char secret[5]{pin[0], pin[1], pin[2], pin[3], 0};
                        result = gs_pair(&state->host, secret);
                        for (volatile char& digit : secret) digit = 0;
                        if (result != GS_OK) state->fail("Moonlight automatic pairing failed", result);
                        else { success = true; state->status("Moonlight host authenticated"); }
                    }
                }
            } catch (...) { state->fail("Moonlight pairing worker failed"); }
            // The worker owns its PIN copy until completion. Never put it in logs.
            for (volatile char& digit : pin) digit = 0;
            state->paired = success; state->pairOK = success; state->pairDone = true; state->changed.notify_all();
        });
    } catch (...) { state->fail("Cannot start Moonlight pairing worker"); state->pairDone = true; return false; }
    return true;
}
bool MoonlightSession::waitReady() {
    auto state = impl_->state; if (!state) return false;
    const auto deadline = std::min(state->pairingDeadline, Clock::now() + std::chrono::seconds(15));
    while (!Settings::instance().pairing_was_dispatched()) {
        if (state->pairDone || Clock::now() >= deadline) return false;
        std::unique_lock<std::mutex> lock(state->mutex); state->changed.wait_for(lock, std::chrono::milliseconds(20));
    }
    // Sunshine blocks getservercert waiting for the provider's PIN. HEADER_OUT
    // proves local dispatch only; allow the remote handler to register its pending
    // pairing request, then let the provider submit PIN. There is no ready ACK.
    const auto settled = Clock::now() + std::chrono::milliseconds(500);
    while (Clock::now() < settled) {
        if (state->pairDone && !state->pairOK) return false;
        std::unique_lock<std::mutex> lock(state->mutex); state->changed.wait_for(lock, std::chrono::milliseconds(20));
    }
    return !state->failed;
}
bool MoonlightSession::complete() {
    auto state = impl_->state; if (!state) return false;
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->changed.wait_until(lock, state->pairingDeadline, [&] { return state->pairDone.load(); });
    }
    if (!state->pairDone) Settings::instance().request_cancel();
    if (impl_->pairing.joinable()) impl_->pairing.join();
    return state->pairOK;
}
bool MoonlightSession::submissionReady() const noexcept {
    const auto state = impl_->state;
    return state && state->valid && !state->pairDone && !state->failed &&
        Settings::instance().pairing_was_dispatched() && !Settings::instance().request_cancelled() &&
        Clock::now() + std::chrono::seconds(5) < state->pairingDeadline;
}
std::string MoonlightSession::diagnostics() const {
    const auto state = impl_->state;
    if (!state) return "Moonlight pairing state=absent";
    std::string result = "Moonlight pairing code=" + std::to_string(state->code.load()) +
        " pair_done=" + std::to_string(state->pairDone.load()) +
        " pair_ok=" + std::to_string(state->pairOK.load()) +
        " dispatched=" + std::to_string(Settings::instance().pairing_was_dispatched()) +
        " cancelled=" + std::to_string(Settings::instance().request_cancelled());
    // Every State error comes from a fixed literal in this adapter. Never use
    // upstream gs_error(), response content, endpoint/identity values or PINs.
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->error.empty()) result += " error=" + state->error;
    return result;
}
void MoonlightSession::cancelAndJoin() noexcept {
    if (!impl_->pairing.joinable()) return;
    if (impl_->state && !impl_->state->pairDone) Settings::instance().request_cancel();
    impl_->pairing.join();
}
bool MoonlightSession::bindRental(const fog::EntityId& rental) {
    if (!impl_->state || !impl_->state->paired || !impl_->state->pairDone || rental.provider != fog::ProviderId::FogHub || rental.empty() || impl_->state->streamInUse) return false;
    impl_->state->rental = rental; return true;
}
std::unique_ptr<stream::StreamingClient> MoonlightSession::create() {
    if (!impl_->state || !impl_->state->paired || impl_->state->rental.empty()) return {};
    return std::make_unique<Client>(impl_->state);
}
}
