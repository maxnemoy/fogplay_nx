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
    struct Snapshot {
        std::array<metrics::LatencyHistogram::Snapshot,3> stages{};
        std::uint64_t lateThresholdUs=0,lateCount=0,latePpm=0,unavailableAge=0,unavailableWait=0;
    };
    void presented(VideoFrameOrigin origin, std::uint64_t nowUs) {
        if (origin.firstPacketMs && origin.firstPacketMs <= nowUs / 1000)
            frameAge_.record(nowUs - origin.firstPacketMs * 1000, nowUs);
        else ++unavailableAge_;
        if (origin.hasQueueWait) queueWait_.record(origin.assembledQueueWaitUs, nowUs);
        else ++unavailableWait_;
        if (havePrevious_ && nowUs >= previousUs_) gap_.record(nowUs - previousUs_, nowUs);
        previousUs_ = nowUs; havePrevious_ = true;
    }
    Snapshot snapshot(std::uint64_t nowUs,int fps) const {
        Snapshot value;
        value.stages={frameAge_.snapshot(nowUs),queueWait_.snapshot(nowUs),gap_.snapshot(nowUs)};
        const auto threshold = std::uint64_t(1500000) / (fps > 0 ? fps : 30);
        const auto late = gap_.countAbove(threshold, nowUs);
        value.lateThresholdUs=threshold;value.lateCount=late;
        value.latePpm=value.stages[2].samples?late*1000000/value.stages[2].samples:0;
        value.unavailableAge=unavailableAge_;value.unavailableWait=unavailableWait_;
        return value;
    }
    std::string diagnostics(std::uint64_t nowUs, int fps) const {
        const auto value=snapshot(nowUs,fps);
        return "videoLatency windowMs=5000 sample=completed-new-present timestampResolutionMs=1 "
            "frameAgeSource=local-first-packet-to-sdl-return frameAge{" + value.stages[0].text() +
            "} assembledQueueWait{" + value.stages[1].text() + "} presentGap{" + value.stages[2].text() +
            "} lateThresholdUs=" + std::to_string(value.lateThresholdUs) + " lateCount=" + std::to_string(value.lateCount) +
            " latePpm=" + std::to_string(value.latePpm) +
            " frameAgeUnavailableLifetime=" + std::to_string(value.unavailableAge) +
            " queueWaitUnavailableLifetime=" + std::to_string(value.unavailableWait);
    }
private:
    metrics::LatencyHistogram frameAge_, queueWait_, gap_;
    std::uint64_t previousUs_ = 0, unavailableAge_ = 0, unavailableWait_ = 0;
    bool havePrevious_ = false;
};
}
