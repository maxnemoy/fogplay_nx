"""Execute production worker admission and AUTO handoff with bounded fakes."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/features/streaming/presentation/playback_controller.cpp').read_text()
begin = source.index('    const bool started = stream_->start(')
end = source.index('    streamStarted_ = lastVideo_', begin)
admission = source[begin:end]
begin = source.index('    const auto workerLoad = media_->workerVideoLoad();')
end = source.index('    const auto now = ', begin)
observation = source[begin:end]
harness = r'''
#include "core/constants/stage3_options.hpp"
#include "features/streaming/domain/streaming_client.hpp"
#include <cassert>
#include <string>
namespace fog {
struct Media {
    enum class WorkerPolicy { FogHub, Mts };
    struct WorkerVideoLoad { std::uint64_t decodeUs=60000,convertUs=9000,generation=7; };
    unsigned starts=0; bool worker=false; int fps=0;
    WorkerPolicy policy=WorkerPolicy::FogHub;
    template<class T> void startWorkers(T&,int rate,WorkerPolicy selected) {++starts;worker=true;fps=rate;policy=selected;}
    WorkerVideoLoad workerVideoLoad() const {return {};}
    bool workersRunning() const {return worker;}
    unsigned presentedFrames() const {return 10;}
    unsigned videoFrames() const {return 12;}
    unsigned supersededFrames() const {return 2;}
    unsigned audioUnderruns() const {return 0;}
    unsigned audioQueueResets() const {return 0;}
    bool waitingForVideoKeyframe() const {return false;}
    int videoWidth() const {return 1280;}
    int videoHeight() const {return 720;}
};
}
struct Application {
    struct Transport {
        bool accept=true; stream::PlaybackObservation observed; unsigned observations=0;
        bool start(const stream::StreamConnection&) {return accept;}
        void observePlayback(const stream::PlaybackObservation& value) {observed=value;++observations;}
    } transport;
    fog::Media media;
    Transport* stream_=&transport; fog::Media* media_=&media;
    fog::EntityId streamRental_{fog::ProviderId::Mts,"test"};
    int activeFramesPerSecond_=60;
    std::uint64_t videoWorkUs_=4000,videoDecodeErrors_=0;
    bool videoLossWaiting_=false;
    struct Recovery {bool recovering()const{return false;}bool active()const{return false;}} transportReconnect_,videoRecovery_;
    void diagnostic(const std::string&){}
    bool start(bool rtcTransport,bool resumeOnly=false) {
'''+admission+r'''
        return started;
    }
    void observe() {
'''+observation+r'''
    }
};
int main() {
    for (bool mts:{false,true}) for(bool accepts:{false,true}) for(int fps:{30,60}) {
        Application app;app.transport.accept=accepts;app.activeFramesPerSecond_=fps;
        app.streamRental_.provider=mts?fog::ProviderId::Mts:fog::ProviderId::FogHub;
        assert(app.start(mts)==accepts);
        const bool expected=accepts&&(!mts||fog::stage3::mtsMediaWorkers);
        assert(app.media.starts==unsigned(expected));
        if(expected) {assert(app.media.fps==fps);assert(app.media.policy==(mts?fog::Media::WorkerPolicy::Mts:fog::Media::WorkerPolicy::FogHub));}
        app.observe();
        assert(app.transport.observations==unsigned(mts));
        if(mts) {
            const auto& sample=app.transport.observed;
            assert(sample.workUs==4000 && sample.decoderWorkUs==69000);
            assert(sample.videoGeneration==7 && sample.workerVideo==expected);
            assert(sample.presentedFrames==10 && sample.decodedFrames==12);
        }
    }
}
'''
with tempfile.TemporaryDirectory(prefix='fog-stage3-glue-') as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(harness)
    for enabled in (0, 1):
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                        '-I'+str(root/'src'), '-DFOGPLAY_MTS_MEDIA_WORKERS='+str(enabled),
                        str(path/'test.cpp'), '-o', str(path/'test')], check=True)
        subprocess.run([str(path/'test')], check=True, timeout=10)
print('Stage3 production glue: MTS/FH start/failure, 30/60 FPS, sync comparison and independent AUTO workload PASS')
