#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace fog::metrics {
inline std::uint64_t steadyNowUs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
// Owner-serialized, allocation-free recording. Exact nearest-rank percentiles
// over the last 5 seconds (at most 512 observations); overflow is explicit.
class LatencyHistogram {
public:
    static constexpr std::size_t Capacity = 512;
    static constexpr std::uint64_t WindowUs = 5000000;
    struct Snapshot {
        std::size_t samples = 0;
        std::uint64_t p50Us = 0, p95Us = 0, p99Us = 0, overflow = 0;
        std::string text() const {
            return "samples=" + std::to_string(samples) + " p50Us=" + std::to_string(p50Us) +
                " p95Us=" + std::to_string(p95Us) + " p99Us=" + std::to_string(p99Us) +
                " overflowLifetime=" + std::to_string(overflow);
        }
    };
    void record(std::uint64_t valueUs, std::uint64_t nowUs) {
        if (size_ == Capacity && live(entries_[next_], nowUs)) ++overflow_;
        entries_[next_] = {valueUs, nowUs};
        next_ = (next_ + 1) % Capacity;
        size_ = std::min(size_ + 1, Capacity);
    }
    Snapshot snapshot(std::uint64_t nowUs) const {
        std::array<std::uint64_t, Capacity> values{};
        Snapshot result;
        result.overflow = overflow_;
        for (std::size_t i = 0; i < size_; ++i)
            if (live(entries_[i], nowUs)) values[result.samples++] = entries_[i].value;
        if (!result.samples) return result;
        std::sort(values.begin(), values.begin() + result.samples);
        result.p50Us = values[(result.samples * 50 + 99) / 100 - 1];
        result.p95Us = values[(result.samples * 95 + 99) / 100 - 1];
        result.p99Us = values[(result.samples * 99 + 99) / 100 - 1];
        return result;
    }
    std::size_t countAbove(std::uint64_t thresholdUs, std::uint64_t nowUs) const {
        std::size_t count = 0;
        for (std::size_t i = 0; i < size_; ++i)
            if (live(entries_[i], nowUs) && entries_[i].value > thresholdUs) ++count;
        return count;
    }
    std::string text(std::uint64_t nowUs) const { return snapshot(nowUs).text(); }
private:
    struct Entry { std::uint64_t value = 0, at = 0; };
    static bool live(const Entry& entry, std::uint64_t nowUs) {
        return nowUs >= entry.at && nowUs - entry.at < WindowUs;
    }
    std::array<Entry, Capacity> entries_{};
    std::size_t next_ = 0, size_ = 0;
    std::uint64_t overflow_ = 0;
};
}
