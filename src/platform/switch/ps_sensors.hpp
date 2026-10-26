#pragma once
#include "features/playstation/domain/sensors.hpp"
#include <switch.h>
#include <array>
#include <cstdint>

namespace fogplay::platform {
class PsSensors {
public:
    PsSensors() = default;
    ~PsSensors();
    PsSensors(const PsSensors&) = delete;
    PsSensors& operator=(const PsSensors&) = delete;
    void start();
    void stop();
    const fog::playstation::PsSensorSnapshot& update(const PadState& pad, std::uint64_t nowMs);
    const fog::playstation::PsSensorSnapshot& snapshot() const { return state_; }

private:
    void stopHandles();
    void bind(const PadState& pad, std::uint64_t nowMs);
    void readTouch(std::uint64_t nowMs);
    void readMotion(std::uint64_t nowMs);
    fog::playstation::PsSensorSnapshot state_;
    std::array<HidSixAxisSensorHandle,2> handles_{};
    std::array<bool,2> started_{};
    bool touchObserved_ = false;
    bool motionObserved_ = false;
    std::uint64_t nextBindMs_ = 0;
};
}
