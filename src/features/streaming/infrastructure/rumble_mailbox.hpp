#pragma once
#include "features/streaming/domain/rumble.hpp"
#include <mutex>
#include <optional>

namespace stream {
// The app sends one logical gamepad (slot 0). Keep only its newest motor state,
// including stops, so network bursts cannot build a backlog of vibration.
class RumbleMailbox {
public:
    void put(const Rumble& value) {
        if (value.controller != 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ = value;
    }
    bool take(Rumble& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_) return false;
        value = *pending_;
        pending_.reset();
        return true;
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.reset();
    }
private:
    std::mutex mutex_;
    std::optional<Rumble> pending_;
};
}
