#pragma once
#include <algorithm>
#include <chrono>
#include <optional>

namespace stream {
// Keyframe (PLI) pacing for manual quality while video waits for recovery.
// Intervals are minimum times since the previous keyframe request.
//  - Within one episode (a continuous keyframe need) requests back off
//    250/500/1000/2000/4000 ms.
//  - The first request of each further episode waits 250/500/1000/2000 ms
//    (cap 2000), so a resync storm of short episodes cannot request a
//    keyframe every 250 ms; clearing stale video itself is never delayed.
//  - 30 s of continuously arriving video without a keyframe need resets the
//    episode level. A new transport or Auto quality starts from reset().
// Auto quality keeps its fixed 250 ms interval and does not use this schedule.
class RecoveryPliSchedule {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds Base{250};
    static constexpr unsigned MaxBurstShift = 4, MaxEpisodeShift = 3;
    static constexpr std::chrono::seconds StableReset{30};

    // Every pump: whether a keyframe is needed and whether video is flowing.
    void update(bool needed, bool videoFlowing, Clock::time_point now) {
        if (needed) {
            if (!inEpisode_) { inEpisode_ = true; episodeRequested_ = false; burst_ = 0; }
            stableSince_.reset();
            return;
        }
        if (inEpisode_) {
            if (episodeRequested_) episodes_ = std::min(episodes_ + 1, MaxEpisodeShift);
            inEpisode_ = false; burst_ = 0;
        }
        if (!videoFlowing) { stableSince_.reset(); return; }
        if (!stableSince_) stableSince_ = now;
        else if (now - *stableSince_ >= StableReset) { episodes_ = 0; stableSince_ = now; }
    }
    std::chrono::milliseconds interval() const {
        const unsigned shift = burst_ ? std::min(burst_, MaxBurstShift) : episodes_;
        return Base * (1u << shift);
    }
    // After a recovery keyframe request was actually sent.
    void requested() {
        if (!inEpisode_) return;
        episodeRequested_ = true;
        burst_ = std::min(burst_ + 1, MaxBurstShift);
    }
    void reset() { *this = RecoveryPliSchedule{}; }
    unsigned episodeLevel() const { return episodes_; }
private:
    bool inEpisode_ = false, episodeRequested_ = false;
    unsigned burst_ = 0, episodes_ = 0;
    std::optional<Clock::time_point> stableSince_;
};
}
