// Local-only STOMP / ICE / DTLS / SRTP / SCTP fixture. Never calls FogPlay.
#include "features/streaming/infrastructure/stream.hpp"
#include "features/streaming/infrastructure/stream_protocol.hpp"
#include "features/streaming/application/remote_input.hpp"
#include <rtc/rtc.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>

using Json = stream::protocol::Json;
using Clock = std::chrono::steady_clock;
namespace {
struct Event {
    enum Type { Socket, Stomp, Offer, Candidate, Control } type;
    std::shared_ptr<rtc::WebSocket> socket;
    std::string text, mid;
    explicit Event(Type t) : type(t) {}
};
struct Events {
    std::mutex mutex;
    std::deque<Event> queue;
    void push(Event e) { std::lock_guard<std::mutex> lock(mutex); queue.push_back(std::move(e)); }
    std::deque<Event> take() { std::lock_guard<std::mutex> lock(mutex); std::deque<Event> result; result.swap(queue); return result; }
};
class Feedback final : public rtc::MediaHandler {
public:
    std::atomic<int> plis{0};
    std::atomic<unsigned> remb{0};
    void incoming(rtc::message_vector& messages, const rtc::message_callback&) override {
        for (const auto& msg : messages) {
            if (msg->size() >= 12 && std::to_integer<unsigned>(msg->at(1)) == 206 &&
                (std::to_integer<unsigned>(msg->at(0)) & 31) == 1) ++plis;
            if (msg->size() >= 24 && std::to_integer<unsigned>(msg->at(1)) == 206 &&
                (std::to_integer<unsigned>(msg->at(0)) & 31) == 15 &&
                msg->at(12) == std::byte{'R'} && msg->at(13) == std::byte{'E'} &&
                msg->at(14) == std::byte{'M'} && msg->at(15) == std::byte{'B'}) {
                const auto exp = std::to_integer<unsigned>(msg->at(17)) >> 2;
                const auto mantissa = ((std::to_integer<unsigned>(msg->at(17)) & 3) << 16) |
                    (std::to_integer<unsigned>(msg->at(18)) << 8) | std::to_integer<unsigned>(msg->at(19));
                assert(exp < 16); remb = mantissa << exp;
            }
        }
    }
};
struct Fixture {
    std::shared_ptr<Events> events = std::make_shared<Events>();
    std::shared_ptr<rtc::WebSocket> ws;
    std::shared_ptr<rtc::PeerConnection> peer;
    std::shared_ptr<rtc::DataChannel> dc;
    std::shared_ptr<rtc::Track> video, audio;
    std::shared_ptr<Feedback> feedback = std::make_shared<Feedback>();
    std::unique_ptr<rtc::WebSocketServer> server;
    stream::protocol::Parser parser;
    bool subscribed = false, enabled = false, answered = false, controlReady = false;
    bool pong = false, pressed = false, neutral = false, closed = false;
    Json remoteControls = Json::array();
    std::string qualityCommand;
    std::vector<std::string> adaptations;
    std::string qualityProfile = "LOW";
    bool confirmAdaptations = true;
    std::vector<rtc::Candidate> candidates;
    std::vector<std::string> mids;
    std::string pendingOffer;
    std::uint32_t frame = 0;
    std::string heartbeat;
    bool initializePeer;
    bool hevc;
    explicit Fixture(std::string serverHeartbeat = "4000,4000", bool initialize = true, bool useHevc = false)
        : heartbeat(std::move(serverHeartbeat)), initializePeer(initialize), hevc(useHevc) {
        rtc::WebSocketServer::Configuration config;
        config.port = 0; config.bindAddress = "127.0.0.1";
        server = std::make_unique<rtc::WebSocketServer>(config);
        const std::weak_ptr<Events> weak(events);
        server->onClient([weak](std::shared_ptr<rtc::WebSocket> socket) {
            socket->onMessage([weak](rtc::message_variant message) {
                if (auto events = weak.lock()) {
                    Event e(Event::Stomp); e.text = std::get<std::string>(message); events->push(std::move(e));
                }
            });
            if (auto events = weak.lock()) {
                Event e(Event::Socket); e.socket = socket; events->push(std::move(e));
            }
        });
    }
    ~Fixture() {
        if (dc) { dc->resetCallbacks(); dc->close(); }
        if (video) video->close();
        if (audio) audio->close();
        if (peer) { peer->resetCallbacks(); peer->close(); }
        if (ws) { ws->resetCallbacks(); ws->close(); }
        server->stop();
    }
    void signal(const Json& json) {
        ws->send(stream::protocol::frame("MESSAGE", {{"destination", "/topic/fixtureabcde/client"}}, json.dump()));
    }
    void createPeer() {
        rtc::Configuration config;
        config.disableAutoNegotiation = true;
        // Host candidates only: no STUN/TURN/service requests.
        peer = std::make_shared<rtc::PeerConnection>(config);
        const std::weak_ptr<Events> weak(events);
        peer->onLocalDescription([weak](rtc::Description desc) {
            if (auto events = weak.lock()) {
                Event e(Event::Offer);
                e.text = Json{{"type", desc.typeString()}, {"sdp", std::string(desc)}}.dump(); events->push(std::move(e));
            }
        });
        peer->onLocalCandidate([weak](rtc::Candidate c) {
            if (auto events = weak.lock()) {
                Event e(Event::Candidate); e.text = c.candidate(); e.mid = c.mid(); events->push(std::move(e));
            }
        });
        rtc::Description::Video v("cam", rtc::Description::Direction::SendOnly);
        if (hevc) v.addH265Codec(98); else v.addH264Codec(98);
        v.addSSRC(1234, "fixture", "fixture", "cam");
        // Advertise unsupported payloads too; client's answer must exclude them.
        v.addVP8Codec(120);
        video = peer->addTrack(v);
        auto vconfig = std::make_shared<rtc::RtpPacketizationConfig>(1234, "fixture", 98, 90000);
        std::shared_ptr<rtc::RtpPacketizer> vp;
        if (hevc) vp = std::make_shared<rtc::H265RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, vconfig);
        else vp = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, vconfig);
        vp->addToChain(std::make_shared<rtc::RtcpSrReporter>(vconfig));
        vp->addToChain(feedback);
        video->setMediaHandler(vp);
        rtc::Description::Audio a("sound", rtc::Description::Direction::SendOnly);
        a.addOpusCodec(103); a.addPCMUCodec(0); a.addSSRC(5678, "fixture", "fixture", "sound");
        audio = peer->addTrack(a);
        auto aconfig = std::make_shared<rtc::RtpPacketizationConfig>(5678, "fixture", 103, 48000);
        auto ap = std::make_shared<rtc::OpusRtpPacketizer>(aconfig);
        ap->addToChain(std::make_shared<rtc::RtcpSrReporter>(aconfig));
        audio->setMediaHandler(ap);
        dc = peer->createDataChannel("input");
        dc->onMessage([weak](rtc::message_variant message) {
            if (auto events = weak.lock()) {
                Event e(Event::Control); e.text = std::get<std::string>(message); events->push(std::move(e));
            }
        });
        peer->setLocalDescription(rtc::Description::Type::Offer);
    }
    void message(const Json& j) {
        const auto id = j.at("id").get<std::string>();
        if (id == "enabled") {
            assert(subscribed && j.at("client_hash") == "fixtureabcde"); enabled = true;
            if (initializePeer) signal({{"id", "init"}, {"data", Json::array({"[]"}).dump()}});
        } else if (id == "init_webrtc") {
            assert(enabled && j.at("limit_fps") == 30 && j.at("gamepad") == true && j.at("control_only") == false);
            // Provider-style empty marker arrives before an offer. The old client
            // aborted here with JSON parse_error instead of continuing negotiation.
            signal({{"id", "offer"}, {"data", ""}});
            signal({{"id", "candidate"}, {"data", ""}});
            signal({{"id", "ice_candidate"}, {"data", stream::protocol::base64Encode(Json{{"candidate", ""}}.dump())}});
            createPeer();
        } else if (id == "answer") {
            const auto data = Json::parse(stream::protocol::base64Decode(j.at("data")));
            assert(data.at("type") == "answer");
            rtc::Description desc(data.at("sdp"), "answer");
            const auto v = std::get<rtc::Description::Media*>(desc.media(0));
            const auto a = std::get<rtc::Description::Media*>(desc.media(1));
            assert(v->payloadTypes() == std::vector<int>{98} && v->direction() == rtc::Description::Direction::RecvOnly);
            assert(a->payloadTypes() == std::vector<int>{103} && a->direction() == rtc::Description::Direction::RecvOnly);
            peer->setRemoteDescription(desc); answered = true;
            signal({{"id", "ice_candidate"}, {"data", ""}});
            for (const auto& c : candidates) peer->addRemoteCandidate(c);
            candidates.clear();
        } else if (id == "ice_candidate") {
            const auto data = Json::parse(stream::protocol::base64Decode(j.at("data")));
            assert(data.contains("sdpMLineIndex") && data.contains("sdpMid"));
            rtc::Candidate c(data.at("candidate"), data.at("sdpMid"));
            if (answered) peer->addRemoteCandidate(c); else candidates.push_back(c);
        } else if (id == "close") closed = true;
    }
    void pump() {
        for (auto& e : events->take()) {
            if (e.type == Event::Socket) ws = e.socket;
            else if (e.type == Event::Stomp) {
                for (const auto& f : parser.feed(e.text)) {
                    if (f.command == "CONNECT") {
                        assert(f.headers.at("accept-version") == "1.1,1.2" && f.headers.at("heart-beat") == "4000,4000");
                        std::map<std::string, std::string> headers{{"version", "1.2"}};
                        if (!heartbeat.empty()) headers.emplace("heart-beat", heartbeat);
                        ws->send(stream::protocol::frame("CONNECTED", headers));
                    } else if (f.command == "SUBSCRIBE") {
                        assert(f.headers.at("destination") == "/topic/fixtureabcde/client" && f.headers.at("ack") == "auto");
                        subscribed = true;
                    } else if (f.command == "SEND") {
                        assert(f.headers.at("destination") == "/topic/fixture/server" && f.headers.at("client-hash") == "fixtureabcde");
                        message(Json::parse(f.body));
                    }
                }
            } else if (e.type == Event::Offer) {
                mids = stream::protocol::mediaMids(Json::parse(e.text).at("sdp"));
                pendingOffer = e.text;
            } else if (e.type == Event::Candidate) {
                const auto it = std::find(mids.begin(), mids.end(), e.mid);
                assert(it != mids.end());
                // Deliberately omit sdpMid; exercise index resolution on the client.
                const Json c{{"candidate", e.text}, {"sdpMLineIndex", std::distance(mids.begin(), it)}};
                signal({{"id", "candidate"}, {"data", stream::protocol::base64Encode(c.dump())}});
                if (!pendingOffer.empty()) {
                    // Exercise the client's early-candidate queue before its remote description.
                    signal({{"id", "offer"}, {"data", stream::protocol::base64Encode(pendingOffer)}});
                    pendingOffer.clear();
                }
            } else if (e.type == Event::Control) {
                const auto j = Json::parse(e.text);
                const auto id = j.at("id").get<std::string>();
                if (id == "connection_ready") { controlReady = true; dc->send(Json{{"id", "ping"}, {"ts", 42}}.dump()); }
                else if (id == "pong") { assert(j.at("ts") == 42); pong = true; }
                else if (id.rfind("set_",0) == 0) {
                    qualityCommand = id;
                    dc->send(Json{{"id","stats"},{"target_bitrate",18000000},{"preset",qualityProfile},
                                  {"fps",30},{"autoquality",true},{"server_codec",hevc ? "H265" : "H264"}}.dump());
                }
                else if (id == "enhancequality" || id == "degradequality") {
                    assert(j.size() == 1); // Recovered protocol is id-only; no invented bitrate payload.
                    adaptations.push_back(id);
                    if (confirmAdaptations) {
                        qualityProfile = id == "enhancequality" ? "MID" : "LOW";
                        dc->send(Json{{"id","stats"},{"preset",qualityProfile},{"fps",30}}.dump());
                    }
                }
                else if (id == "gamepad") {
                    assert(j.at("state").at("buttons").size() == 18 && j.at("state").at("axes").size() == 4);
                    if (j.at("state").at("buttons")[0] == 1) pressed = true;
                    else if (pressed) neutral = true;
                } else if (id == "mousemove" || id == "mousedown" || id == "mouseup" ||
                           id == "mousewheel" || id == "keydown" || id == "keyup" || id == "unicode_input") {
                    remoteControls.push_back(j);
                }
            }
        }
    }
    void sendMedia() {
        if (!video || !video->isOpen() || !audio->isOpen()) return;
        const rtc::binary h264{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1},
                              std::byte{0x65}, std::byte{0x88}, std::byte{0x84}, std::byte{0x21}};
        const rtc::binary opus{std::byte{0xf8}, std::byte{0xff}, std::byte{0xfe}};
        if (hevc) {
            // Synthetic VPS/SPS/PPS/IDR for transport, not a decoder fixture.
            rtc::binary h265{std::byte{0},std::byte{0},std::byte{0},std::byte{1},std::byte{64},std::byte{1},std::byte{8},
                            std::byte{0},std::byte{0},std::byte{0},std::byte{1},std::byte{66},std::byte{1}};
            std::vector<unsigned> rbsp{1}; rbsp.resize(13,0); rbsp.push_back(0xc0);
            unsigned zeros=0;
            for (auto value:rbsp) {
                if (zeros==2 && value<=3) { h265.push_back(std::byte{3}); zeros=0; }
                h265.push_back(std::byte(value)); zeros=value==0 ? zeros+1 : 0;
            }
            for (unsigned value:{0u,0u,0u,1u,68u,1u,192u,0u,0u,0u,1u,38u,1u,160u}) h265.push_back(std::byte(value));
            video->sendFrame(h265, rtc::FrameInfo(++frame * 3000));
        } else video->sendFrame(h264, rtc::FrameInfo(++frame * 3000));
        audio->sendFrame(opus, rtc::FrameInfo(frame * 960));
    }
};
}

int main() {
    const std::vector<Json> remoteInputs{
        fog::remote::absoluteMouse(640, 360, 1280, 720, 1920, 1080),
        fog::remote::mouseButton(true, 0), fog::remote::mouseButton(false, 0),
        fog::remote::mouseButton(true, 2), fog::remote::mouseButton(false, 2),
        fog::remote::wheel(-32), fog::remote::key(true, "Enter"), fog::remote::key(false, "Enter"),
        fog::remote::unicodeText(u8"\u041f\u0440\u0438\u0432\u0435\u0442"),
        fog::remote::unicodeText(u8"\U0001f600")
    };
    // Independent wire expectations: check payload contents and ordered delivery.
    const Json expectedRemoteInputs = Json::parse(R"([
        {"id":"mousemove","data":{"width":1280,"height":720,"frameWidth":1920,"frameHeight":1080,
          "x":640,"y":360,"controls_type":"default"}},
        {"id":"mousedown","data":{"button":0}}, {"id":"mouseup","data":{"button":0}},
        {"id":"mousedown","data":{"button":2}}, {"id":"mouseup","data":{"button":2}},
        {"id":"mousewheel","data":{"dy":-32}},
        {"id":"keydown","data":{"code":"Enter","ctrlKey":false,"shiftKey":false,"altKey":false}},
        {"id":"keyup","data":{"code":"Enter","ctrlKey":false,"shiftKey":false,"altKey":false}},
        {"id":"unicode_input","data":{"input":"HwRABDgEMgQ1BEIE"}},
        {"id":"unicode_input","data":{"input":"PdgA3g=="}}
    ])");
    for (int round = 1; round <= 3; ++round) {
    {
        Fixture fixture("4000,4000",true,round==3);
        stream::Client client;
        fog::video::Settings settings;
        settings.codec = stream::VideoCodec::H265; // Fixture only offers H264: test safe fallback.
        settings.bitrateMbps = 15;
        assert(client.configureVideo(settings));
        auto invalid = settings; invalid.bitrateMbps = 0;
        assert(!client.configureVideo(invalid));
        assert(!client.updateVideoSettings(settings));
        assert(!client.startForTesting("fixture", "abcde", "wss://remoteplay.mts.ru/ws/"));
        assert(client.startForTesting("fixture", "abcde", "ws://127.0.0.1:" + std::to_string(fixture.server->port()) + "/ws/"));
        const auto begin = Clock::now();
        int videos = 0, audios = 0;
        bool sent = false;
        while (Clock::now() - begin < std::chrono::seconds(12)) {
            fixture.pump(); client.pump();
            assert(client.error().empty());
            fixture.sendMedia();
            stream::Packet packet;
            while (client.poll(packet)) {
                if (packet.kind == stream::Kind::Video) {
                    if (fixture.hevc) {
                        assert(packet.data.size() > 20 && packet.data[3] == std::byte{1} && packet.data[4] == std::byte{64});
                        assert(packet.lastSequence - packet.firstSequence == 3);
                    } else {
                        assert(packet.data.size() == 8 && packet.data[3] == std::byte{1} && packet.data[4] == std::byte{0x65});
                        assert(packet.firstSequence == packet.lastSequence);
                    }
                    assert(!client.waitingForVideoKeyframe()); ++videos;
                } else { assert(packet.data.size() == 3 && packet.data[0] == std::byte{0xf8}); ++audios; }
                assert(packet.timestamp > 0);
            }
            if (client.ready() && !sent) {
                stream::Gamepad pad; pad.buttons[0] = 1; pad.axes[0] = 0.5f;
                assert(client.sendGamepad(pad));
                for (const auto& input : remoteInputs) assert(client.sendControl(input.dump()));
                client.elapseForTesting(1);
                assert(client.requestVideoKeyframe());
                sent = true;
            }
            if (videos >= 3 && audios >= 3 && fixture.pong && fixture.pressed && fixture.feedback->plis > 0 &&
                fixture.remoteControls == expectedRemoteInputs && fixture.feedback->remb == 15000000 &&
                fixture.qualityCommand == "set_autoquality" && client.qualityStatus().find("18.0") != std::string::npos) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        assert(client.ready() && fixture.answered && fixture.controlReady && fixture.pong && fixture.pressed);
        assert(videos >= 3 && audios >= 3 && fixture.feedback->plis > 0);
        assert(fixture.remoteControls == expectedRemoteInputs);
        assert(client.diagnostics().find("emptyCandidates=3") != std::string::npos);
        assert(client.videoDiagnostics().find("waitIdr=0") != std::string::npos);
        assert(client.selectedVideoCodec() == (fixture.hevc ? stream::VideoCodec::H265 : stream::VideoCodec::H264));
        assert(fixture.feedback->remb == 15000000 && fixture.qualityCommand == "set_autoquality");
        assert(client.qualityStatus().find("18.0") != std::string::npos);
        assert(client.videoDiagnostics().find("serverPreset=LOW") != std::string::npos);
        assert(client.videoDiagnostics().find("statsCount=1") != std::string::npos);
        if (round == 1) {
            // Lost/throttled decoder PLI must retry even with periodic PLI disabled,
            // while transport assembly itself has already recovered.
            assert(!client.waitingForVideoKeyframe() && settings.keyframeSeconds == 0);
            const auto beforePli = client.videoDiagnostics();
            const auto pliCount = [](const std::string& d) { return std::stoull(d.substr(d.find(" pli=") + 5)); };
            client.setDecoderWaitingForKeyframe(true);
            client.requestVideoKeyframe(); // May be suppressed by the existing 250 ms limiter.
            client.elapseForTesting(1); client.pump();
            const auto firstPli = pliCount(client.videoDiagnostics());
            assert(firstPli > pliCount(beforePli));
            client.elapseForTesting(1); client.pump();
            const auto secondPli = pliCount(client.videoDiagnostics());
            assert(secondPli > firstPli);
            client.setDecoderWaitingForKeyframe(false);
            client.elapseForTesting(1); client.pump();
            assert(pliCount(client.videoDiagnostics()) == secondPli);
            // Inject only the RTC queue pressure decision; actual messages still
            // traverse this fixture's local SCTP channel when pressure clears.
            assert(!client.sendControl("{bad JSON") && client.error().empty());
            client.blockControlForTesting(true);
            stream::Gamepad pad; pad.buttons[0] = 1;
            for (unsigned n=0; n<1000; ++n) assert(client.sendGamepad(pad));
            assert(client.sendGamepad(stream::Gamepad{})); // Latest state must release a held button.
            for (const auto& input : remoteInputs) assert(client.sendControl(input.dump()));
            client.pump();
            assert(client.ready() && client.error().empty());
            assert(client.diagnostics().find("controlPending=11 ") != std::string::npos);
            client.advanceControlForTesting(8000); client.pump();
            assert(client.ready() && client.error().empty()); // Old immediate-failure case.
            client.blockControlForTesting(false);
            Json twiceExpected = expectedRemoteInputs;
            for (const auto& input : expectedRemoteInputs) twiceExpected.push_back(input);
            const auto pressureStart = Clock::now();
            while (Clock::now()-pressureStart < std::chrono::seconds(2) && fixture.remoteControls != twiceExpected) {
                fixture.pump(); client.pump(); fixture.sendMedia();
                stream::Packet packet; while (client.poll(packet)) {}
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            assert(client.error().empty() && fixture.remoteControls == twiceExpected && fixture.neutral);
            assert(client.diagnostics().find("controlPending=0 ") != std::string::npos);
            // A blocked consumer used to terminate a paid session at 128 combined
            // packets. Burst real SRTP media without polling; recovery must keep
            // transports alive and mark the next complete prediction chain.
            for (int n=0; n<64; ++n) fixture.sendMedia();
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            client.pump();
            assert(client.error().empty() && client.ready());
            const auto queued = client.videoDiagnostics();
            assert(queued.find("queueResync=0") == std::string::npos);
            bool generation = false;
            const auto recoverStart = Clock::now();
            while (Clock::now() - recoverStart < std::chrono::seconds(2) && !generation) {
                fixture.pump(); client.pump(); fixture.sendMedia();
                stream::Packet packet;
                while (client.poll(packet)) if (packet.kind == stream::Kind::Video)
                    generation |= packet.videoGeneration > 0;
                assert(client.error().empty());
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            assert(generation);
            // Leave one frame queued long enough to require an IDR-safe age
            // flush rather than rendering hundreds of milliseconds of backlog.
            fixture.sendMedia();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            client.pump();
            assert(client.error().empty() && client.videoDiagnostics().find("queueAge=0") == std::string::npos);
            assert(client.diagnostics().find("iceState=2") != std::string::npos ||
                   client.diagnostics().find("iceState=3") != std::string::npos);
        }
        // Advance only the controller clock, retaining actual local SCTP delivery.
        // Counters emulate 30fps playback here; the host controller tests cover policy edge cases.
        std::uint64_t playbackFrames = 0, playbackWork = 0;
        for (unsigned step = 0; step < 370; ++step) {
            fixture.pump(); client.pump();
            if (step % 5 == 0) fixture.sendMedia();
            stream::Packet drained; while (client.poll(drained)) {}
            if (step % 10 == 0) fixture.dc->send(Json{{"id","stats"},{"preset",fixture.qualityProfile},{"fps",30}}.dump());
            client.advanceQualityForTesting(100);
            playbackFrames += 3; playbackWork += 9000;
            client.observePlayback(playbackFrames,playbackWork,0,false,1920,1080);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        for (unsigned step = 0; step < 10; ++step) {
            fixture.pump(); client.pump();
            client.observePlayback(playbackFrames,playbackWork,0,false,1920,1080);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (fixture.adaptations != std::vector<std::string>{"enhancequality"}) std::cerr << "Auto fixture: " << client.videoDiagnostics() << " actions=" << fixture.adaptations.size() << "\n";
        assert(fixture.adaptations == std::vector<std::string>{"enhancequality"});
        assert(client.videoDiagnostics().find("serverPreset=MID") != std::string::npos);
        bool confirmed = false; std::string qualityEvent;
        while (client.pollVideoDiagnostic(qualityEvent)) {
            if (qualityEvent.find("qualityEvent=confirmed") != std::string::npos) confirmed = true;
            assert(qualityEvent.find("private") == std::string::npos);
        }
        assert(confirmed);
        for (unsigned step = 0; step < 70; ++step) {
            fixture.pump(); client.pump();
            if (step % 5 == 0) fixture.sendMedia();
            stream::Packet drained; while (client.poll(drained)) {}
            if (step % 10 == 0) fixture.dc->send(Json{{"id","stats"},{"preset",fixture.qualityProfile},{"fps",30}}.dump());
            client.advanceQualityForTesting(100);
            playbackFrames += 3; playbackWork += 100000;
            client.observePlayback(playbackFrames,playbackWork,0,false,1920,1080);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        for (unsigned step = 0; step < 10; ++step) {
            fixture.pump(); client.pump();
            client.observePlayback(playbackFrames,playbackWork,0,false,1920,1080);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const std::vector<std::string> expectedAdaptations{"enhancequality", "degradequality"};
        assert(fixture.adaptations == expectedAdaptations);
        assert(client.videoDiagnostics().find("serverPreset=LOW") != std::string::npos);
        assert(!client.configureVideo(settings));
        invalid = settings; invalid.codec = stream::VideoCodec::H264;
        assert(!client.updateVideoSettings(invalid)); // No renegotiation in a live rental.
        invalid = settings; invalid.keyframeSeconds = -1;
        assert(!client.updateVideoSettings(invalid));
        settings.bitrateMbps = 30; settings.quality = 3;
        assert(client.updateVideoSettings(settings));
        const auto qualityStart = Clock::now();
        while (Clock::now() - qualityStart < std::chrono::seconds(2) &&
               (fixture.feedback->remb != 30000000 || fixture.qualityCommand != "set_highquality")) {
            fixture.pump(); client.pump(); fixture.sendMedia();
            stream::Packet drained; while (client.poll(drained)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        assert(fixture.feedback->remb == 30000000 && fixture.qualityCommand == "set_highquality");
        assert(client.error().empty() && client.ready());
        const auto adaptationsBeforeManual = fixture.adaptations.size();
        for (unsigned step=0; step<350; ++step) {
            fixture.pump(); client.pump();
            if (step % 5 == 0) fixture.sendMedia();
            stream::Packet drained; while (client.poll(drained)) {}
            client.advanceQualityForTesting(100);
            playbackFrames += 3; playbackWork += 100000;
            client.observePlayback(playbackFrames,playbackWork,step,false,1920,1080);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        fixture.pump(); assert(fixture.adaptations.size() == adaptationsBeforeManual);
        // Optional stats extensions cannot interrupt play or leak strings.
        fixture.dc->send(Json{{"id","stats"},{"target_bitrate","private-token"},{"preset",Json::array({"secret"})}}.dump());
        for (int tick=0; tick<10; ++tick) {
            fixture.pump(); client.pump(); fixture.sendMedia();
            stream::Packet drained; while (client.poll(drained)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        assert(client.error().empty() && client.videoDiagnostics().find("private") == std::string::npos);
        const int plisBefore = fixture.feedback->plis;
        client.elapseForTesting(3); client.pump();
        for (int tick=0; tick<10; ++tick) {
            fixture.pump(); client.pump(); fixture.sendMedia();
            stream::Packet drained; while (client.poll(drained)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        assert(fixture.feedback->plis == plisBefore); // No unconditional periodic PLI by default.
        settings.keyframeSeconds = 2; assert(client.updateVideoSettings(settings));
        client.elapseForTesting(3); client.pump();
        for (int tick=0; tick<10 && fixture.feedback->plis == plisBefore; ++tick) {
            fixture.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        assert(fixture.feedback->plis > plisBefore);
        // PLI requests share a 250 ms rate limit with transport-loss recovery.
        client.elapseForTesting(1);
        assert(client.requestVideoKeyframe());
        assert(!client.requestVideoKeyframe());
        if (round == 2) {
            assert(client.sendGamepad(stream::Gamepad{}));
            // Ensure the neutral reaches the fixture before intentionally
            // exhausting the control-stall deadline and tearing down SCTP.
            for (unsigned tick=0; tick<20 && !fixture.neutral; ++tick) {
                fixture.pump(); client.pump();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            assert(fixture.neutral);
            client.blockControlForTesting(true);
            assert(client.sendControl(fog::remote::key(false,"Enter").dump()));
            client.advanceControlForTesting(15001);
            client.pump();
            assert(client.error() == "Control channel stalled" && !client.ready());
        }
        client.close();
        const auto finalDiagnostics = client.videoDiagnostics();
        assert(!finalDiagnostics.empty() && client.videoDiagnostics() == finalDiagnostics);
        std::string videoEvent; while (client.pollVideoDiagnostic(videoEvent)) assert(videoEvent.find("private") == std::string::npos);
        assert(!client.requestVideoKeyframe());
        assert(!client.ready());
        const auto closeStart = Clock::now();
        while (Clock::now() - closeStart < std::chrono::seconds(2) && (!fixture.closed || !fixture.neutral)) {
            fixture.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        assert(fixture.closed && fixture.neutral);
        std::cout << "Local transport integration passed: STOMP, actual ICE/DTLS/SRTP/SCTP, "
                     "filtered answer, H264/H265 AnnexB, Opus, PLI, REMB, quality controls/stats/confirmed Auto enhance+degrade/manual isolation, gamepad/mouse/keyboard/close\n";
    }
    stream::Client::cleanupRuntime();
    std::cout << "Full stream runtime cleanup round " << round << " passed\n";
    }
    {
        Fixture fixture;
        stream::Client client;
        assert(client.startForTesting("fixture", "abcde", "ws://127.0.0.1:" + std::to_string(fixture.server->port()) + "/ws/"));
        const auto begin = Clock::now();
        bool rejected = false;
        while (Clock::now() - begin < std::chrono::seconds(3)) {
            fixture.pump(); client.pump();
            if (fixture.ws && fixture.ws->isOpen() && !rejected) {
                fixture.ws->send(stream::protocol::frame("ERROR", {}, "private-sdp-hash-credential")); rejected = true;
            }
            if (!client.error().empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        assert(!client.ready() && !client.error().empty());
        assert(client.error().find("private") == std::string::npos);
        client.close();
        std::cout << "Local transport error/cleanup passed with sanitized diagnostics\n";
    }
    stream::Client::cleanupRuntime();
    // Malformed remote data identifies only the local phase/error class, never body.
    for (bool invalidBase64 : {true, false}) {
        {
            Fixture fixture("4000,4000", false);
            stream::Client client;
            assert(client.startForTesting("fixture", "abcde", "ws://127.0.0.1:" + std::to_string(fixture.server->port()) + "/ws/"));
            const auto begin = Clock::now();
            bool sent = false;
            while (Clock::now() - begin < std::chrono::seconds(3)) {
                fixture.pump(); client.pump();
                if (fixture.enabled && !sent) {
                    const std::string secret = "private-sdp-hash-credential";
                    fixture.signal({{"id","candidate"},{"data",invalidBase64 ? secret : stream::protocol::base64Encode(secret)}});
                    sent = true;
                }
                if (!client.error().empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            assert(sent && !client.error().empty());
            assert(client.error().find(invalidBase64 ? "candidate.base64" : "candidate.json") != std::string::npos);
            if (!invalidBase64) assert(client.error().find("JSON error id=101") != std::string::npos);
            assert(client.error().find("private") == std::string::npos);
            assert(client.diagnostics().find("private") == std::string::npos);
            client.close();
        }
        stream::Client::cleanupRuntime();
    }
    std::cout << "Empty provider offer/ICE markers and safe failure phase diagnostics passed\n";
    // STOMP receive liveness must follow the negotiated server-to-client interval.
    // Keep the fixture before init so no video-stall deadline masks these assertions.
    for (const auto& heartbeat : {"4000,4000", "60000,4000", "0,4000", "", "4000,4000junk", "4000,0,0", "-1,4000"}) {
        {
            Fixture fixture(heartbeat, false);
            stream::Client client;
            assert(client.startForTesting("fixture", "abcde", "ws://127.0.0.1:" + std::to_string(fixture.server->port()) + "/ws/"));
            const auto begin = Clock::now();
            while (Clock::now() - begin < std::chrono::seconds(2)) {
                fixture.pump(); client.pump();
                if (fixture.enabled || !client.error().empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            const bool malformed = std::string(heartbeat).find("junk") != std::string::npos ||
                std::string(heartbeat) == "4000,0,0" || std::string(heartbeat) == "-1,4000";
            if (malformed) {
                assert(!client.error().empty());
            } else {
                assert(fixture.enabled && client.error().empty());
                client.elapseForTesting(20);
                client.pump();
                if (std::string(heartbeat) == "4000,4000") assert(client.error() == "Signaling stream stalled");
                else {
                    assert(client.error().empty());
                    // No heartbeat does not disable the bounded setup timeout.
                    client.elapseForTesting(11);
                    client.pump();
                    assert(client.error() == "Stream connection timed out");
                }
            }
            client.close();
        }
        stream::Client::cleanupRuntime();
    }
    std::cout << "STOMP negotiated liveness, malformed heartbeat rejection, and bounded setup passed\n";
}
