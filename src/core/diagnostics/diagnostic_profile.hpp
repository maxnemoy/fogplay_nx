#pragma once
#include <cstdint>

#ifndef FOGPLAY_DIAGNOSTIC_PROFILE
#define FOGPLAY_DIAGNOSTIC_PROFILE 1
#endif
#if FOGPLAY_DIAGNOSTIC_PROFILE < 0 || FOGPLAY_DIAGNOSTIC_PROFILE > 2
#error "FOGPLAY_DIAGNOSTIC_PROFILE must be 0 (essential), 1 (standard), or 2 (deep)"
#endif

namespace fog::metrics {
enum class DiagnosticProfile : std::uint8_t { Essential, Standard, Deep };
inline constexpr auto diagnosticProfile = static_cast<DiagnosticProfile>(FOGPLAY_DIAGNOSTIC_PROFILE);
// Only periodic detail is optional. Errors, lifecycle, security, event-loss and
// final evidence always remain enabled, including in the Essential profile.
enum class DiagnosticClass : std::uint8_t { Required, PeriodicDetail, StageDistribution };
constexpr bool collects(DiagnosticClass group, DiagnosticProfile profile = diagnosticProfile) noexcept {
    return group == DiagnosticClass::Required ||
        (group == DiagnosticClass::PeriodicDetail && profile != DiagnosticProfile::Essential) ||
        (group == DiagnosticClass::StageDistribution && profile == DiagnosticProfile::Deep);
}
constexpr const char* diagnosticProfileName(DiagnosticProfile profile = diagnosticProfile) noexcept {
    switch (profile) {
        case DiagnosticProfile::Essential: return "essential";
        case DiagnosticProfile::Standard: return "standard";
        case DiagnosticProfile::Deep: return "deep";
    }
    return "unknown";
}
}
