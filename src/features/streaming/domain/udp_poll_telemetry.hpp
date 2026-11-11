#pragma once
#include <cstdint>
namespace stream::telemetry::network {
// Counters describe backend UDP service, not packet loss or CPU time.
struct UdpPollSummary {
    bool hasValue=false,clockAvailable=false,sampleClockAvailable=false,lastClockAvailable=false;
    bool pendingYield=false,rcvbufChecked=false,rcvbufAvailable=false,mappingKnown=false;
    unsigned availability=1,provenance=0;
    int rcvbufBytes=0,rcvbufErrno=0;
    std::uint64_t sampleMonoUs=0,queryBeforeUs=0,queryAfterUs=0,clockInvalid=0;
    std::uint64_t batches=0,bytes=0,timedBatches=0,elapsedTotalUs=0,elapsedMaxUs=0;
    std::uint64_t lastStartMonoUs=0,lastEndMonoUs=0,serviceGaps=0,serviceGapMaxUs=0,readyDelays=0,readyDelayMaxUs=0;
    std::uint64_t eventsTotal=0,eventsOverwritten=0,eventCursor=0,sourceMissed=0,journalSkipped=0;
    // Phase totals cover only valid sampled batches; elapsed includes preemption.
    unsigned phaseSampleEvery=0;
    std::uint64_t legacyEligibleBatches=0,phaseSampledBatches=0,phaseTimedBatches=0,phaseInvalidBatches=0;
    std::uint64_t phaseSampledDatagrams=0,phaseTimedDatagrams=0,phaseSampledReceiveCalls=0,phaseTimedReceiveCalls=0;
    std::uint64_t phaseRecvTotalUs=0,phaseRecvBatchMaxUs=0,phaseCallbackTotalUs=0,phaseCallbackBatchMaxUs=0;
};
struct UdpPollEvent {
    std::uint64_t sequence=0,startMonoUs=0,elapsedUs=0,bytes=0,serviceGapUs=0,readyDelayUs=0;
    std::uint64_t sampleMonoUs=0,queryBeforeUs=0,queryAfterUs=0,sourceMissed=0;
    std::uint64_t phaseRecvUs=0,phaseCallbackUs=0;
    unsigned datagrams=0,flags=0,exitReason=0;
    bool mappingKnown=false;
};
}
