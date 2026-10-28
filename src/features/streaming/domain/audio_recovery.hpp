#pragma once
#include <array>
#include <chrono>
#include <cstddef>

namespace fog {
// One audio owner (application thread or audio worker). A valid queued packet
// ends the episode, but never replenishes the rolling reset budget.
class AudioRecovery {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    enum class Attempt { Wait, Reset, Stop };
    void failed(Time now) {
        if (!active_) { active_ = true; began_ = now; }
    }
    bool badPacket(Time now) {
        if (++badPackets_ < 3) return false;
        failed(now);
        return true;
    }
    bool expired(Time now) const { return active_ && now - began_ >= std::chrono::seconds(10); }
    Attempt attempt(Time now) {
        if (expired(now)) return Attempt::Stop;
        std::size_t retained = 0;
        for (std::size_t i = 0; i < count_; ++i)
            if (now - resets_[i] < std::chrono::seconds(60)) resets_[retained++] = resets_[i];
        count_ = retained;
        if (count_ == resets_.size()) return Attempt::Stop;
        if (hasAttempt_ && now - lastAttempt_ < std::chrono::milliseconds(250)) return Attempt::Wait;
        failed(now);
        resets_[count_++] = now; lastAttempt_ = now; hasAttempt_ = true;
        return Attempt::Reset;
    }
    void resetCompleted() { badPackets_ = 0; }
    void queued() { active_ = false; badPackets_ = 0; }
private:
    bool active_ = false, hasAttempt_ = false;
    unsigned badPackets_ = 0;
    Time began_{}, lastAttempt_{};
    std::array<Time, 3> resets_{};
    std::size_t count_ = 0;
};
}
