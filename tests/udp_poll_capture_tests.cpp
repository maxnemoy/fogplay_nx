#include "features/streaming/infrastructure/udp_poll_capture.hpp"
#include "features/streaming/application/network_diagnostics_formatter.hpp"
#include "icediagnostics.hpp"
#include "features/streaming/infrastructure/bounded_network_events.hpp"
#include <cassert>
#include <iostream>
#include <limits>
#include <vector>
namespace n=stream::telemetry::network;
int main(){
    n::UdpPollCapture capture;
    rtc::IceUdpPollDiagnostics source;
    using I=rtc::IceDiagnostics;
    std::vector<n::UdpPollEvent> events;
    auto emit=[&](const auto& e){events.push_back(e);return true;};
    capture.publish(source,10,20,emit);
    assert(!capture.summary().hasValue&&events.empty());
    source.ice.availability=I::Availability::Available;source.ice.provenance=I::Provenance::Live;
    source.sampleClockAvailable=source.clockAvailable=true;source.sampleMonoUs=500;
    source.phaseSampleEvery=16;source.legacyEligibleBatches=55;
    source.phaseSampledBatches=3;source.phaseTimedBatches=2;source.phaseInvalidBatches=1;
    source.phaseSampledDatagrams=12;source.phaseTimedDatagrams=9;
    source.phaseSampledReceiveCalls=15;source.phaseTimedReceiveCalls=11;
    source.phaseRecvTotalUs=70;source.phaseRecvBatchMaxUs=50;
    source.phaseCallbackTotalUs=20;source.phaseCallbackBatchMaxUs=15;
    source.batches=12;source.eventsTotal=19;source.eventsOverwritten=3;source.eventCount=16;
    for(unsigned i=0;i<16;++i){auto& e=source.events[i];e.sequence=i+4;e.startMonoUs=100+i;
        e.elapsedUs=20;e.flags=8|4|64|128;e.phaseRecvUs=12;e.phaseCallbackUs=5;e.datagrams=1000;e.bytes=600000;e.exitReason=1;}
    capture.publish(source,1000,1200,emit);
    assert(events.size()==8&&events.front().sequence==4&&events.back().sequence==11);
    assert(capture.summary().sourceMissed==3&&capture.summary().eventCursor==19&&capture.summary().journalSkipped==8);
    assert(events.front().mappingKnown&&events.front().sampleMonoUs==500);
    const auto& phase=capture.summary();
    assert(phase.phaseSampleEvery==16&&phase.legacyEligibleBatches==55);
    assert(phase.phaseSampledBatches==3&&phase.phaseTimedBatches==2&&phase.phaseInvalidBatches==1);
    assert(phase.phaseSampledDatagrams==12&&phase.phaseTimedDatagrams==9);
    assert(phase.phaseSampledReceiveCalls==15&&phase.phaseTimedReceiveCalls==11);
    assert(phase.phaseRecvTotalUs==70&&phase.phaseRecvBatchMaxUs==50);
    assert(phase.phaseCallbackTotalUs==20&&phase.phaseCallbackBatchMaxUs==15);
    assert(events.front().phaseRecvUs==12&&events.front().phaseCallbackUs==5);
    const auto phaseText=n::udpPollEventText(events.front());
    assert(phaseText.find("phaseSampled=1 phaseValid=1 phaseRecvUs=12 phaseCallbackUs=5")!=phaseText.npos);
    assert(events.front().queryBeforeUs==1000&&events.front().queryAfterUs==1200);
    // The independent backend epoch is preserved, not passed off as gate time.
    assert(events.front().startMonoUs==100&&events.front().elapsedUs==20);
    capture.publish(source,1200,1300,emit);assert(events.size()==8);
    source.ice.availability=I::Availability::ReadFailed;source.phaseSampledBatches=999;
    capture.publish(source,1500,1600,emit);
    assert(capture.summary().hasValue&&capture.summary().batches==12);
    assert(capture.summary().phaseSampledBatches==3);source.phaseSampledBatches=3;
    assert(capture.summary().queryAfterUs==1300&&capture.summary().availability==3);
    source.ice.availability=I::Availability::Available;source.ice.provenance=I::Provenance::FinalRetained;
    capture.publish(source,9000,10000,emit);
    assert(events.size()==8&&capture.summary().queryAfterUs==1300&&capture.summary().mappingKnown);
    n::UdpPollCapture retainedFirst;retainedFirst.publish(source,9000,10000,[](const auto& e){assert(!e.mappingKnown);return true;});
    assert(!retainedFirst.summary().mappingKnown);
    assert(retainedFirst.summary().phaseSampledBatches==3);
    assert(n::udpPollEventText(n::UdpPollEvent{}).find("phaseSampled=0 phaseValid=0")!=std::string::npos);
    source.ice.provenance=I::Provenance::Live;source.sampleClockAvailable=false;
    source.eventCount=1;source.events[0].sequence=22;source.events[0].flags=1|64;
    source.events[0].phaseRecvUs=source.events[0].phaseCallbackUs=0;
    capture.publish(source,2000,1900,emit);
    assert(capture.summary().sourceMissed==5&&!capture.summary().mappingKnown);
    assert(events.back().flags==(1|64)&&!events.back().mappingKnown);
    assert(n::udpPollEventText(events.back()).find("phaseSampled=1 phaseValid=0")!=std::string::npos);
    assert(n::udpPollEventText(events.back()).find("phaseRecvUs=unknown phaseCallbackUs=unknown")!=std::string::npos);
    // Full recovery journal: optional UDP history cannot evict a single entry.
    n::EventJournal<int> journal;
    for(int i=0;i<32;++i)journal.push(i);
    source.eventCount=16;source.eventsTotal=16;source.eventsOverwritten=0;
    for(unsigned i=0;i<16;++i)source.events[i].sequence=i+1;
    n::UdpPollCapture full;
    full.publish(source,20,30,[&](const auto& e){return journal.tryPushReserved(static_cast<int>(e.sequence),16);});
    assert(journal.size()==32&&journal.lost()==0&&full.summary().journalSkipped==16);
    assert(full.summary().sourceMissed==0&&full.summary().eventCursor==16);
    for(int i=0;i<32;++i){n::EventJournal<int>::Entry entry;assert(journal.peek(entry)&&entry.value==i);assert(journal.retire(entry.sequence,true));}
    // Reserve is enforced under the same lock as append, not a size/push race.
    for(int i=0;i<12;++i)journal.push(i);
    n::UdpPollCapture partial;
    partial.publish(source,20,30,[&](const auto& e){return journal.tryPushReserved(static_cast<int>(e.sequence),16);});
    assert(journal.size()==16&&journal.lost()==0&&partial.summary().journalSkipped==12);
    partial.publish(source,30,40,[&](const auto& e){return journal.tryPushReserved(static_cast<int>(e.sequence),16);});
    assert(partial.summary().journalSkipped==12&&journal.size()==16);
    // Maximum-width evidence is not silently truncated by deferred line caps.
    n::NetworkSnapshot snapshot;snapshot.provider=n::Provider::Mts;snapshot.mtsUdpPoll=capture.summary();
    auto& u=snapshot.mtsUdpPoll;const auto max=std::numeric_limits<std::uint64_t>::max();
    u.sampleMonoUs=u.queryBeforeUs=u.queryAfterUs=u.clockInvalid=u.batches=u.bytes=u.timedBatches=max;
    u.elapsedTotalUs=u.elapsedMaxUs=u.lastStartMonoUs=u.lastEndMonoUs=u.serviceGaps=u.serviceGapMaxUs=max;
    u.readyDelays=u.readyDelayMaxUs=u.eventsTotal=u.eventsOverwritten=u.eventCursor=u.sourceMissed=u.journalSkipped=max;
    u.phaseSampleEvery=std::numeric_limits<unsigned>::max();
    u.legacyEligibleBatches=u.phaseSampledBatches=u.phaseTimedBatches=u.phaseInvalidBatches=max;
    u.phaseSampledDatagrams=u.phaseTimedDatagrams=u.phaseSampledReceiveCalls=u.phaseTimedReceiveCalls=max;
    u.phaseRecvTotalUs=u.phaseRecvBatchMaxUs=u.phaseCallbackTotalUs=u.phaseCallbackBatchMaxUs=max;
    u.rcvbufBytes=std::numeric_limits<int>::max();u.rcvbufErrno=std::numeric_limits<int>::min();
    const auto text=n::networkText(snapshot,n::Line::Ice);
    assert(text.size()<n::lineLimit(n::Line::Ice));
    assert(text.find("udpPollCpu=unsupported")!=text.npos&&text.find("udpPollMappingKnown=0")!=text.npos);
    n::NetworkEvent event;event.kind=n::EventKind::UdpPoll;event.transition.setUdp(events.front());
    assert(n::networkEventText(event).find("UDP poll event:")!=std::string::npos);
    assert(n::networkEventText(event,true).find("finalDrain=1")!=std::string::npos);
    assert(text.find("udpPollSchema=2")!=text.npos&&text.find("udpPollDetailMinUs=50000")!=text.npos);
    assert(text.find("udpPollPhaseSampleEvery=4294967295")!=text.npos);
    std::cout<<"UDP capture: retention, clock mapping, ring loss, dedup and output bounds passed; ICE line="<<text.size()<<"\n";
}
