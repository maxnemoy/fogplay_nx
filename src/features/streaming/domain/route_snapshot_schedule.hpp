#pragma once
#include <cstdint>

namespace stream {
// App-thread schedule. Unknown pairs are retried periodically too; lifecycle
// events and newly admitted endpoint proof bypass the interval immediately.
class RouteSnapshotSchedule {
public:
    static constexpr std::uint64_t IntervalMs = 1000;
    bool due(std::uint64_t now, bool force = false) const {
        return force || !sampled_ || now < last_ || now - last_ >= IntervalMs || (dirty_ && !failed_);
    }
    void invalidate() { dirty_ = true; failed_ = false; }
    // Only after the pair API returns (including a successful no-pair result).
    // An exception must not consume pending lifecycle/provenance invalidation.
    void queried(std::uint64_t now) { last_ = now; sampled_ = true; dirty_ = failed_ = false; }
    // Keep invalidation pending after an exception, but bound retries unless a
    // new event/proof arrives. The caller keeps the previous route unauthorized.
    void failed(std::uint64_t now) { last_ = now; sampled_ = failed_ = true; }
private:
    bool dirty_ = true, sampled_ = false, failed_ = false;
    std::uint64_t last_ = 0;
};
}
