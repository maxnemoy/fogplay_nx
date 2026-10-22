#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace stream {
class TimedMutex {
public:
    enum class Scope : std::size_t { Callback, Client, Diagnostics, Count };
    static constexpr std::uint64_t SlowUs = 1000;
    struct Stats {
        std::uint64_t count = 0, waitNs = 0, holdNs = 0;
        std::uint64_t waitMaxNs = 0, holdMaxNs = 0, waitSlow = 0, holdSlow = 0;
    };
    using Snapshot = std::array<Stats, static_cast<std::size_t>(Scope::Count)>;
    class Guard {
    public:
        Guard(TimedMutex& mutex, Scope scope) : mutex_(mutex), scope_(scope) {
            const auto before = Clock::now();
            mutex_.mutex_.lock();
            acquired_ = Clock::now();
            waitNs_ = nanoseconds(acquired_ - before);
        }
        ~Guard() {
            const auto holdNs = nanoseconds(Clock::now() - acquired_);
            auto& stats = mutex_.stats_[static_cast<std::size_t>(scope_)];
            ++stats.count; stats.waitNs += waitNs_; stats.holdNs += holdNs;
            stats.waitMaxNs = std::max(stats.waitMaxNs, waitNs_);
            stats.holdMaxNs = std::max(stats.holdMaxNs, holdNs);
            stats.waitSlow += waitNs_ >= SlowUs * 1000;
            stats.holdSlow += holdNs >= SlowUs * 1000;
            mutex_.mutex_.unlock();
        }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        // Completed scopes only; the caller already owns the mutex.
        Snapshot snapshot() const { return mutex_.stats_; }
    private:
        TimedMutex& mutex_;
        Scope scope_;
        using Clock = std::chrono::steady_clock;
        Clock::time_point acquired_;
        std::uint64_t waitNs_ = 0;
        static std::uint64_t nanoseconds(Clock::duration duration) {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
        }
    };
    // Use Guard::snapshot when already locked. This read does not reset counters.
    Snapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }
    static std::string diagnostics(const Snapshot& snapshot) {
        static constexpr const char* tags[]{"Callback", "Client", "Diagnostics"};
        std::string text = "scope=transport-lifetime sharedSlowUs=" + std::to_string(SlowUs);
        for (std::size_t i = 0; i < snapshot.size(); ++i) {
            const auto& stats = snapshot[i];
            const auto count = static_cast<unsigned long long>(stats.count);
            const double divisor = stats.count ? static_cast<double>(stats.count) * 1000.0 : 1;
            char fields[400];
            std::snprintf(fields, sizeof(fields),
                " shared%sCount=%llu shared%sWaitAvgUs=%.2f shared%sWaitMaxUs=%.2f shared%sWaitSlow=%llu"
                " shared%sHoldAvgUs=%.2f shared%sHoldMaxUs=%.2f shared%sHoldSlow=%llu",
                tags[i], count, tags[i], stats.waitNs / divisor, tags[i], stats.waitMaxNs / 1000.0,
                tags[i], static_cast<unsigned long long>(stats.waitSlow),
                tags[i], stats.holdNs / divisor, tags[i], stats.holdMaxNs / 1000.0,
                tags[i], static_cast<unsigned long long>(stats.holdSlow));
            text += fields;
        }
        return text;
    }
private:
    mutable std::mutex mutex_;
    Snapshot stats_{};
};
}
