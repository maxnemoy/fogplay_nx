#pragma once
#include "features/streaming/domain/stream_telemetry.hpp"
#include <atomic>
namespace stream::telemetry {
inline std::uint64_t nextStreamEpoch() noexcept {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1,std::memory_order_relaxed);
}
template<class T> Timing captureTiming(const T& t) { return {t.calls,t.totalUs,t.maxUs,0}; }
template<class T> Timing captureAtomicTiming(const T& t) {
    return {t.count.load(std::memory_order_relaxed),t.totalUs.load(std::memory_order_relaxed),
        t.maxUs.load(std::memory_order_relaxed),t.over50ms.load(std::memory_order_relaxed)};
}
template<class P> Percentiles capturePercentiles(const P& p) { return {p.samples,p.p50Us,p.p95Us,p.p99Us,p.overflow}; }
template<class C> MtsControl captureMtsControl(const C& c) {
    MtsControl out;
    out.pending=c.pending;out.bytes=c.bytes;out.rejected=c.rejected;out.sent=c.sent;
    out.pongCoalesced=c.pongCoalesced;out.ageMs=c.ageMs;out.ageMaxMs=c.ageMaxMs;
    out.operationMs=c.operationMs;out.queueResets=c.queueResets;out.inFlightMs=c.inFlightMs;
    out.backpressure=c.backpressure;out.operationStalled=c.operationStalled;out.transportActive=c.transportActive;out.inFlight=c.inFlight;
    out.send={c.sendCalls,c.sendTotalUs,c.sendMaxUs,0};out.idle={c.waitCalls,c.waitTotalUs,c.waitMaxUs,0};
    return out;
}
template<class C> void captureFhCore(FhTransport& f,const C& c) {
    f.receive={{{c.videoReceiveCycleCount,c.videoReceiveCycleTotalUs,c.videoReceiveCycleMaxUs,0},
        {c.videoReceiveCycleCount,c.videoReceiveWaitTotalUs,c.videoReceiveWaitMaxUs,0},
        {c.videoReceiveCycleCount,c.videoReceiveWorkTotalUs,c.videoReceiveWorkMaxUs,0},
        {c.videoReceiveLoopGapCount,c.videoReceiveLoopGapTotalUs,c.videoReceiveLoopGapMaxUs,0},
        {c.videoReceiveAllocationCount,c.videoReceiveAllocationTotalUs,c.videoReceiveAllocationMaxUs,0},
        {c.videoReceiveDecryptCount,c.videoReceiveDecryptTotalUs,c.videoReceiveDecryptMaxUs,0}}};
    f.invalidTimes=c.videoReceiveTimingInvalidTimes;f.bufferRequestedBytes=c.videoReceiveBufferRequestedBytes;
    f.bufferSelectedBytes=c.videoReceiveBufferSelectedBytes;f.bufferActualBytes=c.videoReceiveBufferActualBytes;
    f.bufferSetFailures=c.videoReceiveBufferSetFailures;f.bufferSetLastError=c.videoReceiveBufferSetLastError;
    f.bufferQuerySucceeded=c.videoReceiveBufferQuerySucceeded;f.bufferQueryError=c.videoReceiveBufferQueryError;
}
}
