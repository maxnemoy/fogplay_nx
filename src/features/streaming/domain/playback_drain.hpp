
#pragma once
#include <cstddef>
#include <cstdint>

namespace fog::stream {
enum class PlaybackDrainPolicy { PresentationFirst, BoundedQueue, LegacyPresentationFirst, ImmediatePresentationFirst };

// Each atomic decoder operation may cross the time budget once. Leave the next
// compressed frame queued, preserving its references and yielding to input/UI.
class PlaybackDrainBudget {
public:
    enum class Yield { None, Time, Video, Packets };
    explicit PlaybackDrainBudget(std::size_t queuedVideo, std::uint64_t oldestMs = 0,
                                 PlaybackDrainPolicy policy = PlaybackDrainPolicy::PresentationFirst,
                                 int framesPerSecond = 60)
        // Fresh two-frame bursts get separate presentation turns. Catch up only
        // after two frame intervals or at a depth of three; keep references FIFO.
        : catchup_(policy == PlaybackDrainPolicy::BoundedQueue
            ? queuedVideo >= 3 || (queuedVideo >= 2 && oldestMs >= 16)
            : policy == PlaybackDrainPolicy::LegacyPresentationFirst ? queuedVideo >= 3 && oldestMs >= 80
            : policy == PlaybackDrainPolicy::ImmediatePresentationFirst ? queuedVideo >= 2
            : queuedVideo >= 3 || (queuedVideo >= 2 && oldestMs >= pairAgeThresholdMs(framesPerSecond))),
          maxVideo_((policy == PlaybackDrainPolicy::PresentationFirst ||
                     policy == PlaybackDrainPolicy::ImmediatePresentationFirst) && queuedVideo >= 3 ? 3 : catchup_ ? 2 : 1) {}
    static std::uint64_t pairAgeThresholdMs(int framesPerSecond) {
        const auto fps = static_cast<unsigned>(framesPerSecond > 0 ? framesPerSecond : 60);
        // Queue timestamps have millisecond precision; round up, never shorten
        // the two-interval guard (34 ms at 60 FPS, 67 ms at 30 FPS).
        return 2000u / fps + (2000u % fps != 0);
    }
    Yield yield(std::uint64_t elapsedUs) const {
        if (!packets_) return Yield::None;
        if (elapsedUs >= (catchup_ ? 12000u : 8000u)) return Yield::Time;
        if (video_ >= maxVideo_) return Yield::Video;
        if (packets_ >= 12) return Yield::Packets;
        return Yield::None;
    }
    void consumed(bool video) { ++packets_; if (video) ++video_; }
    bool catchup() const { return catchup_; }
    std::size_t packets() const { return packets_; }
    std::size_t video() const { return video_; }
private:
    bool catchup_;
    std::size_t maxVideo_;
    std::size_t packets_ = 0, video_ = 0;
};
}
