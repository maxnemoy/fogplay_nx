#pragma once

namespace fog::rumble {
enum class Mode { Balanced, DualBand, Chromium, Disabled };
struct Settings { Mode mode = Mode::Balanced; };
inline bool valid(const Settings& value) {
    return value.mode == Mode::Balanced || value.mode == Mode::DualBand ||
           value.mode == Mode::Chromium || value.mode == Mode::Disabled;
}
inline bool operator==(const Settings& a, const Settings& b) { return a.mode == b.mode; }
inline bool operator!=(const Settings& a, const Settings& b) { return !(a == b); }
inline bool adjust(Settings& value, int direction) {
    if (!valid(value) || !direction) return false;
    value.mode = static_cast<Mode>((static_cast<int>(value.mode) + (direction > 0 ? 1 : 3)) % 4);
    return true;
}
inline const char* modeId(Mode mode) {
    switch (mode) {
        case Mode::Balanced: return "balanced";
        case Mode::DualBand: return "dual-band";
        case Mode::Chromium: return "chromium";
        case Mode::Disabled: return "disabled";
    }
    return "invalid";
}
}
