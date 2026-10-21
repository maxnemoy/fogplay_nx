#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>

namespace stream {
// Application-thread outbox. Ordered input retains down/up order; reserve
// capacity for releases even when ordinary commands saturate the queue.
class ControlQueue {
public:
    enum class Mode { Ordered, Gamepad, MouseMove, Pong, Release };
    struct Message { std::string payload, input; Mode mode; };
    static constexpr std::size_t MaxMessages = 64, ReleaseReserve = 16;
    static constexpr std::size_t MaxBytes = 256 * 1024, ReleaseBytes = 16 * 1024;
    static constexpr std::size_t GamepadBytes = 64 * 1024;
    static constexpr std::uint64_t StallMs = 15000;
    bool push(std::string payload, Mode mode, std::string input, std::uint64_t now) {
        if (mode == Mode::Release && !input.empty()) {
            // Multiple best-effort releases for an already released key/button
            // need one entry, unless a newer press for that input intervened.
            for (auto it = queue_.rbegin(); it != queue_.rend(); ++it)
                if (it->input == input) {
                    if (it->mode == Mode::Release) return true;
                    break;
                }
        }
        auto replaced = queue_.end();
        if (mode == Mode::Gamepad || mode == Mode::Pong || mode == Mode::MouseMove) {
            for (auto it = queue_.end(); it != queue_.begin();) {
                --it;
                // A pointer position before a click must stay before that click.
                if (mode == Mode::MouseMove && (it->mode == Mode::Ordered || it->mode == Mode::Release)) break;
                if (it->mode == mode) { replaced = it; break; }
            }
        }
        const bool gamepad = mode == Mode::Gamepad;
        const bool release = mode == Mode::Release;
        const auto retainedCount = queue_.size() - (replaced == queue_.end() ? 0 : 1);
        const auto retainedBytes = bytes_ - (replaced == queue_.end() ? 0 : replaced->payload.size());
        std::size_t stateBytes = 0, stateCount = 0;
        for (auto it = queue_.begin(); it != queue_.end(); ++it)
            if (it != replaced && it->mode == Mode::Gamepad) {
                stateBytes = it->payload.size(); stateCount = 1; break;
            }
        // One independent latest-state slot also guarantees a neutral gamepad
        // can be queued after the ordered command/release budgets are full.
        const auto countLimit = gamepad ? MaxMessages :
            MaxMessages - 1 + stateCount - (release ? 0 : ReleaseReserve);
        const auto byteLimit = gamepad ? MaxBytes :
            MaxBytes - GamepadBytes + stateBytes - (release ? 0 : ReleaseBytes);
        if ((gamepad && payload.size() > GamepadBytes) || retainedCount >= countLimit ||
            payload.size() > byteLimit - std::min(retainedBytes, byteLimit)) {
            ++rejected_; return false;
        }
        // Admit replacements before removing the previous state: rejected
        // oversize updates must leave the last accepted state deliverable.
        if (replaced != queue_.end()) queue_.erase(replaced);
        if (!pending_) { pending_ = true; began_ = now; }
        bytes_ = retainedBytes + payload.size();
        queue_.push_back({std::move(payload), std::move(input), mode});
        return true;
    }
    bool empty() const { return queue_.empty(); }
    const Message& front() const { return queue_.front(); }
    void pop(std::uint64_t now) {
        bytes_ -= queue_.front().payload.size(); queue_.pop_front();
        // Sending is progress even while more messages remain. Coalescing or
        // admitting new messages alone must never extend this stall deadline.
        if (queue_.empty()) pending_ = false;
        else began_ = std::max(began_, now);
    }
    bool stalled(std::uint64_t now) const { return pending_ && now >= began_ && now - began_ >= StallMs; }
    std::size_t size() const { return queue_.size(); }
    std::size_t bytes() const { return bytes_; }
    std::uint64_t rejected() const { return rejected_; }
    void clear() { queue_.clear(); bytes_ = 0; pending_ = false; }
private:
    std::deque<Message> queue_;
    std::size_t bytes_ = 0;
    std::uint64_t began_ = 0, rejected_ = 0;
    bool pending_ = false;
};
}
