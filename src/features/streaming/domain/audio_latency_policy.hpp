#pragma once
#include <algorithm>
#include <cstdint>

namespace fog::detail {
// Worker-only PCM admission: consume/decode every Opus packet to preserve its
// predictive state, then catch up without clearing sound already in SDL.
// 40 ms refill protects against jitter; 80 ms bounds software output buffering.
// The age budget is deliberately less aggressive than Moonlight-Switch's
// 30 ms pending-audio cutoff. SDL/hardware buffering is outside that age.
struct AudioLatencyPolicy {
    static constexpr std::uint32_t sampleRate = 48000, bytesPerFrame = 4;
    static constexpr std::uint32_t refillMs = 40, maximumMs = 80, staleMs = 60;
    struct Selection {
        std::uint32_t skip = 0, frames = 0;
        bool stale = false, overflow = false;
    };
    static Selection select(std::uint32_t frames, std::uint32_t queuedBytes,
        std::uint64_t residenceMs) {
        const auto queued = queuedBytes / bytesPerFrame;
        constexpr auto refill = sampleRate * refillMs / 1000;
        constexpr auto maximum = sampleRate * maximumMs / 1000;
        const bool stale = residenceMs > staleMs;
        if (stale && queued >= refill) return {frames, 0, true, false};
        if (queued >= maximum) return {frames, 0, false, true};
        const auto capacity = maximum - queued;
        if (frames > capacity) {
            // Drop complete ordinary frames instead of feeding tiny fragments
            // at the ceiling. A legal 120 ms Opus frame must still refill an
            // empty device: retain only its newest PCM within the budget.
            if (queued >= refill) return {frames, 0, false, true};
            return {frames - capacity, capacity, false, true};
        }
        return {0, frames, false, false};
    }
};
}
