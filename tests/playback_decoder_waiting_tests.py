"""Run the real presentation coordinator against isolated media/transport doubles."""
from pathlib import Path
import subprocess
import tempfile


root = Path(__file__).resolve().parents[1]
source = (root / 'src/features/streaming/presentation/playback_controller.cpp').read_text()
pump = source[source.index('void ApplicationController::pumpStream('):]
begin = pump.index('    const auto presentationStarted = ')
end = pump.index('    drainTiming_.record(drainStarted);', begin)
presentation = pump[begin:end]

harness = r'''
#include "core/constants/ui_text.hpp"
#include "features/streaming/domain/stream_input_gate.hpp"
#include "features/streaming/domain/video_recovery.hpp"
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
namespace uiText = fog::constants::ui;
namespace fog { struct VideoDecodeError : std::runtime_error { using std::runtime_error::runtime_error; }; }
struct ApplicationController {
    struct Media {
        bool prepared = false, waiting = true, fail = false;
        bool preparePresentation() {
            if (fail) throw fog::VideoDecodeError("prepare failed");
            return prepared;
        }
        unsigned videoFrames() const { return prepared ? 1 : 0; }
        unsigned videoWidth() const { return 1280; }
        unsigned videoHeight() const { return 720; }
        bool hardwareVideo() const { return false; }
        bool waitingForVideoKeyframe() const { return waiting; }
    } mediaStorage;
    struct Stream {
        bool decoderWaiting = true, assemblyWaiting = false, connected = true;
        unsigned clears = 0;
        void setDecoderWaitingForKeyframe(bool waiting) {
            decoderWaiting = waiting;
            if (!waiting) ++clears;
        }
        bool waitingForVideoKeyframe() const { return assemblyWaiting || decoderWaiting; }
        bool ready() const { return connected; }
        bool disconnected() const { return !connected; }
    } streamStorage;
    struct Reconnect {
        bool active = false, allowPrepared = true;
        bool recovering() const { return active; }
        bool prepared(std::chrono::steady_clock::time_point) {
            if (!allowPrepared) return false;
            active = false;
            return true;
        }
        unsigned attempts() const { return 1; }
    } transportReconnect_;
    struct Relay { void connected() {} } relaySelection_;
    struct Timing { void record(std::chrono::steady_clock::time_point) {} } prepareTiming_;
    Media* media_ = &mediaStorage;
    Stream* stream_ = &streamStorage;
    fog::VideoRecovery videoRecovery_;
    fog::stream::StreamInputGate streamInputGate_;
    bool videoLossWaiting_ = true, firstFramePresented_ = false;
    unsigned remembered = 0, recoveryErrors = 0;
    std::uint64_t videoWorkUs_ = 0;
    std::string status_;
    std::chrono::steady_clock::time_point lastVideo_{};
    ApplicationController() { streamInputGate_.suspendVideo(); }
    void diagnostic(const std::string&) {}
    void rememberDecodedGame() { ++remembered; }
    void recoverVideoError(const fog::VideoDecodeError&, const void*) {
        ++recoveryErrors;
        assert(videoRecovery_.begin(std::chrono::steady_clock::now()));
        stream_->setDecoderWaitingForKeyframe(true);
        streamInputGate_.suspendVideo();
    }
    void present() {
'''
harness += presentation
harness += r'''
        (void)prepareWorkUs;
    }
};
void decodedFrame(ApplicationController& app) {
    app.mediaStorage.prepared = true;
    app.mediaStorage.waiting = false;
}
void checkResumed(const ApplicationController& app) {
    assert(!app.streamStorage.decoderWaiting && !app.videoLossWaiting_);
    assert(!app.videoRecovery_.active() && app.firstFramePresented_);
    assert(app.streamStorage.clears == 1 && app.remembered == 1);
    assert(app.streamInputGate_.gamepadAllowed(false));
}
int main() {
    // A normal Moonlight startup has no decoder error/recovery episode.
    ApplicationController startup;
    decodedFrame(startup);
    startup.present();
    checkResumed(startup);

    ApplicationController recovery;
    assert(recovery.videoRecovery_.begin(std::chrono::steady_clock::now()));
    decodedFrame(recovery);
    recovery.present();
    checkResumed(recovery);

    // Decoder reset/generation changes may retain the old displayed texture,
    // but cannot resume input until preparePresentation yields a new image.
    ApplicationController generation;
    generation.present();
    assert(generation.streamStorage.decoderWaiting && generation.videoLossWaiting_);
    assert(generation.streamStorage.clears == 0 && generation.remembered == 0);
    assert(!generation.firstFramePresented_ && generation.streamInputGate_.videoSuspended());
    decodedFrame(generation);
    generation.present();
    checkResumed(generation);

    ApplicationController pendingRecovery;
    assert(pendingRecovery.videoRecovery_.begin(std::chrono::steady_clock::now()));
    pendingRecovery.present();
    assert(pendingRecovery.videoRecovery_.active() && pendingRecovery.streamStorage.decoderWaiting);
    assert(pendingRecovery.streamStorage.clears == 0 && pendingRecovery.streamInputGate_.videoSuspended());

    // Transport assembly loss is independent of the decoder readiness flag.
    ApplicationController assemblyLoss;
    decodedFrame(assemblyLoss);
    assemblyLoss.streamStorage.assemblyWaiting = true;
    assemblyLoss.present();
    assert(!assemblyLoss.streamStorage.decoderWaiting && assemblyLoss.streamStorage.assemblyWaiting);
    assert(assemblyLoss.videoLossWaiting_ && assemblyLoss.streamInputGate_.videoSuspended());

    ApplicationController reconnect;
    decodedFrame(reconnect);
    reconnect.transportReconnect_.active = true;
    reconnect.transportReconnect_.allowPrepared = false;
    reconnect.present();
    assert(!reconnect.streamStorage.decoderWaiting && reconnect.transportReconnect_.recovering());
    assert(reconnect.streamInputGate_.videoSuspended());

    ApplicationController expired;
    assert(expired.videoRecovery_.begin(std::chrono::steady_clock::now()-std::chrono::seconds(11)));
    decodedFrame(expired);
    bool rejected = false;
    try { expired.present(); } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected && expired.streamStorage.decoderWaiting && expired.streamStorage.clears == 0);
    assert(expired.streamInputGate_.videoSuspended());

    ApplicationController failedPrepare;
    failedPrepare.mediaStorage.fail = true;
    failedPrepare.present();
    assert(failedPrepare.recoveryErrors == 1 && failedPrepare.videoRecovery_.active());
    assert(failedPrepare.streamStorage.decoderWaiting && failedPrepare.streamStorage.clears == 0);
    assert(failedPrepare.streamInputGate_.videoSuspended());
    std::cout << "Playback decoder waiting: 8 coordinator scenarios PASS (real presentation glue, fake media/transport)\n";
}
'''
with tempfile.TemporaryDirectory(prefix='fog-playback-decoder-') as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(harness)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root / 'src'), str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=15)
