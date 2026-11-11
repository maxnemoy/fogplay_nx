#pragma once
#include "features/streaming/domain/ice_telemetry.hpp"

namespace stream::telemetry::network {
// A retained SDK sample has no exposed source timestamp. Preserve the last
// known live source clock; a first retained-only result has unknown age.
inline void updateIceSourceClock(IceCounters::Provenance provenance,std::uint64_t readAtUs,std::uint64_t& sourceAtUs,bool& known){
    if(provenance==IceCounters::Provenance::Live){sourceAtUs=readAtUs;known=true;}
    else if(!known)sourceAtUs=0;
}
template<class Source> IceCounters captureIce(const Source& d){
    IceCounters out;
    switch(d.availability){
    case Source::Availability::Available:out.availability=IceCounters::Availability::Available;break;
    case Source::Availability::NotStarted:out.availability=IceCounters::Availability::NotStarted;break;
    case Source::Availability::NotSupported:out.availability=IceCounters::Availability::NotSupported;break;
    case Source::Availability::ReadFailed:out.availability=IceCounters::Availability::ReadFailed;break;
    case Source::Availability::ClosedWithoutSample:out.availability=IceCounters::Availability::ClosedWithoutSample;break;
    default:out.availability=IceCounters::Availability::ReadFailed;break;
    }
    switch(d.provenance){
    case Source::Provenance::Live:out.provenance=IceCounters::Provenance::Live;break;
    case Source::Provenance::FinalRetained:out.provenance=IceCounters::Provenance::FinalRetained;break;
    default:break;
    }
    switch(d.backend){
    case Source::Backend::LibJuice:out.backend=IceCounters::Backend::LibJuice;break;
    case Source::Backend::LibNice:out.backend=IceCounters::Backend::LibNice;break;
    default:break;
    }
    switch(d.failureReason){
#define FOG_ICE_REASON(name) case Source::FailureReason::name:out.failureReason=IceCounters::FailureReason::name;break;
    FOG_ICE_REASON(None) FOG_ICE_REASON(UdpRecv) FOG_ICE_REASON(UdpPoll) FOG_ICE_REASON(Connection)
    FOG_ICE_REASON(ConsentExpired) FOG_ICE_REASON(StunTimeout) FOG_ICE_REASON(StunSend)
    FOG_ICE_REASON(StunResponse) FOG_ICE_REASON(ConnectivityTimeout)
#undef FOG_ICE_REASON
    default:break;
    }
    out.state=d.state;out.udpBudgetYields=d.udpBudgetYields;out.udpDatagrams=d.udpDatagrams;out.udpMaxBatch=d.udpMaxBatch;
    out.recvErrno=d.recvErrno;out.pollFlags=d.pollFlags;out.socketError=d.socketError;out.socketErrorAvailable=d.socketErrorAvailable;
    out.consentExpired=d.consentExpired;out.stunTimeouts=d.stunTimeouts;out.stunSendFailures=d.stunSendFailures;
    out.stunErrorResponses=d.stunErrorResponses;out.stunErrorCode=d.stunErrorCode;out.stunSendErrno=d.stunSendErrno;
    out.connectivityTimeouts=d.connectivityTimeouts;
    return out;
}
}
