#pragma once
#include <string>

// Independent experiments share the same corrected diagnostics and safety fixes.
// Bit order: C1, C2, S5, P9, P10, S14, S10, P4, S16. Normal build keeps S16 off.
#ifndef FOGPLAY_STAGE1_FEATURE_MASK
#define FOGPLAY_STAGE1_FEATURE_MASK 255
#endif
#ifndef FOGPLAY_MTS_CATCHUP_AGE_GUARD
#define FOGPLAY_MTS_CATCHUP_AGE_GUARD 1
#endif
static_assert(FOGPLAY_MTS_CATCHUP_AGE_GUARD == 0 || FOGPLAY_MTS_CATCHUP_AGE_GUARD == 1,
    "Invalid MTS catchup age guard");
static_assert(FOGPLAY_STAGE1_FEATURE_MASK >= 0 && FOGPLAY_STAGE1_FEATURE_MASK <= 511,
    "Invalid Stage 1 experiment mask");
#if (FOGPLAY_STAGE1_FEATURE_MASK & 256) && !defined(FOGPLAY_BOUNDED_SYNC_AUDIO)
#define FOGPLAY_BOUNDED_SYNC_AUDIO
#endif
namespace fog::stage1 {
inline constexpr bool earlyInput = (FOGPLAY_STAGE1_FEATURE_MASK & 1) != 0;
inline constexpr bool radialDeadzone = (FOGPLAY_STAGE1_FEATURE_MASK & 2) != 0;
inline constexpr bool asyncSettings = (FOGPLAY_STAGE1_FEATURE_MASK & 4) != 0;
inline constexpr bool sparseDiagnostics = (FOGPLAY_STAGE1_FEATURE_MASK & 8) != 0;
inline constexpr bool pauseArtwork = (FOGPLAY_STAGE1_FEATURE_MASK & 16) != 0;
inline constexpr bool inputFreezeGate = (FOGPLAY_STAGE1_FEATURE_MASK & 32) != 0;
inline constexpr bool localIdrRetry = (FOGPLAY_STAGE1_FEATURE_MASK & 64) != 0;
inline constexpr bool mtsCatchup = (FOGPLAY_STAGE1_FEATURE_MASK & 128) != 0;
inline constexpr bool mtsCatchupAgeGuard = mtsCatchup && FOGPLAY_MTS_CATCHUP_AGE_GUARD;
#ifdef FOGPLAY_BOUNDED_SYNC_AUDIO
inline constexpr bool boundedSyncAudio = true;
#else
inline constexpr bool boundedSyncAudio = false;
#endif
inline std::string diagnostics() {
    return "Stage1 options early_input=" + std::to_string(earlyInput) +
        " radial_deadzone=" + std::to_string(radialDeadzone) +
        " async_settings=" + std::to_string(asyncSettings) +
        " sparse_diagnostics=" + std::to_string(sparseDiagnostics) +
        " pause_artwork=" + std::to_string(pauseArtwork) +
        " input_freeze_gate=" + std::to_string(inputFreezeGate) +
        " local_idr_retry=" + std::to_string(localIdrRetry) +
        " mts_catchup=" + std::to_string(mtsCatchup) +
        " mts_catchup_age_guard=" + std::to_string(mtsCatchupAgeGuard) +
        " bounded_sync_audio=" + std::to_string(boundedSyncAudio);
}
}
