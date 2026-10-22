#include "features/streaming/infrastructure/stream_protocol.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#ifdef STREAM_RTC_TESTS
#include <rtc/rtc.hpp>
#include "features/streaming/infrastructure/stream_route.hpp"
#endif

namespace {
template<class F> void rejects(F fn) {
    bool rejected = false;
    try { fn(); } catch (const std::exception&) { rejected = true; }
    assert(rejected);
}
const std::string Offer =
    "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n"
    "a=group:BUNDLE cam sound control\r\na=ice-ufrag:local\r\na=ice-pwd:local-password-01234567\r\n"
    "a=fingerprint:sha-256 00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF\r\n"
    "a=setup:actpass\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 120 98 121 122\r\nc=IN IP4 0.0.0.0\r\n"
    "a=mid:cam\r\na=sendonly\r\na=rtcp-mux\r\n"
    "a=rtpmap:120 VP8/90000\r\na=rtpmap:98 H264/90000\r\na=rtpmap:121 rtx/90000\r\n"
    "a=rtpmap:122 H264/90000\r\na=fmtp:122 packetization-mode=2\r\n"
    "a=fmtp:98 packetization-mode=1;profile-level-id=42e01f\r\na=rtcp-fb:98 nack pli\r\n"
    "a=fmtp:121 apt=120\r\na=rtcp-fb:120 nack\r\na=rtcp-fb:* transport-cc\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 103 0\r\nc=IN IP4 0.0.0.0\r\na=mid:sound\r\na=sendonly\r\na=rtcp-mux\r\n"
    "a=rtpmap:103 opus/48000/2\r\na=fmtp:103 minptime=10;useinbandfec=1\r\na=rtpmap:0 PCMU/8000\r\n"
    "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\nc=IN IP4 0.0.0.0\r\na=mid:control\r\na=sctp-port:5000\r\n";
}

int main() {
    using namespace stream::protocol;
    Parser parser;
    const std::string body("hi\0there", 8);
    const auto encoded = frame("MESSAGE", {{"destination", "colon:back\\line\n"}}, body);
    assert(parser.feed("\r").empty());
    assert(parser.feed("\n\n" + encoded.substr(0, 15)).empty());
    auto frames = parser.feed(encoded.substr(15) + frame("CONNECTED", {{"version", "1.2"}}));
    assert(frames.size() == 2 && frames[0].body == body);
    assert(frames[0].headers.at("destination") == "colon:back\\line\n");
    assert(frames[1].command == "CONNECTED");
    Parser malformed;
    rejects([&] { malformed.feed("MESSAGE\ncontent-length:-1\n\nx\0"); });
    Parser badEscape;
    rejects([&] { badEscape.feed(std::string("MESSAGE\na:b\\x\n\n\0", 17)); });
    Parser tooLarge;
    rejects([&] { tooLarge.feed(std::string(256 * 1024 + 1, 'x')); });
    Parser missingTerminator;
    rejects([&] { missingTerminator.feed("MESSAGE\ncontent-length:2\n\nxxx"); });
    Parser separate;
    const auto simple = frame("MESSAGE", {}, "payload");
    std::vector<Frame> got;
    for (char c : simple) {
        auto part = separate.feed(std::string(1, c));
        got.insert(got.end(), part.begin(), part.end());
    }
    assert(got.size() == 1 && got[0].body == "payload");
    for (const auto& bytes : {std::string(), std::string("M"), std::string("Ma"), std::string("Man"), body})
        assert(base64Decode(base64Encode(bytes)) == bytes);
    for (const std::string bad : {"A", "!!!!", "A===", "AA=A", "AB==", "AAF=", "AAAA===="})
        rejects([&] { base64Decode(bad); });
    stream::Gamepad pad;
    pad.buttons[0] = 100; pad.buttons[17] = -1;
    pad.axes = {2, -2, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()};
    pad.rightTrigger = 2; pad.leftTrigger = -1;
    const auto g = gamepad(pad);
    assert(g["id"] == "gamepad" && g["index"] == 0 && g["isPS"] == false);
    assert(g["state"]["buttons"].size() == 18 && g["state"]["axes"].size() == 4);
    assert(g["state"]["buttons"][0] == 1 && g["state"]["buttons"][17] == 1);
    assert(g["state"]["axes"] == Json::array({1.0f, -1.0f, 0.0f, 0.0f}));
    assert(g["state"]["rightTrigger"] == 1.0f && g["state"]["leftTrigger"] == 0.0f);
    const auto neutral = gamepad(stream::Gamepad{});
    for (const auto& b : neutral["state"]["buttons"]) assert(b == 0);
    for (const auto& a : neutral["state"]["axes"]) assert(a == 0);
    const Json servers = Json::array({{{"urls", Json::array({"turn:host:3478", "turns:host:5349"})},
                                      {"username", "u"}, {"credential", "p"}}, {{"urls", "stun:host:3478"}}});
    const Json init{{"id", "init"}, {"data", Json::array({servers.dump(), "game"}).dump()}};
    const auto ice = iceServers(init);
    assert(ice.size() == 3 && ice[0].username == "u" && ice[1].credential == "p" && ice[2].username.empty());
    rejects([] { iceServers(Json{{"data", "[]"}}); });
    rejects([] { iceServers(Json{{"data", "[{}]"}}); });
    rejects([] { iceServers(Json{{"data", "[\"[{\\\"urls\\\":\\\"https://host\\\"}]\"]"}}); });
    const auto filtered = filterOffer(Offer);
    assert(filtered.find("m=video 9 UDP/TLS/RTP/SAVPF 98\r\n") != std::string::npos);
    assert(filtered.find("m=audio 9 UDP/TLS/RTP/SAVPF 103\r\n") != std::string::npos);
    assert(filtered.find("a=fmtp:98 packetization-mode=1;profile-level-id=42e01f") != std::string::npos);
    assert(filtered.find("a=rtcp-fb:98 nack pli") != std::string::npos);
    assert(filtered.find("a=rtcp-fb:98 transport-cc") != std::string::npos);
    assert(filtered.find("a=rtcp-fb:*") == std::string::npos);
    assert(filtered.find("rtpmap:120") == std::string::npos && filtered.find("fmtp:121") == std::string::npos);
    assert(filtered.find("rtpmap:122") == std::string::npos && filtered.find("rtpmap:0 ") == std::string::npos);
    assert(filtered.find("a=sctp-port:5000") != std::string::npos);
    assert(mediaMids(filtered) == std::vector<std::string>({"cam", "sound", "control"}));
    auto unsupported = Offer;
    unsupported.replace(unsupported.find("98 H264"), 7, "98 VP9 ");
    rejects([&] { filterOffer(unsupported); });
    auto cannotSend = Offer;
    cannotSend.replace(cannotSend.find("a=sendonly"), 10, "a=recvonly");
    rejects([&] { filterOffer(cannotSend); });
#ifdef STREAM_RTC_TESTS
    using Type = rtc::Candidate::Type;
    using Transport = rtc::Candidate::TransportType;
    assert(std::string(stream::SelectedRoute{}.actual()) == "unknown");
    assert(std::string(stream::SelectedRoute{Type::Host, Type::Unknown}.actual()) == "unknown");
    for (const auto local : {Type::Host, Type::ServerReflexive, Type::PeerReflexive})
        for (const auto remote : {Type::Host, Type::ServerReflexive})
            assert(std::string(stream::SelectedRoute{local, remote}.actual()) == "direct");
    assert(std::string(stream::SelectedRoute{Type::Relayed, Type::Host}.actual()) == "turn");
    assert(std::string(stream::SelectedRoute{Type::Host, Type::Relayed}.actual()) == "turn");
    assert(std::string(stream::SelectedRoute{Type::Unknown, Type::Relayed}.actual()) == "turn");
    assert(std::string(stream::SelectedRoute::protocol(Transport::TcpActive)) == "tcp-active");
    assert(std::string(stream::SelectedRoute::protocol(static_cast<Transport>(999))) == "unknown");
    assert(std::string(stream::SelectedRoute::type(static_cast<Type>(999))) == "unknown");
    assert(std::string(stream::SelectedRoute{static_cast<Type>(999), Type::Host}.actual()) == "unknown");
    stream::CandidateProofs proofs;
    rtc::Candidate selected("prflx 1 UDP 1 192.0.2.4 50000 typ prflx");
    assert(selected.resolve(rtc::Candidate::ResolveMode::Simple));
    stream::SelectedRoute delayed{Type::Host, Type::PeerReflexive, Transport::Udp, Transport::Udp};
    assert(std::string(delayed.actual()) == "unknown");
    proofs.add(rtc::Candidate("decoy 1 UDP 1 192.0.2.5 50000 typ relay"));
    proofs.add(rtc::Candidate("decoy 1 UDP 1 192.0.2.4 50001 typ relay"));
    proofs.add(rtc::Candidate("decoy 1 TCP 1 192.0.2.4 50000 typ relay tcptype passive"));
    delayed.remoteProof = proofs.find(selected);
    assert(std::string(delayed.actual()) == "unknown");
    proofs.add(rtc::Candidate("different-foundation 1 UDP 1 192.0.2.4 50000 typ relay"));
    delayed.remoteProof = proofs.find(selected);
    assert(delayed.isTurn() && std::string(delayed.actual()) == "turn");
    stream::CandidateProofs directProofs;
    directProofs.add(rtc::Candidate("host 1 UDP 1 192.0.2.4 50000 typ host"));
    delayed.remoteProof = directProofs.find(selected); assert(delayed.isDirect());
    proofs.add(rtc::Candidate("dns 1 UDP 1 forbidden.invalid 50000 typ relay"));
    assert(proofs.size() == 4); // Numeric resolution only; no network lookup.
    for (unsigned n = 0; n < 100; ++n)
        proofs.add(rtc::Candidate("bounded 1 UDP 1 192.0.2.4 " + std::to_string(51000 + n) + " typ relay"));
    assert(proofs.size() == stream::CandidateProofs::Limit && proofs.dropped() > 0);
    // Validate the generated SDP with the pinned library parser, not only string matching.
    rtc::Description desc(filtered, "offer");
    assert(desc.mediaCount() == 3);
    const auto video = std::get<rtc::Description::Media*>(desc.media(0));
    assert(video->payloadTypes() == std::vector<int>({98}));
    const auto audio = std::get<rtc::Description::Media*>(desc.media(1));
    assert(audio->payloadTypes() == std::vector<int>({103}));
    assert(video->reciprocate().direction() == rtc::Description::Direction::RecvOnly);
    // Smoke-check actual transport linkage/lifetime without opening a network connection.
    stream::Client client;
    assert(!client.start("bad\nhash", "abcde", "unused.pem"));
    assert(!client.ready() && !client.error().empty());
    client.close(); client.close();
    stream::Client::cleanupRuntime();
#endif
    std::cout << "Stream protocol tests passed (STOMP/base64/ICE/gamepad/SDP";
#ifdef STREAM_RTC_TESTS
    std::cout << "/pinned RTC parser + transport linkage";
#endif
    std::cout << ")\n";
}
