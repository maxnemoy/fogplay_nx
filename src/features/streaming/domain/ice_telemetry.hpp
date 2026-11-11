#pragma once
#include <cstdint>
#include <string>
#include <type_traits>

namespace stream::telemetry::network {
// Domain copy of the fork's numeric API; no RTC handles, addresses or strings.
struct IceCounters {
    enum class Availability : std::uint8_t { Available, NotStarted, NotSupported, ReadFailed, ClosedWithoutSample };
    enum class Provenance : std::uint8_t { None, Live, FinalRetained };
    enum class Backend : std::uint8_t { Unknown, LibJuice, LibNice };
    enum class FailureReason : std::uint8_t { None, UdpRecv, UdpPoll, Connection, ConsentExpired, StunTimeout, StunSend, StunResponse, ConnectivityTimeout, Unknown };
    Availability availability=Availability::NotStarted;
    Provenance provenance=Provenance::None;
    Backend backend=Backend::Unknown;
    FailureReason failureReason=FailureReason::Unknown;
    unsigned state=0,udpMaxBatch=0,pollFlags=0,stunErrorCode=0;
    int recvErrno=0,socketError=0,stunSendErrno=0;
    bool socketErrorAvailable=false;
    std::uint64_t udpBudgetYields=0,udpDatagrams=0,consentExpired=0,stunTimeouts=0;
    std::uint64_t stunSendFailures=0,stunErrorResponses=0,connectivityTimeouts=0;
};
static_assert(std::is_trivially_copyable_v<IceCounters> && sizeof(IceCounters)<=128);
inline const char* iceAvailabilityName(IceCounters::Availability value){
    switch(value){
    case IceCounters::Availability::Available:return "available";
    case IceCounters::Availability::NotStarted:return "not-started";
    case IceCounters::Availability::NotSupported:return "not-supported";
    case IceCounters::Availability::ReadFailed:return "read-failed";
    case IceCounters::Availability::ClosedWithoutSample:return "closed-without-sample";
    }return "unknown";
}
inline const char* iceReasonName(IceCounters::FailureReason value){
    switch(value){
    case IceCounters::FailureReason::None:return "none";
    case IceCounters::FailureReason::UdpRecv:return "udp-recv";
    case IceCounters::FailureReason::UdpPoll:return "udp-poll";
    case IceCounters::FailureReason::Connection:return "connection";
    case IceCounters::FailureReason::ConsentExpired:return "consent-expired";
    case IceCounters::FailureReason::StunTimeout:return "stun-timeout";
    case IceCounters::FailureReason::StunSend:return "stun-send";
    case IceCounters::FailureReason::StunResponse:return "stun-response";
    case IceCounters::FailureReason::ConnectivityTimeout:return "connectivity-timeout";
    case IceCounters::FailureReason::Unknown:return "unknown";
    }return "unknown";
}
inline std::string iceText(const IceCounters& d,bool hasValue){
    const auto status=std::string(" iceAvailability=")+iceAvailabilityName(d.availability)+
        " iceBackend="+(d.backend==IceCounters::Backend::LibJuice?"libjuice":d.backend==IceCounters::Backend::LibNice?"libnice":"unknown")+
        " iceProvenance="+(d.provenance==IceCounters::Provenance::Live?"live":d.provenance==IceCounters::Provenance::FinalRetained?"final-retained":"none");
    if(!hasValue)return "iceDiagnostics=unavailable"+status;
    return "iceState="+std::to_string(d.state)+" iceReason="+iceReasonName(d.failureReason)+
        " udpBudgetYields="+std::to_string(d.udpBudgetYields)+" udpDatagrams="+std::to_string(d.udpDatagrams)+
        " udpMaxBatch="+std::to_string(d.udpMaxBatch)+" recvErrno="+std::to_string(d.recvErrno)+
        " pollFlags="+std::to_string(d.pollFlags)+" socketError="+std::to_string(d.socketError)+
        " socketErrorAvailable="+std::to_string(d.socketErrorAvailable)+" consentExpired="+std::to_string(d.consentExpired)+
        " stunTimeouts="+std::to_string(d.stunTimeouts)+" stunSendFailures="+std::to_string(d.stunSendFailures)+
        " stunErrorResponses="+std::to_string(d.stunErrorResponses)+" stunErrorCode="+std::to_string(d.stunErrorCode)+
        " stunSendErrno="+std::to_string(d.stunSendErrno)+" connectivityTimeouts="+std::to_string(d.connectivityTimeouts)+status;
}
}
