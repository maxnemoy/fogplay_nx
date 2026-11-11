#pragma once
#include "features/streaming/application/playback_telemetry.hpp"
#include "core/logging/run_log.hpp"
#include <string>
namespace stream::telemetry {
inline std::string timing(const Timing& t,const char* name) {
    return std::string(name)+"Calls="+std::to_string(t.calls)+" "+name+"AvgUs="+
        std::to_string(t.calls?t.totalUs/t.calls:0)+" "+name+"MaxUs="+std::to_string(t.maxUs);
}
inline const char* providerName(Provider p) { return p==Provider::Mts?"MTS":p==Provider::FogHub?"FH":"unknown"; }
inline const char* unavailableReason(Status s) {
    if(s.availability==Availability::NotStarted)return "not-started";
    if(s.reason==Reason::ClosedWithoutFinal)return "closed";
    if(s.reason==Reason::NotMigrated)return "not-migrated";
    if(s.reason==Reason::UnsupportedProvider)return "unsupported";
    if(s.reason==Reason::CaptureFailed)return "capture-failed";
    return "not-published";
}
inline std::string inputText(const Snapshot& s) {
    std::string out=std::string("provider=")+providerName(s.provider);
    if(!available(s.input))return out+" unavailable="+unavailableReason(s.input);
    const auto& p=s.inputLatency;
    return out+" scope=rolling-5s boundary=pad-to-send-return accepted-only=1 samples="+std::to_string(p.samples)+
        " p50Us="+std::to_string(p.p50Us)+" p95Us="+std::to_string(p.p95Us)+" p99Us="+std::to_string(p.p99Us)+
        " overflowLifetime="+std::to_string(p.overflowLifetime);
}
inline std::string transportText(const Snapshot& s) {
    if(!available(s.transport))return std::string("provider=")+providerName(s.provider)+" unavailable="+unavailableReason(s.transport);
    if(s.provider==Provider::Mts){
        static constexpr const char* names[]={"pump","nack","icePublish","iceDiag","icePair","iceSchedule","iceWorkerIdle","iceWork","receive","events","quality","controlFlush","pli","heartbeat"};
        std::string out="scope=transport-lifetime nested=1 iceThread=worker";
        for(std::size_t i=0;i<14;++i){if(i>=3&&i<=7&&!s.mts.iceWorkerPresent)continue;out+=" "+timing(s.mts.stages[i],names[i]);}
        return out;
    }
    const auto& f=s.fh;
    static constexpr const char* names[]={"worker_gap","drain","copy","complete"};
    std::string out="Moonlight cumulative ";
    for(std::size_t i=0;i<4;++i){const auto& t=f.adapter[i];if(i)out+=" | ";out+=std::string(names[i])+"_n="+std::to_string(t.calls)+" avg_us="+std::to_string(t.calls?t.totalUs/t.calls:0)+" max_us="+std::to_string(t.maxUs)+" over50ms="+std::to_string(t.over50ms);}
    out+=" empty_polls="+std::to_string(f.emptyPolls);
    if(!available(f.core))return out+" | core_unavailable="+unavailableReason(f.core);
    static constexpr const char* receive[]={"rx_cycle","rx_wait","rx_work","rx_loop_gap","rx_alloc","rx_decrypt"};
    out+=" | ";
    for(std::size_t i=0;i<6;++i){const auto& t=f.receive[i];if(i)out+=" ";out+=std::string(receive[i])+"(n,avg_us,max_us)="+std::to_string(t.calls)+","+std::to_string(t.calls?t.totalUs/t.calls:0)+","+std::to_string(t.maxUs);}
    return out+" rx_invalid_times="+std::to_string(f.invalidTimes)+" | rcvbuf(requested,selected,actual)="+
        std::to_string(f.bufferRequestedBytes)+","+std::to_string(f.bufferSelectedBytes)+","+std::to_string(f.bufferActualBytes)+
        " set_failures="+std::to_string(f.bufferSetFailures)+" last_set_error="+std::to_string(f.bufferSetLastError)+
        " query_ok="+std::to_string(f.bufferQuerySucceeded)+" query_error="+std::to_string(f.bufferQueryError);
}
inline std::string mtsControlText(const MtsControl& c) {
    return " controlPending="+std::to_string(c.pending)+" controlBytes="+std::to_string(c.bytes)+
        " controlRejected="+std::to_string(c.rejected)+" controlSent="+std::to_string(c.sent)+" controlPongCoalesced="+std::to_string(c.pongCoalesced)+
        " controlAgeMs="+std::to_string(c.ageMs)+" controlAgeMaxMs="+std::to_string(c.ageMaxMs)+
        " controlBackpressure="+std::to_string(c.backpressure)+" controlOperationStalled="+std::to_string(c.operationStalled)+
        " controlOperationMs="+std::to_string(c.operationMs)+" controlQueueResets="+std::to_string(c.queueResets)+
        " controlTransportActive="+std::to_string(c.transportActive)+" controlInFlight="+std::to_string(c.inFlight)+" controlInFlightMs="+std::to_string(c.inFlightMs)+
        " controlSendCalls="+std::to_string(c.send.calls)+" controlSendAvgUs="+std::to_string(c.send.calls?c.send.totalUs/c.send.calls:0)+" controlSendMaxUs="+std::to_string(c.send.maxUs)+
        " controlWorkerIdleCalls="+std::to_string(c.idle.calls)+" controlWorkerIdleAvgUs="+std::to_string(c.idle.calls?c.idle.totalUs/c.idle.calls:0)+" controlWorkerIdleMaxUs="+std::to_string(c.idle.maxUs);
}
inline std::string playbackText(const Playback& p) {
    static constexpr const char* names[]={"drain","present","transportPump","videoFeed","decoderReset","audioFeed","prepare","logEnqueue"};
    std::string out="Playback timing: ";for(std::size_t i=0;i<8;++i){if(i)out+=" ";out+=timing(p.stages[i],names[i]);}
    return out+" pumpGapMaxUs="+std::to_string(p.pumpGapMaxUs)+" outsideMeasuredPumpMaxUs="+std::to_string(p.outsideMeasuredPumpMaxUs)+
        " videoQueueWaitMaxMs="+std::to_string(p.videoQueueWaitMaxMs)+" audioQueueWaitMaxMs="+std::to_string(p.audioQueueWaitMaxMs)+
        " slowCycles="+std::to_string(p.slowCycles)+" batchVideoTotal="+std::to_string(p.batchVideoTotal)+" batchVideoMax="+std::to_string(p.batchVideoMax)+
        " frameBudgetYields="+std::to_string(p.frameBudgetYields)+" budgetYields="+std::to_string(p.budgetYields)+" catchupWindows="+std::to_string(p.catchupWindows)+
        " logQueued="+std::to_string(p.logQueued)+" logDropped="+std::to_string(p.logDropped)+" logWritten="+std::to_string(p.logWritten)+
        " logBatches="+std::to_string(p.logBatches)+" logBatchLastUs="+std::to_string(p.logBatchLastUs)+" logBatchMaxUs="+std::to_string(p.logBatchMaxUs);
}
enum class Line { Transport, TransportFinal, TransportClosed, Input, Playback };
// No owners, callbacks, SDK objects, handles or strings enter this record.
class Record final : public fog::RunLog::DeferredRecord {
public:
    Record(std::uint64_t elapsedMs,Line kind,Snapshot value):elapsedMs_(elapsedMs),kind_(kind),snapshot_(value){}
    Record(std::uint64_t elapsedMs,Playback value):elapsedMs_(elapsedMs),kind_(Line::Playback),playback_(value){}
    std::size_t rawBytes()const noexcept override{return sizeof(*this);}
    std::size_t maxOutputBytes()const noexcept override{return kind_==Line::Input?1100:kind_==Line::Playback?2112:4160;}
    void format(fog::RunLog::DeferredSink& sink)const override{
        sink.append("["+std::to_string(elapsedMs_)+" ms] ");
        if(kind_==Line::Playback){sink.append(playbackText(playback_));return;}
        if(kind_==Line::Input){sink.append("Input latency: ");sink.append(inputText(snapshot_));return;}
        sink.append(kind_==Line::TransportFinal?"Stream transport final: ":kind_==Line::TransportClosed?"Stream transport closed: ":"Stream transport: ");
        sink.append(transportText(snapshot_));
    }
private:
    std::uint64_t elapsedMs_;
    Line kind_;
    Snapshot snapshot_{};
    Playback playback_{};
};
static_assert(sizeof(Record)<=4096);
}
