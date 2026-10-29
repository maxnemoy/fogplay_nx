#pragma once
#include "core/diagnostics/latency_histogram.hpp"
#include <array>
#include <cstdint>
#include <string>

namespace fog::detail {
struct VideoFrameOrigin {
    std::uint64_t firstPacketMs = 0, assembledQueueWaitUs = 0;
    bool hasQueueWait = false;
};
// Decoder-owned metadata, keyed by the PTS carried from packet to AVFrame.
// Never attribute a delayed/reordered output to the currently submitted packet.
class VideoFrameOrigins {
public:
    static constexpr std::size_t Capacity = 64;
    void remember(std::int64_t token, VideoFrameOrigin origin) {
        entries_[next_] = {token, origin};
        next_ = (next_ + 1) % Capacity;
    }
    VideoFrameOrigin find(std::int64_t token, bool& found) const {
        for (const auto& entry : entries_) if (entry.token == token && token > 0) {
            found = true; return entry.origin;
        }
        found = false; return {};
    }
    void clear() { entries_ = {}; next_ = 0; }
private:
    struct Entry { std::int64_t token = 0; VideoFrameOrigin origin; };
    std::array<Entry, Capacity> entries_{};
    std::size_t next_ = 0;
};
// Application-thread only. A sample represents a new frame successfully drawn
// and acknowledged after SDL_RenderPresent returned, never a retained redraw.
class VideoLatencyMetrics {
public:
    void presented(VideoFrameOrigin origin, std::uint64_t nowUs) {
        if (origin.firstPacketMs && origin.firstPacketMs <= nowUs / 1000)
            frameAge_.record(nowUs - origin.firstPacketMs * 1000, nowUs);
        else ++unavailableAge_;
        if (origin.hasQueueWait) queueWait_.record(origin.assembledQueueWaitUs, nowUs);
        else ++unavailableWait_;
        if (havePrevious_ && nowUs >= previousUs_) gap_.record(nowUs - previousUs_, nowUs);
        previousUs_ = nowUs; havePrevious_ = true;
    }
    std::string diagnostics(std::uint64_t nowUs, int fps) const {
        const auto gap = gap_.snapshot(nowUs);
        const auto threshold = std::uint64_t(1500000) / (fps > 0 ? fps : 30);
        const auto late = gap_.countAbove(threshold, nowUs);
        return "videoLatency windowMs=5000 sample=completed-new-present timestampResolutionMs=1 "
            "frameAgeSource=local-first-packet-to-sdl-return frameAge{" + frameAge_.text(nowUs) +
            "} assembledQueueWait{" + queueWait_.text(nowUs) + "} presentGap{" + gap.text() +
            "} lateThresholdUs=" + std::to_string(threshold) + " lateCount=" + std::to_string(late) +
            " latePpm=" + std::to_string(gap.samples ? late * 1000000 / gap.samples : 0) +
            " frameAgeUnavailableLifetime=" + std::to_string(unavailableAge_) +
            " queueWaitUnavailableLifetime=" + std::to_string(unavailableWait_);
    }
private:
    metrics::LatencyHistogram frameAge_, queueWait_, gap_;
    std::uint64_t previousUs_ = 0, unavailableAge_ = 0, unavailableWait_ = 0;
    bool havePrevious_ = false;
};
}
