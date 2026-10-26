#include "features/streaming/infrastructure/moonlight/moonlight_session.hpp"
#include <Settings.hpp>
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
int currentApp = 0;
bool ambiguousApps = false;
std::string initializedEndpoint;
int buttonFlags = 0, mouseButton = 0, keyCode = 0;
short leftY = 0, wheelAmount = 0;
unsigned char leftTrigger = 0;
int videoReturned = 0;
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
void LiInitializeServerInformation(PSERVER_INFORMATION value) { std::memset(value, 0, sizeof(*value)); }
void LiInitializeStreamConfiguration(PSTREAM_CONFIGURATION value) { std::memset(value, 0, sizeof(*value)); }
void LiInitializeVideoCallbacks(PDECODER_RENDERER_CALLBACKS value) { std::memset(value, 0, sizeof(*value)); }
void LiInitializeAudioCallbacks(PAUDIO_RENDERER_CALLBACKS value) { std::memset(value, 0, sizeof(*value)); }
void LiInitializeConnectionCallbacks(PCONNECTION_LISTENER_CALLBACKS value) { std::memset(value, 0, sizeof(*value)); }
int gs_init(PSERVER_DATA server, const std::string address) {
    initializedEndpoint = address;
    server->paired = !Settings::instance().pinned_key().empty();
    server->currentGame = currentApp;
    server->httpsPort = 59999; // Actual discovery, independent of base port.
    return GS_OK;
}
int gs_pair(PSERVER_DATA server, char* pin) {
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
    (*list)->name = ::strdup(ambiguousApps ? "First" : "Desktop"); (*list)->id = 42;
    if (ambiguousApps) {
        (*list)->next = static_cast<APP_LIST*>(std::calloc(1, sizeof(APP_LIST)));
        (*list)->next->name = ::strdup("Second"); (*list)->next->id = 43;
    }
    return GS_OK;
}
int gs_start_app(PSERVER_DATA, PSTREAM_CONFIGURATION config, int app, bool, bool, int mask) {
    assert(app == 42 && mask == 1);
    assert(config->width == 1280 && config->height == 720 && config->fps == 60 && config->supportedVideoFormats == VIDEO_FORMAT_H264);
    ++launches; return GS_OK;
}
int LiStartConnection(PSERVER_INFORMATION, PSTREAM_CONFIGURATION, PCONNECTION_LISTENER_CALLBACKS,
    PDECODER_RENDERER_CALLBACKS video, PAUDIO_RENDERER_CALLBACKS audio, void*, int, void*, int) {
    assert(video->setup(VIDEO_FORMAT_H264, 1280, 720, 60, nullptr, 0) == 0);
    OPUS_MULTISTREAM_CONFIGURATION opus{}; opus.sampleRate = 48000; opus.channelCount = 2;
    opus.streams = 1; opus.coupledStreams = 1; opus.mapping[1] = 1; opus.samplesPerFrame = 240;
    assert(audio->init(AUDIO_CONFIGURATION_STEREO, &opus, nullptr, 0) == 0);
    char bytes[2]{1, 2}; audio->decodeAndPlaySample(bytes, 2); videoReturned = 0;
    return 0;
}
void LiStopConnection() { ++stops; }
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
int LiSendControllerArrivalEvent(uint8_t, uint16_t, uint8_t, uint32_t, uint16_t) { return 0; }
int LiSendMultiControllerEvent(short, short, int buttons, unsigned char lt, unsigned char, short, short ly, short, short) {
    buttonFlags = buttons; leftTrigger = lt; leftY = ly; return 0;
}
int LiSendMousePositionEvent(short, short, short, short) { return 0; }
int LiSendMouseButtonEvent(char, int button) { mouseButton = button; return 0; }
int LiSendKeyboardEvent(short key, char, char) { keyCode = key; return 0; }
int LiSendHighResScrollEvent(short amount) { wheelAmount = amount; return 0; }
int LiSendUtf8TextEvent(const char*, unsigned int) { return 0; }

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
        eventually([&] { return client->queuedVideoPackets() == 1; });
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
        pair(session); assert(Settings::instance().key_dir() != firstIdentity);
        assert(!client->start({rental()})); // Old authenticated generation invalidated.
        auto current = session.create(); currentApp = 999;
        assert(current->start({rental()})); eventually([&] { return !current->error().empty(); }); current->close();
        assert(launches == 1 && stops == 1); // Another app was never resumed/cancelled.
        currentApp = 0; ambiguousApps = true;
        assert(current->start({rental()})); eventually([&] { return !current->error().empty(); }); current->close();
        assert(launches == 1); ambiguousApps = false;
        providerPin = false; assert(session.begin(endpoint(), "1234")); assert(session.waitReady());
        session.cancelAndJoin(); assert(!session.complete() && !session.bindRental(rental()));
        failPair = true; assert(session.begin(endpoint(), "1234")); assert(!session.waitReady()); session.cancelAndJoin();
        const auto failure = session.diagnostics();
        assert(failure.find("pair_ok=0 dispatched=0") != std::string::npos);
        assert(failure.find("Moonlight automatic pairing failed") != std::string::npos);
        assert(failure.find("2001:db8") == std::string::npos && failure.find("1234") == std::string::npos);
    }
    std::filesystem::remove_all(path);
    std::cout << "Moonlight production lifecycle/input tests passed\n";
}
