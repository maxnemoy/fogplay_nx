#pragma once
#include "features/streaming/domain/gamepad.hpp"
#include "features/streaming/domain/input_experiments.hpp"
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace fog::stream {
// Every observed button edge bypasses analog cadence. Rejected samples never
// advance accepted state; no old presses are replayed after a UI transition.
class GamepadSchedule {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    static constexpr auto AnalogInterval = std::chrono::milliseconds(16);
    static constexpr auto KeepaliveInterval = std::chrono::milliseconds(200);
    static constexpr float AxisChangeThreshold = 0.005f;
    void reset() { initialized_ = releasePending_ = false; }
    bool due(const ::stream::Gamepad& value, Time now) const {
        if (releasePending_) return false;
        if (!initialized_ || value.buttons != accepted_.buttons ||
            value.leftTrigger != accepted_.leftTrigger || value.rightTrigger != accepted_.rightTrigger ||
            now < acceptedAt_) return true;
        if constexpr (!fog::input_options::changeOnly) return now - acceptedAt_ >= AnalogInterval;
        for (std::size_t i = 0; i < value.axes.size(); ++i) {
            // Never retain a tiny deflection when the stick returns to centre.
            if ((value.axes[i] == 0 && accepted_.axes[i] != 0) ||
                std::fabs(value.axes[i] - accepted_.axes[i]) >= AxisChangeThreshold) return true;
        }
        return now - acceptedAt_ >= KeepaliveInterval;
    }
    void accepted(const ::stream::Gamepad& value, Time now) {
        for (std::size_t i = 0; i < value.buttons.size(); ++i)
            if (value.buttons[i] && (!initialized_ || !accepted_.buttons[i])) { ++pressEdges_; break; }
        accepted_ = value; acceptedAt_ = now; initialized_ = true;
    }
    // Monotonic count of accepted states that newly pressed a button; lets
    // pending overlay text stop before input that may move the remote focus.
    std::uint64_t pressEdges() const { return pressEdges_; }
    template<class Send> void suspend(Send&& send) {
        initialized_ = false;
        releasePending_ = true;
        retryRelease(send);
    }
    template<class Send> void retryRelease(Send&& send) {
        if (releasePending_ && send(::stream::Gamepad{})) releasePending_ = false;
    }
private:
    bool initialized_ = false, releasePending_ = false;
    std::uint64_t pressEdges_ = 0;
    ::stream::Gamepad accepted_{};
    Time acceptedAt_{};
};
}
