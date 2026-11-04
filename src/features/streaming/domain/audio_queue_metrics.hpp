#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>

namespace fog::detail {
// SDL queued bytes exclude the hardware buffer. These observations do not
// establish an exact device underrun; brief hardware transfers are normal.
struct AudioQueueMetrics {
    using Clock = std::chrono::steady_clock;
    static constexpr std::uint64_t bytesPerSecond = 48000 * 2 * 2;
    static constexpr auto prolongedEmpty = std::chrono::milliseconds(40);
    std::uint64_t emptyObservations = 0, prolongedEmptyObservations = 0;
    std::uint64_t resets = 0, discardedBytes = 0;
    std::uint32_t maximumBytes = 0;
    bool primed = false, empty = false, prolonged = false;
    Clock::time_point emptySince{};
    // Queue floor: the smallest level seen in each 10-s window. A peak is a
    // transient burst; a raised floor is latency that the queue never drains
    // (observed rising to 150-170 ms during an 82-minute MTS session).
    static constexpr auto floorWindow = std::chrono::seconds(10);
    static constexpr std::uint64_t elevatedFloorMs = 100;
    std::uint32_t lastFloorBytes = 0, maximumFloorBytes = 0;
    std::uint64_t floorWindows = 0, elevatedFloorWindows = 0;
    std::uint32_t windowMinimumBytes = std::numeric_limits<std::uint32_t>::max();
    Clock::time_point floorWindowStart{};
    bool floorStarted = false;

    void observe(std::uint32_t bytes, Clock::time_point now) {
        maximumBytes = std::max(maximumBytes, bytes);
        observeFloor(bytes, now);
        if (bytes) { empty = prolonged = false; return; }
        if (!primed) return;
        if (!empty) { empty = true; emptySince = now; ++emptyObservations; }
        if (!prolonged && now - emptySince >= prolongedEmpty) {
            prolonged = true;
            ++prolongedEmptyObservations;
        }
    }
    void queued(std::uint32_t bytes, Clock::time_point now) {
        primed = true;
        // A successful enqueue ends the preceding empty episode, even when
        // the hardware immediately takes the newly queued samples.
        empty = prolonged = false;
        observe(bytes, now);
    }
    void cleared(std::uint32_t bytes) {
        ++resets; discardedBytes += bytes;
        primed = empty = prolonged = false;
    }
    static std::uint64_t milliseconds(std::uint64_t bytes) {
        return bytes * 1000 / bytesPerSecond;
    }
    void observeFloor(std::uint32_t bytes, Clock::time_point now) {
        if (!floorStarted) { floorStarted = true; floorWindowStart = now; }
        windowMinimumBytes = std::min(windowMinimumBytes, bytes);
        if (now - floorWindowStart < floorWindow) return;
        lastFloorBytes = windowMinimumBytes;
        maximumFloorBytes = std::max(maximumFloorBytes, windowMinimumBytes);
        ++floorWindows;
        if (milliseconds(windowMinimumBytes) >= elevatedFloorMs) ++elevatedFloorWindows;
        floorWindowStart = now;
        windowMinimumBytes = std::numeric_limits<std::uint32_t>::max();
    }
};
}
