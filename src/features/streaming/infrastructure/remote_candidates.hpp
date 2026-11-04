#pragma once
#include <rtc/candidate.hpp>
#include <rtc/description.hpp>
#include <utility>

namespace stream::detail {
// The Switch RTC build has no asynchronous DNS/mDNS lookup: a remote candidate
// whose address is a hostname (for example "<uuid>.local") makes
// PeerConnection::addRemoteCandidate() throw, and setRemoteDescription() calls
// it for every candidate embedded in the offer. resolve(Simple) is the same
// numeric-only resolution libdatachannel attempts before that lookup, so this
// predicate matches the patched rejection exactly.
inline bool resolvableWithoutLookup(rtc::Candidate& candidate) {
    return candidate.resolve(rtc::Candidate::ResolveMode::Simple);
}

// Removes the embedded candidates that would need a lookup and keeps the
// others (resolved, with their mid) and the end-of-candidates marker.
// Returns how many were removed; their text is never reported.
inline unsigned dropUnresolvableCandidates(rtc::Description& description) {
    const bool ended = description.ended();
    auto embedded = description.extractCandidates();
    unsigned dropped = 0;
    for (auto& candidate : embedded) {
        if (resolvableWithoutLookup(candidate)) description.addCandidate(std::move(candidate));
        else ++dropped;
    }
    if (ended) description.endCandidates();
    return dropped;
}
}
