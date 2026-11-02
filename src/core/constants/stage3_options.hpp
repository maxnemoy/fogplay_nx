#pragma once
// A comparison switch for future reviewed builds, not a runtime preference.
#ifndef FOGPLAY_MTS_MEDIA_WORKERS
#define FOGPLAY_MTS_MEDIA_WORKERS 1
#endif
static_assert(FOGPLAY_MTS_MEDIA_WORKERS == 0 || FOGPLAY_MTS_MEDIA_WORKERS == 1,
    "Invalid MTS media worker option");
namespace fog::stage3 {
inline constexpr bool mtsMediaWorkers = FOGPLAY_MTS_MEDIA_WORKERS != 0;
}
