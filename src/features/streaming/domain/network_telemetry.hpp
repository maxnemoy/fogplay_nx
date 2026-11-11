#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
namespace stream::telemetry::network {
// Owner-serialized numeric values. Durations/timestamps explicitly use ms or us.
// No packet bytes, source identifiers, strings or SDK objects cross this boundary.
struct RtpCounters {
    std::uint64_t received=0,candidates=0,reordered=0,duplicate=0,late=0;
    std::uint64_t gaps=0,missing=0,malformed=0,dropped=0,emitted=0;
    std::uint64_t resyncs=0,recovered=0,pressure=0,timeout=0,eventOverflow=0;
    std::uint64_t faultSerial=0,foreignSsrc=0,padding=0;
};
struct GapExpiry {
    std::int64_t first=0,last=0,pendingFirst=0,pendingLast=0;
    std::uint64_t detectedMs=0,oldestMs=0,budgetMs=0,deadlineMs=0,expiredMs=0,overshootMs=0;
    std::uint64_t firstNackMs=0,lastNackMs=0,nackAttempts=0,requestedPackets=0,pendingDepth=0;
};
struct LateExpiry {
    std::int64_t sequence=0,first=0,last=0;
    std::uint64_t arrivedMs=0,deadlineMs=0,lateByMs=0,firstNackMs=0,lastNackMs=0,nackAttempts=0;
};
struct RtpSnapshot {
    RtpCounters counters{};
    bool initialized=false,waiting=true,nackEnabled=false,gapActive=false;
    std::size_t pendingPackets=0,pendingBytes=0,assemblyBytes=0,readyFrames=0,readyBytes=0,pendingEvents=0;
    std::int64_t expectedSequence=0,lastFirstSequence=0,lastLastSequence=0;
    std::uint32_t lastTimestamp=0;bool lastTimestampKnown=false;
    std::uint64_t nackPackets=0,repairedPackets=0,repairWindowMs=0,gapRepairWindowMs=0,nackRetryMs=0;
    std::uint64_t arrivalSamples=0,lateSamples=0,retrySamples=0;
    std::uint64_t arrivalLastMs=0,arrivalMeanMs=0,arrivalMaxMs=0,arrivalDeviationMs=0;
    std::uint64_t arrivalP50Ms=0,arrivalP95Ms=0,arrivalBucketMs=10;
    // Existing lifetime 10ms histogram; percentile values remain bucket lower
    // bounds, not exact latency samples or a network RTT measurement.
    std::array<std::uint64_t,201> arrivalHistogram{};
    std::uint64_t roundTripMs=0;bool roundTripKnown=false;
    std::uint64_t arrivalBeforeRtt=0,arrivalAfterRtt=0,arrivalRttUnknown=0;
    std::size_t activeRequests=0,history=0;
    std::uint64_t historyEvicted=0,catchupYields=0;
    std::uint64_t expiredGaps=0,expiredPackets=0,expiredNoNackGaps=0,expiredUnrequestedPackets=0;
    std::uint64_t expiredNackAttempts=0,maxExpiryOvershootMs=0,matchedLatePackets=0;
    GapExpiry lastExpiry{};LateExpiry lastLate{};
    bool lastExpiryKnown=false,lastLateKnown=false;
};
struct RepairPayload { bool present=false,nack=false; };
struct RtxPayload { bool present=false;std::uint32_t originalPayload=0;bool originalValid=false; };
struct RepairSnapshot {
    std::array<RepairPayload,128> media{};std::array<RtxPayload,128> rtx{};
    std::size_t mediaCount=0,rtxCount=0,mediaOutOfRange=0,rtxOutOfRange=0;
    bool fid=false,sourceKnown=false,nackEnabled=false;
    std::uint64_t rtxReceived=0,restored=0,rejected=0,controlRejected=0,nackBatches=0;
};
struct IncomingSnapshot {
    // Ignored,Stats,Ping,Rumble,InvalidRumble,Close,Error,Invalid.
    std::array<std::uint64_t,8> counts{};
    bool statsPending=false,firstPendingKnown=false;
    std::uint64_t firstPendingMs=0,burst=0,highBurst=0,delayHighMs=0,statsCoalesced=0,pongsSent=0,pongsFailed=0;
};
struct IncomingSchemaSnapshot {
    std::uint64_t received=0,coalesced=0,overflow=0,overflowReported=0;
    std::size_t schemas=0,pendingSchemas=0;
    std::uint64_t pendingEvents=0;
};
enum class PerformanceUnit : std::uint8_t { Percent, RawUnspecified };
struct PerformanceField {
    double value=0;std::uint64_t sampledAtMs=0,ageMs=0;
    bool known=false,ageKnown=false,stale=true;
    PerformanceUnit unit=PerformanceUnit::RawUnspecified;
};
struct PerformanceSnapshot {
    // cpu_usage,gpu_usage,ping,encode,encodefull,grab,serverfps,serverqueuefps,
    // packetize,sendsamples_video,sendsamples_audio; exactly the legacy allowlist.
    std::array<PerformanceField,11> fields{};
    std::uint64_t capturedAtMs=0,staleAfterMs=5000;
};
enum class HistoryKind { Peer, Ice, Gathering, ChannelOpen, Closed, Failure };
struct HistoryEntry { HistoryKind kind=HistoryKind::Peer;int state=0;std::uint64_t at=0; };
struct ObservedAge { std::uint64_t valueMs=0;bool known=false; };
struct HistorySnapshot {
    std::array<HistoryEntry,16> entries{};std::size_t count=0; // oldest first
    bool hasTerminal=false;HistoryEntry terminal{};
    ObservedAge terminalVideoAge{},terminalAudioAge{},terminalControlAge{};
};
struct RtcpCounters {
    std::uint64_t rtpPackets=0,rtcpPackets=0,senderReports=0,malformedControl=0;
    std::uint64_t regularRequests=0,coalescedRequests=0,discardedReports=0;
    std::uint64_t sendAttempts=0,sentPackets=0,sendFailures=0,rrPackets=0,rembPackets=0,compoundPackets=0;
    std::uint64_t pendingAgeMaxUs=0,sendTotalUs=0,sendMaxUs=0;
    bool pending=false,inFlight=false;
};
struct RtcpSnapshot {
    RtcpCounters feedback{};
    std::uint64_t flushCalls=0,flushTotalUs=0,flushMaxUs=0,failures=0,rejectedSessions=0;
    std::size_t sessions=0;
    bool running=false,stopped=false;
};
static_assert(std::is_trivially_copyable_v<RtpSnapshot>);
static_assert(std::is_trivially_copyable_v<RepairSnapshot>);
static_assert(std::is_trivially_copyable_v<RtcpSnapshot>);
static_assert(sizeof(RtpSnapshot)<=4096&&sizeof(RepairSnapshot)<=4096);
}
