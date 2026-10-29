#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace stream {
// Local data-channel buffer observations only. Neither an empty buffer nor a
// successful send establishes remote receipt, input processing or round-trip time.
class ControlDelivery {
public:
    static constexpr std::size_t BlockedBytes = 64 * 1024;
    static constexpr std::uint64_t StaleMs = 5000;
    struct Snapshot {
        bool known = false;
        std::size_t currentBytes = 0, maxBytes = 0;
        std::uint64_t sampledAt = 0;
        bool blocked = false;
        std::uint64_t blockedSince = 0;
    };
    void sample(std::size_t buffered, std::uint64_t now) {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool blocked = buffered > BlockedBytes;
        const bool staleGap = state_.known && now >= state_.sampledAt && now - state_.sampledAt > StaleMs;
        if (blocked && (!state_.blocked || now < state_.sampledAt || staleGap)) state_.blockedSince = now;
        if (!blocked) state_.blockedSince = 0;
        state_.known = true;
        state_.currentBytes = buffered;
        if (buffered > state_.maxBytes) state_.maxBytes = buffered;
        state_.sampledAt = now;
        state_.blocked = blocked;
    }
    Snapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }
    std::string diagnostics(std::uint64_t now) const {
        const auto state = snapshot();
        const bool ageKnown = state.known && now >= state.sampledAt;
        const auto age = ageKnown ? now - state.sampledAt : 0;
        const bool stale = !ageKnown || age > StaleMs;
        // A stale last observation cannot establish continuous backpressure
        // after its sample. Freeze that episode's age at the last observed time.
        const auto episodeEnd = stale ? state.sampledAt : now;
        const auto blockedMs = state.blocked && ageKnown && episodeEnd >= state.blockedSince
            ? episodeEnd - state.blockedSince : 0;
        std::array<char, 24> ageText{};
        if (ageKnown) std::snprintf(ageText.data(), ageText.size(), "%llu", static_cast<unsigned long long>(age));
        else std::snprintf(ageText.data(), ageText.size(), "-1");
        std::array<char, 384> text{};
        std::snprintf(text.data(), text.size(),
            "controlBufferedKnown=%d controlBufferedBytes=%llu controlBufferedMaxBytes=%llu "
            "controlBufferedAgeMs=%s controlBufferedStale=%d controlBufferedBlocked=%d controlBufferedBlockedMs=%llu",
            state.known, static_cast<unsigned long long>(state.currentBytes), static_cast<unsigned long long>(state.maxBytes),
            ageText.data(), stale, state.blocked, static_cast<unsigned long long>(blockedMs));
        return text.data();
    }
private:
    mutable std::mutex mutex_;
    Snapshot state_;
};
}
