#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace fog::stream {
// Fixed storage, application-thread only. Quantiles are histogram upper bounds:
// 2 ms buckets through 256 ms; overflow uses the largest observed duration.
struct LoopDuration {
    static constexpr std::uint64_t BucketUs = 2000;
    static constexpr std::size_t RegularBuckets = 128;
    std::array<std::uint64_t, RegularBuckets + 1> buckets{};
    std::uint64_t calls = 0, totalUs = 0, minUs = 0, maxUs = 0, slow50ms = 0;
    void record(std::uint64_t us) noexcept {
        if (!calls || us < minUs) minUs = us;
        ++calls; totalUs += us; maxUs = std::max(maxUs, us);
        if (us >= 50000) ++slow50ms;
        const auto bucket = us ? (us - 1) / BucketUs : 0;
        ++buckets[std::min<std::uint64_t>(bucket, RegularBuckets)];
    }
    std::uint64_t percentileUpper(unsigned percent) const noexcept {
        if (!calls) return 0;
        const auto rank = (calls * percent + 99) / 100;
        std::uint64_t seen = 0;
        for (std::size_t n = 0; n < buckets.size(); ++n) {
            seen += buckets[n];
            if (seen >= rank) return n == RegularBuckets ? maxUs :
                std::min(maxUs, static_cast<std::uint64_t>(n + 1) * BucketUs);
        }
        return maxUs;
    }
    std::string text(const char* name) const {
        return std::string(name) + "N=" + std::to_string(calls) + " " + name + "AvgUs=" +
            std::to_string(calls ? totalUs / calls : 0) + " " + name + "MinUs=" + std::to_string(minUs) +
            " " + name + "MaxUs=" + std::to_string(maxUs) + " " + name + "P95UpperUs=" +
            std::to_string(percentileUpper(95)) + " " + name + "P99UpperUs=" +
            std::to_string(percentileUpper(99)) + " " + name + "Slow50ms=" + std::to_string(slow50ms) +
            " " + name + "Overflow=" + std::to_string(buckets.back());
    }
};
class LoopTiming {
public:
    using Clock = std::chrono::steady_clock;
    enum class Stage : std::size_t { Loop, Events, Poll, Pump, Input, Draw, Artwork, MediaDraw, Overlay, Present, Count };
    struct Snapshot {
        std::array<LoopDuration, static_cast<std::size_t>(Stage::Count)> stages{};
        LoopDuration newPresentGap;
        std::uint64_t anchors = 0, suppressedFrames = 0, continuityBreaks = 0;
        const LoopDuration& at(Stage stage) const { return stages[static_cast<std::size_t>(stage)]; }
    };
    Snapshot window, lifetime;
    void reset() noexcept {
        window = {}; lifetime = {}; ++epoch_;
        active_ = anchored_ = false; lastPresented_ = 0;
    }
    void clearWindow() noexcept { window = {}; }
    void suspendPresentation() noexcept {
        if (anchored_) { ++window.continuityBreaks; ++lifetime.continuityBreaks; }
        anchored_ = false;
    }
    void resetPresentationCounter() noexcept {
        suspendPresentation(); lastPresented_ = 0;
    }
    void presented(std::uint64_t count, bool eligible, Clock::time_point now) noexcept {
        if (count < lastPresented_) suspendPresentation();
        const bool fresh = count && count != lastPresented_;
        lastPresented_ = count;
        if (!eligible) {
            suspendPresentation();
            if (fresh) { ++window.suppressedFrames; ++lifetime.suppressedFrames; }
            return;
        }
        if (!fresh) return; // Repeated texture presents never move the anchor.
        if (anchored_) {
            const auto gap = elapsed(lastPresentAt_, now);
            window.newPresentGap.record(gap); lifetime.newPresentGap.record(gap);
        } else { ++window.anchors; ++lifetime.anchors; }
        lastPresentAt_ = now; anchored_ = true;
    }
    std::string activeText(Clock::time_point now) const {
        return "activeLoop=" + std::to_string(active_) + " activePhase=" + name(phase_) +
            " activeLoopUs=" + std::to_string(active_ ? elapsed(loopAt_, now) : 0) +
            " activePhaseUs=" + std::to_string(active_ ? elapsed(phaseAt_, now) : 0);
    }
    static const char* name(Stage stage) noexcept {
        constexpr std::array<const char*, 11> names{{"loop","events","poll","pumpTotal","input","draw","artwork","mediaDraw","overlay","present","none"}};
        return names[static_cast<std::size_t>(stage)];
    }
    // No allocations/logging in scope construction or destruction. Resetting
    // for a newly started rental invalidates scopes from its previous UI cycle.
    class Scope {
    public:
        Scope(LoopTiming& owner, Stage stage, bool enabled = true, Clock::time_point now = Clock::now()) noexcept
            : owner_(owner), stage_(stage), previous_(owner.phase_), previousAt_(owner.phaseAt_), start_(now),
              epoch_(owner.epoch_), enabled_(enabled && (stage == Stage::Loop || owner.active_)) {
            if (!enabled_) return;
            if (stage == Stage::Loop) { owner.active_ = true; owner.loopAt_ = now; }
            owner.phase_ = stage; owner.phaseAt_ = now;
        }
        ~Scope() noexcept { finish(); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        void finish(Clock::time_point now = Clock::now()) noexcept {
            if (!enabled_) return;
            enabled_ = false;
            if (epoch_ != owner_.epoch_) return;
            const auto us = elapsed(start_, now);
            owner_.window.stages[static_cast<std::size_t>(stage_)].record(us);
            owner_.lifetime.stages[static_cast<std::size_t>(stage_)].record(us);
            owner_.phase_ = previous_; owner_.phaseAt_ = previousAt_;
            if (stage_ == Stage::Loop) owner_.active_ = false;
        }
    private:
        LoopTiming& owner_;
        Stage stage_, previous_;
        Clock::time_point previousAt_, start_;
        std::uint64_t epoch_;
        bool enabled_;
    };
private:
    static std::uint64_t elapsed(Clock::time_point start, Clock::time_point end) noexcept {
        return end > start ? static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end-start).count()) : 0;
    }
    bool active_ = false, anchored_ = false;
    Stage phase_ = Stage::Count;
    Clock::time_point loopAt_{}, phaseAt_{}, lastPresentAt_{};
    std::uint64_t epoch_ = 0, lastPresented_ = 0;
};
}
