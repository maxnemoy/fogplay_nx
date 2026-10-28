#pragma once
#include "features/settings/domain/rumble_settings.hpp"
#include "features/streaming/domain/rumble_playback.hpp"
#include <switch.h>
#include <array>
#include <string>

namespace fogplay::platform {
// Only the application thread calls HID. The logical gamepad uses the same
// handheld/player-one selection as padInitializeDefault().
class Rumble {
public:
    Rumble() = default;
    ~Rumble();
    Rumble(const Rumble&) = delete;
    Rumble& operator=(const Rumble&) = delete;
    void update(const PadState& pad, const stream::Rumble* feedback,
                bool enabled, std::uint64_t nowMs) noexcept;
    void stop(std::uint64_t nowMs) noexcept;
    void setMode(fog::rumble::Mode mode, std::uint64_t nowMs) noexcept;
    std::string diagnostics() const;
private:
    void bind(const PadState& pad, std::uint64_t nowMs) noexcept;
    void apply(std::uint64_t nowMs, bool force = false) noexcept;
    stream::RumblePlayback playback_;
    fog::rumble::Mode mode_ = fog::rumble::Mode::Balanced;
    std::array<HidVibrationDeviceHandle, 2> handles_{};
    int npadId_ = -1;
    std::uint32_t style_ = 0;
    unsigned count_ = 0;
    std::uint16_t appliedLow_ = 0, appliedHigh_ = 0;
    bool dirty_ = false;
    std::uint64_t nextBindMs_ = 0, nextSendMs_ = 0;
    std::uint64_t received_ = 0, applied_ = 0, stops_ = 0, failures_ = 0;
    Result lastResult_ = 0;
};
}
