#include "features/streaming/infrastructure/moonlight/moonlight_session.hpp"
#include "features/streaming/infrastructure/moonlight/resume_retry.hpp"
#include <Settings.hpp>
#include <Diagnostics.hpp>
#include <FogMoonlightDiagnostics.h>
#include <gamestream/client.h>
#include <gamestream/errors.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

namespace {
std::atomic<bool> providerPin{false}, failPair{false};
std::atomic<int> stops{0}, launches{0}, interrupts{0};
std::atomic<ConnListenerConnectionTerminated> terminateConnection{nullptr};
std::atomic<ConnListenerRumble> rumbleConnection{nullptr};
std::atomic<int> connectionResult{0};
std::atomic<bool> rumbleDuringStart{false}, rumbleDuringStop{false};
std::atomic<std::uint16_t> controllerCapabilities{0};
std::atomic<int> currentApp{0}, listedApp{42};
std::atomic<int> refreshes{0}, pairs{0}, launchOnlyCalls{0}, resumeOnlyCalls{0};
std::atomic<int> faultCount{0}, faultsReturned{0}, faultCurl{22}, faultHttp{503}, faultXml{0}, faultXmlResult{-1}, faultGame{-1};
std::atomic<bool> faultDispatched{true}, changeAppAfterFault{false}, dropPairAfterFault{false};
std::atomic<bool> unclassifiedSecondFault{false}, expireAfterAppList{false}, expireAfterCommand{false}, enforceResumeBudget{false};
bool ambiguousApps = false;
std::string initializedEndpoint;
int buttonFlags = 0, mouseButton = 0, keyCode = 0;
short leftY = 0, wheelAmount = 0;
unsigned char leftTrigger = 0;
int videoReturned = 0;
int expectedVideoFormats = VIDEO_FORMAT_H264 | VIDEO_FORMAT_H265;
int negotiatedFormat = VIDEO_FORMAT_H264;
int audioBurst = 1;
std::uint32_t entropy = 1;
template<class F> void eventually(F check) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!check() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    assert(check());
}
fog::PairedEndpoint endpoint() { return {"2001:db8::42", 0, 54321}; }
fog::EntityId rental() { return {fog::ProviderId::FogHub, "synthetic-owned-rental"}; }
void pair(moonlight::MoonlightSession& session) {
    providerPin = false;
    assert(session.begin(endpoint(), "1234")); assert(session.waitReady());
    assert(session.submissionReady());
    providerPin = true;
    assert(session.complete()); assert(!session.submissionReady());
    session.cancelAndJoin(); // Must not destroy successful authentication.
    assert(session.diagnostics().find("pair_ok=1 dispatched=1") != std::string::npos);
    assert(session.diagnostics().find("1234") == std::string::npos);
    assert(session.bindRental(rental()));
}
}

void randomGet(void* target, std::size_t size) {
    auto* bytes = static_cast<unsigned char*>(target);
    for (std::size_t i = 0; i < size; ++i) bytes[i] = static_cast<unsigned char>(entropy++);
}
uint64_t LiGetMicroseconds(void) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
void FogMoonlightGetDiagnostics(FogMoonlightDiagnostics* snapshot) {
    if (!snapshot) return;
    *snapshot = {};
    snapshot->videoReceiveCycleCount = 2; snapshot->videoReceiveCycleTotalUs = 200; snapshot->videoReceiveCycleMaxUs = 150;
    snapshot->videoReceiveWaitTotalUs = 160; snapshot->videoReceiveWaitMaxUs = 120;
    snapshot->videoReceiveWorkTotalUs = 40; snapshot->videoReceiveWorkMaxUs = 30;
    snapshot->videoReceiveLoopGapCount = 1; snapshot->videoReceiveLoopGapTotalUs = 8; snapshot->videoReceiveLoopGapMaxUs = 8;
    snapshot->videoReceiveAllocationCount = 1; snapshot->videoReceiveAllocationTotalUs = 3; snapshot->videoReceiveAllocationMaxUs = 3;
    snapshot->videoReceiveDecryptCount = 2; snapshot->videoReceiveDecryptTotalUs = 10; snapshot->videoReceiveDecryptMaxUs = 7;
    snapshot->videoReceiveTimingInvalidTimes = 4;
    snapshot->videoReceiveBufferRequestedBytes = 98303; snapshot->videoReceiveBufferSelectedBytes = 65535;
    snapshot->videoReceiveBufferActualBytes = 131070; snapshot->videoReceiveBufferSetFailures = 2;
    snapshot->videoReceiveBufferSetLastError = 12; snapshot->videoReceiveBufferQuerySucceeded = 1;
}
void LiInitializeServerInformation(PSERVER_INFORMATION value) { std::memset(value, 0, sizeof(*value)); }
void LiInitializeStreamConfiguration(PSTREAM_CONFIGURATION value) { std::memset(value, 0, sizeof(*value)); }
void LiInitializeVideoCallbacks(PDECODER_RENDERER_CALLBACKS value) { std::memset(value, 0, sizeof(*value)); }
void LiInitializeAudioCallbacks(PAUDIO_RENDERER_CALLBACKS value) { std::memset(value, 0, sizeof(*value)); }
void LiInitializeConnectionCallbacks(PCONNECTION_LISTENER_CALLBACKS value) { std::memset(value, 0, sizeof(*value)); }
int gs_init(PSERVER_DATA server, const std::string address) {
    ++refreshes;
    if (enforceResumeBudget) assert(Settings::instance().remaining_timeout_ms(25000) <= 6000);
    initializedEndpoint = address;
    server->paired = !Settings::instance().pinned_key().empty();
    server->currentGame = currentApp;
    server->httpsPort = 59999; // Actual discovery, independent of base port.
    return GS_OK;
}
int gs_pair(PSERVER_DATA server, char* pin) {
    ++pairs;
    assert(std::strcmp(pin, "1234") == 0);
    if (failPair) return GS_FAILED;
    Settings::instance().pairing_dispatched();
    while (!providerPin && !Settings::instance().request_cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!providerPin) return GS_FAILED;
    Settings::instance().set_pinned_key("synthetic-test-pinned-key");
    server->paired = true; return GS_OK;
}
int gs_applist(PSERVER_DATA, PAPP_LIST* list) {
    *list = static_cast<APP_LIST*>(std::calloc(1, sizeof(APP_LIST)));
    (*list)->name = ::strdup(ambiguousApps ? "First" : "Desktop"); (*list)->id = listedApp;
    if (ambiguousApps) {
        (*list)->next = static_cast<APP_LIST*>(std::calloc(1, sizeof(APP_LIST)));
        (*list)->next->name = ::strdup("Second"); (*list)->next->id = 43;
    }
    if (expireAfterAppList.exchange(false)) {
        Settings::instance().set_timeout_ms(1); std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    return GS_OK;
}
int gs_start_app(PSERVER_DATA server, PSTREAM_CONFIGURATION config, int app, bool, bool, int mask) {
    assert(app == 42 && mask == 1);
    assert(config->width == 1280 && config->height == 720 && config->fps == 30);
    assert(config->supportedVideoFormats == expectedVideoFormats && config->bitrate == 20000);
    ++launches;
    const bool resume = server->currentGame != 0;
    if (resume) ++resumeOnlyCalls; else ++launchOnlyCalls;
    moonlight_diagnostics::beginCommand(resume ? moonlight_diagnostics::CommandOperation::Resume : moonlight_diagnostics::CommandOperation::Launch);
    if (resume && faultCount > 0) {
        const int remaining = faultCount.fetch_sub(1);
        if (!(unclassifiedSecondFault && remaining == 1)) {
            moonlight_diagnostics::commandHttp(faultCurl,faultHttp,1,faultDispatched,false);
            moonlight_diagnostics::commandXmlStatus(faultXml);
            moonlight_diagnostics::commandXmlResult(faultXmlResult);
            moonlight_diagnostics::commandGameSession(faultGame);
        }
        moonlight_diagnostics::finishCommand(GS_FAILED,false);
        if (changeAppAfterFault) { currentApp = 43; listedApp = 43; }
        if (dropPairAfterFault) Settings::instance().set_pinned_key("");
        ++faultsReturned;
        return GS_FAILED;
    }
    moonlight_diagnostics::commandHttp(0,200,1,true,false);
    moonlight_diagnostics::commandXmlStatus(200);
    moonlight_diagnostics::commandXmlResult(GS_OK);
    moonlight_diagnostics::commandGameSession(1);
    if (expireAfterCommand.exchange(false)) {
        Settings::instance().set_timeout_ms(1); std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    moonlight_diagnostics::finishCommand(GS_OK,Settings::instance().request_cancelled());
    return GS_OK;
}
int LiStartConnection(PSERVER_INFORMATION, PSTREAM_CONFIGURATION config, PCONNECTION_LISTENER_CALLBACKS listeners,
    PDECODER_RENDERER_CALLBACKS video, PAUDIO_RENDERER_CALLBACKS audio, void*, int, void*, int) {
    assert(config->fps == 30 && config->supportedVideoFormats == expectedVideoFormats);
    terminateConnection = listeners->connectionTerminated;
    assert(listeners->rumble);
    rumbleConnection = listeners->rumble;
    if (rumbleDuringStart) listeners->rumble(0, 65535, 32768);
    assert(video->setup(VIDEO_FORMAT_H265_MAIN10, 1280, 720, 30, nullptr, 0) != 0);
    assert(video->setup(VIDEO_FORMAT_AV1_MAIN8, 1280, 720, 30, nullptr, 0) != 0);
    assert(video->setup(negotiatedFormat, 0, 720, 30, nullptr, 0) != 0);
    assert(video->setup(negotiatedFormat, 1280, 0, 30, nullptr, 0) != 0);
    assert(video->setup(negotiatedFormat, 1921, 720, 30, nullptr, 0) != 0);
    assert(video->setup(negotiatedFormat, 1280, 1081, 30, nullptr, 0) != 0);
    assert(video->setup(negotiatedFormat, 1280, 720, 30, nullptr, 0) == 0);
    OPUS_MULTISTREAM_CONFIGURATION opus{}; opus.sampleRate = 48000; opus.channelCount = 2;
    opus.streams = 1; opus.coupledStreams = 1; opus.mapping[1] = 1; opus.samplesPerFrame = 240;
    assert(audio->init(AUDIO_CONFIGURATION_STEREO, &opus, nullptr, 0) == 0);
    for (int i = 1; i <= audioBurst; i++) {
        char bytes[2]{static_cast<char>(i), 2}; audio->decodeAndPlaySample(bytes, 2);
    }
    videoReturned = 0;
    return connectionResult;
}
void LiStopConnection() {
    if (rumbleDuringStop) rumbleConnection.load()(0, 65535, 65535);
    ++stops;
}
void LiInterruptConnection() { ++interrupts; }
bool LiPollNextVideoFrame(VIDEO_FRAME_HANDLE* handle, PDECODE_UNIT* unit) {
    if (videoReturned++) return false;
    static char bytes[4]{0, 0, 1, 0x65};
    static LENTRY entry{}; entry.data = bytes; entry.length = 4;
    static DECODE_UNIT frame{}; frame.fullLength = 4; frame.bufferList = &entry; frame.frameType = FRAME_TYPE_IDR;
    frame.rtpTimestamp = 90000; *unit = &frame; *handle = reinterpret_cast<VIDEO_FRAME_HANDLE>(1); return true;
}
void LiCompleteVideoFrame(VIDEO_FRAME_HANDLE, int result) { assert(result == DR_OK); }
void LiRequestIdrFrame() {}
int LiSendControllerArrivalEvent(uint8_t controller, uint16_t mask, uint8_t type, uint32_t buttons, uint16_t capabilities) {
    assert(controller == 0 && mask == 1 && type == LI_CTYPE_XBOX && buttons == 0xffff);
    assert(capabilities & LI_CCAP_RUMBLE);
    controllerCapabilities = capabilities; return 0;
}
int LiSendMultiControllerEvent(short, short, int buttons, unsigned char lt, unsigned char, short, short ly, short, short) {
    buttonFlags = buttons; leftTrigger = lt; leftY = ly; return 0;
}
int LiSendMousePositionEvent(short, short, short, short) { return 0; }
int LiSendMouseButtonEvent(char, int button) { mouseButton = button; return 0; }
int LiSendKeyboardEvent(short key, char, char) { keyCode = key; return 0; }
int LiSendHighResScrollEvent(short amount) { wheelAmount = amount; return 0; }
int LiSendUtf8TextEvent(const char*, unsigned int) { return 0; }

void rumbleScenarios(const std::filesystem::path& path) {
    moonlight::MoonlightSession session(path.string());
    currentApp = 0; listedApp = 42; ambiguousApps = false; failPair = false;
    expectedVideoFormats = VIDEO_FORMAT_H264 | VIDEO_FORMAT_H265;
    negotiatedFormat = VIDEO_FORMAT_H264; audioBurst = 1;
    pair(session);
    auto client = session.create(); assert(client);
    stream::Rumble value;
    assert(!client->pollRumble(value));
    assert(client->start({rental()})); eventually([&] { return client->ready(); });
    eventually([] { return controllerCapabilities.load() & LI_CCAP_RUMBLE; });
    const auto rumble = rumbleConnection.load(); assert(rumble);
    const auto before = LiGetMicroseconds() / 1000;
    std::thread feedback([&] { rumble(0, 123, 456); rumble(0, 65535, 32768); });
    feedback.join();
    // Other host slots neither erase slot 0 nor produce feedback for this pad.
    rumble(1, 1, 2);
    assert(client->pollRumble(value));
    assert(value.controller == 0 && value.lowFrequency == 65535 && value.highFrequency == 32768);
    assert(value.durationMs == 0 && value.receivedAtMs >= before && value.receivedAtMs <= LiGetMicroseconds() / 1000);
    assert(!client->pollRumble(value));
    rumble(65535, 65535, 65535); assert(!client->pollRumble(value));
    // A stop is a motor state and must replace an unconsumed vibration burst.
    rumble(0, 500, 600); rumble(0, 0, 0);
    assert(client->pollRumble(value) && value.lowFrequency == 0 && value.highFrequency == 0);
    assert(!client->pollRumble(value));
    rumble(0, 100, 200); rumbleDuringStop = true;
    client->close(); assert(!client->pollRumble(value));
    rumble(0, 300, 400); assert(!client->pollRumble(value)); // No active context after join.
    assert(client->start({rental()})); eventually([&] { return client->ready(); });
    assert(!client->pollRumble(value));
    // Terminal feedback is suppressed immediately, before teardown completes.
    rumble(0, 500, 600); terminateConnection.load()(-55);
    rumble(0, 700, 800); assert(!client->pollRumble(value));
    eventually([&] { return !client->ready(); }); client->close();
    assert(client->start({rental()})); eventually([&] { return client->ready(); });
    assert(!client->pollRumble(value)); client->close();
    // A core startup failure can follow an early feedback callback.
    connectionResult = -77; rumbleDuringStart = true;
    assert(client->start({rental()})); eventually([&] { return !client->error().empty(); });
    assert(!client->pollRumble(value)); client->close();
    connectionResult = 0; rumbleDuringStart = false;
    assert(client->start({rental()})); eventually([&] { return client->ready(); });
    assert(!client->pollRumble(value));
    rumble(0, 9, 10); assert(client->pollRumble(value) && value.lowFrequency == 9 && value.highFrequency == 10);
    client->close(); rumbleDuringStop = false;
}

void retryScenarios(const std::filesystem::path& path) {
    using Retry = moonlight::detail::ResumeRetry;
    assert(Retry::MaxAttempts == 3 && Retry::BudgetMs == 6000);
    assert(Retry::retryable(22,503,0,-1,true,false));
    assert(Retry::retryable(0,200,503,GS_ERROR,true,false));
    assert(!Retry::retryable(0,200,503,GS_INVALID,true,false));
    assert(!Retry::retryable(22,401,0,-1,true,false));
    assert(!Retry::retryable(22,502,0,-1,true,false));
    assert(!Retry::retryable(22,504,0,-1,true,false));
    assert(!Retry::retryable(60,0,0,-1,false,false));
    assert(!Retry::retryable(42,0,0,-1,false,true));
    assert(!Retry::retryable(28,0,0,-1,true,false));
    assert(!Retry::retryable(56,0,0,-1,true,false));
    assert(Retry::retryable(7,0,0,-1,false,false));
    assert(Retry::retryable(28,0,0,-1,false,false));
    for (const auto* invalid : {"503junk","503 ","50","","-503"}) {
        moonlight_diagnostics::beginCommand(moonlight_diagnostics::CommandOperation::Resume);
        moonlight_diagnostics::commandXmlStatusText(invalid);
        assert(moonlight_diagnostics::commandSnapshot().xmlStatus == 0);
    }
    moonlight_diagnostics::commandXmlStatusText("503");
    assert(moonlight_diagnostics::commandSnapshot().xmlStatus == 503);
    moonlight_diagnostics::finishCommand(GS_FAILED,false);
    moonlight::MoonlightSession session(path.string());
    currentApp = 0; listedApp = 42; ambiguousApps = false; failPair = false;
    expectedVideoFormats = VIDEO_FORMAT_H264 | VIDEO_FORMAT_H265;
    negotiatedFormat = VIDEO_FORMAT_H265; audioBurst = 1;
    pair(session);
    auto client = session.create(); assert(client);
    assert(client->start({rental()})); eventually([&] { return client->ready(); }); client->close();
    const auto pairCount = pairs.load(), launchCount = launchOnlyCalls.load();
    currentApp = 42; enforceResumeBudget = true;
    // An explicit unavailable refusal is retried only after fresh host/app checks.
    faultCount = 1; faultsReturned = 0;
    auto before = resumeOnlyCalls.load(); auto refreshBefore = refreshes.load();
    assert(client->start({rental(),true})); eventually([&] { return client->ready(); });
    assert(resumeOnlyCalls == before + 2 && refreshes == refreshBefore + 2);
    const auto recovered = client->diagnostics();
    assert(recovered.find("Moonlight code=0 command=resume attempt=2 curl=0 http=200") != std::string::npos);
    assert(recovered.find("commandFailures=1 firstFailureCurl=22 firstFailureHttp=503") != std::string::npos);
    const auto closedToResume = recovered.find("closedToResumeMs="); assert(closedToResume != std::string::npos);
    assert(std::stoull(recovered.substr(closedToResume + std::strlen("closedToResumeMs="))) >= 250);
    client->close();
    // Persistent explicit refusals stop after exactly three attempts.
    faultCount = 20; before = resumeOnlyCalls.load();
    assert(client->start({rental(),true})); eventually([&] { return !client->error().empty(); }); client->close();
    assert(resumeOnlyCalls == before + 3);
    assert(client->diagnostics().find("attempt=3") != std::string::npos);
    // Authentication, TLS, malformed XML and a rejected game session are terminal.
    for (int failure = 0; failure < 4; ++failure) {
        faultCount = 1; faultCurl = failure == 0 ? 22 : failure == 1 ? 60 : 0;
        faultHttp = failure == 0 ? 403 : failure == 1 ? 0 : 200;
        faultXml = failure == 2 ? 503 : failure == 3 ? 200 : 0;
        faultXmlResult = failure == 2 ? GS_INVALID : failure == 3 ? GS_OK : -1;
        faultGame = failure == 3 ? 0 : -1;
        before = resumeOnlyCalls.load();
        assert(client->start({rental(),true})); eventually([&] { return !client->error().empty(); }); client->close();
        assert(resumeOnlyCalls == before + 1);
    }
    faultCurl = 22; faultHttp = 503; faultXml = 0; faultXmlResult = -1; faultGame = -1;
    // A stale service-unavailable outcome cannot authorize an unclassified retry.
    faultCount = 2; unclassifiedSecondFault = true; before = resumeOnlyCalls.load();
    assert(client->start({rental(),true})); eventually([&] { return !client->error().empty(); }); client->close();
    assert(resumeOnlyCalls == before + 2); unclassifiedSecondFault = false;
    // Even a different deterministic Desktop app cannot replace the original app.
    faultCount = 1; changeAppAfterFault = true; before = resumeOnlyCalls.load();
    assert(client->start({rental(),true})); eventually([&] { return !client->error().empty(); }); client->close();
    assert(resumeOnlyCalls == before + 1); changeAppAfterFault = false; currentApp = 42; listedApp = 42;
    faultCount = 1; dropPairAfterFault = true; before = resumeOnlyCalls.load();
    assert(client->start({rental(),true})); eventually([&] { return !client->error().empty(); }); client->close();
    assert(resumeOnlyCalls == before + 1); dropPairAfterFault = false;
    Settings::instance().set_pinned_key("synthetic-test-pinned-key");
    // Closing during backoff preempts another metadata or resume request.
    faultCount = 20; faultsReturned = 0; before = resumeOnlyCalls.load();
    assert(client->start({rental(),true})); eventually([&] { return faultsReturned > 0; });
    const auto closing = std::chrono::steady_clock::now(); client->close();
    assert(std::chrono::steady_clock::now() - closing < std::chrono::milliseconds(500));
    assert(resumeOnlyCalls == before + 1);
    faultCount = 0;
    // Deadline expiration cannot be cleared before command dispatch or core handoff.
    expireAfterAppList = true; before = resumeOnlyCalls.load();
    assert(client->start({rental(),true})); eventually([&] { return !client->error().empty(); }); client->close();
    assert(resumeOnlyCalls == before);
    const auto stopsBefore = stops.load(); expireAfterCommand = true;
    assert(client->start({rental(),true})); eventually([&] { return !client->error().empty(); }); client->close();
    assert(stops == stopsBefore && !client->ready());
    assert(pairs == pairCount && launchOnlyCalls == launchCount);
    enforceResumeBudget = false;
}

int main() {
    const auto path = std::filesystem::temp_directory_path() / ("fogplay-moonlight-session-test-" + std::to_string(::getpid()));
    {
        moonlight::MoonlightSession session(path.string());
        assert(!session.create());
        assert(!session.begin({"host/secret", 0, 42}, "1234"));
        assert(!session.begin({"127.0.0.1", 0, 0}, "1234"));
        assert(!session.begin(endpoint(), "12x4"));
        const auto pin = session.generate(); assert(pin.size() == 4 && pin.find_first_not_of("0123456789") == std::string::npos);
        pair(session);
        assert(initializedEndpoint == "[2001:db8::42]:54321");
        const auto firstIdentity = Settings::instance().key_dir();
        auto client = session.create(); assert(client);
        assert(!client->configureRelay(true)); assert(!client->configurePlayStation(true));
        assert(client->configureRelay(false) && client->configurePlayStation(false));
        fog::video::Settings video; assert(client->configureVideo(video));
        assert(client->selectedVideoCodec() == stream::VideoCodec::H264);
        assert(!client->start({{fog::ProviderId::FogHub, "other-rental"}}));
        assert(client->start({rental()}));
        eventually([&] { return client->ready(); });
        // Default H.265 request allows the host to negotiate H.264 fallback.
        assert(client->selectedVideoCodec() == stream::VideoCodec::H264);
        assert(client->diagnostics().find("requestedCodec=H.265") != std::string::npos);
        assert(client->diagnostics().find("fps=30 requestedFps=30 requestedMbps=20") != std::string::npos);
        const auto timings = client->transportTimings();
        assert(timings.size() < 4096);
        for (const auto* field : {"rx_cycle(n,avg_us,max_us)=2,100,150", "rx_wait(n,avg_us,max_us)=2,80,120",
                "rx_work(n,avg_us,max_us)=2,20,30", "rx_loop_gap(n,avg_us,max_us)=1,8,8",
                "rx_alloc(n,avg_us,max_us)=1,3,3", "rx_decrypt(n,avg_us,max_us)=2,5,7", "rx_invalid_times=4",
                "rcvbuf(requested,selected,actual)=98303,65535,131070", "set_failures=2 last_set_error=12 query_ok=1 query_error=0"}) {
            assert(timings.find(field) != std::string::npos);
        }
        eventually([&] { return client->queuedVideoPackets() == 1; });
        const auto gaps = client->gapDiagnostics();
        assert(gaps.find("rtp_reject_seq=") != std::string::npos &&
            gaps.find("fec_abandoned_parity_missing_max=") != std::string::npos &&
            gaps.find("core_waiting_idr_drops=") != std::string::npos);
        assert(!session.begin(endpoint(), "1234")); // Cannot replace an active transport.
        stream::Packet packet; assert(client->poll(packet) && packet.timestamp == 90000 && packet.data.size() == 4);
        assert(client->pollAudio(packet) && packet.kind == stream::Kind::Audio && packet.data.size() == 2);
        stream::Gamepad pad; pad.buttons[0] = 1; pad.buttons[12] = 1; pad.axes[1] = 1; pad.leftTrigger = .5;
        assert(client->sendGamepad(pad)); assert(buttonFlags == (A_FLAG | UP_FLAG) && leftTrigger == 127 && leftY == -32767);
        assert(client->sendInput(fog::remote::MouseButton{true, 2}) && mouseButton == BUTTON_MOUSE_RIGHT);
        assert(client->sendInput(fog::remote::Key{true, "Enter"}) && keyCode == 0x0d);
        assert(client->sendInput(fog::remote::Wheel{-32}) && wheelAmount == 32);
        assert(!client->sendInput(fog::remote::Key{true, "Unknown"}));
        assert(!client->sendInput(fog::remote::Sensor{fog::remote::MotionSensor::Gyroscope, {}}));
        client->close(); assert(stops == 1); assert(!client->ready());
        assert(!client->sendGamepad(pad));
        // Same preference accepts actual negotiated H.265, and a saturated audio
        // FIFO discards just the oldest packet rather than emptying all audio.
        negotiatedFormat = VIDEO_FORMAT_H265; audioBurst = 25;
        assert(client->start({rental()})); eventually([&] { return client->ready(); });
        assert(client->selectedVideoCodec() == stream::VideoCodec::H265);
        for (int i = 6; i <= 25; i++) {
            assert(client->pollAudio(packet) && packet.data.size() == 2);
            assert(static_cast<int>(packet.data[0]) == i);
        }
        assert(!client->pollAudio(packet));
        client->close();
        // An explicit H.264 preference must not advertise HEVC support.
        video.codec = stream::VideoCodec::H264;
        expectedVideoFormats = VIDEO_FORMAT_H264; negotiatedFormat = VIDEO_FORMAT_H264; audioBurst = 1;
        assert(client->configureVideo(video));
        assert(client->start({rental()})); eventually([&] { return client->ready(); });
        assert(client->selectedVideoCodec() == stream::VideoCodec::H264);
        client->close(); assert(stops == 3 && launches == 3);
        // Explicit quality reconnect can only resume the selected app still active
        // on this exact authenticated rental; request mode and codec survive it.
        video.codec = stream::VideoCodec::H265;
        expectedVideoFormats = VIDEO_FORMAT_H264 | VIDEO_FORMAT_H265;
        negotiatedFormat = VIDEO_FORMAT_H265; currentApp = 42;
        assert(client->configureVideo(video));
        assert(client->start({rental(), true})); eventually([&] { return client->ready(); });
        assert(client->selectedVideoCodec() == stream::VideoCodec::H265);
        assert(client->disconnectForRetry());
        assert(!client->ready() && stops == 4 && launches == 4);
        assert(!client->disconnectForRetry()); // An already closed client isn't healthy.
        currentApp = 0;
        assert(client->start({rental(), true})); eventually([&] { return !client->error().empty(); });
        assert(client->error().find("previous application is no longer active") != std::string::npos);
        assert(!client->disconnectForRetry() && stops == 4 && launches == 4);
        currentApp = 999;
        assert(client->start({rental(), true})); eventually([&] { return !client->error().empty(); });
        assert(client->error().find("another running application") != std::string::npos);
        assert(!client->disconnectForRetry() && stops == 4 && launches == 4);
        // A terminal callback invalidates retry permission even if it races close.
        currentApp = 42;
        assert(client->start({rental(), true})); eventually([&] { return client->ready(); });
        const auto terminate = terminateConnection.load(); assert(terminate); terminate(-55);
        eventually([&] { return client->disconnected(); });
        assert(!client->disconnectForRetry() && stops == 5 && launches == 5);
        pair(session); assert(Settings::instance().key_dir() != firstIdentity);
        assert(!client->start({rental()})); // Old authenticated generation invalidated.
        auto current = session.create(); currentApp = 999;
        assert(current->start({rental()})); eventually([&] { return !current->error().empty(); }); current->close();
        assert(launches == 5 && stops == 5); // Another app was never resumed/cancelled.
        currentApp = 0; ambiguousApps = true;
        assert(current->start({rental()})); eventually([&] { return !current->error().empty(); }); current->close();
        assert(launches == 5); ambiguousApps = false;
        providerPin = false; assert(session.begin(endpoint(), "1234")); assert(session.waitReady());
        session.cancelAndJoin(); assert(!session.complete() && !session.bindRental(rental()));
        failPair = true; assert(session.begin(endpoint(), "1234")); assert(!session.waitReady()); session.cancelAndJoin();
        const auto failure = session.diagnostics();
        assert(failure.find("pair_ok=0 dispatched=0") != std::string::npos);
        assert(failure.find("Moonlight automatic pairing failed") != std::string::npos);
        assert(failure.find("2001:db8") == std::string::npos && failure.find("1234") == std::string::npos);
    }
    retryScenarios(path / "resume-tests");
    rumbleScenarios(path / "rumble-tests");
    std::filesystem::remove_all(path);
    std::cout << "Moonlight production lifecycle/input tests passed\n";
}
