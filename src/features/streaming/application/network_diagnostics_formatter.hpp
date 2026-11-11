#pragma once
#include "features/streaming/domain/network_diagnostics_text.hpp"
#include "features/streaming/application/protocol_diagnostics_formatter.hpp"
#include "core/logging/run_log.hpp"
namespace stream::telemetry::network {
enum class Line {Video,VideoFinal,Gap,GapFinal,Queue,QueueFinal,Rtcp,RtcpFinal,ServerPerformance,History,Control,Dtls,DtlsFinal,Ice,IceFinal};
inline std::string rtpNetworkText(const NetworkSnapshot& s){auto value=s.mts.rtp;value.counters.eventOverflow+=s.eventMissed;return rtpText(value);}
inline std::string journalText(const NetworkSnapshot& s){auto text=values({{"journalCursor",s.eventCursor},{"journalMissed",s.eventMissed}});if(s.provider==Provider::FogHub)text+=values({{"coreSourceMissed",s.coreSourceMissed}});return text;}
inline std::string networkText(const NetworkSnapshot& s,Line line){
    if(s.provider==Provider::Unknown)return "provider=unknown unavailable=not-migrated";
    if(s.provider==Provider::FogHub){
        switch(line){case Line::Video:case Line::VideoFinal:return fhVideoText(s.fh)+" last_unit_age_known="+std::to_string(s.fh.lastUnitAgeKnown)+" codec_known="+std::to_string(s.fh.codecKnown);
        case Line::Gap:case Line::GapFinal:return s.core.hasValue?fhGapText(s.fh)+observationText(s.core)+" core_clock=moonlight core_clock_known="+std::to_string(s.fh.coreClockKnown)+values({{"last_packet_age_known",bool(s.fh.coreAgeKnownMask&1)},{"core_idr_wait_age_known",bool(s.fh.coreAgeKnownMask&2)},{"core_idr_request_age_known",bool(s.fh.coreAgeKnownMask&4)},{"core_idr_dispatch_age_known",bool(s.fh.coreAgeKnownMask&8)}})+journalText(s):"Moonlight core cumulative available=0 unavailable="+std::string(stateName(s.core.state));
        case Line::Queue:case Line::QueueFinal:return fhQueueText(s.fh,s.core.hasValue)+" audioCadenceAvailable=0 audioCadenceReason=unsupported-provider";
        case Line::Rtcp:case Line::RtcpFinal:return "Moonlight feedback managed by core";
        case Line::Dtls:case Line::DtlsFinal:return "Moonlight transport has no DTLS";
        default:return "provider=FH unavailable=unsupported";}
    }
    switch(line){
    case Line::Video:case Line::VideoFinal:
        return s.assembly.hasValue?rtpNetworkText(s)+" rtpInitialized="+std::to_string(s.mts.rtp.initialized)+observationText(s.assembly)+journalText(s)+repairText(s.mts.repair)+queueVideoText(s.mts.queue)+qualityText(s.mtsQuality):"provider=MTS unavailable="+std::string(stateName(s.assembly.state));
    case Line::Gap:case Line::GapFinal:return s.assembly.hasValue?gapText(s.mts.rtp)+observationText(s.assembly):"provider=MTS unavailable="+std::string(stateName(s.assembly.state));
    case Line::Queue:case Line::QueueFinal:return s.assembly.hasValue?pressureText(s.mts.queue)+audioCadenceText(s.mts.queue.audioCadence)+observationText(s.assembly):"provider=MTS unavailable="+std::string(stateName(s.assembly.state));
    case Line::Rtcp:case Line::RtcpFinal:return rtcpText(s);
    case Line::Dtls:case Line::DtlsFinal:return dtlsText(s.mtsDtls)+observationText(s.dtls);
    case Line::Ice:case Line::IceFinal:return iceText(s.mtsIce,s.ice.hasValue)+observationText(s.ice)+udpPollText(s.mtsUdpPoll);
    case Line::ServerPerformance:return s.control.hasValue?performanceText(s.mtsControl.performance):"provider=MTS unavailable="+std::string(stateName(s.control.state));
    case Line::History:return s.history.hasValue?historyText(s.mtsHistory)+observationText(s.history):"provider=MTS unavailable="+std::string(stateName(s.history.state));
    case Line::Control:return s.control.hasValue?incomingText(s.mtsControl)+observationText(s.control):"provider=MTS unavailable="+std::string(stateName(s.control.state));
    }
    return {};
}
inline const char* prefix(Line line){switch(line){
    case Line::Video:return "Video RTP: ";case Line::VideoFinal:return "Video RTP final: ";
    case Line::Gap:return "Video gaps: ";case Line::GapFinal:return "Video gaps final: ";
    case Line::Queue:return "Media queue: ";case Line::QueueFinal:return "Media queue final: ";
    case Line::Rtcp:return "Stream RTCP: ";case Line::RtcpFinal:return "Stream RTCP final: ";
    case Line::Dtls:return "Stream DTLS: ";case Line::DtlsFinal:return "Stream DTLS final: ";
    case Line::Ice:return "Stream ICE: ";case Line::IceFinal:return "Stream ICE final: ";
    case Line::ServerPerformance:return "Server performance: ";case Line::History:return "Stream history: ";case Line::Control:return "Stream control totals: ";}return "Network: ";}
inline std::size_t lineLimit(Line line){return line==Line::Queue||line==Line::QueueFinal||line==Line::ServerPerformance||line==Line::History||line==Line::Control?2048:4096;}
class NetworkRecord final:public fog::RunLog::DeferredRecord {
public:
    NetworkRecord(std::uint64_t elapsedMs,Line line,const NetworkSnapshot& snapshot):elapsedMs_(elapsedMs),line_(line),snapshot_(snapshot){}
    std::size_t rawBytes()const noexcept override{return sizeof(*this);}
    std::size_t maxOutputBytes()const noexcept override{return lineLimit(line_)+64;}
    void format(fog::RunLog::DeferredSink& sink)const override{
        auto text=networkText(snapshot_,line_);if(text.size()>lineLimit(line_))text.resize(lineLimit(line_));
        sink.append("["+std::to_string(elapsedMs_)+" ms] ");sink.append(prefix(line_));sink.append(text);
    }
private:
    std::uint64_t elapsedMs_;Line line_;NetworkSnapshot snapshot_;
};
inline const char* coreRoleName(CoreRole role){switch(role){
    case CoreRole::VideoRecv:return "VideoRecv";case CoreRole::VideoDecoder:return "VideoDec";case CoreRole::AudioRecv:return "AudioRecv";
    case CoreRole::AudioDecoder:return "AudioDec";case CoreRole::ControlRecv:return "ControlRecv";case CoreRole::ControlSend:return "ControlSend";
    case CoreRole::Input:return "Input";case CoreRole::Other:return "Other";case CoreRole::VideoPing:return "VideoPing";case CoreRole::AudioPing:return "AudioPing";
    case CoreRole::LossStats:return "LossStats";case CoreRole::ReqIdrFrame:return "ReqIdrFrame";case CoreRole::CtrlAsyncCb:return "CtrlAsyncCb";case CoreRole::InvRefFrames:return "InvRefFrames";
    default:return "unknown";}}
inline std::string coreEventText(const NetworkEvent& e){
    const auto& c=e.core;
    if(e.kind==EventKind::CoreMissed)return "Moonlight IDR journal missed="+std::to_string(c.value)+" through_seq="+std::to_string(c.sequence);
    if(c.kind==12)return "Moonlight receive event=packet-gap"+values({{"seq",c.sequence},{"connection",c.connection},{"wait",c.wait},
        {"core_us",c.timeUs},{"gapUs",c.value},{"thresholdUs",50000}})+
        " previousCoreUs="+(c.timeUs>=c.value?std::to_string(c.timeUs-c.value):std::string("unknown"));
    if(c.kind==11)return "Moonlight thread role="+std::string(coreRoleName(c.role))+" connection="+std::to_string(c.connection)+
        " core="+std::to_string(c.threadCore)+" priority="+std::to_string(c.threadPriority)+" mask="+std::to_string(c.threadAffinityMask)+
        " ideal="+std::to_string(c.threadIdealCore)+" queryErrors="+std::to_string(c.threadQueryErrors)+" available="+std::to_string(c.threadQueryErrors==0)+
        " seq="+std::to_string(c.sequence)+" core_us="+std::to_string(c.timeUs);
    static constexpr const char* names[]={"unknown","wait-enter","request","dispatch","dispatch-result","fec-failure","intact-frame","idr-receive-hint","idr-recognized","wait-exit","idr-assembled"};
    return "Moonlight IDR event="+std::string(c.kind<11?names[c.kind]:names[0])+values({{"seq",c.sequence},{"connection",c.connection},{"wait",c.wait},
        {"core_us",c.timeUs},{"frame",c.frame},{"value",c.value},{"totalsValid",c.totalsValid},{"requests",c.requests},{"intact",c.intact},{"fecFailedBlocks",c.fecFailures}});
}
inline std::string networkEventText(const NetworkEvent& e,bool final=false){
    if(e.kind==EventKind::Core||e.kind==EventKind::CoreMissed)return coreEventText(e)+" videoBindingKnown="+std::to_string(e.videoBindingKnown);
    if(e.kind==EventKind::UdpPoll)return udpPollEventText(e.transition.udp)+" finalDrain="+std::to_string(final)+" videoBindingKnown=0";
    std::string text;
    switch(e.kind){case EventKind::UdpPoll:text=udpPollEventText(e.transition.udp);break;case EventKind::Rtp:text=rtpEventText(e.rtp);break;
    case EventKind::RouteSelected:case EventKind::RouteChanged:text=std::string("routeEvent=")+(e.kind==EventKind::RouteSelected?"selected":"changed")+protocol::routeText(e.transition.route);break;
    case EventKind::Quality:text=quality::numericEventText(e.transition.quality);break;
    case EventKind::Fixed:{const auto& f=e.transition.fixed;switch(f.kind){
        case FixedEventKind::StatsProfile:text=std::string("qualityEvent=stats-profile profile=")+quality::diagnosticProfileName(static_cast<unsigned>(f.value));break;
        case FixedEventKind::VideoGeneration:text="qualityEvent=video-generation generation="+std::to_string(f.value)+" remb=repeat";break;
        case FixedEventKind::Settings:text="qualityEvent=settings quality="+std::to_string(f.value)+" capMbps="+std::to_string(f.other);break;
        case FixedEventKind::ScaleTable:text="scaleEvent=table target=1280x720 preset="+(f.known?std::to_string(f.value):"unavailable")+" tableRevision="+std::to_string(f.other)+" nativeWidth="+std::to_string(f.nativeWidth)+" nativeHeight="+std::to_string(f.nativeHeight)+" entries="+std::to_string(f.tableEntries);break;
        case FixedEventKind::ScaleRequest:text="scaleEvent=request target=1280x720 preset="+std::to_string(f.value)+" enabled="+std::to_string(f.enabled)+" queued="+std::to_string(f.queued)+" actual=observe-decoded-video attempt="+std::to_string(f.other);break;
        case FixedEventKind::Peer:case FixedEventKind::Ice:case FixedEventKind::Gathering:text=std::string("transportEvent=")+(f.kind==FixedEventKind::Peer?"peer":f.kind==FixedEventKind::Ice?"ice":"gathering")+" state="+std::to_string(f.value)+" ms="+std::to_string(f.ms);break;
    }break;}
    default:break;}
    return std::string(final?"Stream final event: ":"Video RTP event: ")+text+" videoBindingKnown="+std::to_string(e.videoBindingKnown);
}
class EventRecord final:public fog::RunLog::DeferredRecord {
public:
    EventRecord(std::uint64_t elapsedMs,NetworkEvent event,bool final=false):elapsedMs_(elapsedMs),event_(event),final_(final){}
    std::size_t rawBytes()const noexcept override{return sizeof(*this);}
    std::size_t maxOutputBytes()const noexcept override{return 2112;}
    void format(fog::RunLog::DeferredSink& sink)const override{
        auto text=networkEventText(event_,final_);
        text+=values({{"journalSequence",event_.journalSequence},{"journalMissed",event_.journalMissed}});
        if(text.size()>2048)text.resize(2048);
        sink.append("["+std::to_string(elapsedMs_)+" ms] ");sink.append(text);
    }
private:std::uint64_t elapsedMs_;NetworkEvent event_;bool final_;
};
static_assert(sizeof(NetworkRecord)<=9216&&sizeof(EventRecord)<=1024);
static_assert(4*(9216+4160+1)<=fog::RunLog::maxQueuedBytes);
}
