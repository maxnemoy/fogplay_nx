#pragma once
#include <cstdint>

namespace stream {
// Transport-independent motor strengths, 0..65535. Both zero means stop.
// durationMs == 0 holds until replaced/stopped (Moonlight); otherwise the
// deadline is measured from receivedAtMs, on the local steady clock.
struct Rumble {
    std::uint16_t controller = 0;
    std::uint16_t lowFrequency = 0, highFrequency = 0;
    std::uint32_t durationMs = 0;
    std::uint64_t receivedAtMs = 0;
};
}
