#include "features/streaming/infrastructure/stream_protocol.hpp"
#include <cassert>
#include <iostream>
#include <string>
using stream::VideoCodec;
using namespace stream::protocol;
static std::string offer(const std::string& fmtp = {}) {
    return "v=0\r\ns=-\r\nt=0 0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96 98 99\r\n"
        "a=mid:v\r\na=sendonly\r\na=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 packetization-mode=1\r\na=rtpmap:98 H265/90000\r\n"
        "a=rtpmap:99 rtx/90000\r\na=fmtp:99 apt=98\r\na=rtcp-fb:* nack pli\r\n" +
        (fmtp.empty() ? "" : "a=fmtp:98 " + fmtp + "\r\n");
}
static bool rejects(const std::string& sdp) {
    try { (void)offeredVideoCodec(sdp); return false; } catch (const Error&) { return true; }
}
int main() {
    const auto defaulted=filterOffer(offer());
    assert(offeredVideoCodec(defaulted)==VideoCodec::H264 && defaulted.find("rtpmap:98")==std::string::npos);
    for (const std::string fmtp : {"", "profile-id=1;sprop-max-don-diff=0;tx-mode=SRST", "Profile-ID = 1 ; sprop-max-don-diff = 0"}) {
        const auto selected=filterOffer(offer(fmtp),VideoCodec::H265);
        assert(offeredVideoCodec(selected)==VideoCodec::H265 && selected.find("rtpmap:96")==std::string::npos);
        assert(selected.find("rtpmap:99")==std::string::npos && selected.find("rtcp-fb:98 nack pli")!=std::string::npos);
    }
    for (const std::string fmtp : {"sprop-max-don-diff=1", "sprop-max-don-diff=65535", "tx-mode=MST", "tx-mode=MSST",
                                  "sprop-depack-buf-nalus=1", "profile-id=2", "profile-space=1", "packetization-mode=1",
                                  "sprop-max-don-diff=0;sprop-max-don-diff=1", "sprop-max-don-diff=", "sprop-max-don-diff"}) {
        assert(offeredVideoCodec(filterOffer(offer(fmtp),VideoCodec::H265))==VideoCodec::H264);
        auto onlyHevc=offer(fmtp); onlyHevc.replace(onlyHevc.find("m=video 9 UDP/TLS/RTP/SAVPF 96 98 99"),std::string("m=video 9 UDP/TLS/RTP/SAVPF 96 98 99").size(),"m=video 9 UDP/TLS/RTP/SAVPF 98");
        bool threw=false; try { (void)filterOffer(onlyHevc,VideoCodec::H265); } catch (const Error&) { threw=true; }
        assert(threw);
    }
    auto tabs=offer("sprop-max-don-diff=1"); tabs.replace(tabs.find("a=fmtp:98 "),10,"a=fmtp:98\t");
    assert(offeredVideoCodec(filterOffer(tabs,VideoCodec::H265))==VideoCodec::H264);
    const auto h264Track="m=video 9 UDP/TLS/RTP/SAVPF 100\r\na=mid:second\r\na=sendonly\r\na=rtpmap:100 H264/90000\r\n";
    assert(offeredVideoCodec(filterOffer(offer()+h264Track,VideoCodec::H265))==VideoCodec::H264);
    assert(rejects(offer())); // Unfiltered mixed codecs cannot be treated as selected.
    assert(rejects("v=0\r\nm=video 0 UDP/TLS/RTP/SAVPF 98\r\na=rtpmap:98 H265/90000\r\n"));
    assert(rejects("m=video 9 UDP/TLS/RTP/SAVPF 98\r\na=rtpmap:98 H265/90000\r\na=fmtp:98 sprop-max-don-diff=1\r\n"));
    std::cout<<"HEVC SDP tests passed: H264 default, HEVC preference, unsupported-mode fallback, global track consistency, strict selected-codec validation\n";
}
