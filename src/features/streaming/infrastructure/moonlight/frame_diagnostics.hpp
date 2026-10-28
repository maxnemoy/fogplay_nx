#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace moonlight::detail {
inline std::uint64_t steadyUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
// One worker records samples; diagnostics may read from the presentation thread.
// Individual counters are atomic. A snapshot is approximate, not a transaction.
struct TimingSamples {
    std::atomic<std::uint64_t> count{0}, totalUs{0}, maxUs{0}, over50ms{0};
    void reset() { count = 0; totalUs = 0; maxUs = 0; over50ms = 0; }
    void add(std::uint64_t us) {
        count.fetch_add(1, std::memory_order_relaxed);
        totalUs.fetch_add(us, std::memory_order_relaxed);
        if (us > maxUs.load(std::memory_order_relaxed)) maxUs.store(us, std::memory_order_relaxed);
        if (us > 50000) over50ms.fetch_add(1, std::memory_order_relaxed);
    }
    std::string text(const char* name) const {
        const auto samples = count.load(std::memory_order_relaxed);
        return std::string(name) + "_n=" + std::to_string(samples) +
            " avg_us=" + std::to_string(samples ? totalUs.load(std::memory_order_relaxed) / samples : 0) +
            " max_us=" + std::to_string(maxUs.load(std::memory_order_relaxed)) +
            " over50ms=" + std::to_string(over50ms.load(std::memory_order_relaxed));
    }
};

struct FrameDiagnostics {
    TimingSamples assembly, coreAge, arrivalGap, hostProcessing, workerGap, drain, copy, queueLock, completion;
    std::atomic<std::uint64_t> units{0}, payloadBytes{0}, idrs{0}, skippedFrameNumbers{0}, repeatedOrReordered{0};
    std::atomic<std::uint64_t> invalidTimes{0}, emptyPolls{0}, lastUnitUs{0};
    // Only the worker accesses sequence state; reset is called before it starts.
    std::uint32_t previousFrame = 0;
    bool hasFrame = false;
    std::uint64_t previousWorkerUs = 0;

    void reset() {
        assembly.reset(); coreAge.reset(); arrivalGap.reset(); hostProcessing.reset();
        workerGap.reset(); drain.reset(); copy.reset(); queueLock.reset(); completion.reset();
        units = 0; payloadBytes = 0; idrs = 0; skippedFrameNumbers = 0; repeatedOrReordered = 0;
        invalidTimes = 0; emptyPolls = 0; lastUnitUs = 0;
        previousFrame = 0; hasFrame = false; previousWorkerUs = 0;
    }
    void frame(std::uint32_t number, bool idr, int bytes, std::uint64_t received,
               std::uint64_t queued, std::uint64_t now, std::uint16_t hostTenthsMs) {
        units.fetch_add(1, std::memory_order_relaxed);
        if (bytes > 0) payloadBytes.fetch_add(static_cast<std::uint64_t>(bytes), std::memory_order_relaxed);
        if (idr) idrs.fetch_add(1, std::memory_order_relaxed);
        if (hasFrame) {
            const std::uint32_t delta = number - previousFrame;
            if (delta > 0 && delta < 0x80000000u) {
                skippedFrameNumbers.fetch_add(delta - 1, std::memory_order_relaxed);
                previousFrame = number;
            } else repeatedOrReordered.fetch_add(1, std::memory_order_relaxed);
        } else { previousFrame = number; hasFrame = true; }
        const auto previous = lastUnitUs.load(std::memory_order_relaxed);
        if (previous && now >= previous) arrivalGap.add(now - previous);
        lastUnitUs.store(now, std::memory_order_relaxed);
        // All three timestamps use Moonlight's clock (platform dependent).
        if (received && queued >= received && now >= queued) {
            assembly.add(queued - received); coreAge.add(now - queued);
        } else invalidTimes.fetch_add(1, std::memory_order_relaxed);
        if (hostTenthsMs) hostProcessing.add(static_cast<std::uint64_t>(hostTenthsMs) * 100);
    }
    void worker(std::uint64_t now) {
        if (previousWorkerUs && now >= previousWorkerUs) workerGap.add(now - previousWorkerUs);
        previousWorkerUs = now;
    }
    std::uint64_t ageUs(std::uint64_t now) const {
        const auto last = lastUnitUs.load(std::memory_order_relaxed);
        return last && now >= last ? now - last : 0;
    }
};
}
