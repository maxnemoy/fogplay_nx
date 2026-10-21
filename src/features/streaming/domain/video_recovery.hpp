#pragma once
#include <chrono>
#include <deque>
namespace fog {
// Pure deadline/reset budget. Repeated errors do not extend the current episode.
class VideoRecovery {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    bool begin(Time now) {
        if (expired(now)) return false;
        while (!resets_.empty() && now-resets_.front() >= std::chrono::seconds(60)) resets_.pop_front();
        if (resets_.size() >= 3) return false;
        if (!active_) { active_=true; began_=now; }
        resets_.push_back(now);
        return true;
    }
    bool expired(Time now) const { return active_ && now-began_ >= std::chrono::seconds(10); }
    bool active() const { return active_; }
    void decoded() { active_=false; }
    void reset() { active_=false; resets_.clear(); }
    unsigned recentResets() const { return static_cast<unsigned>(resets_.size()); }
private:
    bool active_=false;
    Time began_{};
    std::deque<Time> resets_;
};
}
