#pragma once
#include "core/diagnostics/diagnostic_profile.hpp"
#include "core/diagnostics/latency_histogram.hpp"
#include <type_traits>

namespace fog::metrics {
enum class DistributionAvailability : std::uint8_t { NotCollected, InstrumentationDisabled, Available };
struct StageDistributionSnapshot {
    DistributionAvailability availability = DistributionAvailability::NotCollected;
    std::uint64_t capturedAtSteadyUs = 0;
    std::uint64_t samples = 0, p50Us = 0, p95Us = 0, p99Us = 0, overflowLifetime = 0;
    // Exact nearest-rank samples in [now-5s, now], excluding the lower boundary;
    // at most 512 observations, with overwritten live samples counted explicitly.
    static constexpr auto windowUs = LatencyHistogram::WindowUs;
    static constexpr auto capacity = LatencyHistogram::Capacity;
};
static_assert(std::is_trivially_copyable_v<StageDistributionSnapshot>);

// A recorder belongs to ONE serialized owner. It is not an atomic/thread-safe
// object. Publish snapshot values from that owner; readers must not access the
// recorder or a reference to its storage. Disabled builds contain no sample ring.
template<bool Enabled = collects(DiagnosticClass::StageDistribution)>
class StageDistribution {
public:
    void record(std::uint64_t elapsedUs, std::uint64_t nowUs) noexcept {
        if constexpr (Enabled) histogram_.record(elapsedUs, nowUs);
    }
    StageDistributionSnapshot snapshot(std::uint64_t nowUs, bool instrumented = true) const noexcept {
        StageDistributionSnapshot result;
        result.capturedAtSteadyUs = nowUs;
        if (!instrumented) {
            result.availability = DistributionAvailability::InstrumentationDisabled;
            return result;
        }
        if constexpr (Enabled) {
            const auto value = histogram_.snapshot(nowUs);
            result.availability = DistributionAvailability::Available;
            result.samples = value.samples;
            result.p50Us = value.p50Us;
            result.p95Us = value.p95Us;
            result.p99Us = value.p99Us;
            result.overflowLifetime = value.overflow;
        }
        return result;
    }
private:
    struct Disabled {};
    std::conditional_t<Enabled, LatencyHistogram, Disabled> histogram_{};
};
static_assert(sizeof(StageDistribution<false>) == 1);
static_assert(sizeof(StageDistribution<true>) <= 8256);
}
