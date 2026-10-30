#pragma once
#include <algorithm>
#include <cmath>

namespace fog::stream {
struct StickAxes { float x = 0, y = 0; };

inline StickAxes cursorStick(int x, int y) {
    const float nx = std::clamp(x / 32767.0f,-1.0f,1.0f);
    const float ny = std::clamp(y / 32767.0f,-1.0f,1.0f);
    // Cursor movement has no host/game dead zone. Preserve its drift guard.
    return {std::abs(nx) < 0.12f ? 0.0f : nx,std::abs(ny) < 0.12f ? 0.0f : ny};
}

// Preserve direction and remap the usable radius continuously to [0, 1].
// A circular threshold avoids a square dead zone around diagonal movement.
inline StickAxes radialStick(int x, int y) {
    constexpr float deadzone = 0.04f;
    const float nx = std::clamp(x / 32767.0f, -1.0f, 1.0f);
    const float ny = std::clamp(y / 32767.0f, -1.0f, 1.0f);
    const float radius = std::hypot(nx, ny);
    if (radius <= deadzone) return {};
    const float magnitude = (std::min(radius, 1.0f) - deadzone) / (1.0f - deadzone);
    return {nx / radius * magnitude, ny / radius * magnitude};
}
}
