#pragma once

// Host diagnostics tests exercise the real logger without libnx/SDL startup.
inline int fsdevCommitDevice(const char*) { return 0; }
inline bool R_SUCCEEDED(int result) { return result >= 0; }
namespace fogplay::platform {
inline constexpr const char* dataDir = ".";
}
