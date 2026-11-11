#include "core/diagnostics/stage_distribution.hpp"
#include <cassert>
#include <iostream>

int main() {
    using namespace fog::metrics;
    for (const auto profile : {DiagnosticProfile::Essential, DiagnosticProfile::Standard, DiagnosticProfile::Deep}) {
        assert(collects(DiagnosticClass::Required, profile));
        assert(collects(DiagnosticClass::PeriodicDetail, profile) == (profile != DiagnosticProfile::Essential));
        assert(collects(DiagnosticClass::StageDistribution, profile) == (profile == DiagnosticProfile::Deep));
    }
    StageDistribution<> configured;
    configured.record(17, 100);
    const auto value = configured.snapshot(100);
    assert((value.availability == DistributionAvailability::Available) ==
        (diagnosticProfile == DiagnosticProfile::Deep));
    assert(value.samples == (diagnosticProfile == DiagnosticProfile::Deep ? 1u : 0u));
    assert(configured.snapshot(100, false).availability == DistributionAvailability::InstrumentationDisabled);

    StageDistribution<true> exact;
    for (std::uint64_t i = 1; i <= 100; ++i) exact.record(i, 1000 + i);
    const auto ranked = exact.snapshot(1100);
    assert(ranked.samples == 100 && ranked.p50Us == 50 && ranked.p95Us == 95 && ranked.p99Us == 99);
    assert(ranked.overflowLifetime == 0);
    assert(exact.snapshot(1000).samples == 0); // Future timestamps are not observations.
    assert(exact.snapshot(5001100).samples == 0); // Exact five-second expiry boundary.
    assert(ranked.samples == 100); // Published values remain immutable after recording.
    StageDistribution<true> saturated;
    for (std::uint64_t i = 0; i < 600; ++i) saturated.record(i, 10000);
    const auto full = saturated.snapshot(10000);
    assert(full.samples == 512 && full.overflowLifetime == 88);
    assert(full.p50Us == 343 && full.p95Us == 574 && full.p99Us == 594);
    saturated.record(1, 5010000); // Overwriting expired history does not count as a live loss.
    assert(saturated.snapshot(5010000).overflowLifetime == 88);
    assert(saturated.snapshot(5010000).samples == 1);
    assert(sizeof(StageDistribution<false>) == 1);
    std::cout << "Diagnostic profile " << diagnosticProfileName()
              << ": exact window, overflow, availability and compiled storage checks passed; recorderBytes="
              << sizeof(configured) << '\n';
}
