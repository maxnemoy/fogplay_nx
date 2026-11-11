#pragma once
#include "features/playstation/domain/sensors.hpp"
#include "features/playstation/domain/touch_trace.hpp"
#include "features/streaming/application/protocol_diagnostics_formatter.hpp"
#include "core/logging/run_log.hpp"
#include <iomanip>
#include <locale>
#include <sstream>
#include <type_traits>

namespace fog::playstation::telemetry {
struct NativeCounters {
    std::uint64_t down=0,move=0,up=0,accel=0,gyro=0,rejected=0,taps=0,suppressed=0,resetUpQueued=0,resetRejected=0;
};
struct Snapshot {
    PsSensorSnapshot sensor{};
    TouchTrace touch{};
    NativeCounters native{};
    ::stream::telemetry::protocol::ProtocolSnapshot transport{};
    std::uint64_t elapsedMs=0,gamepadAccepted=0,gamepadRejected=0,motionMovedFrames=0;
    float gyroPeak=0,accelPeak=0;
    bool final=false,contactsInitialized=false,hasTransport=false;
};
static_assert(std::is_trivially_copyable_v<Snapshot> && sizeof(Snapshot)<=8192);
template<std::size_t N> std::string values(const std::array<float,N>& array){
    std::ostringstream out;out.imbue(std::locale::classic());out<<std::fixed<<std::setprecision(4);
    for(std::size_t i=0;i<N;++i){if(i)out<<',';out<<array[i];}
    return out.str();
}
inline std::string inputText(const Snapshot& value){
    const auto& s=value.sensor;const auto& c=s.counters;const auto& t=value.touch;
    return std::string(value.final?"PS input final: ":"PS input: ")+"scope=transport ack=unavailable enabled="+std::to_string(s.enabled)+
        " touchValid="+std::to_string(s.touchValid)+" contacts="+std::to_string(s.touchCount)+
        " hidContacts="+std::to_string(s.hidTouchCount)+" sample="+std::to_string(s.touchSampleNumber)+
        " down="+std::to_string(t.down)+" up="+std::to_string(t.up)+" moves="+std::to_string(t.moves)+" swipes="+std::to_string(t.swipes)+
        " swipeThresholdScreenPx=60 lastFinger="+std::to_string(t.lastFinger)+" lastPS5="+std::to_string(t.lastX)+","+std::to_string(t.lastY)+
        " lastGestureScreenDelta="+std::to_string(t.lastDx)+","+std::to_string(t.lastDy)+" lastGestureMs="+std::to_string(t.lastDurationMs)+
        " suppressedFrames="+std::to_string(t.suppressedFrames)+" touchStaleReleases="+std::to_string(c.touchStaleReleases)+
        " gamepadQueued="+std::to_string(value.gamepadAccepted)+" gamepadRejected="+std::to_string(value.gamepadRejected);
}
inline std::string sensorText(const Snapshot& value){
    const auto& s=value.sensor;const auto& c=s.counters;const auto& m=s.motion;
    return "PS sensors: npad="+std::to_string(s.npadId)+" style="+std::to_string(s.style)+
        " handles="+std::to_string(s.handleCount)+" bindCount="+std::to_string(c.handleRefreshes)+
        " getResult="+std::to_string(s.lastGetHandlesResult)+" startResult="+std::to_string(s.lastStartResult)+
        " stopResult="+std::to_string(s.lastStopResult)+" startFailures="+std::to_string(c.startFailures)+
        " stopFailures="+std::to_string(c.stopFailures)+" valid="+std::to_string(m.valid)+" fresh="+std::to_string(m.fresh)+
        " source="+std::to_string(m.sourceIndex)+" gyroPeakRad="+std::to_string(value.gyroPeak)+" accelPeakG="+std::to_string(value.accelPeak)+
        " motionMovedFrames="+std::to_string(value.motionMovedFrames)+" sample="+std::to_string(m.sampleNumber)+
        " samples="+std::to_string(c.motionSamples)+" missing="+std::to_string(c.motionMissing)+" invalid="+std::to_string(c.motionInvalid)+
        " stale="+std::to_string(c.motionStaleReleases)+" gyroTurnsPerSecond="+values(m.gyroTurnsPerSecond)+
        " accelerationG="+values(m.accelerationG)+" psGyroRadiansPerSecond="+values(m.psGyroRadiansPerSecond)+" psAccelerationG="+values(m.psAccelerationG);
}
inline std::string nativeText(const Snapshot& value){
    const auto& n=value.native;
    return "PS native queued: down="+std::to_string(n.down)+" move="+std::to_string(n.move)+" up="+std::to_string(n.up)+
        " accel="+std::to_string(n.accel)+" gyro="+std::to_string(n.gyro)+" rejected="+std::to_string(n.rejected)+" taps="+std::to_string(n.taps)+
        " suppressed="+std::to_string(n.suppressed)+" resetUpQueued="+std::to_string(n.resetUpQueued)+" resetRejected="+std::to_string(n.resetRejected)+
        " contactsInitialized="+std::to_string(value.contactsInitialized)+" ack=unavailable";
}
class Record final:public ::fog::RunLog::DeferredRecord {
public:
    static constexpr std::size_t MaximumOutputBytes=2048+2048+1024+3072+128;
    explicit Record(const Snapshot& snapshot):value_(snapshot){}
    std::size_t rawBytes()const noexcept override{return sizeof(*this);}
    std::size_t lineCount()const noexcept override{return value_.hasTransport?4:3;}
    std::size_t maxOutputBytes()const noexcept override{return MaximumOutputBytes-(value_.hasTransport?0:3072);}
    void format(::fog::RunLog::DeferredSink& sink)const override{
        line(sink,inputText(value_),2048);sink.nextLine();
        line(sink,sensorText(value_),2048);sink.nextLine();
        line(sink,nativeText(value_),1024);
        if(value_.hasTransport){sink.nextLine();line(sink,"PS transport input: "+::stream::telemetry::protocol::inputText(value_.transport),3072);}
    }
private:
    void line(::fog::RunLog::DeferredSink& sink,std::string text,std::size_t limit)const{
        if(text.size()>limit)text.resize(limit);
        sink.append("["+std::to_string(value_.elapsedMs)+" ms] ");sink.append(text);
    }
    const Snapshot value_;
};
static_assert(sizeof(Record)<=::fog::RunLog::maxDeferredRawBytes);
inline bool enqueue(::fog::RunLog& log,const Snapshot& value)noexcept{
    try{return log.appendDeferred(std::make_unique<Record>(value));}
    catch(...){log.appendDeferred({});return false;}
}
}
