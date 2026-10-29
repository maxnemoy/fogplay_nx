#pragma once
#include "features/streaming/domain/frame_cadence.hpp"
#include "features/streaming/infrastructure/video_latency.hpp"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
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
        Clock::time_point readyAt{};
        VideoFrameOrigin origin;
    };
    struct Stats {
        std::uint64_t readyEmpty = 0, timingCadenceBlocked = 0, timingRtpBlocked = 0;
        std::uint64_t pushCapacityDrop = 0, takeStaleDrop = 0, timedLateCoalesce = 0, taken = 0, resets = 0;
        bool displayPaced = false;
        int framesPerSecond = 30;
        // Format an immutable stats() copy after releasing the external queue lock.
        std::string diagnostics() const {
            char line[352];
            std::snprintf(line, sizeof(line),
                "displayPaced=%d queueFps=%d readyEmpty=%llu cadenceBlocked=%llu rtpBlocked=%llu "
                "pushCapacityDrop=%llu takeStaleDrop=%llu timedLateCoalesce=%llu taken=%llu resets=%llu",
                displayPaced ? 1 : 0, framesPerSecond,
                static_cast<unsigned long long>(readyEmpty), static_cast<unsigned long long>(timingCadenceBlocked),
                static_cast<unsigned long long>(timingRtpBlocked), static_cast<unsigned long long>(pushCapacityDrop),
                static_cast<unsigned long long>(takeStaleDrop), static_cast<unsigned long long>(timedLateCoalesce),
                static_cast<unsigned long long>(taken), static_cast<unsigned long long>(resets));
            return line;
        }
    };
    static constexpr std::size_t capacity = 3;
    // displayPaced is valid only for 60 fps and a caller that takes at most once
    // per VSYNC-backed render cycle. Other modes retain RTP/cadence scheduling.
    // Reset drops buffered pixels and timing state, retaining lifetime counters.
    void reset(int fps, bool displayPaced = false) {
        frames_.clear();
        interval_ = std::chrono::nanoseconds(1000000000 / (fps > 0 ? fps : 30));
        initialized_ = false;
        cadence_.configure(fps);
        stats_.displayPaced = displayPaced && fps == 60;
        stats_.framesPerSecond = fps > 0 ? fps : 30;
        ++stats_.resets;
    }
    std::uint64_t push(Frame frame, std::uint32_t timestamp, Clock::time_point now) {
        frame.readyAt = now;
        if (stats_.displayPaced) {
            std::uint64_t dropped = 0;
            if (frames_.size() == capacity) {
                frames_.pop_front(); ++dropped; ++stats_.pushCapacityDrop;
            }
            frame.due = now;
            frames_.push_back(std::move(frame));
            return dropped;
        }
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
            frames_.pop_front(); ++dropped; ++stats_.pushCapacityDrop;
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
        if (frames_.empty()) { ++stats_.readyEmpty; return false; }
        if (stats_.displayPaced) {
            // Ready pixels already crossed the decoder boundary. The display
            // supplies cadence; an additional RTP gate would discard good frames
            // when refresh timestamps jitter around the 60 Hz boundary.
            while (frames_.size() > 1 && now > frames_.front().readyAt && now - frames_.front().readyAt > interval_ * 2) {
                frames_.pop_front(); ++dropped; ++stats_.takeStaleDrop;
            }
            output = std::move(frames_.front()); frames_.pop_front(); ++stats_.taken;
            return true;
        }
        constexpr auto tolerance = std::chrono::milliseconds(1);
        if (frames_.front().due > now + tolerance) { ++stats_.timingRtpBlocked; return false; }
        // A fresh receive burst can rebase retained frame deadlines. Preserve
        // the established presentation cadence instead of immediately uploading.
        if (!cadence_.due(now)) { ++stats_.timingCadenceBlocked; return false; }
        // Skip only frames already late, retaining future presentation deadlines.
        while (frames_.size() > 1 && frames_[1].due <= now + tolerance) {
            frames_.pop_front(); ++dropped; ++stats_.timedLateCoalesce;
        }
        output = std::move(frames_.front()); frames_.pop_front();
        // Keep phase across slightly early/late VSYNCs. Anchoring every upload
        // to its actual time would turn a 33.2 ms pair of refreshes into 20 fps.
        cadence_.uploaded(now);
        ++stats_.taken;
        return true;
    }
    std::size_t size() const { return frames_.size(); }
    Stats stats() const { return stats_; }
private:
    std::deque<Frame> frames_;
    bool initialized_ = false;
    FrameCadence cadence_;
    std::uint32_t lastTimestamp_ = 0;
    Clock::time_point nextDue_{};
    std::chrono::nanoseconds interval_{33333333};
    Stats stats_;
};
}
