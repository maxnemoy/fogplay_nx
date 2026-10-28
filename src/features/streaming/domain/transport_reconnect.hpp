#pragma once
#include <array>
#include <chrono>

namespace fog::stream {
// Count recovery attempts in a rolling two-minute window: transport replacement
// for MTS, or ownership inspection followed by resume for FH. Successful
// playback ends recovery, but does not clear recent attempts. All deadlines use
// the caller's monotonic clock.
class TransportReconnect {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    enum class Action { Continue, CloseTransport, StartTransport, Stop };
    enum class Preparation { Ready, Retry, Refused };
    static constexpr unsigned MaxAttempts = 3;

    void reset() { phase_ = Phase::Idle; attempts_ = 0; }
    void cancel() { phase_ = Phase::Cancelled; }
    // Recent starts as of the last supplied clock value.
    unsigned attempts() const { return attempts_; }
    bool recovering() const {
        return phase_ == Phase::Grace || phase_ == Phase::Backoff || phase_ == Phase::Connecting;
    }
    bool connecting() const { return phase_ == Phase::Connecting; }
    bool disconnected(Time now) {
        if (phase_ != Phase::Idle) return recovering();
        expireAttempts(now);
        phase_ = Phase::Grace;
        windowBegan_ = phaseBegan_ = now;
        return true;
    }
    bool retry(Time now) {
        if (phase_ == Phase::Cancelled || phase_ == Phase::Exhausted) return false;
        if (recovering() && expired(now)) { phase_ = Phase::Exhausted; return false; }
        expireAttempts(now);
        if (attempts_ >= MaxAttempts) { phase_ = Phase::Exhausted; return false; }
        if (!recovering()) windowBegan_ = now;
        // Duplicate loss reports must not extend a pending backoff.
        if (phase_ != Phase::Backoff) { phase_ = Phase::Backoff; phaseBegan_ = now; }
        return true;
    }
    Action poll(Time now) {
        if (phase_ != Phase::Cancelled && phase_ != Phase::Exhausted) expireAttempts(now);
        if (!recovering()) return phase_ == Phase::Exhausted ? Action::Stop : Action::Continue;
        if (expired(now)) { phase_ = Phase::Exhausted; return Action::Stop; }
        if (phase_ == Phase::Grace && now-phaseBegan_ >= std::chrono::seconds(5))
            return retry(now) ? Action::CloseTransport : Action::Stop;
        if (phase_ == Phase::Backoff && now-phaseBegan_ >= std::chrono::seconds(2)) {
            if (attempts_ >= MaxAttempts) { phase_ = Phase::Exhausted; return Action::Stop; }
            attemptStarts_[attempts_++] = now; phase_ = Phase::Connecting; phaseBegan_ = now;
            return Action::StartTransport;
        }
        if (phase_ == Phase::Connecting && now-phaseBegan_ >= std::chrono::seconds(30))
            return retry(now) ? Action::CloseTransport : Action::Stop;
        return Action::Continue;
    }
    // Only call for a newly prepared frame AND ready control on a connected peer.
    // A late image may not resurrect an expired or cancelled recovery.
    bool prepared(Time now) {
        if ((phase_ != Phase::Grace && phase_ != Phase::Connecting) || expired(now) ||
            (phase_ == Phase::Connecting && now-phaseBegan_ >= std::chrono::seconds(30))) return false;
        expireAttempts(now);
        phase_ = Phase::Idle;
        return true;
    }
private:
    enum class Phase { Idle, Grace, Backoff, Connecting, Cancelled, Exhausted };
    void expireAttempts(Time now) {
        unsigned expiredCount = 0;
        while (expiredCount < attempts_ &&
               now-attemptStarts_[expiredCount] >= std::chrono::seconds(120)) ++expiredCount;
        for (unsigned i = expiredCount; i < attempts_; ++i)
            attemptStarts_[i-expiredCount] = attemptStarts_[i];
        attempts_ -= expiredCount;
    }
    bool expired(Time now) const { return now-windowBegan_ >= std::chrono::seconds(90); }
    Phase phase_ = Phase::Idle;
    unsigned attempts_ = 0;
    std::array<Time, MaxAttempts> attemptStarts_{};
    Time windowBegan_{}, phaseBegan_{};
};
}
