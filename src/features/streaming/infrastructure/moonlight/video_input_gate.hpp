#pragma once
#include <cstdint>

namespace moonlight::detail {
inline bool videoInputSuspended(bool assemblyWaiting, bool decoderWaiting, bool coreWaiting,
                                std::uint64_t lastFrameMs, std::uint64_t nowMs) {
    constexpr std::uint64_t FreezeMs = 150;
    return assemblyWaiting || decoderWaiting || coreWaiting || !lastFrameMs ||
        (nowMs >= lastFrameMs && nowMs - lastFrameMs >= FreezeMs);
}
}
