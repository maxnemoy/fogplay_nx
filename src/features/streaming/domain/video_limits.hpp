#pragma once
#include <cstddef>

namespace stream {
// Shared by RTP assembly, Moonlight ingestion and the decoder boundary.
inline constexpr std::size_t MaxVideoAccessUnitBytes = 8 * 1024 * 1024;
}
