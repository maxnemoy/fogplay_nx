#pragma once
#include "features/streaming/domain/udp_poll_telemetry.hpp"
#include <algorithm>
#include <type_traits>
namespace stream::telemetry::network {
// Application owner only. The rich source was read by the existing ICE worker.
// No backend lock, formatting or ring copies in per-frame numeric snapshots.
class UdpPollCapture {
public:
    static constexpr unsigned MaxEventsPerPublication=8;
    const UdpPollSummary& summary() const noexcept { return value_; }
    template<class Source,class Emit>
    void publish(const Source& d,std::uint64_t before,std::uint64_t after,Emit emit) {
        using Ice=std::decay_t<decltype(d.ice)>;
        value_.availability=static_cast<unsigned>(d.ice.availability);
        if(d.ice.availability!=Ice::Availability::Available)return;
        const bool live=d.ice.provenance==Ice::Provenance::Live;
        // Retained samples keep their original query bracket, never stop-time age.
        if(live){value_.queryBeforeUs=before;value_.queryAfterUs=after;
            value_.mappingKnown=d.sampleClockAvailable&&after>=before;}
        else if(!value_.hasValue||value_.sampleMonoUs!=d.sampleMonoUs)value_.mappingKnown=false;
        value_.hasValue=true;value_.provenance=static_cast<unsigned>(d.ice.provenance);
        value_.clockAvailable=d.clockAvailable;value_.sampleClockAvailable=d.sampleClockAvailable;
        value_.lastClockAvailable=d.lastClockAvailable;value_.pendingYield=d.pendingYield;
        value_.rcvbufChecked=d.rcvbufChecked;value_.rcvbufAvailable=d.rcvbufAvailable;
        value_.rcvbufBytes=d.rcvbufBytes;value_.rcvbufErrno=d.rcvbufErrno;
        value_.sampleMonoUs=d.sampleMonoUs;value_.clockInvalid=d.clockInvalid;
        value_.batches=d.batches;value_.bytes=d.bytes;value_.timedBatches=d.timedBatches;
        value_.elapsedTotalUs=d.elapsedTotalUs;value_.elapsedMaxUs=d.elapsedMaxUs;
        value_.lastStartMonoUs=d.lastStartMonoUs;value_.lastEndMonoUs=d.lastEndMonoUs;
        value_.serviceGaps=d.serviceGaps;value_.serviceGapMaxUs=d.serviceGapMaxUs;
        value_.readyDelays=d.readyDelays;value_.readyDelayMaxUs=d.readyDelayMaxUs;
        value_.eventsTotal=d.eventsTotal;value_.eventsOverwritten=d.eventsOverwritten;
        value_.phaseSampleEvery=d.phaseSampleEvery;
        value_.legacyEligibleBatches=d.legacyEligibleBatches;
        value_.phaseSampledBatches=d.phaseSampledBatches;
        value_.phaseTimedBatches=d.phaseTimedBatches;
        value_.phaseInvalidBatches=d.phaseInvalidBatches;
        value_.phaseSampledDatagrams=d.phaseSampledDatagrams;
        value_.phaseTimedDatagrams=d.phaseTimedDatagrams;
        value_.phaseSampledReceiveCalls=d.phaseSampledReceiveCalls;
        value_.phaseTimedReceiveCalls=d.phaseTimedReceiveCalls;
        value_.phaseRecvTotalUs=d.phaseRecvTotalUs;
        value_.phaseRecvBatchMaxUs=d.phaseRecvBatchMaxUs;
        value_.phaseCallbackTotalUs=d.phaseCallbackTotalUs;
        value_.phaseCallbackBatchMaxUs=d.phaseCallbackBatchMaxUs;
        const auto count=std::min<std::size_t>(d.eventCount,d.events.size());
        unsigned admitted=0;
        for(std::size_t i=0;i<count;++i){const auto& e=d.events[i];
            if(e.sequence<=value_.eventCursor)continue;
            value_.sourceMissed+=e.sequence-value_.eventCursor-1;
            UdpPollEvent out;
            out.sequence=e.sequence;out.startMonoUs=e.startMonoUs;out.elapsedUs=e.elapsedUs;
            out.readyDelayUs=e.readyDelayUs;out.bytes=e.bytes;out.serviceGapUs=e.serviceGapUs;out.datagrams=e.datagrams;
            out.phaseRecvUs=e.phaseRecvUs;out.phaseCallbackUs=e.phaseCallbackUs;
            out.flags=e.flags;out.exitReason=e.exitReason;out.sampleMonoUs=d.sampleMonoUs;
            out.queryBeforeUs=value_.queryBeforeUs;out.queryAfterUs=value_.queryAfterUs;
            out.mappingKnown=value_.mappingKnown;out.sourceMissed=value_.sourceMissed;
            if(admitted<MaxEventsPerPublication&&emit(out))++admitted;
            else ++value_.journalSkipped;
            value_.eventCursor=e.sequence;
        }
    }
private:UdpPollSummary value_{};
};
}
