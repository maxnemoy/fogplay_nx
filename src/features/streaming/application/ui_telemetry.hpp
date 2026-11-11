#pragma once
#include "features/streaming/domain/loop_timing.hpp"
#include "core/diagnostics/thread_observation.hpp"

namespace stream::telemetry::ui {
using LoopTiming = fog::stream::LoopTiming;
enum class LoopScope : std::uint8_t { Window, FinalWindow, FinalLifetime };
// Preserve the histogram algorithm at capture, without deferring its 129 buckets.
struct DurationSummary {
    std::uint64_t calls = 0, totalUs = 0, minUs = 0, maxUs = 0, slow50ms = 0;
    std::uint64_t p95UpperUs = 0, p99UpperUs = 0, overflow = 0;
};
inline DurationSummary captureDuration(const fog::stream::LoopDuration& value) noexcept {
    return {value.calls, value.totalUs, value.minUs, value.maxUs, value.slow50ms,
        value.percentileUpper(95), value.percentileUpper(99), value.buckets.back()};
}
struct CompletedLoopSummary {
    std::array<DurationSummary, static_cast<std::size_t>(LoopTiming::Stage::Count)> stages{};
    DurationSummary newPresentGap;
    std::uint64_t anchors = 0, suppressedFrames = 0, continuityBreaks = 0;
    const DurationSummary& at(LoopTiming::Stage stage) const { return stages[static_cast<std::size_t>(stage)]; }
};
inline CompletedLoopSummary captureCompletedLoop(const LoopTiming::Snapshot& value) noexcept {
    CompletedLoopSummary result;
    for (std::size_t i = 0; i < result.stages.size(); ++i) result.stages[i] = captureDuration(value.stages[i]);
    result.newPresentGap = captureDuration(value.newPresentGap);
    result.anchors = value.anchors; result.suppressedFrames = value.suppressedFrames; result.continuityBreaks = value.continuityBreaks;
    return result;
}
struct LoopSnapshot {
    CompletedLoopSummary completed;
    LoopTiming::ActiveSnapshot active;
    std::uint64_t elapsedMs = 0;
    std::int64_t capturedSteadyUs = 0, spanMs = 0;
    LoopScope scope = LoopScope::Window;
};
inline LoopSnapshot captureLoop(const LoopTiming& timing, LoopScope scope,
        LoopTiming::Clock::time_point now, LoopTiming::Clock::time_point began, std::uint64_t elapsedMs) noexcept {
    return {captureCompletedLoop(scope == LoopScope::FinalLifetime ? timing.lifetime : timing.window), timing.activeSnapshot(now), elapsedMs,
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count(),
        std::chrono::duration_cast<std::chrono::milliseconds>(now - began).count(), scope};
}
inline constexpr std::size_t ThreadChunkRows = 64;
struct ThreadChunk {
    std::array<fog::diagnostics::ThreadObservationValue, ThreadChunkRows> entries{};
    std::uint64_t elapsedMs = 0, epoch = 0, overflow = 0, persistentOverflow = 0;
    std::int64_t capturedSteadyUs = 0;
    std::size_t total = 0, first = 0, size = 0;
};
inline std::size_t threadChunkCount(const fog::diagnostics::ThreadObservationSnapshot& value) noexcept {
    // Even an empty registry has its legacy summary line.
    return std::max<std::size_t>(1, (std::min(value.size, value.entries.size()) + ThreadChunkRows - 1) / ThreadChunkRows);
}
// Capture registry exactly once, then make at most two chunks from that value.
// No per-chunk re-read: epoch/rows must refer to the same observation history.
inline ThreadChunk captureThreadChunk(const fog::diagnostics::ThreadObservationSnapshot& value,
        std::size_t chunk, std::uint64_t elapsedMs, LoopTiming::Clock::time_point now) noexcept {
    ThreadChunk result;
    result.elapsedMs = elapsedMs; result.epoch = value.epoch; result.overflow = value.overflow;
    result.persistentOverflow = value.persistentOverflow;
    result.capturedSteadyUs = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    result.total = std::min(value.size, value.entries.size());
    result.first = std::min(chunk, threadChunkCount(value)) * ThreadChunkRows;
    if (result.first < result.total) {
        result.size = std::min(ThreadChunkRows, result.total - result.first);
        std::copy_n(value.entries.begin() + result.first, result.size, result.entries.begin());
    }
    return result;
}
static_assert(std::is_trivially_copyable_v<LoopSnapshot>);
static_assert(std::is_trivially_copyable_v<ThreadChunk>);
static_assert(sizeof(LoopSnapshot) <= 16 * 1024);
static_assert(sizeof(ThreadChunk) <= 16 * 1024);
}
