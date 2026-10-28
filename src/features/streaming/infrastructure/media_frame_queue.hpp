#pragma once
#include "features/streaming/domain/frame_cadence.hpp"
#include <chrono>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace fog::detail {
// External synchronization. Only converted CPU pixels cross the worker/UI boundary;
// no decoder surface, renderer or transport lifetime escapes into this queue.
class MediaFrameQueue {
public:
    using Clock = std::chrono::steady_clock;
    struct Frame {
        std::vector<unsigned char> pixels;
        int width = 0, height = 0;
        Clock::time_point due{};
    };
    static constexpr std::size_t capacity = 3;
    void reset(int fps) {
        frames_.clear();
        interval_ = std::chrono::nanoseconds(1000000000 / (fps > 0 ? fps : 30));
        initialized_ = false;
        cadence_.configure(fps);
    }
    std::uint64_t push(Frame frame, std::uint32_t timestamp, Clock::time_point now) {
        if (!initialized_) {
            nextDue_ = now;
            initialized_ = true;
        } else {
            // RTP wrap is intentional. Ignore absent/backwards/implausible clocks.
            const auto ticks = static_cast<std::int32_t>(timestamp - lastTimestamp_);
            const auto step = ticks > 0 && ticks <= 9000
                ? std::chrono::nanoseconds(static_cast<std::int64_t>(ticks) * 1000000000 / 90000)
                : interval_;
            nextDue_ += step;
            // A long receive stall must not create a permanent stale timeline.
            if (nextDue_ + interval_ * 3 < now) nextDue_ = now;
        }
        lastTimestamp_ = timestamp;
        std::uint64_t dropped = 0;
        if (frames_.size() == capacity) {
            frames_.pop_front(); ++dropped;
            // Dropped burst frames must not reserve future display slots. Keep
            // at most the retained three frames on the presentation timeline.
            if (frames_.front().due > now) {
                const auto shift = frames_.front().due - now;
                for (auto& queued : frames_) queued.due -= shift;
                nextDue_ -= shift;
            }
        }
        frame.due = nextDue_;
        frames_.push_back(std::move(frame));
        return dropped;
    }
    bool take(Frame& output, Clock::time_point now, std::uint64_t& dropped) {
        constexpr auto tolerance = std::chrono::milliseconds(1);
        if (frames_.empty() || frames_.front().due > now + tolerance) return false;
        // A fresh receive burst can rebase retained frame deadlines. Preserve
        // the established presentation cadence instead of immediately uploading.
        if (!cadence_.due(now)) return false;
        // Skip only frames already late, retaining future presentation deadlines.
        while (frames_.size() > 1 && frames_[1].due <= now + tolerance) {
            frames_.pop_front(); ++dropped;
        }
        output = std::move(frames_.front()); frames_.pop_front();
        // Keep phase across slightly early/late VSYNCs. Anchoring every upload
        // to its actual time would turn a 33.2 ms pair of refreshes into 20 fps.
        cadence_.uploaded(now);
        return true;
    }
    std::size_t size() const { return frames_.size(); }
private:
    std::deque<Frame> frames_;
    bool initialized_ = false;
    FrameCadence cadence_;
    std::uint32_t lastTimestamp_ = 0;
    Clock::time_point nextDue_{};
    std::chrono::nanoseconds interval_{33333333};
};
}
