#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>

namespace stream {
// Outbox, externally serialized by its owner. Ordered input retains down/up order; reserve
// capacity for releases even when ordinary commands saturate the queue.
class ControlQueue {
public:
    enum class Mode { Ordered, Gamepad, MouseMove, Pong, Release, GamepadEdge, GamepadRelease, TouchMove, Motion, GamepadNeutral };
    struct InputTrace {
        bool playStation = false, touchDown = false, touchEdge = false;
        std::uint64_t sequence = 0;
        unsigned nativeInput = 0; // 1=touch down, 2=move, 3=up, 4=accel, 5=gyro.
    };
    struct Message { std::string payload, input; Mode mode; std::uint64_t enqueuedMs = 0; std::string release; InputTrace trace{}; };
    static constexpr std::size_t MaxMessages = 64, ReleaseReserve = 16;
    static constexpr std::size_t MaxBytes = 256 * 1024, ReleaseBytes = 16 * 1024;
    static constexpr std::size_t GamepadBytes = 64 * 1024;
    static constexpr std::uint64_t StallMs = 15000;
    bool push(std::string payload, Mode mode, std::string input, std::uint64_t now, std::string releasePayload = {}, InputTrace trace = {false, false, false, 0, 0}) {
        if (mode == Mode::Release && !input.empty()) {
            // Multiple best-effort releases for an already released key/button
            // need one entry, unless a newer press for that input intervened.
            for (auto it = queue_.rbegin(); it != queue_.rend(); ++it)
                if (it->input == input) {
                    if (it->mode == Mode::Release) {
                        ++coalesced_;
                        if (trace.nativeInput < nativeCoalesced_.size()) ++nativeCoalesced_[trace.nativeInput];
                        return true;
                    }
                    break;
                }
        }
        // Edges can separate several analog snapshots. Reclaim only redundant
        // analog entries when they consume the neutral reserve; every button
        // transition and its relative order must remain deliverable.
        if (mode == Mode::GamepadNeutral && !reserveNeutral(payload.size(), releasePayload.size())) {
            ++rejected_; return false;
        }
        auto replaced = queue_.end();
        if (mode == Mode::Gamepad || mode == Mode::Pong || mode == Mode::MouseMove || mode == Mode::TouchMove || mode == Mode::Motion) {
            for (auto it = queue_.end(); it != queue_.begin();) {
                --it;
                // Every gamepad button transition is an ordered boundary. A later
                // analog snapshot must never replace a state before that edge.
                if (mode == Mode::Gamepad && (it->mode == Mode::GamepadEdge || it->mode == Mode::GamepadRelease ||
                    it->mode == Mode::GamepadNeutral)) break;
                // A pointer position before a click must stay before that click.
                if (mode == Mode::MouseMove && (it->mode == Mode::Ordered || it->mode == Mode::Release)) break;
                if (mode == Mode::TouchMove && (it->mode == Mode::Ordered || it->mode == Mode::Release)) break;
                if (it->mode == mode && ((mode != Mode::TouchMove && mode != Mode::Motion) || it->input == input)) { replaced = it; break; }
            }
        }
        const bool gamepad = mode == Mode::Gamepad || mode == Mode::GamepadNeutral;
        const bool release = mode == Mode::Release || mode == Mode::GamepadRelease || mode == Mode::GamepadNeutral;
        const auto retainedCount = queue_.size() - (replaced == queue_.end() ? 0 : 1);
        const auto retainedBytes = bytes_ - (replaced == queue_.end() ? 0 : (replaced->payload.size() + replaced->release.size()));
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
            payload.size() + releasePayload.size() > byteLimit - std::min(retainedBytes, byteLimit)) {
            ++rejected_; return false;
        }
        // Admit replacements before removing the previous state: rejected
        // oversize updates must leave the last accepted state deliverable.
        if (replaced != queue_.end()) {
            if (replaced->trace.nativeInput < nativeCoalesced_.size()) ++nativeCoalesced_[replaced->trace.nativeInput];
            queue_.erase(replaced); ++coalesced_;
        }
        if (!pending_) { pending_ = true; began_ = now; }
        bytes_ = retainedBytes + payload.size() + releasePayload.size();
        queue_.push_back({std::move(payload), std::move(input), mode, now, std::move(releasePayload), trace});
        return true;
    }
    bool empty() const { return queue_.empty(); }
    const Message& front() const { return queue_.front(); }
    void pop(std::uint64_t now) {
        bytes_ -= queue_.front().payload.size() + queue_.front().release.size(); queue_.pop_front();
        // Sending is progress even while more messages remain. Coalescing or
        // admitting new messages alone must never extend this stall deadline.
        if (queue_.empty()) pending_ = false;
        else began_ = std::max(began_, now);
    }
    void progress(std::uint64_t now) {
        if (pending_) began_ = std::max(began_, now);
    }
    bool stalled(std::uint64_t now) const { return pending_ && now >= began_ && now - began_ >= StallMs; }
    std::size_t size() const { return queue_.size(); }
    std::size_t bytes() const { return bytes_; }
    std::uint64_t rejected() const { return rejected_; }
    std::uint64_t coalesced() const { return coalesced_; }
    const std::array<std::uint64_t, 6>& nativeCoalesced() const { return nativeCoalesced_; }
    std::array<std::uint64_t, 6> nativePending() const {
        std::array<std::uint64_t, 6> result{};
        for (const auto& message : queue_)
            if (message.trace.nativeInput < result.size()) ++result[message.trace.nativeInput];
        return result;
    }
    std::size_t touchEdges() const {
        return std::count_if(queue_.begin(), queue_.end(), [](const Message& message) { return message.trace.touchEdge; });
    }
    void clear() { queue_.clear(); bytes_ = 0; pending_ = false; }
private:
    bool reserveNeutral(std::size_t payloadBytes, std::size_t releaseBytes) {
        if (payloadBytes > GamepadBytes || releaseBytes > MaxBytes - payloadBytes) return false;
        const auto incomingBytes = payloadBytes + releaseBytes;
        auto retainedCount = queue_.size(), retainedBytes = bytes_;
        std::size_t removeStates = 0;
        for (const auto& message : queue_) {
            if (retainedCount < MaxMessages && incomingBytes <= MaxBytes - retainedBytes) break;
            if (message.mode != Mode::Gamepad) continue;
            --retainedCount;
            retainedBytes -= message.payload.size() + message.release.size();
            ++removeStates;
        }
        if (retainedCount >= MaxMessages || incomingBytes > MaxBytes - retainedBytes) return false;
        // Validate the whole admission before erasing anything. An invalid or
        // oversized neutral must leave the previously accepted queue intact.
        for (auto it = queue_.begin(); it != queue_.end() && removeStates;) {
            if (it->mode != Mode::Gamepad) { ++it; continue; }
            if (it->trace.nativeInput < nativeCoalesced_.size()) ++nativeCoalesced_[it->trace.nativeInput];
            bytes_ -= it->payload.size() + it->release.size();
            it = queue_.erase(it);
            --removeStates;
            ++coalesced_;
        }
        return true;
    }
    std::deque<Message> queue_;
    std::size_t bytes_ = 0;
    std::uint64_t began_ = 0, rejected_ = 0, coalesced_ = 0;
    bool pending_ = false;
    std::array<std::uint64_t, 6> nativeCoalesced_{};
};
}
