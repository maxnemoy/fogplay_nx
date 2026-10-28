#pragma once
#include "features/streaming/domain/streaming_client.hpp"
#include <chrono>

namespace fog::stream {
// Every observed button edge bypasses analog cadence. Rejected samples never
// advance accepted state; no old presses are replayed after a UI transition.
class GamepadSchedule {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    static constexpr auto AnalogInterval = std::chrono::milliseconds(16);
    void reset() { initialized_ = releasePending_ = false; }
    bool due(const ::stream::Gamepad& value, Time now) const {
        if (releasePending_) return false;
        return !initialized_ || value.buttons != accepted_.buttons ||
            value.leftTrigger != accepted_.leftTrigger || value.rightTrigger != accepted_.rightTrigger ||
            now < acceptedAt_ || now - acceptedAt_ >= AnalogInterval;
    }
    void accepted(const ::stream::Gamepad& value, Time now) {
        accepted_ = value; acceptedAt_ = now; initialized_ = true;
    }
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
    ::stream::Gamepad accepted_{};
    Time acceptedAt_{};
};
}
