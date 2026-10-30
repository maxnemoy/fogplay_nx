"""Production input-only suspension/presentation glue with deterministic frames."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/features/streaming/presentation/playback_controller.cpp').read_text()
begin = source.index('    const auto suspension = ')
end = source.index('    const auto diagnosticNow = ',begin)
observe = source[begin:end]
assert 'loopTiming_.suspendPresentation' not in observe
presenter = (root / 'src/app/presentation/application_presenter.cpp').read_text()
begin = presenter.index('        loopTiming_.presented(')
end = presenter.index(';',begin)+1
present = presenter[begin:end].replace('std::chrono::steady_clock::now()', 'now')
overlay = presenter[presenter.index('void ApplicationController::drawStreamOverlay('):presenter.index('void ApplicationController::text(')]
assert 'inputSuspended' not in overlay and 'inputFreezeWaiting_' not in overlay

harness = r'''
#include "core/constants/stage1_options.hpp"
#include "features/streaming/domain/input_suspension.hpp"
#include "features/streaming/domain/loop_timing.hpp"
#include "features/streaming/domain/stream_input_gate.hpp"
#include <cassert>
#include <iostream>
namespace fog::stream { enum class OverlayMode { Closed, Mouse, Menu }; }
struct ApplicationController {
    struct Transport {
        stream::InputSuspension reason;
        stream::InputSuspension inputSuspension() const { return reason; }
    } transport;
    Transport* stream_=&transport;
    struct Media {
        std::uint64_t presented=1;
        std::uint64_t presentedFrames() const { return presented; }
        bool waitingForVideoKeyframe() const { return false; }
    } media;
    Media* media_=&media;
    struct Recovery { bool recovering() const { return false; } bool active() const { return false; } } transportReconnect_,videoRecovery_;
    struct Overlay { fog::stream::OverlayMode mode() const { return fog::stream::OverlayMode::Closed; } } overlay_;
    fog::stream::InputSuspensionStats inputSuspensionStats_;
    fog::stream::StreamInputGate streamInputGate_;
    fog::stream::LoopTiming loopTiming_;
    bool inputFreezeWaiting_=false,stopAfterTask_=false,videoLossWaiting_=false;
    unsigned neutralized=0;
    void suspendVideoInput() { ++neutralized; streamInputGate_.suspendVideo(); }
    void tick() {
'''+observe+r'''
    }
    void present(std::chrono::steady_clock::time_point now) {
'''+present+r'''
    }
};
int main() {
    using Clock=std::chrono::steady_clock;
    const auto origin=Clock::time_point{};
    for (const bool core : {false,true}) {
        ApplicationController app;
        app.present(origin);
        app.transport.reason={core,false,true};
        app.tick();
        assert(app.neutralized==1 && app.streamInputGate_.videoSuspended());
        assert(!app.videoLossWaiting_); // Recovery banner remains unchanged.
        for (unsigned ms=160;ms<1500;ms+=16) {
            app.tick();
            app.present(origin+std::chrono::milliseconds(ms)); // Same old texture.
        }
        app.transport.reason={}; app.tick();
        assert(app.streamInputGate_.videoSuspended()); // New decoded image still required.
        app.streamInputGate_.videoPrepared(false);
        app.media.presented=2;
        app.present(origin+std::chrono::milliseconds(1520));
        assert(app.loopTiming_.lifetime.newPresentGap.maxUs==1520000);
        assert(app.loopTiming_.lifetime.continuityBreaks==0 && app.neutralized==1);
    }
    fog::stream::InputSuspensionStats stats;
    stats.observe({},100);
    stats.observe({true,false,true},200);
    stats.observe({true,true,false},350);
    stats.observe({false,true,false},500);
    assert(stats.count(0)==1 && stats.count(1)==1 && stats.count(2)==1);
    assert(stats.durationMs(0)==300 && stats.durationMs(1)==150 && stats.durationMs(2)==150);
    assert(stats.text(550).find("localMs=200")!=std::string::npos);
    stats.observe({},600);
    stats.observe({false,false,true},650);
    stats.observe({},750);
    assert(stats.count(2)==2 && stats.durationMs(2)==250);
    std::cout << "Input-only freeze: production gate/presentation, 1520ms gap preserved, one neutralization, reason counts/durations PASS\n";
}
'''
with tempfile.TemporaryDirectory(prefix='fog-input-suspension-') as directory:
    path=Path(directory)
    (path/'test.cpp').write_text(harness)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(root/'src'),
                    str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=15)
