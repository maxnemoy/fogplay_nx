#pragma once
#include <array>
#include <chrono>
#include <string>

namespace fog::connection {
enum class Mode { Normal, Auto, Relay };
struct Settings { Mode mode = Mode::Normal; };
inline bool valid(const Settings& value) {
    return value.mode == Mode::Normal || value.mode == Mode::Auto || value.mode == Mode::Relay;
}
inline bool operator==(const Settings& a, const Settings& b) { return a.mode == b.mode; }
inline bool adjust(Settings& value, int direction) {
    if (!valid(value) || !direction) return false;
    value.mode = static_cast<Mode>((static_cast<int>(value.mode) + (direction > 0 ? 1 : 2)) % 3);
    return true;
}

// One break per outage, armed only by successful visible playback and ready
// control. Initial connection failures and failed retries are not new breaks.
// Once Auto falls back, keep TURN until this rental closes to avoid oscillation.
class RelaySelection {
public:
    using Time = std::chrono::steady_clock::time_point;
    void reset(Settings settings = {}) {
        mode_ = settings.mode; relay_ = mode_ == Mode::Relay;
        connected_ = false; count_ = 0;
    }
    void connected() { connected_ = true; }
    bool broken(Time now) {
        if (!connected_) return false;
        connected_ = false;
        unsigned expired = 0;
        while (expired < count_ && now-breaks_[expired] >= std::chrono::seconds(120)) ++expired;
        for (unsigned i = expired; i < count_; ++i) breaks_[i-expired] = breaks_[i];
        count_ -= expired;
        if (count_ == breaks_.size()) { breaks_[0] = breaks_[1]; --count_; }
        breaks_[count_++] = now;
        if (mode_ == Mode::Auto && count_ == breaks_.size()) relay_ = true;
        return true;
    }
    bool relay() const { return relay_; }
    unsigned breaks() const { return count_; }
private:
    Mode mode_ = Mode::Normal;
    bool relay_ = false, connected_ = false;
    unsigned count_ = 0;
    std::array<Time, 2> breaks_{};
};
}
