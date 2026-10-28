"""Run Moonlight's real optional input methods against deterministic core doubles."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/features/streaming/infrastructure/moonlight/moonlight_session.cpp').read_text()
helpers = source[source.index('    bool accepted(int result)'):source.index('    static short axis(')]
optional = source[source.index('    bool send(const fog::remote::Touch&'):source.index('    bool take(stream::Packet&')]
program = r'''
#include "features/streaming/infrastructure/moonlight/controller_input.hpp"
#include "features/playstation/infrastructure/native_input.hpp"
#include <atomic>
#include <cassert>
#include <iostream>
#include <limits>
#include <memory>

using stream::ControllerProfile;
using namespace fog::remote;
namespace detail = moonlight::detail;
unsigned hostFeatures = LI_FF_CONTROLLER_TOUCH_EVENTS;
int touchResult = 0, motionResult = 0, touchCalls = 0, motionCalls = 0;
int lastTouchType = -1, lastMotionType = -1, lastFinger = -1;
float lastX = 0, lastY = 0, lastPressure = 0;
std::array<float, 3> lastMotion{};
std::uint64_t currentMs = 1000;
std::uint64_t nowMs() { return currentMs; }
std::uint32_t LiGetHostFeatureFlags() { return hostFeatures; }
int LiSendControllerTouchEvent(std::uint8_t controller, std::uint8_t type, std::uint32_t finger, float x, float y, float pressure) {
    assert(controller == 0); ++touchCalls;
    lastTouchType = type; lastFinger = static_cast<int>(finger); lastX = x; lastY = y; lastPressure = pressure;
    return touchResult;
}
int LiSendControllerMotionEvent(std::uint8_t controller, std::uint8_t type, float x, float y, float z) {
    assert(controller == 0); ++motionCalls; lastMotionType = type; lastMotion = {x, y, z}; return motionResult;
}
struct State {
    std::atomic<std::uint64_t> inputAccepted{0}, inputRejected{0}, inputIgnored{0};
    std::atomic<unsigned short> accelRateHz{0}, gyroRateHz{0};
};
struct Client {
    std::shared_ptr<State> state_ = std::make_shared<State>();
    ControllerProfile profile_ = ControllerProfile::DualSense;
    std::array<std::uint64_t, 2> lastMotionMs_{};
HELPERS
OPTIONAL
};
int main() {
    using namespace moonlight::detail;
    assert(controllerType(ControllerProfile::Xbox) == LI_CTYPE_XBOX);
    assert(controllerType(ControllerProfile::DualSense) == LI_CTYPE_PS);
    assert(controllerCapabilities(ControllerProfile::Xbox) == LI_CCAP_RUMBLE);
    assert(controllerCapabilities(ControllerProfile::DualSense) == (LI_CCAP_RUMBLE | LI_CCAP_TOUCHPAD | LI_CCAP_ACCEL | LI_CCAP_GYRO));
    assert((controllerButtons(ControllerProfile::DualSense) & TOUCHPAD_FLAG) != 0);
    std::array<int, 18> buttons{}; buttons[0] = buttons[16] = buttons[17] = 1;
    assert(controllerButtonFlags(buttons, ControllerProfile::DualSense) == (A_FLAG | SPECIAL_FLAG | TOUCHPAD_FLAG));
    assert(controllerButtonFlags(buttons, ControllerProfile::Xbox) == (A_FLAG | SPECIAL_FLAG));
    Client client;
    assert(client.send(Touch{TouchPhase::Down, 1, .25f, .75f}));
    assert(touchCalls == 1 && lastTouchType == LI_TOUCH_EVENT_DOWN && lastFinger == 1 && lastX == .25f && lastY == .75f && lastPressure == 1);
    assert(client.send(Touch{TouchPhase::Move, 1, .5f, .6f}));
    assert(lastTouchType == LI_TOUCH_EVENT_MOVE);
    assert(client.send(Touch{TouchPhase::Up, 1, .5f, .6f}));
    assert(lastTouchType == LI_TOUCH_EVENT_UP && lastPressure == 0);
    for (const auto touch : {Touch{TouchPhase::Down, 2, 0, 0}, Touch{TouchPhase::Down, 0, -1, 0},
            Touch{TouchPhase::Down, 0, 0, std::numeric_limits<float>::quiet_NaN()},
            Touch{static_cast<TouchPhase>(-1), 0, 0, 0}}) assert(!client.send(touch));
    assert(touchCalls == 3);
    client.profile_ = ControllerProfile::Xbox;
    assert(!client.send(Touch{TouchPhase::Up, 0, 0, 0}));
    assert(!client.send(Sensor{MotionSensor::Gyroscope, {0, 0, 0}}));
    client.profile_ = ControllerProfile::DualSense;
    // Unsupported hosts still complete the actual NativeInput contact reset.
    hostFeatures = 0;
    fog::playstation::NativeInput native;
    assert(native.initializeContacts([&](const Input& input) { return client.send(std::get<Touch>(input)); }));
    assert(native.contactsInitialized() && touchCalls == 3);
    assert(client.send(Sensor{MotionSensor::Accelerometer, {0, 9.80665f, 0}}) && motionCalls == 0);
    hostFeatures = LI_FF_CONTROLLER_TOUCH_EVENTS;
    touchResult = LI_ERR_UNSUPPORTED;
    assert(client.send(Touch{TouchPhase::Up, 0, 0, 0}));
    touchResult = -1;
    assert(!client.send(Touch{TouchPhase::Up, 0, 0, 0}));
    // Motion stays off until the host asks, then each sensor follows its rate.
    assert(client.send(Sensor{MotionSensor::Gyroscope, {1, 2, 3}}) && motionCalls == 0);
    client.state_->gyroRateHz = 20;
    assert(client.send(Sensor{MotionSensor::Gyroscope, {1, -2, 3}}));
    assert(motionCalls == 1 && lastMotionType == LI_MOTION_TYPE_GYRO);
    assert(std::abs(lastMotion[0] - 57.2957795f) < .0001f && std::abs(lastMotion[1] + 114.591559f) < .0001f);
    ++currentMs;
    assert(client.send(Sensor{MotionSensor::Gyroscope, {2, 3, 4}}) && motionCalls == 1);
    currentMs += 49;
    assert(client.send(Sensor{MotionSensor::Gyroscope, {2, 3, 4}}) && motionCalls == 2);
    client.state_->accelRateHz = 100;
    assert(client.send(Sensor{MotionSensor::Accelerometer, {1, 9.80665f, -2}}));
    assert(motionCalls == 3 && lastMotionType == LI_MOTION_TYPE_ACCEL && lastMotion[1] == 9.80665f);
    client.state_->gyroRateHz = 0;
    currentMs += 100;
    assert(client.send(Sensor{MotionSensor::Gyroscope, {2, 3, 4}}) && motionCalls == 3);
    assert(!client.send(Sensor{static_cast<MotionSensor>(-1), {0, 0, 0}}));
    assert(!client.send(Sensor{MotionSensor::Accelerometer, {0, std::numeric_limits<float>::infinity(), 0}}));
    client.state_->gyroRateHz = 100;
    motionResult = -1;
    assert(!client.send(Sensor{MotionSensor::Gyroscope, {0, 0, 0}}));
    motionResult = LI_ERR_UNSUPPORTED;
    assert(client.send(Sensor{MotionSensor::Gyroscope, {0, 0, 0}}));
    motionResult = 0;
    currentMs += 100;
    assert(client.send(Sensor{MotionSensor::Gyroscope, {1, 2, 3}}));
    const auto beforeRelease = motionCalls;
    assert(client.send(Sensor{MotionSensor::Gyroscope, {0, 0, 0}}));
    assert(motionCalls == beforeRelease + 1 && lastMotion == (std::array<float, 3>{0, 0, 0}));
    fog::playstation::NativeInput activeNative;
    fog::playstation::PsSensorSnapshot sample;
    sample.motion.valid = true; sample.motion.sampleNumber = 1;
    sample.motion.psGyroRadiansPerSecond = {1, 2, 3}; sample.motion.psAccelerationG = {1, 1, 1};
    currentMs += 100;
    client.state_->gyroRateHz = client.state_->accelRateHz = 10;
    touchResult = 0;
    const auto emit = [&](const Input& input) {
        if (const auto* touch = std::get_if<Touch>(&input)) return client.send(*touch);
        return client.send(std::get<Sensor>(input));
    };
    activeNative.update(sample, currentMs, true, emit);
    const auto beforeSuspend = motionCalls;
    activeNative.suspend(emit);
    assert(motionCalls == beforeSuspend + 2 && lastMotion == (std::array<float, 3>{0, 9.80665f, 0}));
    std::cout << "Moonlight controller input: profiles, buttons, touch reset/failures, motion units/rates PASS\n";
}
'''.replace('HELPERS', helpers).replace('OPTIONAL', optional)
with tempfile.TemporaryDirectory(prefix='fog-moonlight-input-') as directory:
    cpp = Path(directory) / 'test.cpp'
    binary = Path(directory) / 'test'
    cpp.write_text(program)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-Isrc', '-Ithird_party/moonlight/switch/include', str(cpp), '-o', str(binary)], cwd=root, check=True)
    subprocess.run([str(binary)], cwd=root, check=True)
