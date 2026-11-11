#pragma once
#include "features/streaming/domain/gamepad.hpp"
#include "features/streaming/domain/controller_profile.hpp"
#include "features/streaming/domain/remote_input.hpp"
#include "features/streaming/domain/rumble.hpp"
#include "features/streaming/domain/input_suspension.hpp"

namespace stream {
// Application-owner input and feedback boundary. No transport teardown or media pulls.
class StreamInputSink {
public:
    virtual ~StreamInputSink() = default;
    // Pre-start controller preference, retained across start(). The host chooses
    // the virtual device supported by its driver for the requested profile.
    virtual bool configureController(ControllerProfile profile) = 0;
    // False means the provider has no freeze detector; zero reasons are not proof of freshness.
    virtual bool supportsInputSuspension() const noexcept { return false; }
    virtual InputSuspension inputSuspension() const { return {}; }
    bool inputSuspended() const { return inputSuspension().suspended(); }
    virtual bool sendGamepad(const Gamepad& state) = 0;
    // Main-thread feedback drain. Adapters coalesce network callbacks; never
    // touch controller hardware from a transport thread. Optional for fakes.
    virtual bool pollRumble(Rumble&) { return false; }
    virtual bool sendInput(const fog::remote::Input& input) = 0;
};
}
