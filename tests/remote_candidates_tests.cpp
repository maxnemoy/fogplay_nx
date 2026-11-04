#include "features/streaming/infrastructure/remote_candidates.hpp"
#include <cassert>
#include <iostream>
#include <string>

namespace {
std::string offer(const std::string& candidates, bool ended) {
    return "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\na=group:BUNDLE 0\r\n"
           "a=ice-ufrag:abcd\r\na=ice-pwd:0123456789abcdefghijkl\r\n"
           "m=video 9 UDP/TLS/RTP/SAVPF 96\r\nc=IN IP4 0.0.0.0\r\na=mid:0\r\na=recvonly\r\n"
           "a=rtpmap:96 H264/90000\r\n" + candidates + (ended ? "a=end-of-candidates\r\n" : "");
}
const std::string hostname = "a=candidate:1 1 UDP 2122252543 fixture-peer.local 50000 typ host\r\n";
const std::string ipv4 = "a=candidate:2 1 UDP 2122252542 192.0.2.10 50001 typ host\r\n";
const std::string ipv6 = "a=candidate:3 1 UDP 1686052607 2001:db8::1 50002 typ srflx raddr 0.0.0.0 rport 0\r\n";
const std::string tcp = "a=candidate:4 1 TCP 1518280447 192.0.2.11 9 typ host tcptype active\r\n";
}

int main() {
    using stream::detail::dropUnresolvableCandidates;
    {
        // Mixed embedded candidates: only the hostname one is removed; the
        // numeric ones keep their mid and end-of-candidates is preserved.
        rtc::Description mixed(offer(hostname + ipv4 + ipv6 + tcp, true), "offer");
        assert(mixed.candidates().size() == 4 && mixed.ended());
        assert(dropUnresolvableCandidates(mixed) == 1);
        const auto kept = mixed.candidates();
        assert(kept.size() == 3 && mixed.ended());
        for (const auto& candidate : kept) {
            assert(candidate.isResolved() && candidate.mid() == "0");
            assert(std::string(candidate).find(".local") == std::string::npos);
        }
        assert(std::string(mixed).find("fixture-peer.local") == std::string::npos);
    }
    {
        // Hostname-only: nothing remains to throw in setRemoteDescription();
        // the retryable connection deadline handles the absent route.
        rtc::Description only(offer(hostname, false), "offer");
        assert(dropUnresolvableCandidates(only) == 1);
        assert(only.candidates().empty() && !only.ended());
    }
    {
        // No embedded candidates (trickle ICE): unchanged.
        rtc::Description none(offer("", false), "offer");
        assert(dropUnresolvableCandidates(none) == 0 && none.candidates().empty());
    }
    {
        // Single trickled candidates use the same predicate.
        rtc::Candidate name(hostname.substr(2, hostname.size() - 4), "0");
        rtc::Candidate number(ipv4.substr(2, ipv4.size() - 4), "0");
        assert(!stream::detail::resolvableWithoutLookup(name));
        assert(stream::detail::resolvableWithoutLookup(number) && number.isResolved());
    }
    std::cout << "Remote candidates: embedded hostname dropped, numeric IPv4/IPv6/TCP kept with mid and end marker, hostname-only and trickle PASS\n";
}
