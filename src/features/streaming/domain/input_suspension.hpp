#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace stream {
// Input-only evidence. It must not change keyframe/recovery UI or presentation
// continuity: a frozen displayed texture is still part of the measured gap.
struct InputSuspension {
    bool coreWaiting = false, localWaiting = false, noRecentFrame = false;
    bool suspended() const { return coreWaiting || localWaiting || noRecentFrame; }
    unsigned mask() const { return unsigned(coreWaiting) | unsigned(localWaiting) << 1 | unsigned(noRecentFrame) << 2; }
};
}
namespace fog::stream {
// Main-thread counters. Reasons can overlap; their durations must not be added.
class InputSuspensionStats {
public:
    void observe(::stream::InputSuspension value, std::uint64_t nowMs) {
        if (initialized_ && nowMs >= lastMs_)
            for (unsigned i=0;i<3;++i) if (active_ & (1u<<i)) durationMs_[i] += nowMs-lastMs_;
        const auto next = value.mask();
        for (unsigned i=0;i<3;++i) if ((next & (1u<<i)) && !(active_ & (1u<<i))) ++count_[i];
        active_ = next; lastMs_ = nowMs; initialized_ = true;
    }
    std::string text(std::uint64_t nowMs) const {
        auto snapshot = *this;
        snapshot.observe({bool(active_ & 1),bool(active_ & 2),bool(active_ & 4)},nowMs);
        constexpr std::array<const char*,3> names{"core","local","noFrame"};
        std::string result = "activeBits=" + std::to_string(active_);
        for (unsigned i=0;i<3;++i) result += " " + std::string(names[i]) + "Count=" + std::to_string(count_[i]) +
            " " + names[i] + "Ms=" + std::to_string(snapshot.durationMs_[i]);
        return result;
    }
    std::uint64_t count(unsigned reason) const { return count_.at(reason); }
    std::uint64_t durationMs(unsigned reason) const { return durationMs_.at(reason); }
private:
    std::array<std::uint64_t,3> count_{}, durationMs_{};
    std::uint64_t lastMs_ = 0;
    unsigned active_ = 0;
    bool initialized_ = false;
};
}
