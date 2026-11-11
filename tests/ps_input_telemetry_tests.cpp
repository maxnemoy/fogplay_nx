#include "features/playstation/application/input_telemetry.hpp"
#include <cassert>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <unistd.h>

namespace p=fog::playstation::telemetry;
namespace {
std::mutex mutex;std::condition_variable changed;bool entered=false,released=false;
class Gate final:public fog::RunLog::DeferredRecord {
public:
    std::size_t rawBytes()const noexcept override{return sizeof(*this);}
    std::size_t maxOutputBytes()const noexcept override{return 16;}
    void format(fog::RunLog::DeferredSink& sink)const override{
        std::unique_lock lock(mutex);entered=true;changed.notify_all();changed.wait(lock,[]{return released;});sink.append("gate");
    }
};
void golden(){
    p::Snapshot value;value.final=true;value.elapsedMs=17;value.sensor.enabled=true;
    value.touch.lastDx=-2;value.touch.lastDy=3;value.native.resetUpQueued=2;
    value.contactsInitialized=true;value.sensor.motion.gyroTurnsPerSecond={1.25f,-2.5f,0};
    assert(p::inputText(value)=="PS input final: scope=transport ack=unavailable enabled=1 touchValid=0 contacts=0 hidContacts=0 sample=0 down=0 up=0 moves=0 swipes=0 swipeThresholdScreenPx=60 lastFinger=0 lastPS5=0,0 lastGestureScreenDelta=-2,3 lastGestureMs=0 suppressedFrames=0 touchStaleReleases=0 gamepadQueued=0 gamepadRejected=0");
    assert(p::nativeText(value)=="PS native queued: down=0 move=0 up=0 accel=0 gyro=0 rejected=0 taps=0 suppressed=0 resetUpQueued=2 resetRejected=0 contactsInitialized=1 ack=unavailable");
    assert(p::sensorText(value).find("gyroTurnsPerSecond=1.2500,-2.5000,0.0000")!=std::string::npos);
    p::Record frozen(value);value.final=false;value.native.resetUpQueued=99;
    fog::RunLog::DeferredSink sink(frozen.maxOutputBytes(),frozen.lineCount());frozen.format(sink);sink.finish();
    assert(frozen.lineCount()==3&&sink.text().find("[17 ms] PS input final:")==0);
    assert(sink.text().find("resetUpQueued=2 ")!=std::string::npos);
    value.hasTransport=true;value.transport.provider=stream::telemetry::protocol::Provider::Mts;
    value.transport.state=stream::telemetry::protocol::State::Live;
    p::Record withTransport(value);fog::RunLog::DeferredSink next(withTransport.maxOutputBytes(),withTransport.lineCount());
    withTransport.format(next);next.finish();assert(withTransport.lineCount()==4);
    assert(next.text().find("[17 ms] PS transport input: ")!=std::string::npos);
}
void limits(){
    p::Snapshot value;value.hasTransport=true;value.elapsedMs=UINT64_MAX;
    value.sensor.motion.gyroTurnsPerSecond.fill(std::numeric_limits<float>::max());
    value.sensor.motion.accelerationG.fill(-std::numeric_limits<float>::max());
    value.sensor.motion.psGyroRadiansPerSecond.fill(std::numeric_limits<float>::max());
    value.sensor.motion.psAccelerationG.fill(-std::numeric_limits<float>::max());
    value.gyroPeak=value.accelPeak=std::numeric_limits<float>::max();
    value.gamepadAccepted=value.gamepadRejected=value.motionMovedFrames=UINT64_MAX;
    value.native={UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX};
    value.sensor.counters={UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX};
    value.transport.provider=stream::telemetry::protocol::Provider::Mts;value.transport.state=stream::telemetry::protocol::State::Live;
    auto& input=value.transport.mts.input;input.nativeQueued.fill(UINT64_MAX);input.nativeRejected.fill(UINT64_MAX);
    input.nativeAttempt.fill(UINT64_MAX);input.nativeSent.fill(UINT64_MAX);input.nativeFailed.fill(UINT64_MAX);
    input.nativeDropped.fill(UINT64_MAX);input.nativeCoalesced.fill(UINT64_MAX);
    p::Record record(value);fog::RunLog::DeferredSink sink(record.maxOutputBytes(),record.lineCount());record.format(sink);sink.finish();
    assert(sink.text().size()<=record.maxOutputBytes());
    assert(sink.text().find("contactsInitialized=0 ack=unavailable")!=std::string::npos);
}
void writerLifetime(){
    char directory[]="/tmp/fog-ps-telemetry-XXXXXX";assert(mkdtemp(directory));
    fog::RunLog log;assert(log.start(directory));assert(log.appendDeferred(std::make_unique<Gate>()));log.requestFlush();
    {std::unique_lock lock(mutex);assert(changed.wait_for(lock,std::chrono::seconds(2),[]{return entered;}));}
    {
        p::Snapshot owner;owner.elapsedMs=31;owner.final=true;owner.gamepadAccepted=71;
        assert(p::enqueue(log,owner));owner.gamepadAccepted=999;
    }
    assert(log.append("NEXT_STREAM"));
    {std::lock_guard lock(mutex);released=true;}changed.notify_all();
    assert(log.flush());const auto stats=log.stats();assert(stats.writtenLines==5&&stats.formatFailures==0);log.close();
    std::ifstream file(std::string(directory)+"/last-run.log");std::string output{std::istreambuf_iterator<char>(file),{}};
    assert(output.find("[31 ms] PS input final:")<output.find("NEXT_STREAM"));
    assert(output.find("gamepadQueued=71 ")!=std::string::npos&&output.find("gamepadQueued=999 ")==std::string::npos);
    std::filesystem::remove_all(directory);
}
}
int main(){golden();limits();writerLifetime();std::cout<<"PS typed legacy keys, immutable final, checked bounds and real FIFO writer PASS; Snapshot="<<sizeof(p::Snapshot)<<" Record="<<sizeof(p::Record)<<'\n';}
