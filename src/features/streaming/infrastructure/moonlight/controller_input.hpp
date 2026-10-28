#pragma once
#include "features/streaming/domain/controller_profile.hpp"
#include "features/streaming/domain/remote_input.hpp"
#include <Limelight.h>
#include <array>
#include <cmath>
#include <cstdint>

namespace moonlight::detail {
inline std::uint8_t controllerType(stream::ControllerProfile profile) {
    return profile == stream::ControllerProfile::DualSense ? LI_CTYPE_PS : LI_CTYPE_XBOX;
}
inline std::uint16_t controllerCapabilities(stream::ControllerProfile profile) {
    return LI_CCAP_RUMBLE | (profile == stream::ControllerProfile::DualSense ?
        LI_CCAP_TOUCHPAD | LI_CCAP_ACCEL | LI_CCAP_GYRO : 0);
}
inline std::uint32_t controllerButtons(stream::ControllerProfile profile) {
    return 0xffff | (profile == stream::ControllerProfile::DualSense ? TOUCHPAD_FLAG : 0);
}
inline int controllerButtonFlags(const std::array<int, 18>& buttons, stream::ControllerProfile profile) {
    static constexpr int mapping[18] = { A_FLAG, B_FLAG, X_FLAG, Y_FLAG, LB_FLAG, RB_FLAG, 0, 0,
        BACK_FLAG, PLAY_FLAG, LS_CLK_FLAG, RS_CLK_FLAG, UP_FLAG, DOWN_FLAG, LEFT_FLAG, RIGHT_FLAG, SPECIAL_FLAG, TOUCHPAD_FLAG };
    int flags = 0;
    for (std::size_t i = 0; i < buttons.size(); ++i)
        if (buttons[i] && (i != 17 || profile == stream::ControllerProfile::DualSense)) flags |= mapping[i];
    return flags;
}
inline int controllerTouchType(const fog::remote::Touch& touch) {
    if (touch.finger < 0 || touch.finger > 1 || !std::isfinite(touch.x) || !std::isfinite(touch.y) ||
        touch.x < 0 || touch.x > 1 || touch.y < 0 || touch.y > 1) return -1;
    switch (touch.phase) {
        case fog::remote::TouchPhase::Down: return LI_TOUCH_EVENT_DOWN;
        case fog::remote::TouchPhase::Move: return LI_TOUCH_EVENT_MOVE;
        case fog::remote::TouchPhase::Up: return LI_TOUCH_EVENT_UP;
    }
    return -1;
}
inline int controllerMotion(const fog::remote::Sensor& sensor, std::array<float, 3>& xyz) {
    // Shared native input already uses m/s² for acceleration and rad/s for gyro.
    // Its axes follow SDL's controller convention (+X right, +Y up, +Z toward player).
    const bool gyro = sensor.kind == fog::remote::MotionSensor::Gyroscope;
    if (!gyro && sensor.kind != fog::remote::MotionSensor::Accelerometer) return -1;
    for (std::size_t i = 0; i < xyz.size(); ++i) {
        xyz[i] = sensor.xyz[i] * (gyro ? 57.2957795f : 1.f);
        if (!std::isfinite(xyz[i])) return -1;
    }
    return gyro ? LI_MOTION_TYPE_GYRO : LI_MOTION_TYPE_ACCEL;
}
}
