#pragma once
#include <cstddef>
#include <cstdint>

namespace fog::stream {
// Each atomic decoder operation may cross the time budget once. Leave the next
// compressed frame queued, preserving its references and yielding to input/UI.
class PlaybackDrainBudget {
public:
    enum class Yield { None, Time, Video, Packets };
    explicit PlaybackDrainBudget(std::size_t queuedVideo, std::uint64_t oldestMs = 0)
        : catchup_(queuedVideo >= 3 && oldestMs >= 80) {}
    Yield yield(std::uint64_t elapsedUs) const {
        if (!packets_) return Yield::None;
        if (elapsedUs >= (catchup_ ? 12000u : 8000u)) return Yield::Time;
        if (video_ >= (catchup_ ? 2u : 1u)) return Yield::Video;
        if (packets_ >= 12) return Yield::Packets;
        return Yield::None;
    }
    void consumed(bool video) { ++packets_; if (video) ++video_; }
    bool catchup() const { return catchup_; }
    std::size_t packets() const { return packets_; }
    std::size_t video() const { return video_; }
private:
    bool catchup_;
    std::size_t packets_ = 0, video_ = 0;
};
}
