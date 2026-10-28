#pragma once
#include "features/streaming/domain/rumble.hpp"

namespace stream {
// Application-thread effect lifetime. Pausing discards feedback, rather than
// replaying it when the menu closes. A delayed packet never restarts its timer.
class RumblePlayback {
public:
    void stop(std::uint64_t nowMs) noexcept {
        current_ = {};
        clearedThroughMs_ = nowMs;
    }
    void update(const Rumble* incoming, bool enabled, std::uint64_t nowMs) noexcept {
        if (!enabled) { stop(nowMs); return; }
        if (incoming && incoming->controller == 0) {
            if (!incoming->lowFrequency && !incoming->highFrequency) current_ = {};
            else if (incoming->receivedAtMs > clearedThroughMs_ && incoming->receivedAtMs <= nowMs) {
                current_ = *incoming;
                // Never start old feedback after a blocked UI frame.
                if (nowMs - current_.receivedAtMs > 250) current_ = {};
            }
        }
        if (current_.durationMs && nowMs >= current_.receivedAtMs &&
            nowMs - current_.receivedAtMs >= current_.durationMs) current_ = {};
    }
    const Rumble& current() const noexcept { return current_; }
private:
    Rumble current_{};
    std::uint64_t clearedThroughMs_ = 0;
};
}
