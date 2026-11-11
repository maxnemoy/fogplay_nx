"""Exercise the production early-input method with transport/platform doubles."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/features/streaming/presentation/playback_controller.cpp').read_text()
begin = source.index('void ApplicationController::sendStreamInput(')
brace = source.index('{',begin)
depth,end = 1,brace+1
while depth:
    depth += (source[end]=='{')-(source[end]=='}'); end+=1
method = source[begin:end]
inputs = (root / 'src/features/streaming/presentation/stream_input_controller.cpp').read_text()
remote_begin = inputs.index('bool ApplicationController::sendRemote(')
remote_end = inputs.index('void ApplicationController::releaseMouse(', remote_begin)
remote_methods = inputs[remote_begin:remote_end]
pump = source[source.index('void ApplicationController::pumpStream('):source.index('void ApplicationController::logOutputTimings(')]
# Wiring checks complement the executable coordinator test below.
for gate in ('handleStreamOverlay(', 'player_.beforeMedia('):
    assert pump.index(gate) < pump.index('sendStreamInput('), gate
for work in ('player_.media(',):
    assert pump.index('sendStreamInput(') < pump.index(work), work
assert pump.index('player_.observe(') < pump.index('handleStreamOverlay(')

harness = r'''
#include "features/session/application/rental_stop_coordinator.hpp"
#include "features/streaming/application/player.hpp"
#include "features/streaming/domain/streaming_client.hpp"
#include "core/constants/stage1_options.hpp"
#include "features/streaming/domain/stream_input_gate.hpp"
#include "features/streaming/domain/gamepad_schedule.hpp"
#include "features/streaming/domain/key_releases.hpp"
#include "features/streaming/application/remote_input.hpp"
#include "features/streaming/domain/stick_deadzone.hpp"
#include "features/streaming/domain/video_recovery.hpp"
#include "features/settings/domain/controller_layout.hpp"
#include "features/playstation/domain/controller.hpp"
#include <cassert>
#include <iostream>
#include <vector>
using u64 = std::uint64_t;
enum : u64 {
    HidNpadButton_B=1ull<<0, HidNpadButton_A=1ull<<1, HidNpadButton_Y=1ull<<2, HidNpadButton_X=1ull<<3,
    HidNpadButton_L=1ull<<4, HidNpadButton_R=1ull<<5, HidNpadButton_ZL=1ull<<6, HidNpadButton_ZR=1ull<<7,
    HidNpadButton_Minus=1ull<<8, HidNpadButton_Plus=1ull<<9, HidNpadButton_StickL=1ull<<10, HidNpadButton_StickR=1ull<<11,
    HidNpadButton_Up=1ull<<12, HidNpadButton_Down=1ull<<13, HidNpadButton_Left=1ull<<14, HidNpadButton_Right=1ull<<15
};
constexpr fog::controller::FaceButtonMasks faceMasks{HidNpadButton_A,HidNpadButton_B,HidNpadButton_X,HidNpadButton_Y};
struct Stick { int x=0,y=0; };
struct PadState { Stick sticks[2]; };
Stick padGetStickPos(PadState* pad, int index) { return pad->sticks[index]; }
enum class Screen { Streaming, StopConfirm, Stopping };
struct ApplicationController {
    struct Transport {
        bool waiting=false, broken=false, connected=true, accept=true, freeze=false;
        bool waitAfterKeyDown=false, acceptRelease=true;
        std::uint64_t generation=1;
        stream::RecoveryState state=stream::RecoveryState::Connected;
        std::vector<stream::Gamepad> sent;
        std::vector<fog::remote::Input> remote;
        bool waitingForVideoKeyframe() const { return waiting; }
        bool inputSuspended() const { return freeze; }
        bool disconnected() const { return broken; }
        bool ready() const { return connected; }
        stream::RecoveryState recoveryState() const { return state; }
        std::uint64_t currentVideoGeneration() const { return generation; }
        bool sendGamepad(const stream::Gamepad& value) { sent.push_back(value); return accept; }
        bool sendInput(const fog::remote::Input& value) {
            const auto* key=std::get_if<fog::remote::Key>(&value);
            if (key && !key->down && !acceptRelease) return false;
            remote.push_back(value);
            if (key && key->down && waitAfterKeyDown) waiting=true;
            return true;
        }
    } transport;
    struct Media {
        bool waiting=false, workers=false;
        std::uint64_t generation=1;
        bool waitingForVideoKeyframe() const { return waiting; }
        bool workersRunning() const { return workers; }
        std::uint64_t workerVideoGeneration() const { return generation; }
    } media;
    struct Reconnect { bool active=false; bool recovering() const { return active; } } transportReconnect_;
    struct Native {
        bool initialized=true;
        bool contactsInitialized() const { return initialized; }
        bool click(std::uint64_t,bool) { return false; }
    } psNativeInput_;
    Screen screen_=Screen::Streaming;
    bool  exitAfterStop_=false, firstFramePresented_=true, streamPlayStation_=false; fog::RentalStopCoordinator rentalSession_;
    bool videoLossWaiting_=false, psTouchLastSubmitted_=false, nativeAllowed=false;
    unsigned psGamepadAccepted_=0,psGamepadRejected_=0;
    std::uint64_t lastVideoGeneration_=1,padSampledAtUs_=123;
    Transport* stream_=&transport;
    Media* media_=&media;
    fog::stream::StreamInputGate streamInputGate_;
    fog::stream::GamepadSchedule gamepadSchedule_;
    fog::stream::KeyReleases keyReleases_;
    fog::VideoRecovery videoRecovery_;
    fog::controller::Settings controllerSettings_;
    std::chrono::steady_clock::time_point lastVideo_=std::chrono::steady_clock::now();
    void dispatchPlayStationInput(bool allowed) { nativeAllowed=allowed; }
    void sendStreamInput(PadState& pad,u64 held,bool overlayHandled);
    bool sendRemote(const fog::remote::Input& command);
    void retryKeyReleases() noexcept;
    struct PlayerView {
        ApplicationController* owner;
        Transport* inputSink() const { return owner->stream_; }
        fog::stream::Player::Snapshot snapshot() const {
            fog::stream::Player::Snapshot value;
            value.media=bool(owner->media_);
            value.ready=owner->transport.connected;value.disconnected=owner->transport.broken;
            value.connected=owner->transport.state==stream::RecoveryState::Connected;
            value.inputAllowed=owner->firstFramePresented_&&!owner->transportReconnect_.active&&!owner->videoLossWaiting_&&
                !owner->transport.waiting&&!owner->media.waiting&&!owner->transport.broken&&value.connected&&value.ready&&
                !owner->transport.freeze&&!owner->videoRecovery_.active()&&
                std::chrono::steady_clock::now()-owner->lastVideo_<std::chrono::seconds(10)&&
                (owner->media.workers?owner->media.generation:owner->transport.generation)==owner->lastVideoGeneration_;
            return value;
        }
    } player_{this};
};
'''
harness += method
harness += remote_methods
harness += r'''
int main() {
    PadState pad{{{16000,-8000},{0,0}}};
    ApplicationController normal;
    normal.sendStreamInput(pad,HidNpadButton_B|HidNpadButton_ZL,false);
    assert(normal.transport.sent.size()==1 && normal.nativeAllowed);
    const auto& first=normal.transport.sent.front();
    assert(first.buttons[0]==1 && first.leftTrigger==1 && first.sampledAtUs==123);
    assert(first.axes[0]>0 && first.axes[1]>0);
    // Rejected edges retry the fresh state rather than advancing accepted state.
    ApplicationController rejected;
    rejected.transport.accept=false;
    rejected.sendStreamInput(pad,HidNpadButton_B,false);
    rejected.sendStreamInput(pad,0,false);
    assert(rejected.transport.sent.size()==2 && rejected.transport.sent.back().buttons[0]==0);
    for (int prohibition=0;prohibition<18;++prohibition) {
        ApplicationController app;
        switch(prohibition) {
            case 0: app.rentalSession_.request(); break;
            case 1: app.exitAfterStop_=true; break;
            case 2: app.screen_=Screen::StopConfirm; break;
            case 3: app.transportReconnect_.active=true; break;
            case 4: app.videoLossWaiting_=true; break;
            case 5: app.transport.waiting=true; break;
            case 6: app.media.waiting=true; break;
            case 7: app.transport.broken=true; break;
            case 8: app.transport.state=stream::RecoveryState::TerminalFailure; break;
            case 9: app.transport.connected=false; break;
            case 10: app.firstFramePresented_=false; break;
            case 11: app.streamInputGate_.suspendVideo(); break;
            case 12: app.media.workers=true; app.media.generation=2; break;
            case 13: assert(app.videoRecovery_.begin(std::chrono::steady_clock::now())); break;
            case 14: app.lastVideo_-=std::chrono::seconds(11); break;
            case 15: app.streamInputGate_.overlayTransition(); break;
            case 16: app.transport.generation=2; break; // MTS fault + assembled IDR, decoder still old.
            case 17: app.transport.freeze=true; break; // Input-only core/no-frame freeze.
        }
        app.sendStreamInput(pad,HidNpadButton_B,false);
        assert(app.transport.sent.empty() && !app.nativeAllowed);
        if (prohibition != 15) // Overlay release latch only applies to gamepad click-through.
            assert(!app.sendRemote(fog::remote::key(true,"Enter")));
    }
    ApplicationController menu;
    menu.sendStreamInput(pad,HidNpadButton_B,true);
    assert(menu.transport.sent.empty() && !menu.nativeAllowed);
    ApplicationController resumed;
    resumed.streamInputGate_.suspendVideo();
    resumed.sendStreamInput(pad,HidNpadButton_B,false);
    resumed.streamInputGate_.videoPrepared(false);
    resumed.sendStreamInput(pad,HidNpadButton_Y,false);
    assert(resumed.transport.sent.size()==1 && resumed.transport.sent.back().buttons[0]==0);
    assert(resumed.transport.sent.back().buttons[2]==1); // No old press replay.
    ApplicationController mts;
    mts.transport.generation=2; // Assembly fault and repaired IDR between UI cycles.
    assert(!mts.transport.waiting && !mts.media.waiting && !mts.media.workers);
    mts.sendStreamInput(pad,HidNpadButton_B,false);
    assert(mts.transport.sent.empty());
    assert(!mts.sendRemote(fog::remote::key(true,"Enter")));
    mts.lastVideoGeneration_=2; // Owner resets decoder, decodes and prepares generation 2.
    mts.sendStreamInput(pad,HidNpadButton_Y,false);
    assert(mts.transport.sent.size()==1 && mts.transport.sent.back().buttons[2]==1);
    ApplicationController freezeRecovery;
    freezeRecovery.transport.freeze=true;
    freezeRecovery.streamInputGate_.suspendVideo();
    assert(!freezeRecovery.sendRemote(fog::remote::key(true,"Enter")));
    // A newly assembled frame clears the raw core/no-frame signal before the
    // owner prepares it. The input-only latch must still block overlay actions.
    freezeRecovery.transport.freeze=false;
    assert(freezeRecovery.streamInputGate_.videoSuspended());
    assert(!freezeRecovery.sendRemote(fog::remote::key(true,"Enter")));
    freezeRecovery.sendStreamInput(pad,HidNpadButton_B,false);
    assert(freezeRecovery.transport.sent.empty() && freezeRecovery.transport.remote.empty());
    // Releases remain safe even while the same latch awaits prepared output.
    assert(freezeRecovery.sendRemote(fog::remote::key(false,"Enter")));
    assert(freezeRecovery.transport.remote.size()==1);
    freezeRecovery.streamInputGate_.videoPrepared(false);
    assert(freezeRecovery.sendRemote(fog::remote::key(true,"Enter")));
    assert(freezeRecovery.transport.remote.size()==2);
    ApplicationController keyboard;
    keyboard.transport.waitAfterKeyDown=true;
    assert(keyboard.sendRemote(fog::remote::key(true,"Enter")));
    assert(keyboard.transport.waiting);
    assert(keyboard.sendRemote(fog::remote::key(false,"Enter")));
    assert(keyboard.transport.remote.size()==2 && !keyboard.keyReleases_.pending("Enter"));
    keyboard.transport.waiting=false;
    assert(keyboard.sendRemote(fog::remote::key(true,"Tab")));
    keyboard.transport.acceptRelease=false;
    assert(!keyboard.sendRemote(fog::remote::key(false,"Tab")) && keyboard.keyReleases_.pending("Tab"));
    keyboard.retryKeyReleases();
    assert(keyboard.keyReleases_.pending("Tab"));
    keyboard.transport.acceptRelease=true;
    keyboard.transport.state=stream::RecoveryState::TerminalFailure;
    keyboard.retryKeyReleases();
    assert(keyboard.keyReleases_.pending("Tab") && keyboard.transport.remote.size()==3);
    keyboard.transport.state=stream::RecoveryState::Connected;
    keyboard.retryKeyReleases();
    assert(!keyboard.keyReleases_.pending("Tab") && keyboard.transport.remote.size()==4);
    assert(!std::get<fog::remote::Key>(keyboard.transport.remote.back()).down);
    keyboard.keyReleases_.pressed("Escape");
    keyboard.keyReleases_.reset(); // A replaced transport must not inherit pending key-ups.
    keyboard.retryKeyReleases();
    assert(keyboard.transport.remote.size()==4);
    std::cout << "Early stream input: production methods, 18 prohibitions, MTS repaired-IDR generation, keyboard release/retry and fresh resume PASS\n";
}
'''
with tempfile.TemporaryDirectory(prefix='fog-early-input-') as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(harness)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I' + str(root / 'src'),
                    str(path / 'test.cpp'), str(root / 'src/features/settings/domain/controller_layout.cpp'),
                    str(root / 'src/features/streaming/application/remote_input.cpp'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=15)
