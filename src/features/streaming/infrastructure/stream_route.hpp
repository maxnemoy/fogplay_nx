#pragma once
#include <rtc/candidate.hpp>
#include <string>
#include <vector>

namespace stream {
// Only enum values enter diagnostics; remote candidate text/addresses never do.
struct SelectedRoute {
    enum class Proof { Unknown, Direct, Relay };
    rtc::Candidate::Type local = rtc::Candidate::Type::Unknown;
    rtc::Candidate::Type remote = rtc::Candidate::Type::Unknown;
    rtc::Candidate::TransportType localProtocol = rtc::Candidate::TransportType::Unknown;
    rtc::Candidate::TransportType remoteProtocol = rtc::Candidate::TransportType::Unknown;
    Proof remoteProof = Proof::Unknown;
    static const char* proofName(Proof value) {
        switch (value) {
        case Proof::Direct: return "direct-endpoint";
        case Proof::Relay: return "relay-endpoint";
        default: return "none";
        }
    }
    static const char* type(rtc::Candidate::Type value) {
        switch (value) {
        case rtc::Candidate::Type::Host: return "host";
        case rtc::Candidate::Type::ServerReflexive: return "srflx";
        case rtc::Candidate::Type::PeerReflexive: return "prflx";
        case rtc::Candidate::Type::Relayed: return "relay";
        default: return "unknown";
        }
    }
    static const char* protocol(rtc::Candidate::TransportType value) {
        switch (value) {
        case rtc::Candidate::TransportType::Udp: return "udp";
        case rtc::Candidate::TransportType::TcpActive: return "tcp-active";
        case rtc::Candidate::TransportType::TcpPassive: return "tcp-passive";
        case rtc::Candidate::TransportType::TcpSo: return "tcp-so";
        case rtc::Candidate::TransportType::TcpUnknown: return "tcp-unknown";
        default: return "unknown";
        }
    }
    static bool knownDirect(rtc::Candidate::Type value) {
        return value == rtc::Candidate::Type::Host || value == rtc::Candidate::Type::ServerReflexive;
    }
    bool isTurn() const {
        return local == rtc::Candidate::Type::Relayed || remote == rtc::Candidate::Type::Relayed ||
            (remote == rtc::Candidate::Type::PeerReflexive && remoteProof == Proof::Relay);
    }
    bool isDirect() const {
        return (knownDirect(local) || local == rtc::Candidate::Type::PeerReflexive) &&
            (knownDirect(remote) || (remote == rtc::Candidate::Type::PeerReflexive && remoteProof == Proof::Direct));
    }
    const char* actual() const {
        if (isTurn()) return "turn";
        if (isDirect()) return "direct";
        return "unknown";
    }
    bool operator==(const SelectedRoute& other) const {
        return local == other.local && remote == other.remote && localProtocol == other.localProtocol &&
            remoteProtocol == other.remoteProtocol && remoteProof == other.remoteProof;
    }
    std::string diagnostics() const {
        return std::string(" actual=") + actual() + " localType=" + type(local) +
            " remoteType=" + type(remote) + " localProtocol=" + protocol(localProtocol) +
            " remoteProtocol=" + protocol(remoteProtocol) + " remoteProof=" + proofName(remoteProof);
    }
};

// SDP/trickle provenance can arrive after authenticated checks create a prflx
// candidate. Numeric endpoint matching proves its type without exposing it.
class CandidateProofs {
public:
    static constexpr std::size_t Limit = 64;
    static bool sameEndpoint(const rtc::Candidate& a, const rtc::Candidate& b) {
        return a.transportType() != rtc::Candidate::TransportType::Unknown && a.port().value_or(0) != 0 &&
            a.address().has_value() && b.address().has_value() && a.port().has_value() && b.port().has_value() &&
            a.address() == b.address() && a.port() == b.port() && a.transportType() == b.transportType();
    }
    void add(rtc::Candidate candidate) {
        if (candidate.type() != rtc::Candidate::Type::Host && candidate.type() != rtc::Candidate::Type::ServerReflexive &&
            candidate.type() != rtc::Candidate::Type::Relayed) return;
        if (!candidate.resolve(rtc::Candidate::ResolveMode::Simple)) return;
        for (const auto& known : candidates_)
            if (known.type() == candidate.type() && sameEndpoint(known, candidate)) return;
        if (candidates_.size() >= Limit) { ++dropped_; return; }
        candidates_.push_back(std::move(candidate));
    }
    SelectedRoute::Proof find(const rtc::Candidate& selected) const {
        bool direct = false;
        for (const auto& known : candidates_) {
            if (!sameEndpoint(known, selected)) continue;
            if (known.type() == rtc::Candidate::Type::Relayed) return SelectedRoute::Proof::Relay;
            direct = true;
        }
        return direct ? SelectedRoute::Proof::Direct : SelectedRoute::Proof::Unknown;
    }
    std::size_t size() const { return candidates_.size(); }
    unsigned dropped() const { return dropped_; }
private:
    std::vector<rtc::Candidate> candidates_;
    unsigned dropped_ = 0;
};
}
