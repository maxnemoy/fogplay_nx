#include "features/streaming/domain/stream_input_sink.hpp"
#include "features/streaming/domain/stream_lifecycle.hpp"
#include "features/streaming/domain/playback_observation.hpp"
#include "features/streaming/domain/gamepad_schedule.hpp"
#include "features/streaming/application/transport_teardown.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <type_traits>
#include <utility>

namespace {
template<class T, class = void> struct HasClose : std::false_type {};
template<class T> struct HasClose<T,std::void_t<decltype(std::declval<T&>().close())>> : std::true_type {};
template<class T, class = void> struct HasGamepad : std::false_type {};
template<class T> struct HasGamepad<T,std::void_t<decltype(std::declval<T&>().sendGamepad(stream::Gamepad{}))>> : std::true_type {};
template<class T, class = void> struct HasSignals : std::false_type {};
template<class T> struct HasSignals<T,std::void_t<decltype(std::declval<T&>().mediaSignals())>> : std::true_type {};
static_assert(!HasClose<stream::StreamInputSink>::value);
static_assert(!HasGamepad<stream::StreamLifecycle>::value);
static_assert(!HasSignals<stream::StreamInputSink>::value && !HasSignals<stream::StreamLifecycle>::value);
static_assert(std::has_virtual_destructor_v<stream::StreamInputSink> && std::has_virtual_destructor_v<stream::StreamLifecycle>);

class InputOnly : public stream::StreamInputSink {
public:
    bool configureController(stream::ControllerProfile value) override { profile=value; return true; }
    bool sendGamepad(const stream::Gamepad& value) override { last=value; ++gamepads; return accept; }
    bool sendInput(const fog::remote::Input&) override { ++inputs; return accept; }
    stream::ControllerProfile profile=stream::ControllerProfile::Xbox;
    stream::Gamepad last{};
    unsigned gamepads=0, inputs=0;
    bool accept=true;
};
class SuspendedInput final : public InputOnly {
public:
    stream::InputSuspension inputSuspension() const override { return {false,true,false}; }
};
struct Calls {
    std::atomic<unsigned> closes{0}, retries{0}, destroyed{0};
    std::atomic<bool> onWorker{false};
    const std::thread::id owner=std::this_thread::get_id();
};
class LifecycleOnly : public stream::StreamLifecycle {
public:
    explicit LifecycleOnly(std::shared_ptr<Calls> value=std::make_shared<Calls>()) : calls(std::move(value)) {}
    ~LifecycleOnly() override { ++calls->destroyed; }
    bool start(const stream::StreamConnection& value) override { connection=value; return startResult; }
    bool configureVideo(const fog::video::Settings& value) override { video=value; return true; }
    bool configureRelay(bool value) override { relay=value; return true; }
    bool updateVideoSettings(const fog::video::Settings& value) override { video=value; return true; }
    void pump() override { ++pumps; }
    bool ready() const override { return isReady; }
    std::string status() const override { return rawStatus; }
    std::string error() const override { return rawError; }
    bool retryableError() const override { return retryable; }
    bool disconnected() const override { return isDisconnected; }
    bool disconnectForRetry() override { ++calls->retries; calls->onWorker=std::this_thread::get_id()!=calls->owner; return allowRetry; }
    void close() override { ++calls->closes; calls->onWorker=std::this_thread::get_id()!=calls->owner; }
    std::shared_ptr<Calls> calls;
    stream::StreamConnection connection{};
    fog::video::Settings video{};
    std::string rawStatus="fixture status", rawError;
    unsigned pumps=0;
    bool isReady=false, isDisconnected=false, retryable=false, allowRetry=false, startResult=true, relay=false;
};
class CallbackFailure final : public LifecycleOnly {
public:
    stream::RecoveryState recoveryState() const override { return stream::RecoveryState::TerminalFailure; }
};
void inputPolicy() {
    InputOnly owner;
    stream::StreamInputSink& sink=owner;
    assert(sink.configureController(stream::ControllerProfile::DualSense));
    assert(owner.profile==stream::ControllerProfile::DualSense && !sink.inputSuspended());
    stream::Rumble rumble;
    assert(!sink.pollRumble(rumble));
    SuspendedInput suspended;
    stream::StreamInputSink& blocked=suspended;
    assert(blocked.inputSuspended() && blocked.inputSuspension().localWaiting);
    using Schedule=fog::stream::GamepadSchedule;
    Schedule schedule;
    const auto now=Schedule::Time{};
    stream::Gamepad pressed; pressed.buttons[0]=1;
    assert(schedule.due(pressed,now) && sink.sendGamepad(pressed)); schedule.accepted(pressed,now);
    owner.accept=false;
    schedule.suspend([&](const stream::Gamepad& value) { return sink.sendGamepad(value); });
    assert(owner.gamepads==2 && !schedule.due(pressed,now));
    owner.accept=true;
    schedule.retryRelease([&](const stream::Gamepad& value) { return sink.sendGamepad(value); });
    assert(owner.gamepads==3 && owner.last.buttons[0]==0 && schedule.due(pressed,now));
    assert(sink.sendInput(fog::remote::Key{false,"Enter"}) && owner.inputs==1);
}
void lifecyclePolicy() {
    LifecycleOnly owner;
    stream::StreamLifecycle& lifecycle=owner;
    stream::StreamConnection connection{{fog::ProviderId::FogHub,"fixture-rental"},true};
    assert(lifecycle.start(connection) && owner.connection.rental==connection.rental && owner.connection.resumeOnly);
    fog::video::Settings settings; settings.framesPerSecond=60;
    assert(lifecycle.configureVideo(settings) && lifecycle.configureRelay(true) && lifecycle.updateVideoSettings(settings));
    lifecycle.pump(); assert(owner.pumps==1 && owner.relay && owner.video.framesPerSecond==60);
    assert(!lifecycle.ready() && lifecycle.status()=="fixture status" && lifecycle.error().empty());
    // Preserve the original default state decision, including its precedence.
    assert(lifecycle.recoveryState()==stream::RecoveryState::Connected);
    owner.retryable=true; assert(lifecycle.recoveryState()==stream::RecoveryState::Connected);
    owner.isDisconnected=true; assert(lifecycle.recoveryState()==stream::RecoveryState::Disconnected);
    owner.rawError="fixture error"; assert(lifecycle.recoveryState()==stream::RecoveryState::RetryableFailure);
    owner.retryable=false; assert(lifecycle.recoveryState()==stream::RecoveryState::TerminalFailure);
    assert(lifecycle.error()=="fixture error");
    CallbackFailure callback;
    stream::StreamLifecycle& failed=callback;
    assert(failed.error().empty() && !failed.disconnected() && failed.recoveryState()==stream::RecoveryState::TerminalFailure);
}
void teardownPolicy(bool retry) {
    auto calls=std::make_shared<Calls>();
    std::unique_ptr<stream::StreamLifecycle> client=std::make_unique<LifecycleOnly>(calls);
    auto* identity=client.get();
    // Actual unchanged teardown consumer needs only the lifecycle contract.
    fog::stream::TransportTeardown<stream::StreamLifecycle> teardown;
    assert(teardown.begin(client,retry) && !client);
    decltype(teardown)::Result result;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (!(result=teardown.take()).client && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
    assert(result.client.get()==identity && result.retry==retry && result.allowed==!retry);
    assert(calls->onWorker && calls->closes==(retry?0u:1u) && calls->retries==(retry?1u:0u));
    assert(!calls->destroyed); teardown.finish(); assert(!calls->destroyed);
    result.client.reset(); assert(calls->destroyed==1);
}
}
int main() {
    stream::Gamepad gamepad;
    for (auto value:gamepad.buttons) assert(value==0);
    for (auto value:gamepad.axes) assert(value==0);
    assert(gamepad.leftTrigger==0 && gamepad.rightTrigger==0 && gamepad.sampledAtUs==0);
    stream::StreamConnection connection; assert(!connection.resumeOnly);
    stream::PlaybackObservation observation;
    assert(!observation.workerVideo && !observation.recovering && !observation.videoGeneration && !observation.decoderWorkUs);
    inputPolicy(); lifecyclePolicy(); teardownPolicy(false); teardownPolicy(true);
    std::cout<<"Focused input/lifecycle contracts: schedule rejection/release, raw recovery dispatch and actual lifecycle-only teardown ownership passed\n";
}