#pragma once

namespace stream {
enum class ControllerProfile { Xbox, DualSense };
inline bool validControllerProfile(ControllerProfile profile) {
    return profile == ControllerProfile::Xbox || profile == ControllerProfile::DualSense;
}
}
