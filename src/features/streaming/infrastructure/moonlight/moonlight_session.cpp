#include "moonlight_session.hpp"
#include "app_selection.hpp"
#include "features/streaming/application/remote_input.hpp"
#include <Settings.hpp>
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
    std::atomic<int> code{0};
    std::atomic<std::uint64_t> frames{0}, audioFrames{0}, videoDrops{0}, audioDrops{0}, inputAccepted{0}, inputRejected{0};
    std::atomic<std::uint64_t> videoFullYields{0}, videoQueueHigh{0}, videoBatchMax{0}, videoQueueAgeMaxMs{0};
    std::atomic<std::uint64_t> videoInvalid{0}, videoWaitDrops{0}, videoResyncs{0};
    mutable std::mutex mutex, apiMutex;
    std::condition_variable changed;
    std::deque<stream::Packet> video, audio;
    std::string message = "Moonlight ready for pairing", error;
    std::uint64_t generation = 1;
    Clock::time_point pairingDeadline;
    State() { LiInitializeServerInformation(&host.serverInfo); LiInitializeStreamConfiguration(&config); }
    ~State() { delete[] host.serverInfo.rtspSessionUrl; releaseLease(); }
    bool claimLease() { bool expected = false; ownsLease = protocolClaimed.compare_exchange_strong(expected, true); return ownsLease; }
    void releaseLease() { if (ownsLease) { ownsLease = false; protocolClaimed = false; } }
    void status(const char* value) { std::lock_guard<std::mutex> lock(mutex); message = value; }
    void fail(const char* value, int failure = -1) {
        { std::lock_guard<std::mutex> lock(mutex); error = value; message = value; }
        code = failure; failed = true; changed.notify_all();
    }
};
// Core callbacks are process-global. An exclusive protocol lease prevents two
// contexts, and the worker clears this pointer only after LiStopConnection joins
// every callback thread (including the prepared termination callback shim).
std::atomic<State*> active{nullptr};
void stageStarting(int) {}
void stageFailed(int, int code) { if (auto* state = active.load()) state->code = code; }
void terminated(int code) {
    if (auto* state = active.load()) { state->code = code; state->terminated = true; }
}
void ignoreLog(const char*, ...) {}
int videoSetup(int format, int width, int height, int, void*, int) {
    return format == VIDEO_FORMAT_H264 && width > 0 && width <= 1920 && height > 0 && height <= 1080 ? 0 : -1;
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
        if (state->audio.size() >= 20) { state->audioDrops += state->audio.size(); state->audio.clear(); }
        state->audio.push_back(std::move(packet)); ++state->audioFrames;
    } catch (...) { state->failed = true; state->terminated = true; state->code = -900; }
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
            std::lock_guard<std::mutex> lock(state.mutex);
            // This worker is the sole producer. Leave complete frames in the
            // core's bounded queue until the consumer has room, preserving FIFO.
            if (state.video.size() >= State::VideoCapacity) { ++state.videoFullYields; break; }
        }
        if (!LiPollNextVideoFrame(&handle, &unit)) break;
        ++budget;
        int result = DR_NEED_IDR;
        try {
            if (unit && (!state.assemblyWaiting || unit->frameType == FRAME_TYPE_IDR)) {
                stream::Packet packet;
                if (copyFrame(*unit, packet.data)) {
                    packet.timestamp = unit->rtpTimestamp; packet.enqueueMs = nowMs();
                    std::lock_guard<std::mutex> lock(state.mutex);
                    state.assemblyWaiting = false;
                    packet.videoGeneration = state.generation;
                    state.video.push_back(std::move(packet)); ++state.frames; result = DR_OK;
                    state.videoQueueHigh = std::max(state.videoQueueHigh.load(), static_cast<std::uint64_t>(state.video.size()));
                } else ++state.videoInvalid;
            } else if (!unit) ++state.videoInvalid;
            else ++state.videoWaitDrops;
        } catch (...) { state.failed = true; state.terminated = true; state.code = -901; }
        if (result != DR_OK) {
            std::lock_guard<std::mutex> lock(state.mutex);
            ++state.videoDrops;
            if (!state.assemblyWaiting) {
                state.videoDrops += state.video.size(); state.video.clear();
                ++state.generation; ++state.videoResyncs;
            }
            state.assemblyWaiting = true;
        }
        LiCompleteVideoFrame(handle, result);
        if (state.failed) break;
    }
    state.videoBatchMax = std::max(state.videoBatchMax.load(), static_cast<std::uint64_t>(budget));
}
void freeApps(APP_LIST* app) {
    while (app) { auto* next = app->next; std::free(app->name); std::free(app); app = next; }
}
bool launch(State& state) {
    APP_LIST* raw = nullptr;
    int code = gs_applist(&state.host, &raw);
    std::unique_ptr<APP_LIST, decltype(&freeApps)> apps(raw, freeApps);
    if (code != GS_OK) { state.fail("Moonlight application list unavailable", code); return false; }
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
    // Resume only the deterministic app on this exact authenticated rental host.
    // Stopping the paid host belongs to its rental guard, never /cancel here.
    code = gs_start_app(&state.host, &state.config, *selected, false, false, 1);
    if (code != GS_OK) { state.fail("Moonlight application start failed", code); return false; }
    return true;
}
class Client final : public stream::StreamingClient {
public:
    explicit Client(std::shared_ptr<State> state) : state_(std::move(state)) {}
    ~Client() override { close(); }
    bool start(const stream::StreamConnection& connection) override {
        close();
        if (!state_ || !state_->valid || !state_->paired || connection.rental.empty() ||
            connection.rental != state_->rental || state_->streamInUse.exchange(true)) return false;
        state_->stop = false; state_->failed = false; state_->terminated = false;
        state_->frames = 0; state_->audioFrames = 0; state_->videoDrops = 0; state_->audioDrops = 0;
        state_->inputAccepted = 0; state_->inputRejected = 0;
        state_->videoFullYields = 0; state_->videoQueueHigh = 0; state_->videoBatchMax = 0; state_->videoQueueAgeMaxMs = 0;
        state_->videoInvalid = 0; state_->videoWaitDrops = 0; state_->videoResyncs = 0;
        state_->decoderWaiting = true; state_->assemblyWaiting = true;
        { std::lock_guard<std::mutex> lock(state_->mutex); state_->video.clear(); state_->audio.clear(); state_->error.clear(); ++state_->generation; }
        state_->config.width = 1280; state_->config.height = 720; state_->config.fps = 60;
        state_->config.bitrate = settings_.bitrateMbps * 1000;
        state_->config.packetSize = 1024; state_->config.streamingRemotely = STREAM_CFG_REMOTE;
        state_->config.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
        state_->config.supportedVideoFormats = VIDEO_FORMAT_H264;
        state_->config.clientRefreshRateX100 = 6000; state_->config.colorSpace = COLORSPACE_REC_709;
        state_->config.colorRange = COLOR_RANGE_LIMITED;
        state_->status("Connecting Moonlight 720p60 H.264");
        Settings::instance().reset_cancel(); Settings::instance().set_timeout_ms(25000);
        try { worker_ = std::thread(&Client::run, this); }
        catch (...) { state_->streamInUse = false; state_->fail("Cannot start Moonlight worker"); return false; }
        return true;
    }
    bool configureVideo(const fog::video::Settings& value) override {
        if (!fog::video::valid(value) || (state_ && state_->streamInUse)) return false;
        // Saved app preferences may use H.265/RTC quality. This backend advertises
        // its actual selected codec/status and only applies the requested bitrate.
        settings_ = value; return true;
    }
    bool configureRelay(bool enabled) override { return !enabled; }
    bool configurePlayStation(bool enabled) override { return !enabled; }
    bool updateVideoSettings(const fog::video::Settings& value) override {
        return value == settings_; // Live bitrate/quality/keyframe changes unsupported.
    }
    stream::VideoCodec selectedVideoCodec() const override { return stream::VideoCodec::H264; }
    std::string qualityStatus() const override { return "Moonlight 720p60 H.264 / " + std::to_string(settings_.bitrateMbps) + " Mbps; fixed at connect"; }
    void pump() override {}
    void observePlayback(const stream::PlaybackObservation&) override {}
    bool poll(stream::Packet& packet) override { return take(packet, false); }
    bool pollAudio(stream::Packet& packet) override { return take(packet, true); }
    std::uint64_t queuedVideoAgeMs() const override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->video.empty() ? 0 : nowMs() - state_->video.front().enqueueMs;
    }
    std::size_t queuedVideoPackets() const override { std::lock_guard<std::mutex> lock(state_->mutex); return state_->video.size(); }
    bool ready() const override { return state_ && state_->running; }
    std::string status() const override { std::lock_guard<std::mutex> lock(state_->mutex); return state_->message; }
    std::string error() const override { std::lock_guard<std::mutex> lock(state_->mutex); return state_->error; }
    bool retryableError() const override { return false; } // Rental inspection must precede another host attempt.
    bool disconnected() const override { return state_->terminated; }
    bool disconnectForRetry() override { close(); return false; }
    std::string diagnostics() const override {
        return "Moonlight code=" + std::to_string(state_->code.load()) +
            " video=" + std::to_string(state_->frames.load()) + " audio=" + std::to_string(state_->audioFrames.load()) +
            " assemblyWaiting=" + std::to_string(state_->assemblyWaiting.load()) +
            " decoderWaiting=" + std::to_string(state_->decoderWaiting.load()) +
            " inputAccepted=" + std::to_string(state_->inputAccepted.load()) +
            " inputRejected=" + std::to_string(state_->inputRejected.load());
    }
    std::string transportTimings() const override { return "Moonlight transport timing unavailable"; }
    std::string videoDiagnostics() const override { return "Moonlight H264 pull renderer"; }
    std::string gapDiagnostics() const override { return "Moonlight FEC managed by core"; }
    std::string queueDiagnostics() const override {
        return "Moonlight video_drop=" + std::to_string(state_->videoDrops.load()) + " audio_drop=" + std::to_string(state_->audioDrops.load()) +
            " video_capacity=" + std::to_string(State::VideoCapacity) + " video_high=" + std::to_string(state_->videoQueueHigh.load()) +
            " video_full_yields=" + std::to_string(state_->videoFullYields.load()) + " video_batch_max=" + std::to_string(state_->videoBatchMax.load()) +
            " video_age_max_ms=" + std::to_string(state_->videoQueueAgeMaxMs.load()) +
            " invalid_frames=" + std::to_string(state_->videoInvalid.load()) + " waiting_idr_drops=" + std::to_string(state_->videoWaitDrops.load()) +
            " invalid_resyncs=" + std::to_string(state_->videoResyncs.load());
    }
    std::string mutexDiagnostics() const override { return "Moonlight mutex telemetry unavailable"; }
    std::string dtlsDiagnostics() const override { return "Moonlight transport has no DTLS"; }
    std::string rtcpDiagnostics() const override { return "Moonlight feedback managed by core"; }
    std::string inputDiagnostics() const override { return "Moonlight input accepted=" + std::to_string(state_->inputAccepted.load()) + " rejected=" + std::to_string(state_->inputRejected.load()); }
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
        static constexpr int buttons[18] = { A_FLAG, B_FLAG, X_FLAG, Y_FLAG, LB_FLAG, RB_FLAG, 0, 0,
            BACK_FLAG, PLAY_FLAG, LS_CLK_FLAG, RS_CLK_FLAG, UP_FLAG, DOWN_FLAG, LEFT_FLAG, RIGHT_FLAG, SPECIAL_FLAG, 0 };
        int flags = 0;
        for (std::size_t i = 0; i < 18; ++i) if (gamepad.buttons[i]) flags |= buttons[i];
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
        { std::lock_guard<std::mutex> lock(state_->apiMutex); if (state_->starting) LiInterruptConnection(); }
        // Cancel pending /applist, /launch or /resume as well as LiStartConnection.
        while (state_->streamInUse) {
            Settings::instance().request_cancel();
            { std::lock_guard<std::mutex> lock(state_->apiMutex); if (state_->starting) LiInterruptConnection(); }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        worker_.join();
    }
private:
    std::shared_ptr<State> state_;
    fog::video::Settings settings_;
    std::thread worker_;
    bool accepted(int result) { if (result == 0) { ++state_->inputAccepted; return true; } return rejected(); }
    bool rejected() { ++state_->inputRejected; return false; }
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
    bool send(const fog::remote::Touch&) { return rejected(); }
    bool send(const fog::remote::Sensor&) { return rejected(); }
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
            // Refresh signed metadata on reconnect; gs_init preserves this rental's
            // certificate and pinned key. Release old RTSP allocation explicitly.
            delete[] state_->host.serverInfo.rtspSessionUrl; state_->host.serverInfo.rtspSessionUrl = nullptr;
            const int initialized = gs_init(&state_->host, state_->endpoint);
            if (initialized != GS_OK || !state_->host.paired) state_->fail("Moonlight authenticated host unavailable", initialized);
            else if (!state_->stop && launch(*state_) && !state_->stop) {
                DECODER_RENDERER_CALLBACKS video{}; LiInitializeVideoCallbacks(&video);
                video.setup = videoSetup; video.capabilities = CAPABILITY_PULL_RENDERER;
                AUDIO_RENDERER_CALLBACKS audio{}; LiInitializeAudioCallbacks(&audio);
                audio.init = audioInit; audio.decodeAndPlaySample = audioSample; audio.capabilities = CAPABILITY_SLOW_OPUS_DECODER;
                CONNECTION_LISTENER_CALLBACKS listeners{}; LiInitializeConnectionCallbacks(&listeners);
                listeners.stageStarting = stageStarting; listeners.stageFailed = stageFailed;
                listeners.connectionTerminated = terminated; listeners.logMessage = ignoreLog;
                active = state_.get();
                { std::lock_guard<std::mutex> lock(state_->apiMutex); if (!state_->stop) { Settings::instance().reset_cancel(); state_->starting = true; } }
                int result = -1;
                if (state_->starting) result = LiStartConnection(&state_->host.serverInfo, &state_->config, &listeners, &video, &audio, nullptr, 0, nullptr, 0);
                { std::lock_guard<std::mutex> lock(state_->apiMutex); state_->starting = false; }
                if (result != 0) { if (!state_->stop) state_->fail("Moonlight connection failed", result); }
                else {
                    started = true; state_->running = true;
                    { std::lock_guard<std::mutex> lock(state_->apiMutex); LiSendControllerArrivalEvent(0, 1, LI_CTYPE_XBOX, 0xffff, 0); }
                    state_->status("Moonlight stream active: 720p60 H.264");
                    while (!state_->stop && !state_->terminated) {
                        drainVideo(*state_); std::this_thread::sleep_for(std::chrono::milliseconds(3));
                    }
                    if (state_->terminated) state_->fail("Moonlight host disconnected", state_->code);
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
            }
            active = nullptr;
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
