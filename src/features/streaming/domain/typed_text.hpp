#pragma once
#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fog::stream {
// Ordered, paced delivery of overlay text. Each text segment is one remote text
// message; the pause lets the host's game observe separate presses of the same
// physical key across segments. Overlay keys (Enter, Tab, ...) and further text
// issued while text is pending queue behind it, so they never overtake it.
// A refused item is retried on later pumps; after RetryLimit the rest fails.
// The owner cancels the rest before other input may move the remote focus.
// Text is transient: never logged, persisted or replayed on another transport.
class TypedText {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds Interval{120};
    static constexpr std::chrono::milliseconds RetryLimit{2000};
    enum class Result { Idle, Pending, Completed, Failed };
    enum class Kind { Text, Key };
    struct Item { Kind kind; std::string value; };

    void appendText(std::vector<std::string> segments, Clock::time_point now) {
        const bool idle = items_.empty();
        for (auto& segment : segments) if (!segment.empty()) items_.push_back({Kind::Text, std::move(segment)});
        if (idle && !items_.empty()) nextAt_ = now;
    }
    void appendKey(std::string code, Clock::time_point now) {
        if (items_.empty()) nextAt_ = now;
        items_.push_back({Kind::Key, std::move(code)});
    }
    template<class Send> Result pump(Clock::time_point now, Send&& send) {
        if (items_.empty()) return Result::Idle;
        if (now < nextAt_) return Result::Pending;
        if (!send(static_cast<const Item&>(items_.front()))) {
            if (!refusedSince_) refusedSince_ = now;
            if (now - *refusedSince_ < RetryLimit) return Result::Pending;
            clear();
            return Result::Failed;
        }
        items_.pop_front();
        refusedSince_.reset();
        if (items_.empty()) return Result::Completed;
        nextAt_ = now + Interval;
        return Result::Pending;
    }
    // Drops everything not yet sent; returns how many items were dropped.
    std::size_t cancel() { const auto dropped = items_.size(); clear(); return dropped; }
    void clear() { items_.clear(); refusedSince_.reset(); nextAt_ = {}; }
    bool active() const { return !items_.empty(); }
    std::size_t remaining() const { return items_.size(); }
private:
    std::deque<Item> items_;
    Clock::time_point nextAt_{};
    std::optional<Clock::time_point> refusedSince_;
};
}
