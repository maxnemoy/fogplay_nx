#include "moonlight_session.hpp"
#include "app_selection.hpp"
#include "frame_diagnostics.hpp"
#include "resume_retry.hpp"
#include "transport_retry.hpp"
#include "controller_input.hpp"
#include "features/streaming/application/remote_input.hpp"
#include "features/streaming/infrastructure/rumble_mailbox.hpp"
#include <Settings.hpp>
#include <Diagnostics.hpp>
#include <FogMoonlightDiagnostics.h>
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
#include <utility>
#include <arpa/inet.h>
#include <sys/socket.h>

namespace moonlight {
namespace {
using Clock = std::chrono::steady_clock;
std::atomic<bool> protocolClaimed{false};
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
    static constexpr std::size_t VideoCapacity = 4;
    bool ownsLease = false;
    SERVER_DATA host{};
    STREAM_CONFIGURATION config{};
    fog::EntityId rental;
    std::string identity, endpoint;
    std::atomic<bool> valid{true}, paired{false}, pairDone{false}, pairOK{false};
    std::atomic<bool> stop{false}, running{false}, starting{false}, streamInUse{false};
    std::atomic<bool> terminated{false}, failed{false}, decoderWaiting{true}, assemblyWaiting{true};
    std::atomic<bool> fatalFailure{false}, retryableFailure{false};
    std::atomic<bool> coreDiagnosticsReady{false};
    std::atomic<int> code{0};
    std::atomic<int> terminationCode{0};
    std::atomic<int> application{0};
    std::atomic<std::uint64_t> transportClosedMs{0};
    std::atomic<stream::VideoCodec> selectedCodec{stream::VideoCodec::H264};
    std::atomic<int> negotiatedWidth{0}, negotiatedHeight{0}, negotiatedFps{0};
    std::atomic<std::uint64_t> frames{0}, audioFrames{0}, videoDrops{0}, audioDrops{0}, inputAccepted{0}, inputRejected{0};
    std::atomic<std::uint64_t> inputIgnored{0};
    std::atomic<unsigned short> accelRateHz{0}, gyroRateHz{0};
    std::atomic<std::uint64_t> videoFullYields{0}, videoQueueHigh{0}, videoBatchMax{0}, videoQueueAgeMaxMs{0};
    std::atomic<std::uint64_t> videoInvalid{0}, videoWaitDrops{0}, videoResyncs{0};
    detail::FrameDiagnostics trace;
    stream::RumbleMailbox rumble;
    mutable std::mutex mutex, apiMutex;
    std::condition_variable changed;
    std::deque<stream::Packet> video, audio;
    std::string message = "Moonlight ready for pairing", error;
    std::uint64_t generation = 1;
    moonlight_diagnostics::CommandSnapshot command;
    moonlight_diagnostics::CommandSnapshot firstCommandFailure;
    unsigned commandAttempt = 0;
    unsigned commandFailures = 0;
    std::uint64_t closedToResumeMs = 0;
    Clock::time_point pairingDeadline;
    State() { LiInitializeServerInformation(&host.serverInfo); LiInitializeStreamConfiguration(&config); }
    ~State() { delete[] host.serverInfo.rtspSessionUrl; releaseLease(); }
    bool claimLease() { bool expected = false; ownsLease = protocolClaimed.compare_exchange_strong(expected, true); return ownsLease; }
    void releaseLease() { if (ownsLease) { ownsLease = false; protocolClaimed = false; } }
    void status(const char* value) { std::lock_guard<std::mutex> lock(mutex); message = value; }
    void fail(const char* value, int failure = -1, bool retryable = false) {
        { std::lock_guard<std::mutex> lock(mutex); error = value; message = value;
            if (!terminated) code = failure;
            retryableFailure = retryable;
            if (!retryable) fatalFailure = true;
            failed = true; rumble.clear(); }
        changed.notify_all();
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
        std::lock_guard<std::mutex> lock(state->mutex);
        state->code = code; state->terminationCode = code;
        if (!detail::retryableTransportError(code)) state->fatalFailure = true;
        state->terminated = true; state->rumble.clear();
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
        if (!bytes || size <= 0 || size > 4096) { ++state->audioDrops; return; }
        stream::Packet packet; packet.kind = stream::Kind::Audio; packet.enqueueMs = nowMs();
        packet.data.resize(static_cast<std::size_t>(size)); std::memcpy(packet.data.data(), bytes, size);
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->audio.size() >= 20) { ++state->audioDrops; state->audio.pop_front(); }
        state->audio.push_back(std::move(packet)); ++state->audioFrames;
    } catch (...) { state->fatalFailure = true; state->failed = true; state->terminated = true; state->code = -900; }
}
bool copyFrame(const DECODE_UNIT& unit, std::vector<std::byte>& target) {
    constexpr std::size_t limit = 8 * 1024 * 1024;
    if (unit.fullLength <= 0 || static_cast<std::size_t>(unit.fullLength) > limit) return false;
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
void drainVideo(State& state) {
    VIDEO_FRAME_HANDLE handle; PDECODE_UNIT unit;
    unsigned budget = 0;
    while (budget < 8 && !state.stop) {
        {
            const auto lockStarted = detail::steadyUs();
            std::lock_guard<std::mutex> lock(state.mutex);
            state.trace.queueLock.add(detail::steadyUs() - lockStarted);
            // This worker is the sole producer. Leave complete frames in the
            // core's bounded queue until the consumer has room, preserving FIFO.
            if (state.video.size() >= State::VideoCapacity) { ++state.videoFullYields; break; }
        }
        if (!LiPollNextVideoFrame(&handle, &unit)) { ++state.trace.emptyPolls; break; }
        ++budget;
        if (unit) state.trace.frame(static_cast<std::uint32_t>(unit->frameNumber), unit->frameType == FRAME_TYPE_IDR,
            unit->fullLength, unit->receiveTimeUs, unit->enqueueTimeUs, LiGetMicroseconds(), unit->frameHostProcessingLatency);
        int result = DR_NEED_IDR;
        try {
            if (unit && (!state.assemblyWaiting || unit->frameType == FRAME_TYPE_IDR)) {
                stream::Packet packet;
                const auto copyStarted = detail::steadyUs();
                const bool copied = copyFrame(*unit, packet.data);
                state.trace.copy.add(detail::steadyUs() - copyStarted);
                if (copied) {
                    packet.timestamp = unit->rtpTimestamp; packet.enqueueMs = nowMs();
                    const auto lockStarted = detail::steadyUs();
                    std::lock_guard<std::mutex> lock(state.mutex);
                    state.trace.queueLock.add(detail::steadyUs() - lockStarted);
                    state.assemblyWaiting = false;
                    packet.videoGeneration = state.generation;
                    state.video.push_back(std::move(packet)); ++state.frames; result = DR_OK;
                    state.videoQueueHigh = std::max(state.videoQueueHigh.load(), static_cast<std::uint64_t>(state.video.size()));
                } else ++state.videoInvalid;
            } else if (!unit) ++state.videoInvalid;
            else ++state.videoWaitDrops;
        } catch (...) { state.fatalFailure = true; state.failed = true; state.terminated = true; state.code = -901; }
        if (result != DR_OK) {
            std::lock_guard<std::mutex> lock(state.mutex);
            ++state.videoDrops;
            if (!state.assemblyWaiting) {
                state.videoDrops += state.video.size(); state.video.clear();
                ++state.generation; ++state.videoResyncs;
            }
            state.assemblyWaiting = true;
        }
        const auto completeStarted = detail::steadyUs();
        LiCompleteVideoFrame(handle, result);
        state.trace.completion.add(detail::steadyUs() - completeStarted);
        if (state.failed) break;
    }
    state.videoBatchMax = std::max(state.videoBatchMax.load(), static_cast<std::uint64_t>(budget));
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
        if (applicationRequestAborted(state)) return false;
        if (resumeOnly && Clock::now() >= deadline) {
            state.fail("Moonlight application resume deadline exceeded"); return false;
        }
        if (code == GS_OK) {
            if (!resumeOnly) state.application = *selected;
            return true;
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
        state_->fatalFailure = false; state_->retryableFailure = false;
        state_->code = 0; state_->terminationCode = 0;
        state_->frames = 0; state_->audioFrames = 0; state_->videoDrops = 0; state_->audioDrops = 0;
        state_->inputAccepted = 0; state_->inputRejected = 0; state_->inputIgnored = 0;
        state_->accelRateHz = 0; state_->gyroRateHz = 0; lastMotionMs_ = {};
        state_->videoFullYields = 0; state_->videoQueueHigh = 0; state_->videoBatchMax = 0; state_->videoQueueAgeMaxMs = 0;
        state_->videoInvalid = 0; state_->videoWaitDrops = 0; state_->videoResyncs = 0;
        state_->trace.reset();
        state_->coreDiagnosticsReady = false;
        state_->selectedCodec = stream::VideoCodec::H264;
        state_->negotiatedWidth = 0; state_->negotiatedHeight = 0; state_->negotiatedFps = 0;
        state_->decoderWaiting = true; state_->assemblyWaiting = true;
        { std::lock_guard<std::mutex> lock(state_->mutex); state_->video.clear(); state_->audio.clear(); state_->error.clear();
            state_->command = {}; state_->firstCommandFailure = {}; state_->commandFailures = 0;
            state_->commandAttempt = 0; state_->closedToResumeMs = 0; ++state_->generation; }
        state_->config.width = 1280; state_->config.height = 720; state_->config.fps = fog::video::DefaultFramesPerSecond;
        state_->config.bitrate = settings_.bitrateMbps * 1000;
        state_->config.packetSize = 1024; state_->config.streamingRemotely = STREAM_CFG_REMOTE;
        state_->config.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
        state_->config.supportedVideoFormats = VIDEO_FORMAT_H264;
        if (settings_.codec == stream::VideoCodec::H265) state_->config.supportedVideoFormats |= VIDEO_FORMAT_H265;
        state_->config.clientRefreshRateX100 = 6000; state_->config.colorSpace = COLORSPACE_REC_709;
        state_->config.colorRange = COLOR_RANGE_LIMITED;
        state_->status("Connecting Moonlight 720p30");
        Settings::instance().reset_cancel();
        Settings::instance().set_timeout_ms(resumeOnly_ ? detail::ResumeRetry::BudgetMs : 25000);
        try { worker_ = std::thread(&Client::run, this); }
        catch (...) { state_->streamInUse = false; state_->fail("Cannot start Moonlight worker"); return false; }
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
        return "FogHub 720p30 / " + std::string(stream::videoCodecName(selectedVideoCodec())) +
            " / " + std::to_string(settings_.bitrateMbps) + " Мбит/с (запрос)";
    }
    void pump() override {}
    void observePlayback(const stream::PlaybackObservation&) override {}
    bool poll(stream::Packet& packet) override { return take(packet, false); }
    bool pollAudio(stream::Packet& packet) override { return take(packet, true); }
    bool pollRumble(stream::Rumble& value) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->valid || !state_->running || state_->stop || state_->failed || state_->terminated) {
            state_->rumble.clear(); return false;
        }
        return state_->rumble.take(value);
    }
    std::uint64_t queuedVideoAgeMs() const override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->video.empty() ? 0 : nowMs() - state_->video.front().enqueueMs;
    }
    std::size_t queuedVideoPackets() const override { std::lock_guard<std::mutex> lock(state_->mutex); return state_->video.size(); }
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
            " requestedFps=" + std::to_string(fog::video::DefaultFramesPerSecond) +
            " requestedMbps=" + std::to_string(settings_.bitrateMbps) +
            " inputAccepted=" + std::to_string(state_->inputAccepted.load()) +
            " inputRejected=" + std::to_string(state_->inputRejected.load()) +
            " terminationCode=" + std::to_string(state_->terminationCode.load()) +
            " retryableFailure=" + std::to_string(state_->retryableFailure.load()) +
            " fatalFailure=" + std::to_string(state_->fatalFailure.load());
    }
    std::string transportTimings() const override {
        const auto& t = state_->trace;
        const auto core = coreSnapshot();
        const auto timing = [](const char* name, std::uint64_t count, std::uint64_t totalUs, std::uint64_t maxUs) {
            return std::string(name) + "(n,avg_us,max_us)=" + std::to_string(count) + "," +
                std::to_string(count ? totalUs / count : 0) + "," + std::to_string(maxUs);
        };
        return "Moonlight cumulative " + t.workerGap.text("worker_gap") + " | " + t.drain.text("drain") +
            " | " + t.copy.text("copy") + " | " + t.completion.text("complete") +
            " empty_polls=" + std::to_string(t.emptyPolls.load()) +
            " | " + timing("rx_cycle", core.videoReceiveCycleCount, core.videoReceiveCycleTotalUs, core.videoReceiveCycleMaxUs) +
            " " + timing("rx_wait", core.videoReceiveCycleCount, core.videoReceiveWaitTotalUs, core.videoReceiveWaitMaxUs) +
            " " + timing("rx_work", core.videoReceiveCycleCount, core.videoReceiveWorkTotalUs, core.videoReceiveWorkMaxUs) +
            " " + timing("rx_loop_gap", core.videoReceiveLoopGapCount, core.videoReceiveLoopGapTotalUs, core.videoReceiveLoopGapMaxUs) +
            " " + timing("rx_alloc", core.videoReceiveAllocationCount, core.videoReceiveAllocationTotalUs, core.videoReceiveAllocationMaxUs) +
            " " + timing("rx_decrypt", core.videoReceiveDecryptCount, core.videoReceiveDecryptTotalUs, core.videoReceiveDecryptMaxUs) +
            " rx_invalid_times=" + std::to_string(core.videoReceiveTimingInvalidTimes) +
            " | rcvbuf(requested,selected,actual)=" + std::to_string(core.videoReceiveBufferRequestedBytes) + "," +
            std::to_string(core.videoReceiveBufferSelectedBytes) + "," + std::to_string(core.videoReceiveBufferActualBytes) +
            " set_failures=" + std::to_string(core.videoReceiveBufferSetFailures) +
            " last_set_error=" + std::to_string(core.videoReceiveBufferSetLastError) +
            " query_ok=" + std::to_string(core.videoReceiveBufferQuerySucceeded) +
            " query_error=" + std::to_string(core.videoReceiveBufferQueryError);
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
            " video_capacity=" + std::to_string(State::VideoCapacity) + " video_high=" + std::to_string(state_->videoQueueHigh.load()) +
            " video_full_yields=" + std::to_string(state_->videoFullYields.load()) + " video_batch_max=" + std::to_string(state_->videoBatchMax.load()) +
            " video_age_max_ms=" + std::to_string(state_->videoQueueAgeMaxMs.load()) +
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
    bool waitingForVideoKeyframe() const override { return state_->assemblyWaiting || state_->decoderWaiting; }
    bool requestVideoKeyframe() override {
        std::lock_guard<std::mutex> lock(state_->apiMutex);
        if (!state_->running || state_->stop) return false;
        LiRequestIdrFrame(); return true;
    }
    void setDecoderWaitingForKeyframe(bool waiting) override { state_->decoderWaiting = waiting; }
    bool sendGamepad(const stream::Gamepad& gamepad) override {
        std::lock_guard<std::mutex> lock(state_->apiMutex);
        if (!state_->running || state_->stop) return rejected();
        const int flags = detail::controllerButtonFlags(gamepad.buttons, profile_);
        return accepted(LiSendMultiControllerEvent(0, 1, flags, trigger(gamepad.leftTrigger), trigger(gamepad.rightTrigger),
            axis(gamepad.axes[0]), axis(-gamepad.axes[1]), axis(gamepad.axes[2]), axis(-gamepad.axes[3])));
    }
    bool sendInput(const fog::remote::Input& input) override {
        std::lock_guard<std::mutex> lock(state_->apiMutex);
        if (!state_->running || state_->stop) return rejected();
        return std::visit([this](const auto& value) { return send(value); }, input);
    }
    void close() override {
        if (!worker_.joinable()) return;
        state_->stop = true;
        state_->rumble.clear();
        { std::lock_guard<std::mutex> lock(state_->apiMutex); if (state_->starting) LiInterruptConnection(); }
        // Cancel pending /applist, /launch or /resume as well as LiStartConnection.
        while (state_->streamInUse) {
            Settings::instance().request_cancel();
            { std::lock_guard<std::mutex> lock(state_->apiMutex); if (state_->starting) LiInterruptConnection(); }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        worker_.join();
        state_->rumble.clear();
    }
private:
    std::shared_ptr<State> state_;
    fog::video::Settings settings_;
    stream::ControllerProfile profile_ = stream::ControllerProfile::Xbox;
    std::array<std::uint64_t, 2> lastMotionMs_{};
    bool resumeOnly_ = false;
    std::thread worker_;
    bool retryIdentityValid() const {
        return state_->valid && state_->paired && state_->application > 0;
    }
    bool retryableFailure() const {
        if (state_->fatalFailure) return false;
        return state_->terminated ? detail::retryableTransportError(state_->terminationCode) :
            state_->failed && state_->retryableFailure;
    }
    FogMoonlightDiagnostics coreSnapshot() const {
        FogMoonlightDiagnostics result{};
        if (state_->coreDiagnosticsReady) FogMoonlightGetDiagnostics(&result);
        return result;
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
    bool take(stream::Packet& packet, bool audio) {
        std::lock_guard<std::mutex> lock(state_->mutex); auto& queue = audio ? state_->audio : state_->video;
        if (queue.empty()) return false;
        if (!audio) {
            const auto now = nowMs();
            const auto age = now >= queue.front().enqueueMs ? now - queue.front().enqueueMs : 0;
            state_->videoQueueAgeMaxMs = std::max(state_->videoQueueAgeMaxMs.load(), age);
        }
        packet = std::move(queue.front()); queue.pop_front(); return true;
    }
    void run() noexcept {
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
                        Settings::instance().reset_cancel(); state_->starting = true;
                    }
                }
                int result = -1;
                if (state_->starting) result = LiStartConnection(&state_->host.serverInfo, &state_->config, &listeners, &video, &audio, nullptr, 0, nullptr, 0);
                { std::lock_guard<std::mutex> lock(state_->apiMutex); state_->starting = false; }
                if (result != 0) {
                    if (!state_->stop) state_->fail("Moonlight connection failed", result,
                        resumeOnly_ && result != -1 && detail::retryableTransportError(result));
                }
                else {
                    started = true; state_->coreDiagnosticsReady = true;
                    { std::lock_guard<std::mutex> lock(state_->apiMutex);
                        LiSendControllerArrivalEvent(0, 1, detail::controllerType(profile_),
                            detail::controllerButtons(profile_), detail::controllerCapabilities(profile_));
                        state_->running = true;
                    }
                    state_->status("Moonlight stream active: 720p30");
                    while (!state_->stop && !state_->terminated) {
                        const auto began = detail::steadyUs();
                        state_->trace.worker(began);
                        drainVideo(*state_);
                        state_->trace.drain.add(detail::steadyUs() - began);
                        std::this_thread::sleep_for(std::chrono::milliseconds(3));
                    }
                    if (state_->terminated) state_->fail("Moonlight host disconnected", state_->terminationCode,
                        detail::retryableTransportError(state_->terminationCode));
                }
            }
        } catch (...) { state_->fail("Moonlight worker failed"); }
        {
            std::lock_guard<std::mutex> lock(state_->apiMutex);
            state_->running = false; state_->starting = false;
            if (started) {
                LiSendMultiControllerEvent(0, 0, 0, 0, 0, 0, 0, 0, 0);
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_MOUSE_LEFT);
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_MOUSE_RIGHT);
                for (short key : {short(0x0d), short(0x08), short(0x09), short(0x1b)}) LiSendKeyboardEvent(key, KEY_ACTION_UP, 0);
                LiStopConnection();
                state_->transportClosedMs = nowMs();
            }
            active = nullptr;
            state_->rumble.clear();
        }
        state_->streamInUse = false;
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
