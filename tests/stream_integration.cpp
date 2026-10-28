#include "features/streaming/infrastructure/input_protocol.hpp"
// Local-only STOMP / ICE / DTLS / SRTP / SCTP fixture. Never calls FogPlay.
#include "features/streaming/infrastructure/stream.hpp"
#include "features/streaming/infrastructure/stream_protocol.hpp"
#include "features/streaming/application/remote_input.hpp"
#include "features/playstation/infrastructure/native_input.hpp"
#include <rtc/rtc.hpp>
#include <juice/juice.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cerrno>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <net/if.h>

using Json = stream::protocol::Json;
using Clock = std::chrono::steady_clock;
namespace {
// Linker-local firewall for exactly one fixture peer socket. TURN allocation
// traffic remains real; no system firewall or production transport is changed.
std::atomic<unsigned> restrictedPeerPort{0}, allowedTurnPort{0}, blockedDirectSends{0};
std::atomic<uint32_t> allowedTurnAddress{0};
std::atomic<unsigned> allowedTurnSends{0}, permissionSends{0}, relayDataSends{0};
}
extern "C" ssize_t __real_sendto(int, const void*, size_t, int, const sockaddr*, socklen_t);
extern "C" ssize_t __wrap_sendto(int fd, const void* data, size_t size, int flags, const sockaddr* destination, socklen_t length) {
    const auto peerPort = restrictedPeerPort.load();
    if (peerPort && destination && destination->sa_family == AF_INET) {
        sockaddr_in source{}; socklen_t sourceLength = sizeof(source);
        if (getsockname(fd, reinterpret_cast<sockaddr*>(&source), &sourceLength) == 0 &&
            source.sin_family == AF_INET && ntohs(source.sin_port) == peerPort) {
            const auto* target = reinterpret_cast<const sockaddr_in*>(destination);
            if ((ntohs(target->sin_port) != allowedTurnPort.load() && ntohs(target->sin_port) != peerPort) ||
                target->sin_addr.s_addr != allowedTurnAddress.load()) {
                ++blockedDirectSends;
                errno = ENETUNREACH; return -1;
            }
            if (ntohs(target->sin_port) == allowedTurnPort.load()) {
                ++allowedTurnSends;
                if (size >= 2) {
                    const auto* bytes = static_cast<const unsigned char*>(data);
                    const auto type = (unsigned(bytes[0]) << 8) | unsigned(bytes[1]);
                    if (type == 0x0008) ++permissionSends;
                    if (type == 0x0016 || (bytes[0] & 0xc0) == 0x40) ++relayDataSends;
                }
            }
        }
    }
    return __real_sendto(fd, data, size, flags, destination, length);
}
namespace {
std::string localFixtureAddress() {
    ifaddrs* interfaces = nullptr; assert(getifaddrs(&interfaces) == 0);
    std::string result = "127.0.0.1";
    for (auto* item = interfaces; item; item = item->ifa_next) {
        if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET || (item->ifa_flags & IFF_LOOPBACK) ||
            !(item->ifa_flags & IFF_UP)) continue;
        const auto* address = reinterpret_cast<const sockaddr_in*>(item->ifa_addr);
        if ((ntohl(address->sin_addr.s_addr) >> 24) == 127 || !address->sin_addr.s_addr) continue;
        char numeric[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &address->sin_addr, numeric, sizeof(numeric))) { result = numeric; break; }
    }
    freeifaddrs(interfaces); return result;
}
unsigned availableUdpPort() {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0); assert(fd >= 0);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_ANY);
    assert(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t size = sizeof(address);
    assert(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0);
    const auto port = ntohs(address.sin_port); close(fd); return port;
}
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
    std::atomic<int> plis{0}, nacks{0}, retransmits{0};
    std::atomic<bool> dropNext{false};
    std::atomic<unsigned> reportSsrc{0}, lastPliSsrc{0};
    bool useRtx = true;
    std::atomic<unsigned> responseDelayMs{0};
    std::atomic<unsigned> actualResponseDelayMs{0};
    bool requestKeyframe(const rtc::message_callback& send) override {
        const auto source=reportSsrc.exchange(0); if (!source) return false;
        rtc::binary report(28);report[0]=std::byte{0x80};report[1]=std::byte{200};report[3]=std::byte{6};
        report[4]=std::byte(source>>24);report[5]=std::byte(source>>16);report[6]=std::byte(source>>8);report[7]=std::byte(source);
        send(rtc::make_message(std::move(report),rtc::Message::Control)); return true;
    }
    std::mutex lostMutex;
    rtc::binary lost;
    rtc::binary pendingResponse;
    rtc::message_callback pendingSend;
    Clock::time_point responseQueuedAt{}, responseDue{};
    void pump() {
        rtc::binary response;
        rtc::message_callback send;
        {
            std::lock_guard<std::mutex> lock(lostMutex);
            if (!pendingResponse.empty() && Clock::now() >= responseDue) {
                response.swap(pendingResponse);
                send = std::move(pendingSend);
                actualResponseDelayMs = static_cast<unsigned>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    Clock::now() - responseQueuedAt).count());
            }
        }
        if (send) { send(rtc::make_message(std::move(response))); ++retransmits; }
    }
    void cancelResponse() {
        std::lock_guard<std::mutex> lock(lostMutex);
        pendingResponse.clear(); pendingSend = {};
    }
    void outgoing(rtc::message_vector& messages, const rtc::message_callback&) override {
        for (auto it = messages.begin(); it != messages.end();) {
            const auto& msg = *it;
            if (msg->type == rtc::Message::Binary && msg->size() >= 12 &&
                (std::to_integer<unsigned>(msg->at(1)) & 127) == 98 && dropNext.exchange(false)) {
                std::lock_guard<std::mutex> lock(lostMutex);
                lost.assign(msg->begin(), msg->end()); it = messages.erase(it);
            } else ++it;
        }
    }
    std::atomic<unsigned> remb{0};
    void incoming(rtc::message_vector& messages, const rtc::message_callback& send) override {
        // Regular RR and REMB share one SRTCP datagram. Inspect every RTCP
        // member, preserving the original compound for the remaining chain.
        rtc::message_vector packets;
        for (const auto& message : messages) {
            if (message->type != rtc::Message::Control) continue;
            std::size_t offset = 0;
            while (offset < message->size()) {
                assert(message->size() - offset >= sizeof(rtc::RtcpHeader));
                const auto *header = reinterpret_cast<const rtc::RtcpHeader *>(message->data() + offset);
                const auto bytes = header->lengthInBytes();
                assert(header->version() == 2 && bytes >= sizeof(rtc::RtcpHeader) && bytes <= message->size() - offset);
                packets.push_back(rtc::make_message(message->begin() + offset,
                    message->begin() + offset + bytes, rtc::Message::Control));
                offset += bytes;
            }
        }
        for (const auto& msg : packets) {
            if (msg->size() >= 16 && msg->at(1) == std::byte{205} && msg->at(0) == std::byte{0x81}) {
                ++nacks; rtc::binary rtx;
                {
                    std::lock_guard<std::mutex> lock(lostMutex);
                    assert(msg->at(8)==std::byte{0} && msg->at(9)==std::byte{0} && msg->at(10)==std::byte{4} && msg->at(11)==std::byte{0xd2});
                    if (!lost.empty()) {
                        const auto seq = (std::to_integer<unsigned>(lost[2]) << 8) | std::to_integer<unsigned>(lost[3]);
                        for (std::size_t at=12; at+4<=msg->size(); at+=4) {
                            const auto pid=(std::to_integer<unsigned>(msg->at(at)) << 8) | std::to_integer<unsigned>(msg->at(at+1));
                            const auto mask=(std::to_integer<unsigned>(msg->at(at+2)) << 8) | std::to_integer<unsigned>(msg->at(at+3));
                            const auto delta=std::uint16_t(seq-pid);
                            if (delta==0 || (delta<=16 && (mask & (1u << (delta-1))))) {
                                rtx = lost;
                                if (useRtx) {
                                    rtx.insert(rtx.begin()+12,{lost[2],lost[3]});
                                    rtx[1]=std::byte((std::to_integer<unsigned>(rtx[1]) & 128) | 99);
                                    rtx[2]=std::byte{0};rtx[3]=std::byte{77};
                                    rtx[8]=std::byte{0};rtx[9]=std::byte{0};rtx[10]=std::byte{0x10};rtx[11]=std::byte{0xe1};
                                }
                                lost.clear(); break;
                            }
                        }
                    }
                }
                if (!rtx.empty()) {
                    const auto delayMs = responseDelayMs.load();
                    if (delayMs) {
                        std::lock_guard<std::mutex> lock(lostMutex);
                        responseQueuedAt = Clock::now();
                        responseDue = responseQueuedAt + std::chrono::milliseconds(delayMs);
                        pendingResponse = std::move(rtx); pendingSend = send;
                    } else { send(rtc::make_message(std::move(rtx))); ++retransmits; }
                }
            }
            if (msg->size() >= 12 && std::to_integer<unsigned>(msg->at(1)) == 206 &&
                (std::to_integer<unsigned>(msg->at(0)) & 31) == 1) {
                lastPliSsrc=(std::to_integer<unsigned>(msg->at(8))<<24)|(std::to_integer<unsigned>(msg->at(9))<<16)|
                    (std::to_integer<unsigned>(msg->at(10))<<8)|std::to_integer<unsigned>(msg->at(11)); ++plis;
            }
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
    std::vector<int> gamepadButton0;
    unsigned burstPongs = 0;
    Json remoteControls = Json::array();
    std::string qualityCommand;
    std::vector<Json> scales;
    std::vector<std::string> configurationOrder;
    std::vector<std::string> adaptations;
    std::string qualityProfile = "LOW";
    bool confirmAdaptations = true;
    std::vector<rtc::Candidate> candidates;
    std::vector<std::string> advertisedRelayCandidates;
    std::vector<std::string> mids;
    std::string pendingOffer;
    std::uint32_t frame = 0;
    std::string heartbeat;
    std::chrono::milliseconds serverHeartbeatInterval{0};
    Clock::time_point lastServerHeartbeat{};
    void enableHeartbeats() {
        const auto comma = heartbeat.find(',');
        assert(comma != std::string::npos);
        const auto advertised = std::stoul(heartbeat.substr(0, comma));
        assert(advertised <= 60000);
        serverHeartbeatInterval = advertised ? std::chrono::milliseconds(std::max(4000ul, advertised)) : std::chrono::milliseconds(0);
        lastServerHeartbeat = Clock::now();
    }
    bool initializePeer;
    bool expectRelay;
    bool expectPlayStation;
    int expectedFps = 30;
    bool rejectWebrtc = false;
    juice_server_t* turn = nullptr;
    std::string turnAddress;
    bool hevc;
    bool omitRtxSource = false;
    bool advertiseRtx = true;
    std::string fixtureClientHash;
    explicit Fixture(std::string serverHeartbeat = "4000,4000", bool initialize = true, bool useHevc = false,
                     std::string clientSuffix = "abcde", bool useTurn = false, bool requestedRelay = false,
                     bool playStation = false)
        : heartbeat(std::move(serverHeartbeat)), initializePeer(initialize), expectRelay(requestedRelay),
          expectPlayStation(playStation), hevc(useHevc),
          fixtureClientHash("fixture" + std::move(clientSuffix)) {
        if (useTurn) {
            juice_server_credentials_t credentials{"fixture-user", "fixture-password", 4};
            juice_server_config_t turnConfig{};
            turnAddress = localFixtureAddress();
            turnConfig.bind_address = turnAddress.c_str();
            turnConfig.external_address = turnAddress.c_str();
            turnConfig.credentials = &credentials; turnConfig.credentials_count = 1;
            turnConfig.max_allocations = 4; turnConfig.max_peers = 8;
            turnConfig.realm = "fixture";
            turn = juice_server_create(&turnConfig);
            assert(turn && juice_server_get_port(turn));
            blockedDirectSends = 0; allowedTurnSends = 0; permissionSends = 0; relayDataSends = 0; allowedTurnPort = juice_server_get_port(turn);
            in_addr numeric{}; assert(inet_pton(AF_INET, turnAddress.c_str(), &numeric) == 1);
            allowedTurnAddress = numeric.s_addr;
            restrictedPeerPort = availableUdpPort();
        }
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
        feedback->cancelResponse();
        if (dc) { dc->resetCallbacks(); dc->close(); }
        if (video) video->close();
        if (audio) audio->close();
        if (peer) { peer->resetCallbacks(); peer->close(); }
        if (ws) { ws->resetCallbacks(); ws->close(); }
        server->stop();
        if (turn) { restrictedPeerPort = 0; juice_server_destroy(turn); }
    }
    void signal(const Json& json) {
        ws->send(stream::protocol::frame("MESSAGE", {{"destination", "/topic/" + fixtureClientHash + "/client"}}, json.dump()));
    }
    void createPeer() {
        rtc::Configuration config;
        config.disableAutoNegotiation = true;
        // Local fixture only, including its embedded loopback TURN server.
        if (turn) {
            config.bindAddress = turnAddress;
            config.portRangeBegin = static_cast<uint16_t>(restrictedPeerPort.load());
            config.portRangeEnd = config.portRangeBegin;
            config.iceTransportPolicy = rtc::TransportPolicy::Relay;
            config.iceServers.emplace_back(turnAddress, juice_server_get_port(turn), "fixture-user", "fixture-password");
        }
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
        if (advertiseRtx) {
            v.addVideoCodec(99, "rtx", "apt=98");
            if (!omitRtxSource) v.addSSRC(4321, "fixture", "fixture", "cam");
            v.addAttribute("ssrc-group:FID 1234 4321");
        }
        v.rtpMap(98)->addFeedback("nack");
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
            assert(subscribed && j.at("client_hash") == fixtureClientHash); enabled = true;
            if (initializePeer) signal({{"id", "init"}, {"data", Json::array({"[]"}).dump()}});
        } else if (id == "init_webrtc") {
            assert(j.at("iceTransportPolicy") == (expectRelay ? "relay" : "all"));
            assert(j.at("isPS") == expectPlayStation);
            if (rejectWebrtc) { signal({{"id", "init_webrtc_error"}}); return; }
            assert(enabled && j.at("limit_fps") == expectedFps && j.at("gamepad") == true && j.at("control_only") == false);
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
            assert(v->payloadTypes() == (advertiseRtx ? std::vector<int>{98,99} : std::vector<int>{98}) &&
                   v->direction() == rtc::Description::Direction::RecvOnly);
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
        feedback->pump();
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
                        assert(f.headers.at("destination") == "/topic/" + fixtureClientHash + "/client" && f.headers.at("ack") == "auto");
                        subscribed = true;
                    } else if (f.command == "SEND") {
                        assert(f.headers.at("destination") == "/topic/fixture/server" && f.headers.at("client-hash") == fixtureClientHash);
                        message(Json::parse(f.body));
                    }
                }
            } else if (e.type == Event::Offer) {
                mids = stream::protocol::mediaMids(Json::parse(e.text).at("sdp"));
                pendingOffer = e.text;
            } else if (e.type == Event::Candidate) {
                if (rtc::Candidate(e.text).type() == rtc::Candidate::Type::Relayed) advertisedRelayCandidates.push_back(e.text);
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
                else if (id == "pong") {
                    if (j.at("ts") == 42) pong = true;
                    else { assert(j.at("ts").get<unsigned>() >= 1000 && j.at("ts").get<unsigned>() < 1010); ++burstPongs; }
                }
                else if (id.rfind("set_",0) == 0) {
                    qualityCommand = id;
                    configurationOrder.push_back(id);
                    dc->send(Json{{"id","stats"},{"target_bitrate",18000000},{"preset",qualityProfile},
                                  {"fps",30},{"autoquality",true},{"server_codec",hevc ? "H265" : "H264"}}.dump());
                }
                else if (id == "apply_scale") {
                    assert(j.size() == 3 && j.at("preset").is_number_unsigned() && j.at("enabled").is_boolean());
                    scales.push_back(j);
                    configurationOrder.push_back(id);
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
                    assert(j.at("isPS") == expectPlayStation);
                    assert(j.at("state").at("buttons").size() == 18 && j.at("state").at("axes").size() == 4);
                    gamepadButton0.push_back(j.at("state").at("buttons")[0].get<int>());
                    if (j.at("state").at("buttons")[0] == 1) pressed = true;
                    else if (pressed) neutral = true;
                } else if (id == "gamepad_touch" || id == "gamepad_sensor" || id == "mousemove" || id == "mousedown" || id == "mouseup" ||
                           id == "mousewheel" || id == "keydown" || id == "keyup" || id == "unicode_input") {
                    remoteControls.push_back(j);
                }
            }
        }
        const auto now = Clock::now();
        if (subscribed && ws && ws->isOpen() && serverHeartbeatInterval.count() &&
            now - lastServerHeartbeat >= serverHeartbeatInterval) {
            ws->send(std::string("\n"));
            lastServerHeartbeat = now;
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

static std::uint64_t signalingCount(const std::string& diagnostics, const char* name) {
    const auto key = std::string(name) + "=";
    const auto start = diagnostics.find(key);
    assert(start != std::string::npos);
    return std::stoull(diagnostics.substr(start + key.size()));
}
static void rumbleLifecycle() {
    {
        Fixture fixture("0,0", false);
        stream::Client client;
        stream::Rumble vibration;
        const auto endpoint = "ws://127.0.0.1:" + std::to_string(fixture.server->port()) + "/ws/";
        const std::string pulse = R"({"id":"gamepad_notification","index":0,"large_motor":255,"small_motor":1})";
        assert(!client.pollRumble(vibration));
        assert(client.startForTesting("fixture", "abcde", endpoint));
        client.receiveControlForTesting(pulse);
        // Restart while feedback is still pending, without an explicit close.
        assert(client.startForTesting("fixture", "abcdf", endpoint));
        assert(!client.pollRumble(vibration));
        client.receiveControlForTesting(pulse);
        client.close();
        client.receiveControlForTesting(pulse);
        assert(!client.pollRumble(vibration));
        assert(client.startForTesting("fixture", "abcdg", endpoint));
        client.receiveControlForTesting(pulse);
        client.failThenReceiveForTesting("{}", false);
        client.pump();
        assert(!client.error().empty() && !client.pollRumble(vibration));
        client.receiveControlForTesting(pulse);
        assert(!client.pollRumble(vibration));
        client.close();
    }
    stream::Client::cleanupRuntime();
    std::cout << "MTS rumble close, active restart, transport failure and late callback isolation passed\n";
}
static void controlDiagnosticsLifecycle() {
    {
        Fixture fixture("0,0", false);
        stream::Client client;
        const auto endpoint = "ws://127.0.0.1:" + std::to_string(fixture.server->port()) + "/ws/";
        std::string event;
        assert(!client.pollControlDiagnostic(event));
        assert(client.startForTesting("fixture", "abcde", endpoint));
        const std::string unknown = R"({"id":"controller_effect","payload":{"left":25,"right":10,"token":"secret-feedback-token"},"session":"secret-feedback-session"})";
        client.receiveControlForTesting(unknown);
        assert(client.pollControlDiagnostic(event));
        assert(event.find("provider=mts") != std::string::npos && event.find("controller_effect") != std::string::npos);
        assert(event.find("parsed=ignored") != std::string::npos && event.find("left") != std::string::npos);
        assert(event.find("secret-feedback") == std::string::npos);
        assert(!client.pollControlDiagnostic(event));
        for (unsigned i = 0; i < 1000; ++i) client.receiveControlForTesting(unknown);
        client.receiveControlForTesting(R"({"id":"gamepad_notification","index":0,"large_motor":255,"small_motor":128})");
        client.receiveControlForTesting(R"({"id":"gamepad_notification","index":0,"large_motor":256,"small_motor":128})");
        assert(client.error().empty());
        client.close(); // Pending diagnostic samples survive teardown.
        bool ignored = false, rumble = false, invalidRumble = false;
        unsigned count = 0;
        while (client.pollControlDiagnostic(event, true)) {
            assert(++count <= 33 && event.find("secret-feedback") == std::string::npos);
            ignored |= event.find("parsed=ignored") != std::string::npos;
            rumble |= event.find("parsed=rumble") != std::string::npos;
            invalidRumble |= event.find("parsed=invalid-rumble") != std::string::npos;
        }
        assert(ignored && rumble && invalidRumble);
        assert(client.startForTesting("fixture", "abcdf", endpoint));
        assert(!client.pollControlDiagnostic(event, true)); // New generation has its own counts.
        client.receiveControlForTesting("{invalid-private-control");
        client.pump();
        assert(!client.error().empty());
        assert(client.pollControlDiagnostic(event, true));
        assert(event.find("parsed=invalid") != std::string::npos && event.find("private-control") == std::string::npos);
        client.close();
    }
    stream::Client::cleanupRuntime();
    std::cout << "MTS control diagnostic samples, redaction, coalescing and teardown retention passed\n";
}
static void signalingDiagnostics() {
    // Real loopback handshake, deliberately no remote init. Exercise each
    // tracked send's immediate/buffered/throwing result without paid services.
    for (unsigned mode = 0; mode < 5; ++mode) {
        {
            Fixture fixture("0,0", false);
            stream::Client client;
            assert(client.startForTesting("fixture", "abcde", "ws://127.0.0.1:" + std::to_string(fixture.server->port()) + "/ws/"));
            const bool enabled = mode == 2 || mode == 4;
            const bool rejected = mode >= 3;
            if (mode) client.overrideSignalingSendForTesting(enabled, rejected);
            const auto began = Clock::now();
            while (!fixture.enabled && client.error().empty() && Clock::now() - began < std::chrono::seconds(2)) {
                fixture.pump(); client.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            const auto before = client.diagnostics();
            assert(signalingCount(before, "wsMessages") == 1 && signalingCount(before, "stompFrames") == 1);
            assert(signalingCount(before, "stompConnected") == 1 && signalingCount(before, "signalInit") == 0);
            assert(signalingCount(before, "stompMessage") == 0 && signalingCount(before, "stompError") == 0 &&
                signalingCount(before, "stompOther") == 0);
            assert(signalingCount(before, "subscribeAttempts") == 1);
            assert(signalingCount(before, "subscribeLocalAccepted") == (mode == 3 ? 0 : 1));
            assert(signalingCount(before, "subscribeBuffered") == (mode == 1 ? 1 : 0));
            assert(signalingCount(before, "subscribeRejected") == (mode == 3 ? 1 : 0));
            assert(signalingCount(before, "enabledAttempts") == (mode == 3 ? 0 : 1));
            assert(signalingCount(before, "enabledLocalAccepted") == (rejected ? 0 : 1));
            assert(signalingCount(before, "enabledBuffered") == (mode == 2 ? 1 : 0));
            assert(signalingCount(before, "enabledRejected") == (mode == 4 ? 1 : 0));
            assert(before.find("fixture") == std::string::npos && before.find("/topic/") == std::string::npos);
            if (rejected) {
                assert(!client.error().empty() && client.retryableError() && !fixture.enabled);
                assert(client.error().find("Fixture") == std::string::npos);
            } else {
                assert(fixture.subscribed && fixture.enabled && client.error().empty());
                assert(client.status() == "Waiting for remote host");
                // Receive counters distinguish heartbeat/partial input from
                // complete parsed commands without retaining any remote text.
                fixture.ws->send(stream::protocol::frame("RECEIPT", {{"receipt-id", "private-receipt"}}, "private-body"));
                const auto message = stream::protocol::frame("MESSAGE", {}, R"({"id":"private-unknown-message"})");
                fixture.ws->send(message.substr(0, 10));
                fixture.ws->send(message.substr(10));
                fixture.ws->send(std::string("\n"));
                const auto receivedAt = Clock::now();
                while (signalingCount(client.diagnostics(), "wsMessages") < 5 && Clock::now() - receivedAt < std::chrono::seconds(2)) {
                    fixture.pump(); client.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                const auto received = client.diagnostics();
                assert(signalingCount(received, "wsMessages") == 5 && signalingCount(received, "stompFrames") == 3);
                assert(signalingCount(received, "stompMessage") == 1 && signalingCount(received, "stompOther") == 1);
                assert(received.find("private") == std::string::npos && client.error().empty());
                fixture.ws->send(stream::protocol::frame("ERROR", {}, "private-error"));
                const auto errorAt = Clock::now();
                while (client.error().empty() && Clock::now() - errorAt < std::chrono::seconds(2)) {
                    fixture.pump(); client.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                assert(!client.error().empty() && !client.retryableError());
                assert(signalingCount(client.diagnostics(), "stompError") == 1);
                assert(client.diagnostics().find("private") == std::string::npos && client.error().find("private") == std::string::npos);
            }
            client.close();
            assert(signalingCount(client.diagnostics(), "stompConnected") == 1); // retained after stop
        }
        stream::Client::cleanupRuntime();
    }
    std::cout << "Safe STOMP diagnostics: handshake acceptance/buffering/rejection, partial frames, commands, silence, terminal errors and redaction passed\n";
}

int main(int argc, char** argv) {
    const bool turnOnly = argc == 2 && std::string(argv[1]) == "--turn-only";
    if (!turnOnly) rumbleLifecycle();
    if (!turnOnly) controlDiagnosticsLifecycle();
    if (!turnOnly) signalingDiagnostics();
    if (argc == 2 && std::string(argv[1]) == "--signaling-only") return 0;
    {
        Feedback feedback;
        auto compound = rtc::make_message(rtc::RtcpRr::SizeWithReportBlocks(1), rtc::Message::Control);
        auto *report = reinterpret_cast<rtc::RtcpRr *>(compound->data());
        report->preparePacket(1234, 1);
        report->getReportBlock(0)->preparePacket(1234, 0, 0, 100, 0, 0, 0, 0);
        auto remb = rtc::make_message(rtc::RtcpRemb::SizeWithSSRCs(1), rtc::Message::Control);
        auto *bitrate = reinterpret_cast<rtc::RtcpRemb *>(remb->data());
        bitrate->preparePacket(1234, 1, 30000000); bitrate->setSsrc(0, 1234);
        compound->insert(compound->end(), remb->begin(), remb->end());
        rtc::message_vector messages{compound};
        feedback.incoming(messages, [](rtc::message_ptr) { assert(false); });
        assert(feedback.remb == 30000000 && feedback.plis == 0 && feedback.nacks == 0);
        assert(messages.size() == 1 && messages.front() == compound);
        std::cout << "Compound RR/REMB fixture parser passed\n";
    }
    const std::vector<fog::remote::Input> remoteInputs{
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
    // Relay requested but a provider selects direct: never expose ready/control/media.
    {
        Fixture fixture("4000,4000", true, false, "abcde", false, true);
        stream::Client client;
        assert(client.configureRelay(true));
        assert(client.diagnostics().find("requested=relay actual=unknown") != std::string::npos);
        assert(client.startForTesting("fixture", "abcde", "ws://127.0.0.1:" + std::to_string(fixture.server->port()) + "/ws/"));
        assert(!client.configureRelay(false));
        const auto begin = Clock::now();
        while (client.error().empty() && Clock::now() - begin < std::chrono::seconds(4)) {
            fixture.pump(); client.pump(); fixture.sendMedia();
            stream::Packet packet; assert(!client.poll(packet));
            assert(!client.ready() && !fixture.controlReady);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        assert(client.error() == "TURN required but direct path selected" && !client.retryableError());
        assert(client.diagnostics().find("requested=relay actual=direct") != std::string::npos);
    }
    {
        Fixture fixture("4000,4000", true, false, "abcde", false, true);
        fixture.rejectWebrtc = true;
        stream::Client client;
        assert(client.configureRelay(true));
        assert(client.startForTesting("fixture", "abcde", "ws://127.0.0.1:" + std::to_string(fixture.server->port()) + "/ws/"));
        const auto begin = Clock::now();
        while (client.error().empty() && Clock::now() - begin < std::chrono::seconds(2)) {
            fixture.pump(); client.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        assert(client.error() == "Relay transport unavailable: remote initialization rejected");
        assert(!client.ready() && client.diagnostics().find("actual=unknown") != std::string::npos);
    }
    for (int round = turnOnly ? 4 : 1; round <= 4; ++round) {
    {
        const bool turn = round == 4;
        Fixture fixture("4000,4000", true, round == 3, "abcde", turn, turn);
        // Long streaming scenarios advertise heartbeats and must actually send
        // them. Dedicated liveness fixtures below deliberately keep them off.
        fixture.enableHeartbeats();
        stream::Client client;
        if (turn) assert(client.configureRelay(true));
        fog::video::Settings settings;
        settings.codec = stream::VideoCodec::H265; // Fixture only offers H264: test safe fallback.
        settings.bitrateMbps = 15;
        settings.framesPerSecond = fixture.expectedFps = round == 2 ? 60 : 30;
        assert(client.configureVideo(settings));
        auto invalid = settings; invalid.bitrateMbps = 0;
        assert(!client.configureVideo(invalid));
        invalid = settings; invalid.framesPerSecond = 144;
        assert(!client.configureVideo(invalid));
        assert(!client.updateVideoSettings(settings));
        assert(!client.startForTesting("fixture", "abcde", "wss://remoteplay.mts.ru/ws/"));
        assert(client.startForTesting("fixture", "abcde", "ws://127.0.0.1:" + std::to_string(fixture.server->port()) + "/ws/"));
        const auto begin = Clock::now();
        int videos = 0, audios = 0;
        bool sent = false;
        while (Clock::now() - begin < std::chrono::seconds(12)) {
            fixture.pump(); client.pump();
            if (!client.error().empty()) {
                std::cerr << "Stream fixture round=" << round << " failure=" << client.error() << " " << client.diagnostics() << "\n";
                if (turn) {
                    rtc::Candidate local, remote;
                    const auto selected = fixture.peer->getSelectedCandidatePair(&local, &remote);
                    bool relayMatch = false;
                    for (const auto& advertised : fixture.advertisedRelayCandidates)
                        relayMatch |= client.selectedRemoteMatchesForTesting(advertised);
                    std::cerr << "TURN fixture selected=" << selected << " providerLocalType=" << static_cast<int>(local.type())
                        << " selectedRemoteMatchesAdvertisedRelay=" << relayMatch << "\n";
                }
            }
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
                for (const auto& input : remoteInputs) assert(client.sendInput(input));
                client.elapseForTesting(1);
                assert(client.requestVideoKeyframe());
                sent = true;
            }
            if (videos >= 3 && audios >= 3 && fixture.pong && fixture.pressed && fixture.feedback->plis > 0 &&
                fixture.remoteControls == expectedRemoteInputs && fixture.feedback->remb == 15000000 &&
                fixture.qualityCommand == "set_autoquality" && client.qualityStatus().find("18.0") != std::string::npos) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!client.ready() || !fixture.controlReady || !fixture.pong || !fixture.pressed) {
            std::cerr << "Unready fixture round=" << round << " ready=" << client.ready() << " answered=" << fixture.answered
                << " control=" << fixture.controlReady << " pong=" << fixture.pong << " pressed=" << fixture.pressed
                << " blocked=" << blockedDirectSends.load() << " allowedTurn=" << allowedTurnSends.load()
                << " permissions=" << permissionSends.load() << " relayData=" << relayDataSends.load()
                << " provider=" << fixture.peer->iceDiagnostics() << " client=" << client.diagnostics() << "\n";
        }
        assert(client.ready() && fixture.answered && fixture.controlReady && fixture.pong && fixture.pressed);
        const auto route = client.diagnostics();
        assert(signalingCount(route, "signalInit") == 1 && signalingCount(route, "stompMessage") > 0);
        assert(route.find(turn ? "requested=relay actual=turn" : "requested=all actual=direct") != std::string::npos);
        assert(route.find("fixture-user") == std::string::npos && route.find("fixture-password") == std::string::npos);
        assert(route.find("127.0.0.1") == std::string::npos);
        std::string routeEvent;
        assert(client.pollVideoDiagnostic(routeEvent) && routeEvent.find("routeEvent=selected") == 0);
        assert(routeEvent.find(turn ? "actual=turn" : "actual=direct") != std::string::npos);
        if (turn) assert(blockedDirectSends > 0);
        std::cout << "Selected route fixture passed: " << routeEvent << "\n";
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
            // The exact preset comes from the announced table, even when 720p
            // is not at the usual index. Late repeated stats must not spam it.
            const Json table = Json::array({Json::array({1920,1080}), Json::array({1600,900}), Json::array({1280,720})});
            const auto announce = [&](const Json& resolutions) {
                client.receiveControlForTesting(Json{{"id","stats"},{"resolutions",resolutions}}.dump());
                for (unsigned tick = 0; tick < 20; ++tick) {
                    fixture.pump(); client.pump(); fixture.sendMedia();
                    stream::Packet packet; while (client.poll(packet)) {}
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            };
            announce(table);
            assert(fixture.scales.size() == 1 && fixture.scales[0].at("preset") == 2 && fixture.scales[0].at("enabled") == true);
            assert(fixture.configurationOrder.size() >= 2 && fixture.configurationOrder.front() == "set_autoquality" &&
                fixture.configurationOrder[1] == "apply_scale");
            for (unsigned n = 0; n < 20; ++n)
                client.receiveControlForTesting(Json{{"id","stats"},{"resolutions",table}}.dump());
            announce(Json::array({Json::array({1920,1080}), Json::array({1024,576})}));
            announce(table);
            announce(Json::array({Json::array({1920,1080}), Json::array({1280.0,720})}));
            announce(Json::array({Json::array({1920,1080}), Json::array({18446744073709551615ULL,720})}));
            assert(fixture.scales.size() == 1 && client.error().empty());
            announce(Json::array({Json::array({0,0}), Json::array({1280,720})}));
            assert(fixture.scales.size() == 2 && fixture.scales[1].at("preset") == 1 && fixture.scales[1].at("enabled") == true);
            announce(Json::array({Json::array({1280,720}), Json::array({1920,1080})}));
            assert(fixture.scales.size() == 3 && fixture.scales[2].at("preset") == 0 && fixture.scales[2].at("enabled") == false);
            announce(Json::array({Json::array({2560,1440}), Json::array({1280,720})}));
            assert(fixture.scales.size() == 3 && client.error().empty());
            assert(client.videoDiagnostics().find("scaleAttempts=3 scaleQueued=3 scaleRejected=0 scaleInvalidTables=2") != std::string::npos);
            assert(!client.waitingForVideoKeyframe());
            assert(client.videoDiagnostics().find("queueResync=0") != std::string::npos);
            assert(fixture.qualityCommand == "set_autoquality");
            std::cout << "MTS 720p probe: advertised preset, quality-first ordering, repeated/missing/malformed tables and request bound passed\n";
        }
        if (turn) {
            const auto settleIce = [&client] {
                const auto began = Clock::now();
                for (;;) {
                    client.pump();
                    const auto d = client.diagnostics();
                    if (d.find("iceInFlight=0 ") != std::string::npos && d.find("icePending=0 ") != std::string::npos) {
                        client.pump(); return;
                    }
                    assert(Clock::now() - began < std::chrono::seconds(2));
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            };
            // Freeze only route sampling, independent of RTC/heartbeat deadlines.
            client.setRouteSnapshotTimeForTesting(1000);
            settleIce();
            const auto pairCount = [](const stream::Client& current) {
                const auto text = current.transportTimings();
                const auto at = text.find("icePairCalls=");
                assert(at != std::string::npos);
                return std::stoull(text.substr(at + 13));
            };
            // Expiry also applies without a route event or frame pump. The
            // autonomous sender must withhold an old queued input on its own.
            client.blockControlForTesting(true);
            stream::Gamepad repeatedPress; repeatedPress.buttons[0] = 1;
            assert(client.sendGamepad(repeatedPress));
            const auto sentBeforeExpiryText = client.diagnostics();
            const auto sentBeforeExpiry = std::stoull(sentBeforeExpiryText.substr(sentBeforeExpiryText.find("controlSent=") + 12));
            const auto expiryAt = Clock::now();
            while (Clock::now() - expiryAt < std::chrono::milliseconds(2050)) {
                fixture.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            assert(!client.ready() && !client.sendGamepad(repeatedPress));
            client.blockControlForTesting(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            const auto expiredText = client.diagnostics();
            assert(std::stoull(expiredText.substr(expiredText.find("controlSent=") + 12)) == sentBeforeExpiry);
            client.repeatIceStateForTesting(); settleIce(); assert(client.ready());
            std::cout << "Fresh TURN proof expires without pump/events and independently gates autonomous queued sends\n";
            // A getter that began with TURN proof cannot publish it after a
            // route event replaces its epoch. Media and queued input stay gated.
            client.blockIceForTesting(true);
            client.pump();
            const auto blockedIceAt = Clock::now();
            while (client.diagnostics().find("iceInFlight=1 ") == std::string::npos) {
                assert(Clock::now() - blockedIceAt < std::chrono::seconds(2));
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            client.setSelectedCandidatesForTesting("local 1 UDP 1 192.0.2.20 50001 typ host",
                                                  "unknown 1 UDP 1 192.0.2.10 50000 typ prflx");
            const auto pumping = Clock::now();
            for (unsigned tick = 0; tick < 100; ++tick) {
                client.pump(); assert(!client.ready() && client.error().empty());
                assert(!client.sendGamepad(stream::Gamepad{}));
                stream::Packet withheld; assert(!client.poll(withheld));
                (void)client.diagnostics(); (void)client.dtlsDiagnostics();
            }
            assert(Clock::now() - pumping < std::chrono::milliseconds(200));
            client.blockIceForTesting(false); settleIce();
            assert(!client.ready() && client.diagnostics().find("actual=unknown") != std::string::npos);
            assert(client.diagnostics().find("iceDiscarded=0 ") == std::string::npos);
            client.setSelectedCandidatesForTesting("local 1 UDP 1 192.0.2.20 50001 typ host",
                                                  "relay 1 UDP 1 192.0.2.30 50002 typ relay");
            settleIce(); assert(client.ready());
            std::cout << "Blocked ICE read keeps pump/diagnostics responsive, rejects stale TURN proof, and gates input/media until fresh epoch\n";
            // A periodic completion in the SAME epoch must revoke cached TURN
            // authorization too, including the worker's queued-send gate.
            const auto generation = client.diagnostics().substr(client.diagnostics().find(" iceGeneration="), 18);
            client.blockControlForTesting(true);
            stream::Gamepad heldPad; heldPad.buttons[0] = 1;
            assert(client.sendGamepad(heldPad));
            const auto controlSent = [](const stream::Client& current) {
                const auto d = current.diagnostics(); return std::stoull(d.substr(d.find("controlSent=") + 12));
            };
            const auto sentBeforeNoPair = controlSent(client);
            client.clearSelectedCandidatesForTesting(false);
            client.setRouteSnapshotTimeForTesting(2000); settleIce();
            assert(client.diagnostics().substr(client.diagnostics().find(" iceGeneration="), 18) == generation);
            assert(!client.ready() && !client.sendGamepad(heldPad));
            client.blockControlForTesting(false);
            for (unsigned tick = 0; tick < 20; ++tick) {
                client.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            assert(controlSent(client) == sentBeforeNoPair);
            client.setSelectedCandidatesForTesting("local 1 UDP 1 192.0.2.20 50001 typ host",
                                                  "relay 1 UDP 1 192.0.2.30 50002 typ relay");
            client.setRouteSnapshotTimeForTesting(1000); settleIce(); assert(client.ready());
            const auto stableQueries = pairCount(client);
            for (unsigned tick = 0; tick < 20; ++tick) client.pump();
            assert(pairCount(client) == stableQueries && client.ready());
            // Query exceptions must gate historical TURN and obey the same
            // retry interval, without consuming new-event invalidation.
            client.failSelectedPairForTesting(true); settleIce();
            assert(pairCount(client) == stableQueries + 1 && !client.ready());
            assert(!client.sendGamepad(stream::Gamepad{}));
            for (unsigned tick = 0; tick < 20; ++tick) client.pump();
            assert(pairCount(client) == stableQueries + 1);
            client.setRouteSnapshotTimeForTesting(1999); settleIce();
            assert(pairCount(client) == stableQueries + 1);
            client.setRouteSnapshotTimeForTesting(2000); settleIce();
            assert(pairCount(client) == stableQueries + 2 && !client.ready());
            client.failSelectedPairForTesting(false); settleIce();
            assert(pairCount(client) == stableQueries + 3 && client.ready());
            const auto verifiedQueries = pairCount(client);
            client.repeatIceStateForTesting(); settleIce();
            assert(pairCount(client) == verifiedQueries + 1 && client.ready());
            // Never reuse the preceding TURN proof when the selected pair disappears.
            client.clearSelectedCandidatesForTesting(); settleIce();
            assert(pairCount(client) == verifiedQueries + 2 && !client.ready());
            assert(client.diagnostics().find("routeCurrent=0") != std::string::npos);
            assert(!client.sendGamepad(stream::Gamepad{}) && !client.requestVideoKeyframe());
            stream::Packet unavailable; assert(!client.poll(unavailable));
            for (unsigned tick = 0; tick < 20; ++tick) client.pump();
            assert(pairCount(client) == verifiedQueries + 2);
            client.setRouteSnapshotTimeForTesting(2999); settleIce();
            assert(pairCount(client) == verifiedQueries + 2);
            client.setRouteSnapshotTimeForTesting(3000); settleIce();
            assert(pairCount(client) == verifiedQueries + 3 && !client.ready());
            const auto timing = client.transportTimings();
            assert(timing.find("scope=transport-lifetime nested=1") == 0);
            for (const auto* stage : {"pump", "nack", "iceDiag", "icePair", "receive", "events", "quality", "controlFlush", "pli", "heartbeat"}) {
                assert(timing.find(std::string(stage) + "Calls=") != std::string::npos);
                assert(timing.find(std::string(stage) + "AvgUs=") != std::string::npos);
                assert(timing.find(std::string(stage) + "MaxUs=") != std::string::npos);
            }
            assert(timing.size() < 2048 && timing.find("192.0.") == std::string::npos);
            std::cout << "Route sampling: unchanged/no-pair cadence, ICE invalidation, required-TURN gate and bounded stage timings passed\n";
            client.setSelectedCandidatesForTesting("local 1 UDP 1 192.0.2.20 50001 typ host",
                                                  "relay 1 UDP 1 192.0.2.30 50002 typ relay");
            settleIce(); assert(client.ready());
            // Keep the real TURN transport connected, but inject a newly selected
            // prflx route before its delayed SDP/trickle provenance arrives.
            assert(!fixture.neutral);
            client.blockControlForTesting(true);
            assert(client.sendGamepad(stream::Gamepad{}));
            client.setSelectedCandidatesForTesting("local 1 UDP 1 192.0.2.20 50001 typ host",
                                                  "unknown 1 UDP 1 192.0.2.10 50000 typ prflx");
            client.blockControlForTesting(false);
            settleIce();
            assert(!client.ready() && client.error().empty());
            assert(!client.sendGamepad(stream::Gamepad{}) && !client.sendInput(remoteInputs.front()));
            assert(!client.requestVideoKeyframe());
            stream::Packet withheld; assert(!client.poll(withheld));
            client.addCandidateProofForTesting("decoy 1 UDP 1 192.0.2.11 50000 typ relay");
            for (unsigned tick=0; tick<10; ++tick) {
                fixture.pump(); settleIce(); fixture.sendMedia();
                assert(!client.ready() && !client.poll(withheld));
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            assert(!fixture.neutral && client.diagnostics().find("controlPending=1 ") != std::string::npos);
            assert(client.diagnostics().find("actual=unknown") != std::string::npos);
            client.elapseForTesting(29);
            const auto beforeProof = pairCount(client);
            client.addCandidateProofForTesting("relay-new-foundation 1 UDP 1 192.0.2.10 50000 typ relay");
            fixture.signal({{"id", "ping"}, {"ts", 0}});
            fixture.sendMedia();
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            settleIce();
            assert(pairCount(client) == beforeProof + 1 && client.ready());
            const auto proofAt = Clock::now();
            while (!fixture.neutral && Clock::now() - proofAt < std::chrono::seconds(1)) {
                fixture.pump(); settleIce(); fixture.sendMedia();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            assert(client.ready() && fixture.neutral && client.error().empty());
            assert(client.diagnostics().find("actual=turn") != std::string::npos);
            assert(client.diagnostics().find("remoteType=prflx") != std::string::npos);
            assert(client.diagnostics().find("remoteProof=relay-endpoint") != std::string::npos);
            const auto resumeAt = Clock::now();
            bool resumedVideo = false;
            while ((!resumedVideo || client.waitingForVideoKeyframe()) && Clock::now() - resumeAt < std::chrono::seconds(1)) {
                fixture.pump(); settleIce(); fixture.sendMedia();
                while (client.poll(withheld)) resumedVideo |= withheld.kind == stream::Kind::Video;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            assert(resumedVideo && !client.waitingForVideoKeyframe() && client.error().empty());
            std::cout << "TURN route transition gates ready/input/queue/media/PLI until exact delayed endpoint proof\n";
            client.setRouteSnapshotTimeForTesting(0); // Resume real cadence for later long-running quality checks.
        }
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
            const auto gamepadBeforePressure = fixture.gamepadButton0.size();
            client.blockControlForTesting(true);
            stream::Gamepad pad; pad.buttons[0] = 1;
            for (unsigned n=0; n<1000; ++n) assert(client.sendGamepad(pad));
            assert(client.sendGamepad(stream::Gamepad{})); // Latest state must release a held button.
            for (const auto& input : remoteInputs) assert(client.sendInput(input));
            client.pump();
            assert(client.ready() && client.error().empty());
            assert(client.diagnostics().find("controlPending=12 ") != std::string::npos);
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
            assert(fixture.gamepadButton0.size() == gamepadBeforePressure + 2);
            assert(fixture.gamepadButton0[gamepadBeforePressure] == 1 && fixture.gamepadButton0.back() == 0);
            assert(client.diagnostics().find("controlPending=0 ") != std::string::npos);
            // Authenticated SRTP burst: backpressure now keeps the complete
            // ordered backlog in the assembler instead of overflowing MaxVideo.
            for (int n=0; n<64; ++n) fixture.sendMedia();
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            client.pump();
            assert(client.error().empty() && client.ready());
            const auto queued = client.videoDiagnostics();
            assert(queued.find("queueResync=0 ") != std::string::npos);
            unsigned burstFrames = 0;
            const auto recoverStart = Clock::now();
            while (Clock::now() - recoverStart < std::chrono::seconds(2) && burstFrames < 64) {
                fixture.pump(); client.pump();
                stream::Packet packet;
                while (client.poll(packet)) if (packet.kind == stream::Kind::Video) {
                    assert(packet.videoGeneration == 0); ++burstFrames;
                }
                assert(client.error().empty());
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            assert(burstFrames >= 64 && client.videoDiagnostics().find("queueResync=0 ") != std::string::npos);
            // A genuinely stale consumer still needs an IDR-safe age flush.
            fixture.sendMedia();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            client.pump();
            assert(client.error().empty() && client.videoDiagnostics().find("queueAge=0") == std::string::npos);
            bool generation = false;
            const auto staleRecoveryStart = Clock::now();
            while (Clock::now() - staleRecoveryStart < std::chrono::seconds(2) && !generation) {
                fixture.pump(); client.pump(); fixture.sendMedia();
                stream::Packet packet;
                while (client.poll(packet)) if (packet.kind == stream::Kind::Video)
                    generation |= packet.videoGeneration > 0;
                assert(client.error().empty());
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            assert(generation);
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
            client.observePlayback({playbackFrames,playbackFrames,0,playbackWork,0,0,0,false,1920,1080});
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        for (unsigned step = 0; step < 10; ++step) {
            fixture.pump(); client.pump();
            client.observePlayback({playbackFrames,playbackFrames,0,playbackWork,0,0,0,false,1920,1080});
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
            client.observePlayback({playbackFrames,playbackFrames,0,playbackWork,0,0,0,false,1920,1080});
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        for (unsigned step = 0; step < 10; ++step) {
            fixture.pump(); client.pump();
            client.observePlayback({playbackFrames,playbackFrames,0,playbackWork,0,0,0,false,1920,1080});
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const std::vector<std::string> expectedAdaptations{"enhancequality", "degradequality"};
        assert(fixture.adaptations == expectedAdaptations);
        assert(client.videoDiagnostics().find("serverPreset=LOW") != std::string::npos);
        assert(!client.configureVideo(settings));
        invalid = settings; invalid.codec = stream::VideoCodec::H264;
        assert(!client.updateVideoSettings(invalid)); // No renegotiation in a live rental.
        invalid = settings; invalid.framesPerSecond = settings.framesPerSecond == 30 ? 60 : 30;
        assert(!client.updateVideoSettings(invalid));
        assert(client.videoDiagnostics().find("requestedFps=" + std::to_string(settings.framesPerSecond)) != std::string::npos);
        assert(client.qualityStatus().find(std::to_string(settings.framesPerSecond) + " FPS") != std::string::npos);
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
            client.observePlayback({playbackFrames,playbackFrames,0,playbackWork,step,0,0,false,1920,1080});
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
        if (round == 1 || round == 3) {
            // No render pump while more than the observed 702 SCTP messages arrive.
            for (unsigned n=0; n<1000; ++n) {
                fixture.dc->send(Json{{"id","ignored-private-id"},{"payload","private"}}.dump());
                fixture.dc->send(Json{{"id","stats"},{"fps",30},{"preset",Json::array({"private"})}}.dump());
                if (n % 100 == 0) fixture.dc->send(Json{{"id","ping"},{"ts",1000+n/100}}.dump());
                if (round == 3 && n == 500) fixture.dc->send(Json{{"id","close"}}.dump());
            }
            const auto burstStart = Clock::now();
            while (Clock::now()-burstStart < std::chrono::seconds(3)) {
                fixture.pump(); // Deliberately no client.pump()/poll().
                if (fixture.burstPongs == 10 && client.diagnostics().find("incomingIgnored=1000 ") != std::string::npos) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            assert(fixture.burstPongs == 10);
            assert(client.diagnostics().find("incomingIgnored=1000 ") != std::string::npos);
            assert(client.diagnostics().find("eventOverflow=0") != std::string::npos);
            assert(client.diagnostics().find("private") == std::string::npos);
            client.pump();
            if (round == 3) {
                assert(client.error() == "Remote control closed" && !client.retryableError());
                assert(!client.ready() && !client.disconnected());
            } else {
                if (!client.error().empty() || !client.ready() || client.disconnected())
                    std::cerr << ("Post-burst failure round=" + std::to_string(round) + " error=[" + client.error() +
                                  "] status=" + client.status() + " " + client.rtcpDiagnostics() + "\n");
                assert(client.error().empty() && client.ready() && !client.disconnected());
                assert(client.videoDiagnostics().find("serverPreset=LOW") != std::string::npos);
                // A public input send can fail before pump sees terminal signaling.
                client.blockControlForTesting(true);
                assert(client.sendInput(fog::remote::key(false,"Enter")));
                client.advanceControlForTesting(15001);
                assert(!client.sendGamepad(stream::Gamepad{}));
                assert(!client.sendInput(fog::remote::key(false,"Enter")));
                assert(client.error().empty() && client.ready());
                client.failThenReceiveForTesting(stream::protocol::frame("MESSAGE",{},R"({"id":"close"})"),true);
                client.pump();
                assert(client.error() == "Remote signaling closed stream" && !client.retryableError());
            }
        }
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
            assert(client.sendInput(fog::remote::key(false,"Enter")));
            client.advanceControlForTesting(15001);
            client.pump();
            assert(client.error() == "Control channel stalled" && !client.ready() && !client.retryableError());
        }
        client.close();
        assert(client.diagnostics().find(turn ? "requested=relay actual=turn" : "requested=all actual=direct") != std::string::npos);
        assert(client.diagnostics().find("routeSnapshot=final") != std::string::npos);
        assert(client.diagnostics().find("iceReason=") != std::string::npos);
        const auto finalDtls = client.dtlsDiagnostics();
        assert(finalDtls.find("available=1") != std::string::npos);
        assert(finalDtls.find("enqueued=0 ") == std::string::npos);
        assert(finalDtls.find("recvRuns=0 ") == std::string::npos);
        assert(finalDtls == client.dtlsDiagnostics());
        const auto finalMutex = client.mutexDiagnostics();
        assert(!finalMutex.empty() && finalMutex == client.mutexDiagnostics());
        assert(finalDtls.find("private") == std::string::npos);

        const auto finalGaps = client.gapDiagnostics();
        assert(!finalGaps.empty() && finalGaps == client.gapDiagnostics());
        assert(finalGaps.find("private") == std::string::npos);
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
    if (turnOnly) return 0;
    for (const bool hevc : {false,true}) {
        Fixture fixture("4000,4000",true,hevc);
        fixture.omitRtxSource = true; // FID announces RTX, with no individual a=ssrc attribute.
        stream::Client client;
        fog::video::Settings settings; settings.codec=hevc ? stream::VideoCodec::H265 : stream::VideoCodec::H264;
        assert(client.configureVideo(settings));
        assert(client.startForTesting("fixture", "abcde", "ws://127.0.0.1:" + std::to_string(fixture.server->port()) + "/ws/"));
        const auto start = Clock::now();
        unsigned frames = 0; bool requestedLoss = false;
        while (Clock::now()-start < std::chrono::seconds(5)) {
            fixture.pump(); client.pump();
            if (frames >= 3 && !requestedLoss) { fixture.feedback->dropNext=true; requestedLoss=true; }
            fixture.sendMedia();
            stream::Packet packet;
            while (client.poll(packet)) if (packet.kind == stream::Kind::Video) {
                assert(packet.videoGeneration==0); ++frames;
            }
            assert(client.error().empty());
            if (requestedLoss && frames >= 10 && fixture.feedback->retransmits > 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const auto diagnostic = client.videoDiagnostics();
        assert(frames >= 10 && fixture.feedback->nacks > 0 && fixture.feedback->retransmits==1);
        assert(diagnostic.find("rtxRestored=1")!=std::string::npos && diagnostic.find("repairedPackets=1")!=std::string::npos);
        assert(diagnostic.find("gaps=0 ")!=std::string::npos && diagnostic.find("resync=0 ")!=std::string::npos);
        assert(fixture.peer->remoteDescription()->generateSdp().find("rtpmap:99 rtx/90000")!=std::string::npos);
        // No primary RTP after this point: an RTX SR must not overwrite the
        // remembered media SSRC used for the next PLI by the pinned RTCP receiver.
        fixture.feedback->reportSsrc=4321;assert(fixture.video->requestKeyframe());
        for(unsigned tick=0;tick<60;++tick){fixture.pump();client.pump();stream::Packet ignored;while(client.poll(ignored)){}std::this_thread::sleep_for(std::chrono::milliseconds(5));}
        const auto plis=fixture.feedback->plis.load();
        const bool requested=client.requestVideoKeyframe();
        if(!requested) std::cerr<<"SR regression request failed error="<<client.error()<<" video="<<client.videoDiagnostics()<<" transport="<<client.diagnostics()<<std::endl;
        assert(requested);
        for(unsigned tick=0;tick<30 && fixture.feedback->plis==plis;++tick){fixture.pump();client.pump();std::this_thread::sleep_for(std::chrono::milliseconds(5));}
        assert(fixture.feedback->plis>plis && fixture.feedback->lastPliSsrc==1234);
        assert(client.videoDiagnostics().find("repairControlRejected=1")!=std::string::npos);
        client.close();
        std::cout<<"Local WebRTC NACK/RTX passed: deliberate authenticated media loss, RTCP NACK to sender, RTX SSRC/PT/OSN restore, frame continued without decoder generation reset\n";
    }
    stream::Client::cleanupRuntime();
    // Provider-style repair: NACK is negotiated, but retransmissions keep the
    // primary SSRC/PT/sequence and arrive beyond the former fixed 120ms window.
    for (const unsigned scenario : {0u, 1u, 2u}) {
        const bool hevc = scenario != 0, catchupStress = scenario == 2;
        Fixture fixture("4000,4000", true, hevc);
        fixture.advertiseRtx = false;
        fixture.feedback->useRtx = false;
        fixture.feedback->responseDelayMs = 140;
        stream::Client client;
        fog::video::Settings settings;
        settings.codec = hevc ? stream::VideoCodec::H265 : stream::VideoCodec::H264;
        assert(client.configureVideo(settings));
        assert(client.startForTesting("fixture", "abcde", "ws://127.0.0.1:" + std::to_string(fixture.server->port()) + "/ws/"));
        const auto start = Clock::now();
        auto nextMedia = start;
        unsigned frames = 0, audioFrames = 0, requestedLosses = 0;
        std::uint32_t previousTimestamp = 0;
        std::int64_t previousSequence = -1;
        while (Clock::now() - start < std::chrono::seconds(5)) {
            fixture.pump(); client.pump();
            if ((requestedLosses == 0 && frames >= 3) || (requestedLosses == 1 && frames >= 15)) {
                if (catchupStress && requestedLosses == 1) fixture.feedback->responseDelayMs = 180;
                fixture.feedback->dropNext = true; ++requestedLosses;
            }
            if (Clock::now() >= nextMedia) {
                fixture.sendMedia();
                nextMedia = Clock::now() + std::chrono::milliseconds(catchupStress && requestedLosses == 2 ? 16 : 20);
            }
            stream::Packet packet;
            while (client.poll(packet)) {
                if (packet.kind == stream::Kind::Video) {
                    assert(packet.videoGeneration == 0);
                    if (frames) assert(std::int32_t(packet.timestamp - previousTimestamp) > 0);
                    assert(packet.firstSequence > previousSequence);
                    previousTimestamp = packet.timestamp; previousSequence = packet.lastSequence;
                    ++frames;
                } else ++audioFrames;
            }
            assert(client.error().empty());
            if (requestedLosses == 2 && frames >= 27 && fixture.feedback->retransmits == 2) break;
            // Independent media and pump cadences avoid overfilling queues
            // and keep the scheduled reply independent of frame production.
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const auto diagnostic = client.videoDiagnostics();
        std::cout << "Delayed primary repair codec=" << (hevc ? "H265" : "H264")
                  << " catchupStress=" << catchupStress << " responseDelayMs=" << fixture.feedback->actualResponseDelayMs << " " << diagnostic << "\n";
        assert(frames >= 27 && audioFrames >= 27 && fixture.feedback->nacks > 0 && fixture.feedback->retransmits == 2);
        const auto expectedDelay = catchupStress ? 180u : 140u;
        assert(fixture.feedback->actualResponseDelayMs >= expectedDelay);
        assert(diagnostic.find("repairedPackets=2 ") != std::string::npos);
        assert(diagnostic.find("rtxRestored=0 ") != std::string::npos);
        assert(diagnostic.find("gaps=0 ") != std::string::npos && diagnostic.find("resync=0 ") != std::string::npos);
        const auto sampleAt = diagnostic.find("nackArrivalSamples=");
        assert(sampleAt != std::string::npos && std::stoull(diagnostic.substr(sampleAt + 19)) >= 2);
        const auto delayAt = diagnostic.find("nackFirstArrivalMaxMs=");
        assert(delayAt != std::string::npos && std::stoull(diagnostic.substr(delayAt + 22)) >= expectedDelay);
        if (catchupStress) {
            const auto yieldsAt = diagnostic.find("catchupYields=");
            assert(yieldsAt != std::string::npos && std::stoull(diagnostic.substr(yieldsAt + 14)) > 0);
            assert(diagnostic.find("queueResync=0 ") != std::string::npos);
        }
        const auto answer = fixture.peer->remoteDescription()->generateSdp();
        assert(answer.find("rtpmap:99 rtx/90000") == std::string::npos);
        assert(answer.find("a=rtcp-fb:98 nack") != std::string::npos);
        client.close();
        std::cout << "Delayed primary NACK repair passed: no RTX SDP, two authenticated same-SSRC/PT responses,"
                  << " measured latency, ordered assembled frame chain, catchupStress=" << catchupStress << "\n";
    }
    stream::Client::cleanupRuntime();
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
        assert(!client.ready() && !client.error().empty() && !client.retryableError());
        assert(client.error().find("private") == std::string::npos);
        client.close();
        std::cout << "Local transport error/cleanup passed with sanitized diagnostics\n";
    }
    stream::Client::cleanupRuntime();
    for (const bool playStation : {false, true}) {
        // Reuse one Client and rental hash, changing only its connection suffix.
        // Old callbacks must not contaminate the replacement RTC generation.
        stream::Client client;
        assert(!client.configureController(static_cast<stream::ControllerProfile>(-1)));
        assert(client.configureController(playStation ? stream::ControllerProfile::DualSense : stream::ControllerProfile::Xbox));
        for (unsigned attempt=0; attempt<2; ++attempt) {
            const std::string suffix = attempt == 0 ? "abcde" : "abcdf";
            Fixture fixture("4000,4000",true,false,suffix,false,false,playStation);
            assert(client.startForTesting("fixture",suffix,"ws://127.0.0.1:" + std::to_string(fixture.server->port()) + "/ws/"));
            stream::Rumble vibration;
            assert(!client.pollRumble(vibration)); // A new transport never exposes prior-generation feedback.
            assert(!client.configureController(playStation ? stream::ControllerProfile::Xbox : stream::ControllerProfile::DualSense));
            // Callback ping before sender publication must survive in a bounded
            // slot and transfer to the worker only after route authorization.
            client.receiveControlForTesting(R"({"id":"ping","ts":42})");
            assert(client.error().empty() && client.diagnostics().find("incomingPing=1 ") != std::string::npos);
            const auto begin = Clock::now();
            bool gotFrame = false;
            while (Clock::now()-begin < std::chrono::seconds(5)) {
                fixture.pump(); client.pump(); fixture.sendMedia();
                stream::Packet packet;
                while (client.poll(packet)) gotFrame |= packet.kind == stream::Kind::Video;
                assert(client.error().empty());
                if (gotFrame && client.ready() && fixture.controlReady && fixture.pong) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            assert(gotFrame && client.ready() && fixture.controlReady && fixture.pong);
            const auto rumbleAt = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
            fixture.dc->send(R"({"id":"gamepad_notification","index":0,"large_motor":255,"small_motor":128})");
            const auto feedbackAt = Clock::now();
            while (client.diagnostics().find("incomingRumble=1 ") == std::string::npos &&
                Clock::now() - feedbackAt < std::chrono::seconds(1)) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            assert(client.pollRumble(vibration)); // Actual authenticated SCTP callback, without a frame pump.
            assert(vibration.lowFrequency == 65535 && vibration.highFrequency == 32896 && vibration.durationMs == 5100);
            assert(vibration.receivedAtMs >= static_cast<std::uint64_t>(rumbleAt));
            assert(!client.pollRumble(vibration));
            std::string controlEvent;
            bool loggedRumble = false;
            while (client.pollControlDiagnostic(controlEvent, true)) {
                if (controlEvent.find("parsed=rumble") != std::string::npos) {
                    loggedRumble = true;
                    assert(controlEvent.find("large_motor:255") != std::string::npos);
                }
            }
            assert(loggedRumble);
            fixture.dc->send(rtc::binary(8, std::byte{0x7f}));
            const auto binaryAt = Clock::now();
            bool loggedBinary = false;
            while (!loggedBinary && Clock::now() - binaryAt < std::chrono::seconds(1)) {
                while (client.pollControlDiagnostic(controlEvent, true))
                    loggedBinary |= controlEvent.find("event=<binary> bytes=8") != std::string::npos;
                if (!loggedBinary) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            assert(loggedBinary && client.error().empty());
            for (unsigned n = 0; n < 1000; ++n)
                client.receiveControlForTesting(Json{{"id", "gamepad_notification"}, {"index", 0},
                    {"large_motor", n % 256}, {"small_motor", 1}}.dump());
            client.receiveControlForTesting(R"({"id":"gamepad_notification","index":0,"large_motor":0,"small_motor":0})");
            // Optional malformed packets neither terminate the stream nor replace a pending stop.
            for (const auto* malformed : {
                R"({"id":"gamepad_notification","index":0,"large_motor":1.0,"small_motor":1})",
                R"({"id":"gamepad_notification","index":1,"large_motor":255,"small_motor":255})",
                R"({"id":"gamepad_notification","index":0,"large_motor":256,"small_motor":0})"})
                client.receiveControlForTesting(malformed);
            assert(client.pollRumble(vibration) && vibration.lowFrequency == 0 && vibration.highFrequency == 0);
            assert(!client.pollRumble(vibration) && client.error().empty());
            assert(client.diagnostics().find("incomingRumble=1002 ") != std::string::npos);
            assert(client.diagnostics().find("incomingInvalidRumble=3 ") != std::string::npos);
            assert(client.diagnostics().find("eventOverflow=0") != std::string::npos);
            assert(client.inputDiagnostics().find("nativeDownQueued=0 ") != std::string::npos);
            const Json touch{{"id", "gamepad_touch"}, {"index", 0},
                {"data", {{"type", 0x656}, {"finger", 0}, {"x", 0.25}, {"y", 0.75}}}};
            if (playStation) {
                for (const auto invalid : {"index", "finger", "x", "type"}) {
                    auto bad = touch;
                    if (std::string(invalid) == "index") bad["index"] = 1;
                    else bad["data"][invalid] = std::string(invalid) == "x" ? -0.1 : 9;
                    assert(!client.sendControl(bad.dump()));
                }
                assert(client.sendControl(touch.dump()));
                auto move = touch; move["data"]["type"] = 0x657; move["data"]["x"] = 0.9;
                assert(client.sendControl(move.dump()));
                auto up = move; up["data"]["type"] = 0x658; assert(client.sendControl(up.dump()));
                for (const auto sensor : {1, 2}) {
                    const Json motion{{"id", "gamepad_sensor"}, {"index", 0},
                        {"data", {{"sensor", sensor}, {"x", 0.1}, {"y", 0.2}, {"z", 0.3}}}};
                    auto bad = motion; bad["data"]["sensor"] = 3; assert(!client.sendControl(bad.dump()));
                    assert(client.sendControl(motion.dump()));
                }
                const auto nativeAt = Clock::now();
                while (fixture.remoteControls.size() < 5 && Clock::now() - nativeAt < std::chrono::seconds(1)) {
                    fixture.pump(); client.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                assert(fixture.remoteControls.size() == 5 && fixture.remoteControls[1]["data"]["x"] == 0.9);
                const auto native = client.inputDiagnostics();
                assert(native.find("nativeDownSent=1 ") != std::string::npos && native.find("nativeMoveSent=1 ") != std::string::npos);
                assert(native.find("nativeUpSent=1 ") != std::string::npos && native.find("gyroSent=1 ") != std::string::npos);
                assert(native.find("invalidNative=6 ") != std::string::npos);
            } else assert(!client.sendControl(touch.dump()));
            assert(client.diagnostics().find("incomingIgnored=0 ") != std::string::npos);
            assert(!client.configureController(playStation ? stream::ControllerProfile::Xbox : stream::ControllerProfile::DualSense));
            stream::Gamepad pad; pad.buttons[0] = 1; pad.buttons[17] = 1;
            pad.axes[0] = 0.5f; pad.leftTrigger = 0.25f; pad.rightTrigger = 0.75f;
            assert(!client.sendControl(stream::protocol::gamepad(pad, !playStation).dump()));
            auto missingMode = stream::protocol::gamepad(pad, playStation);
            missingMode.erase("isPS");
            assert(!client.sendControl(missingMode.dump()));
            assert(client.sendGamepad(pad));
            const auto pressedAt = Clock::now();
            while (!fixture.pressed && Clock::now() - pressedAt < std::chrono::seconds(1)) {
                fixture.pump(); client.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            assert(fixture.pressed);
            if (playStation) {
                const auto input = client.inputDiagnostics();
                assert(input.find("touchQueuedDown=1 ") != std::string::npos);
                assert(input.find("touchSentDown=1 ") != std::string::npos);
                assert(input.find("ack=unsupported") != std::string::npos);
            }
            if (attempt == 0) {
                assert(client.sendGamepad(stream::Gamepad{}));
                const auto neutralAt = Clock::now();
                while (!fixture.neutral && Clock::now() - neutralAt < std::chrono::seconds(1)) {
                    fixture.pump(); client.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                assert(fixture.neutral);
                client.receiveControlForTesting(R"({"id":"gamepad_notification","index":0,"large_motor":255,"small_motor":0})");
                assert(client.disconnectForRetry());
                assert(!client.pollRumble(vibration));
                assert(client.error().empty() && !client.ready() && !client.disconnected());
            } else {
                client.receiveControlForTesting(R"({"id":"gamepad_notification","index":0,"large_motor":255,"small_motor":0})");
                client.receiveControlForTesting(R"({"id":"close"})");
                assert(!client.pollRumble(vibration)); // Terminal callback seals feedback before pump.
                client.receiveControlForTesting(R"({"id":"gamepad_notification","index":0,"large_motor":255,"small_motor":0})");
                assert(!client.pollRumble(vibration));
                client.pump();
                assert(!client.error().empty());
                // Failed transports still own their final neutral until close.
                assert(!client.configureController(playStation ? stream::ControllerProfile::Xbox : stream::ControllerProfile::DualSense));
                client.close();
                assert(!client.pollRumble(vibration));
            }
            for (unsigned tick=0; tick<20; ++tick) {
                fixture.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            assert(fixture.closed == (attempt == 1));
            assert(fixture.neutral); // Final close releases with the negotiated controller type.
            if (playStation) {
                const auto input = client.inputDiagnostics();
                assert(input.find("touchSentDown=1 ") != std::string::npos);
                assert(input.find("touchSentUp=1 ") != std::string::npos);
                assert(input.find("touchFailedDown=0 ") != std::string::npos);
                const std::string sensorSent = attempt == 0 ? "1 " : "2 ";
                assert(input.find("accelSent=" + sensorSent) != std::string::npos && input.find("gyroSent=" + sensorSent) != std::string::npos);
            }
        }
    }
    stream::Client::cleanupRuntime();
    std::cout << "Both controller modes: same Client reinit, mode lock, matching init/input/neutral, retry no-close/final-close passed\n";
    {
        stream::Client client;
        assert(client.configureController(stream::ControllerProfile::DualSense));
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            const std::string suffix = attempt == 0 ? "abcde" : "abcdf";
            Fixture fixture("4000,4000", true, false, suffix, false, false, true);
            assert(client.startForTesting("fixture", suffix,
                "ws://127.0.0.1:" + std::to_string(fixture.server->port()) + "/ws/"));
            const auto begin = Clock::now();
            while (Clock::now() - begin < std::chrono::seconds(5)) {
                fixture.pump(); client.pump(); fixture.sendMedia();
                stream::Packet packet; while (client.poll(packet)) {}
                assert(client.error().empty());
                if (client.ready() && fixture.controlReady && fixture.pong) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            assert(client.ready() && fixture.controlReady && fixture.pong && fixture.remoteControls.empty());
            // Application transport startup creates a new NativeInput, even
            // when the same Client reconnects while a finger remains held.
            fog::playstation::NativeInput native;
            fog::playstation::PsSensorSnapshot sample;
            sample.enabled = sample.touchValid = sample.touchFresh = true;
            sample.touchCount = 1;
            sample.touches[0] = {true, 42, 1279, 719, 0, 0};
            sample.motion.valid = true;
            sample.motion.sampleNumber = 1;
            sample.motion.psAccelerationG = {0, 1, 0};
            sample.motion.psGyroRadiansPerSecond = {1, 2, 3};
            const auto emit = [&](const fog::remote::Input& command) { return client.sendInput(command); };
            // Both initial UP commands must survive the real outbox without
            // any preceding DOWN, before held touch and motion are forwarded.
            client.blockControlSendForTesting(true);
            native.update(sample, 100, true, emit);
            const Json expected = Json::array({
                stream::protocol::remoteInput(fog::playstation::NativeInput::touch(0x658, 0, 0, 0)),
                stream::protocol::remoteInput(fog::playstation::NativeInput::touch(0x658, 1, 0, 0)),
                stream::protocol::remoteInput(fog::playstation::NativeInput::touch(0x656, 0, 1, 1)),
                stream::protocol::remoteInput(fog::playstation::NativeInput::sensor(1, {0, fog::playstation::NativeInput::Gravity, 0})),
                stream::protocol::remoteInput(fog::playstation::NativeInput::sensor(2, {1, 2, 3}))});
            assert(native.contactsInitialized() && native.counters().resetUpQueued == 2 &&
                native.counters().up == 0 && native.counters().down == 1 && native.counters().rejected == 0);
            native.update(sample, 140, true, emit);
            assert(native.counters().resetUpQueued == 2 && native.counters().up == 0 && native.counters().down == 1 &&
                native.counters().accel == 1 && native.counters().gyro == 1);
            client.blockControlSendForTesting(false);
            const auto inputAt = Clock::now();
            while (fixture.remoteControls.size() < expected.size() &&
                   Clock::now() - inputAt < std::chrono::seconds(1)) {
                fixture.pump(); client.pump();
                assert(client.error().empty());
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            assert(fixture.remoteControls == expected);
            const auto input = client.inputDiagnostics();
            assert(input.find("nativeUpSent=2 ") != std::string::npos &&
                input.find("nativeDownSent=1 ") != std::string::npos &&
                input.find("accelSent=1 ") != std::string::npos && input.find("gyroSent=1 ") != std::string::npos);
            if (attempt == 0) assert(client.disconnectForRetry());
            else client.close();
        }
    }
    stream::Client::cleanupRuntime();
    std::cout << "PS native transport reset: two initial UPs before held DOWN/sensors, once per generation, same Client reconnect passed\n";
    // Failure before any control channel exists must retain the ICE owner too.
    {
        Fixture fixture("4000,4000", false);
        stream::Client client;
        assert(client.startForTesting("fixture", "abcde", "ws://127.0.0.1:" + std::to_string(fixture.server->port()) + "/ws/"));
        const auto begin = Clock::now();
        while (!fixture.enabled) {
            fixture.pump(); client.pump();
            assert(Clock::now() - begin < std::chrono::seconds(2));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        client.blockIceForTesting(true);
        fixture.signal({{"id", "init"}, {"data", Json::array({"[]"}).dump()}});
        // Do not pump provider events: no offer/data channel can be delivered.
        while (client.diagnostics().find("iceInFlight=1 ") == std::string::npos) {
            client.pump(); assert(client.error().empty());
            assert(Clock::now() - begin < std::chrono::seconds(2));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto failedAt = Clock::now();
        client.failThenReceiveForTesting("{}", false); client.pump();
        assert(Clock::now() - failedAt < std::chrono::milliseconds(200));
        assert(client.error() == "ICE snapshot in flight; ending rental before transport cleanup" && !client.retryableError());
        assert(!client.disconnectForRetry());
        client.blockIceForTesting(false); client.close();
    }
    stream::Client::cleanupRuntime();
    // Block each application-facing RTC read stage. Failure/retry must expose
    // terminal state without joining, so the app can send rental HTTP stop first.
    for (unsigned stage : {0u, 1u}) for (unsigned failureMode : {0u, 1u, 2u}) {
        Fixture fixture;
        stream::Client client;
        assert(client.startForTesting("fixture", "abcde", "ws://127.0.0.1:" + std::to_string(fixture.server->port()) + "/ws/"));
        const auto begin = Clock::now();
        while (!client.ready() && Clock::now() - begin < std::chrono::seconds(4)) {
            fixture.pump(); client.pump(); fixture.sendMedia();
            stream::Packet packet; while (client.poll(packet)) {}
            assert(client.error().empty());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        assert(client.ready());
        client.blockIceForTesting(true, stage); client.pump();
        const auto blockedAt = Clock::now();
        while (client.diagnostics().find("iceInFlight=1 ") == std::string::npos) {
            assert(Clock::now() - blockedAt < std::chrono::seconds(2));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto failedAt = Clock::now();
        if (failureMode) {
            client.failThenReceiveForTesting(failureMode == 2 ? "{\"id\":\"close\"}" : "{}", false);
            client.pump();
        }
        assert(!client.disconnectForRetry());
        assert(Clock::now() - failedAt < std::chrono::milliseconds(200));
        assert(!client.ready() && !client.retryableError());
        const std::string expectedError = failureMode == 2 ? "Remote control closed" :
            "ICE snapshot in flight; ending rental before transport cleanup";
        if (client.error() != expectedError)
            std::cerr << "Blocked ICE assertion stage=" << stage << " failureMode=" << failureMode <<
                " actual=" << client.error() << " " << client.rtcpDiagnostics() << '\n';
        assert(client.error() == expectedError);
        const auto frozen = client.diagnostics();
        for (unsigned n = 0; n < 1000; ++n) client.receiveControlForTesting("{}");
        const auto after = client.diagnostics();
        assert(after.substr(after.find("incomingIgnored=")) == frozen.substr(frozen.find("incomingIgnored=")));
        // This boundary models the controller's already-tested HTTP dispatch.
        // The transport worker remains owned and joinable until explicit close.
        bool rentalStopDispatched = true;
        assert(rentalStopDispatched && client.diagnostics().find("iceInFlight=1 ") != std::string::npos);
        client.blockIceForTesting(false); client.close();
        assert(client.diagnostics().find("iceStopping=1") != std::string::npos);
    }
    stream::Client::cleanupRuntime();
    std::cout << "Blocked diagnostic/pair read: retry/failure terminal publication, terminal-control precedence and HTTP-before-join ownership passed\n";
    // A transport event or retry request while a control send is blocked must
    // publish a terminal error without joining. The application then dispatches
    // rental termination before close() owns the unavoidable transport join.
    for (bool transportFailure : {false, true}) {
        Fixture fixture;
        stream::Client client;
        assert(client.startForTesting("fixture", "abcde", "ws://127.0.0.1:" + std::to_string(fixture.server->port()) + "/ws/"));
        const auto begin = Clock::now();
        while (!client.ready() && Clock::now() - begin < std::chrono::seconds(4)) {
            fixture.pump(); client.pump(); fixture.sendMedia();
            stream::Packet packet; while (client.poll(packet)) {}
            assert(client.error().empty());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        assert(client.ready());
        client.blockControlSendForTesting(true);
        stream::Gamepad pad; pad.buttons[0] = 1;
        assert(client.sendGamepad(pad));
        const auto blockedAt = Clock::now();
        while (client.diagnostics().find("controlInFlight=1 ") == std::string::npos && Clock::now() - blockedAt < std::chrono::seconds(2))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        assert(client.diagnostics().find("controlInFlight=1 ") != std::string::npos);
        assert(client.sendInput(fog::remote::key(true, "Enter")));
        const auto failedAt = Clock::now();
        if (transportFailure) {
            client.failThenReceiveForTesting("{}", false); client.pump();
            assert(!client.retryableError() && !client.error().empty());
        }
        assert(!client.disconnectForRetry());
        assert(Clock::now() - failedAt < std::chrono::milliseconds(200));
        assert(!client.ready() && !client.retryableError());
        const auto frozen = client.diagnostics();
        for (unsigned n = 0; n < 1000; ++n) client.receiveControlForTesting("{}");
        const auto after = client.diagnostics();
        assert(after.find("incomingIgnored=") != std::string::npos);
        assert(after.substr(after.find("incomingIgnored=")) == frozen.substr(frozen.find("incomingIgnored=")));
        client.blockControlSendForTesting(false);
        client.close();
        for (unsigned n = 0; n < 20; ++n) { fixture.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        assert(fixture.closed && fixture.remoteControls.empty()); // Queued old-generation Enter never drains.
    }
    stream::Client::cleanupRuntime();
    std::cout << "Blocked sender failure/retry publishes terminal error without join; frozen callbacks and old queued inputs discarded\n";
    {
        // The remote DTLS close can make PeerConnection release ICE before the
        // application processes its queued callbacks. Preserve the RTC snapshot.
        Fixture fixture;
        stream::Client client;
        assert(client.startForTesting("fixture", "abcde", "ws://127.0.0.1:" + std::to_string(fixture.server->port()) + "/ws/"));
        const auto begin = Clock::now();
        while (!client.ready() && Clock::now() - begin < std::chrono::seconds(4)) {
            fixture.pump(); client.pump(); fixture.sendMedia();
            stream::Packet packet; while (client.poll(packet)) {}
            assert(client.error().empty());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        assert(client.ready());
        fixture.peer->close();
        // Let the RTC's own close task run before Client asks for its snapshot.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto closedAt = Clock::now();
        while (client.error().empty() && Clock::now() - closedAt < std::chrono::seconds(2)) {
            fixture.pump(); client.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        assert(!client.error().empty() && !client.ready());
        const auto final = client.diagnostics();
        assert(final.find("requested=all actual=direct") != std::string::npos);
        assert(final.find("routeSnapshot=final") != std::string::npos);
        assert(final.find("iceSnapshotAvailable=1") != std::string::npos);
        assert(final.find("iceReason=") != std::string::npos && final.find("udpDatagrams=") != std::string::npos);
        assert(final.find("127.0.0.1") == std::string::npos);
    }
    stream::Client::cleanupRuntime();
    std::cout << "Automatic remote peer close retains final safe ICE diagnostics\n";
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
            assert(sent && !client.error().empty() && !client.retryableError());
            assert(client.error().find(invalidBase64 ? "candidate.base64" : "candidate.json") != std::string::npos);
            if (!invalidBase64) assert(client.error().find("JSON error id=101") != std::string::npos);
            assert(client.error().find("private") == std::string::npos);
            assert(client.diagnostics().find("private") == std::string::npos);
            client.close();
        }
        stream::Client::cleanupRuntime();
    }
    std::cout << "Empty provider offer/ICE markers and safe failure phase diagnostics passed\n";
    // A retryable transport error must not hide terminal evidence later in the batch.
    for (unsigned test=0; test<11; ++test) {
        {
            Fixture fixture("0,4000", false);
            stream::Client client;
            assert(client.startForTesting("fixture", "abcde", "ws://127.0.0.1:" + std::to_string(fixture.server->port()) + "/ws/"));
            const auto begin = Clock::now();
            while (Clock::now()-begin < std::chrono::seconds(2) && !fixture.enabled) {
                fixture.pump(); client.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            assert(fixture.enabled && client.error().empty());
            if (test == 0) client.failThenReceiveForTesting(R"({"id":"close"})",false);
            else if (test == 1) client.failThenReceiveForTesting(R"({"id":"error"})",false);
            else if (test == 2) client.failThenReceiveForTesting("{private-malformed",false);
            else if (test == 3) client.failThenReceiveForTesting(stream::protocol::frame("MESSAGE",{},R"({"id":"close"})"),true);
            else if (test == 4) client.failThenReceiveForTesting(stream::protocol::frame("MESSAGE",{},"{private-malformed"),true);
            else if (test == 5) client.failThenReceiveForTesting(stream::protocol::frame("ERROR",{},"private"),true);
            else if (test == 6) {
                for (unsigned n=0; n<129; ++n) client.failThenReceiveForTesting("{}",false);
            } else if (test == 7) client.failThenReceiveForTesting("{}",false);
            else {
                // Queue SIGNAL ping followed by a terminal frame in ONE WsData.
                // Close the socket first to force ping's send failure.
                fixture.ws->close();
                const auto ping = stream::protocol::frame("MESSAGE",{},R"({"id":"ping"})");
                const auto fatal = test == 8 ? stream::protocol::frame("ERROR",{},"private") :
                    stream::protocol::frame("MESSAGE",{},test == 9 ? R"({"id":"close"})" : "{private-malformed");
                client.failThenReceiveForTesting(ping+fatal,true);
            }
            if (test < 8) assert(client.disconnectForRetry() == (test == 7));
            else client.pump();
            assert(!client.error().empty() && !client.ready());
            assert(client.retryableError() == (test == 7));
            assert(client.error().find("private") == std::string::npos);
            client.close();
        }
        stream::Client::cleanupRuntime();
    }
    std::cout << "Transport-failure-before-terminal-control/signaling/queue-overflow precedence passed\n";
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
                assert(!client.error().empty() && !client.retryableError());
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
            if (!malformed) {
                assert(client.retryableError());
                for (unsigned tick=0; tick<10; ++tick) {
                    fixture.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                assert(!fixture.closed);
            }
            client.close();
        }
        stream::Client::cleanupRuntime();
    }
    std::cout << "STOMP negotiated liveness, malformed heartbeat rejection, and bounded setup passed\n";
}
