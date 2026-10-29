#pragma once
#include <chrono>

namespace fog {
// Main-thread scheduling only. Each provider request has its own transport timeout.
class StopRetryBudget {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr unsigned MaxAttempts = 3;
    static constexpr auto MaxElapsed = std::chrono::seconds(30);
    void reset() noexcept { attempts_ = 0; started_ = {}; }
    bool exhausted(Clock::time_point now) const noexcept {
        return attempts_ >= MaxAttempts || (attempts_ && now - started_ >= MaxElapsed);
    }
    bool begin(Clock::time_point now) noexcept {
        if (exhausted(now)) return false;
        if (!attempts_) started_ = now;
        ++attempts_;
        return true;
    }
    unsigned attempts() const noexcept { return attempts_; }
private:
    unsigned attempts_ = 0;
    Clock::time_point started_{};
};
}
