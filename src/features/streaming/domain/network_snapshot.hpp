#pragma once
#include "features/streaming/domain/network_telemetry.hpp"
#include "features/streaming/domain/audio_rtp_cadence.hpp"
#include <string>
#include <new>
#include "features/streaming/domain/control_discovery.hpp"
#include "features/streaming/domain/protocol_telemetry.hpp"
#include "features/streaming/domain/rtp_event.hpp"
#include "features/streaming/domain/auto_quality_snapshot.hpp"
#include "features/streaming/domain/auto_quality_event.hpp"
#include "features/streaming/domain/moonlight_network_fields.hpp"
#include "features/streaming/domain/dtls_network_fields.hpp"
#include "features/streaming/domain/ice_telemetry.hpp"
#include "features/streaming/domain/udp_poll_telemetry.hpp"
namespace stream::telemetry::network {
enum class Provider:unsigned char{Unknown,Mts,FogHub};
enum class State:unsigned char{NotPublished,Live,FinalRetained,CaptureFailed,ClosedWithoutFinal,NotSupported};
struct Observation {
    State state=State::NotPublished;bool hasValue=false,ageKnown=false,stale=true;
    std::uint64_t epoch=0,generation=0,revision=0,capturedAtUs=0,ageUs=0;
};
struct QueueSnapshot {
    AudioRtpCadenceSnapshot audioCadence{};
    // SPSC fields are independent observations. Its legacy age API cannot
    // distinguish a zero-age packet from a raced read; retain that uncertainty.
    bool videoAgeKnown=false,audioAgeKnown=false,videoEmptyObserved=true,audioEmptyObserved=true;

    std::uint64_t nackSendErrors=0,queue=0,queueBytes=0,videoQueue=0,audioQueue=0,videoAgeMs=0,audioAgeMs=0;
    std::uint64_t videoAgeHighMs=0,audioAgeHighMs=0,videoHigh=0,audioHigh=0,bytesHigh=0,resyncs=0,capacity=0,age=0,audioDrop=0,audioReject=0,generation=0,pli=0,ingressDrops=0;
    std::uint64_t ingressWaitSamples=0,ingressWaitTotalMs=0,ingressWaitMaxMs=0;
    unsigned pressureReason=0;
    std::uint64_t pressureCount=0,pressureAtMs=0,pressureVideoCount=0,pressureAudioCount=0,pressureVideoBytes=0,pressureAudioBytes=0;
    std::uint64_t pressureVideoAgeMs=0,pressureAudioAgeMs=0,pressureIncomingBytes=0,audioAgeDropped=0,audioCapacityDropped=0,audioPriorityPopped=0;
};
struct AssemblySnapshot {RtpSnapshot rtp{};RepairSnapshot repair{};QueueSnapshot queue{};};
struct ControlSnapshot {IncomingSnapshot incoming{};IncomingSchemaSnapshot schemas{};PerformanceSnapshot performance{};};
struct QualitySnapshot {
    bool targetAgeKnown=false,presetAgeKnown=false,statsAgeKnown=false,autoAgeKnown=false,fpsAgeKnown=false,codecAgeKnown=false;
    bool negotiatedCodecKnown=false;
    bool targetKnown=false,profileKnown=false,automaticKnown=false,fpsKnown=false,codecKnown=false,statsKnown=false;
    bool codecH265=false,requestedH265=false,qualityPending=false;int capMbps=0,quality=0,pliSeconds=0,requestedFps=0;
    double receiveMbps=0,targetMbps=-1,fps=-1;int profile=0,automatic=-1,serverCodec=0;
    std::int64_t targetAgeMs=-1,presetAgeMs=-1,statsAgeMs=-1,autoAgeMs=-1,fpsAgeMs=-1,codecAgeMs=-1;
    std::uint64_t statsCount=0,qualityRequests=0,scaleAttempts=0,scaleQueued=0,scaleRejected=0,scaleInvalidTables=0;
    std::uint64_t scaleTableRevision=0,scaleTableEntries=0;
    int scaleNativeWidth=0,scaleNativeHeight=0,scalePreset=-1,scaleLastPreset=-1;
    unsigned scalePendingReason=0;
    unsigned offeredLossProtection=0;quality::NumericSnapshot automaticPolicy{};
};
struct CoreCounters {
#define FOG_FH_DECLARE(name) std::uint64_t name=0;
    FOG_NETWORK_FH_FIELDS(FOG_FH_DECLARE)
#undef FOG_FH_DECLARE
};
struct DtlsCounters {
    bool available=false;
#define FOG_DTLS_DECLARE(name) std::uint64_t name=0;
    FOG_NETWORK_DTLS_FIELDS(FOG_DTLS_DECLARE)
#undef FOG_DTLS_DECLARE
};
struct StageTiming {std::uint64_t calls=0,totalUs=0,maxUs=0,over50ms=0;};
struct FhSnapshot {
    CoreCounters core{};std::array<StageTiming,4> videoTimings{};
    bool codecKnown=false,codecH265=false,coreClockKnown=false,lastUnitAgeKnown=false;
    unsigned coreAgeKnownMask=0; // packet, IDR wait, request, dispatch; core clock ordering validated
    std::uint64_t coreNowUs=0,units=0,payloadBytes=0,idrs=0,skippedFrameNumbers=0,repeatedOrReordered=0,invalidTimes=0,lastUnitAgeUs=0;
    std::uint64_t videoDrops=0,audioDrops=0,audioOverflowDrops=0,audioInvalidDrops=0,audioDropsVideoWaiting=0,audioDropsVideoStalled=0;
    std::uint64_t audioDropLastMs=0,audioDropLastQueueAgeMs=0,audioDropLastVideoAgeMs=0,videoInvalid=0,videoWaitDrops=0,videoResyncs=0;
};
struct NetworkSnapshot {
    Provider provider=Provider::Unknown;
    std::uint64_t streamEpoch=0,transportEpoch=0,videoGeneration=0,capturedAtSteadyUs=0,completedAtSteadyUs=0;
    Observation assembly{},control{},history{},rtcp{},core{},dtls{},ice{};
    IceCounters mtsIce{};
    UdpPollSummary mtsUdpPoll{};
    DtlsCounters mtsDtls{};
    AssemblySnapshot mts{};ControlSnapshot mtsControl{};HistorySnapshot mtsHistory{};RtcpSnapshot mtsRtcp{};QualitySnapshot mtsQuality{};
    bool rtcpWorkerActive=false,rtcpStopped=false;std::uint64_t rtcpWorkerActiveMs=0;
    FhSnapshot fh{};
    std::uint64_t eventMissed=0,eventCursor=0,coreSourceMissed=0;
    // Set only for the MTS numeric SDK path; FogHub has no ICE transport.
    bool detailedIceMigrated=false,iceSourceClockKnown=false;
};
enum class EventKind:unsigned char{Rtp,Core,CoreMissed,RouteSelected,RouteChanged,Quality,Fixed,UdpPoll};
enum class FixedEventKind:unsigned char{StatsProfile,VideoGeneration,Settings,ScaleTable,ScaleRequest,Peer,Ice,Gathering};
struct FixedEvent {FixedEventKind kind=FixedEventKind::StatsProfile;std::uint64_t ms=0,value=0,other=0;bool known=false,enabled=false,queued=false;unsigned nativeWidth=0,nativeHeight=0,tableEntries=0;};
struct TransitionPayload {
    union { protocol::Route route;quality::NumericEvent quality;FixedEvent fixed;UdpPollEvent udp; };
    TransitionPayload():route{}{}
    void setRoute(const protocol::Route& v){new(&route)protocol::Route(v);}
    void setQuality(const quality::NumericEvent& v){new(&quality)quality::NumericEvent(v);}
    void setUdp(const UdpPollEvent& v){new(&udp)UdpPollEvent(v);}
    void setFixed(const FixedEvent& v){new(&fixed)FixedEvent(v);}
};
enum class CoreRole:unsigned char{Unknown,VideoRecv,VideoDecoder,AudioRecv,AudioDecoder,ControlRecv,ControlSend,Input,Other,VideoPing,AudioPing,LossStats,ReqIdrFrame,CtrlAsyncCb,InvRefFrames};
struct CoreEvent {
    std::uint64_t sequence=0,connection=0,wait=0,timeUs=0,kind=0,frame=0,value=0,requests=0,intact=0,fecFailures=0,totalsValid=0;
    std::int32_t threadCore=0,threadIdealCore=0,threadPriority=0;std::uint32_t threadQueryErrors=0;std::uint64_t threadAffinityMask=0;
    CoreRole role=CoreRole::Unknown;
};
struct NetworkEvent {
    Provider provider=Provider::Unknown;EventKind kind=EventKind::Rtp;
    std::uint64_t streamEpoch=0,transportEpoch=0,videoGeneration=0,journalSequence=0,journalMissed=0;
    bool transportBindingKnown=false,videoBindingKnown=false;
    RtpEvent rtp{};CoreEvent core{};TransitionPayload transition{};
};
static_assert(std::is_trivially_copyable_v<NetworkSnapshot> && sizeof(NetworkSnapshot)<=9216);
static_assert(std::is_trivially_copyable_v<NetworkEvent> && sizeof(NetworkEvent)<=504);
}
namespace stream {
class StreamNetworkTelemetrySource : public StreamProtocolTelemetrySource {
public:
    virtual ~StreamNetworkTelemetrySource()=default;
    virtual telemetry::network::NetworkSnapshot captureNetworkTelemetry()const noexcept{return {};}
    virtual void enableNetworkEvents(bool) noexcept{}
    virtual bool peekNetworkEvent(telemetry::network::NetworkEvent&)const noexcept{return false;}
    // One immutable prefix; no retirement until the caller knows enqueue outcome.
    virtual std::size_t peekNetworkEvents(telemetry::network::NetworkEvent* out,std::size_t capacity)const noexcept{
        return out&&capacity&&peekNetworkEvent(*out)?1:0;
    }
    virtual bool retireNetworkEvent(std::uint64_t,bool)noexcept{return false;}
    // Upper bound after the transport's producer threads have joined, including
    // source-loss markers. Final cleanup must account for every retained entry.
    virtual unsigned networkEventFinalDiscardBudget()const noexcept{return 32;}
    // Explicit transition path for route/quality discovery events which still
    // have legacy producers. It never consumes the typed RTP/core journal.
    virtual bool supportsControlDiscoveryTelemetry()const noexcept{return false;}
    virtual std::size_t peekControlDiscoveryEvents(telemetry::network::ControlDiscoveryEvent*,std::size_t,bool=false)const noexcept{return 0;}
    virtual bool retireControlDiscoveryEvent(std::uint64_t,bool)noexcept{return false;}
    virtual bool pollUnmigratedNetworkEvent(std::string&){return false;}
};
}
