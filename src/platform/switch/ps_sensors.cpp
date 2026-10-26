#include "platform/switch/ps_sensors.hpp"
#include <algorithm>
#include <cmath>

namespace fogplay::platform {
namespace {
constexpr std::uint64_t staleMs = 250;
constexpr float tau = 6.2831853071795864769f;

std::uint16_t scale(std::uint32_t coordinate, std::uint32_t sourceMax, std::uint32_t targetMax) {
    return static_cast<std::uint16_t>((std::min(coordinate, sourceMax) * targetMax + sourceMax / 2) / sourceMax);
}

bool finite(const HidSixAxisSensorState& sample) {
    const std::array<float,9> values{sample.angular_velocity.x, sample.angular_velocity.y,
        sample.angular_velocity.z, sample.acceleration.x, sample.acceleration.y, sample.acceleration.z,
        sample.angle.x, sample.angle.y, sample.angle.z};
    for (float value : values) if (!std::isfinite(value)) return false;
    for (const auto& row : sample.direction.direction)
        for (float value : row) if (!std::isfinite(value)) return false;
    return true;
}
}

PsSensors::~PsSensors() { stop(); }

void PsSensors::start() {
    if (state_.enabled) return;
    state_ = {};
    state_.enabled = true;
    touchObserved_ = motionObserved_ = false;
    nextBindMs_ = 0;
    // The application initializes HID before constructing a stream.
    hidInitializeTouchScreen();
}

void PsSensors::stopHandles() {
    for (std::size_t i = 0; i < started_.size(); ++i) {
        if (!started_[i]) continue;
        const Result result = hidStopSixAxisSensor(handles_[i]);
        if (R_FAILED(result)) {
            state_.lastStopResult = result;
            ++state_.counters.stopFailures;
        }
        started_[i] = false;
    }
    state_.handleCount = 0;
    state_.motion = {};
    motionObserved_ = false;
}

void PsSensors::stop() {
    stopHandles();
    state_.enabled = false;
    state_.touches = {};
    state_.touchCount = state_.hidTouchCount = 0;
    state_.touchFresh = state_.touchValid = false;
    state_.npadId = -1;
    state_.style = 0;
    touchObserved_ = false;
}

void PsSensors::bind(const PadState& pad, std::uint64_t nowMs) {
    int nextId = -1;
    std::uint32_t nextStyle = 0;
    const auto choose = [&](HidNpadIdType id) {
        if (!padIsNpadActive(&pad, id)) return false;
        const auto styles = hidGetNpadStyleSet(id);
        for (const auto style : {HidNpadStyleTag_NpadHandheld, HidNpadStyleTag_NpadFullKey,
                                HidNpadStyleTag_NpadJoyDual, HidNpadStyleTag_NpadJoyLeft,
                                HidNpadStyleTag_NpadJoyRight}) {
            if (styles & style) {
                nextId = static_cast<int>(id);
                nextStyle = style;
                return true;
            }
        }
        return false;
    };
    if (!choose(HidNpadIdType_Handheld)) {
        for (unsigned id = HidNpadIdType_No1; id <= HidNpadIdType_No8; ++id)
            if (choose(static_cast<HidNpadIdType>(id))) break;
    }
    const bool changed = nextId != state_.npadId || nextStyle != state_.style;
    if (!changed && (state_.handleCount > 0 || nowMs < nextBindMs_)) return;
    stopHandles();
    state_.npadId = nextId;
    state_.style = nextStyle;
    nextBindMs_ = nowMs + 2000;
    if (nextId < 0) return;
    ++state_.counters.handleRefreshes;
    state_.lastGetHandlesResult = 0;
    state_.lastStartResult = 0;
    const unsigned count = nextStyle == HidNpadStyleTag_NpadJoyDual ? 2 : 1;
    const Result result = hidGetSixAxisSensorHandles(handles_.data(), static_cast<s32>(count),
        static_cast<HidNpadIdType>(nextId), static_cast<HidNpadStyleTag>(nextStyle));
    state_.lastGetHandlesResult = result;
    if (R_FAILED(result)) {
        ++state_.counters.startFailures;
        return;
    }
    for (unsigned i = 0; i < count; ++i) {
        const Result started = hidStartSixAxisSensor(handles_[i]);
        if (R_SUCCEEDED(started)) {
            started_[i] = true;
            ++state_.handleCount;
        } else {
            state_.lastStartResult = started;
            ++state_.counters.startFailures;
        }
    }
}

void PsSensors::readTouch(std::uint64_t nowMs) {
    state_.touchFresh = false;
    HidTouchScreenState sample{};
    if (hidGetTouchScreenStates(&sample, 1) == 0) {
        ++state_.counters.touchMissing;
    } else if (touchObserved_ && state_.touchSampleNumber == sample.sampling_number) {
        ++state_.counters.touchDuplicates;
    } else {
        ++state_.counters.touchSamples;
        state_.touchSampleNumber = sample.sampling_number;
        state_.touchReadAtMs = nowMs;
        state_.touchFresh = state_.touchValid = true;
        state_.touchStale = false;
        touchObserved_ = true;
        state_.hidTouchCount = static_cast<unsigned>(std::clamp(sample.count, 0, 16));
        const auto previous = state_.touches;
        state_.touches = {};
        std::array<bool,16> used{};
        // Existing finger IDs retain their slots when HID reorders its touch array.
        for (std::size_t slot = 0; slot < previous.size(); ++slot) {
            if (!previous[slot].active) continue;
            for (unsigned i = 0; i < state_.hidTouchCount; ++i) {
                if (sample.touches[i].finger_id == previous[slot].fingerId) {
                    state_.touches[slot] = previous[slot];
                    used[i] = true;
                    break;
                }
            }
        }
        for (unsigned i = 0; i < state_.hidTouchCount; ++i) {
            if (used[i]) continue;
            const auto slot = std::find_if(state_.touches.begin(), state_.touches.end(),
                [](const auto& contact) { return !contact.active; });
            if (slot == state_.touches.end()) break;
            slot->active = true;
            slot->fingerId = sample.touches[i].finger_id;
        }
        state_.touchCount = 0;
        for (auto& contact : state_.touches) {
            if (!contact.active) continue;
            for (unsigned i = 0; i < state_.hidTouchCount; ++i) {
                const auto& point = sample.touches[i];
                if (point.finger_id != contact.fingerId) continue;
                contact.screenX = static_cast<std::uint16_t>(std::min(point.x, 1279u));
                contact.screenY = static_cast<std::uint16_t>(std::min(point.y, 719u));
                contact.x = scale(point.x, 1279, 1919);
                contact.y = scale(point.y, 719, 1079);
                ++state_.touchCount;
                break;
            }
        }
    }
    if (touchObserved_ && nowMs >= state_.touchReadAtMs && nowMs - state_.touchReadAtMs > staleMs) {
        if (state_.touchCount > 0) ++state_.counters.touchStaleReleases;
        state_.touches = {};
        state_.touchCount = state_.hidTouchCount = 0;
        state_.touchValid = false;
        state_.touchStale = true;
    }
}

void PsSensors::readMotion(std::uint64_t nowMs) {
    auto& motion = state_.motion;
    motion.fresh = false;
    bool found = false;
    for (std::size_t index = 0; index < started_.size(); ++index) {
        if (!started_[index]) continue;
        HidSixAxisSensorState sample{};
        if (hidGetSixAxisSensorStates(handles_[index], &sample, 1) == 0) continue;
        if (!(sample.attributes & HidSixAxisSensorAttribute_IsConnected) || !finite(sample)) {
            ++state_.counters.motionInvalid;
            continue;
        }
        found = true;
        if (motionObserved_ && motion.sourceIndex == static_cast<int>(index) &&
            motion.sampleNumber == sample.sampling_number) {
            ++state_.counters.motionDuplicates;
            break;
        }
        ++state_.counters.motionSamples;
        motionObserved_ = true;
        motion.valid = motion.fresh = true;
        motion.sourceIndex = static_cast<int>(index);
        motion.sampleNumber = sample.sampling_number;
        motion.deltaTime = sample.delta_time;
        motion.readAtMs = nowMs;
        motion.gyroTurnsPerSecond = {sample.angular_velocity.x, sample.angular_velocity.y, sample.angular_velocity.z};
        motion.accelerationG = {sample.acceleration.x, sample.acceleration.y, sample.acceleration.z};
        for (unsigned row = 0; row < 3; ++row)
            for (unsigned col = 0; col < 3; ++col)
                motion.direction[row * 3 + col] = sample.direction.direction[row][col];
        // Same unit and axis convention used by Switch PS Remote Play clients.
        // These converted values are measurements, not a Fog Play wire contract.
        motion.psGyroRadiansPerSecond = {sample.angular_velocity.x * tau,
            sample.angular_velocity.z * tau, -sample.angular_velocity.y * tau};
        motion.psAccelerationG = {-sample.acceleration.x, -sample.acceleration.z, sample.acceleration.y};
        break;
    }
    if (!found) ++state_.counters.motionMissing;
    if (!found || (motionObserved_ && nowMs >= motion.readAtMs && nowMs - motion.readAtMs > staleMs)) {
        if (motion.valid) ++state_.counters.motionStaleReleases;
        motion.valid = false;
        motion.gyroTurnsPerSecond = {};
        motion.accelerationG = {};
        motion.psGyroRadiansPerSecond = {};
        motion.psAccelerationG = {};
        motion.direction = {};
    }
}

const fog::playstation::PsSensorSnapshot& PsSensors::update(const PadState& pad, std::uint64_t nowMs) {
    if (!state_.enabled) return state_;
    ++state_.counters.updates;
    bind(pad, nowMs);
    readTouch(nowMs);
    readMotion(nowMs);
    return state_;
}
}

