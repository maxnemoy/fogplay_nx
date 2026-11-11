#pragma once
#include <array>
#include <cstdint>

namespace stream {
struct Gamepad {
    std::array<int, 18> buttons{};
    std::array<float, 4> axes{};
    float rightTrigger = 0;
    float leftTrigger = 0;
    std::uint64_t sampledAtUs = 0; // Local pad poll timestamp; never serialized to the host.
};
}
