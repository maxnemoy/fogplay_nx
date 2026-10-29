#pragma once
#include <chrono>

namespace fog {
// A successful upload advances the phase; probing an empty queue or a failed
// upload does not. The unconfigured state preserves legacy unpaced callers.
// due() and uploaded() must receive the same refresh-slot timestamp, captured
// before variable decode/conversion work rather than after the upload finishes.
class FrameCadence {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    void configure(int fps) {
        interval_ = std::chrono::nanoseconds(1000000000 / (fps > 0 ? fps : 30));
        enabled_ = true;
        reset();
    }
    void reset() { scheduled_ = false; }
    bool configured() const { return enabled_; }
    bool due(Time now) const {
        return !enabled_ || !scheduled_ || now + std::chrono::milliseconds(1) >= next_;
    }
    void uploaded(Time now) {
        if (!enabled_) return;
        if (!scheduled_ || now > next_ + interval_) next_ = now + interval_;
        else next_ += interval_;
        scheduled_ = true;
    }
private:
    bool enabled_ = false, scheduled_ = false;
    std::chrono::nanoseconds interval_{33333333};
    Time next_{};
};
}
