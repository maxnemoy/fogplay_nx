#pragma once
#include "features/streaming/application/player.hpp"
#include "features/streaming/domain/input_suspension.hpp"
#include "core/diagnostics/diagnostic_profile.hpp"
#include "features/streaming/application/network_diagnostics_formatter.hpp"
#include "features/streaming/application/protocol_diagnostics_formatter.hpp"
#include "features/streaming/application/control_discovery_formatter.hpp"
#include "features/streaming/application/rumble_diagnostics_formatter.hpp"
#include "features/streaming/application/stream_diagnostics_formatter.hpp"
#include "features/streaming/application/media_diagnostics_formatter.hpp"

namespace fog::stream::diagnostics {
// Projection deliberately excludes compatibility strings and event containers.
// Every byte retained by RunLog is an owned bounded numeric value.
struct OutputInput {
    InputSuspensionStats suspension;
    std::uint64_t capturedAtMs=0;
    bool enabled=false,playStation=false;
    ::stream::telemetry::rumble::Snapshot rumble;
};
struct PlayerValues {
    ::stream::telemetry::Snapshot transport;
    ::stream::telemetry::network::NetworkSnapshot network;
    ::stream::telemetry::protocol::ProtocolSnapshot protocol;
    ::fog::media::telemetry::Snapshot media;
    ::stream::telemetry::Playback playback;
    PlayerMedia::Progress progress;
    InputGateSnapshot inputGate;
    std::array<std::uint64_t,14> cycle{};
    std::uint64_t videoWorkUs=0,networkRetirementMissed=0,controlDiscoveryRetirementMissed=0;
    bool controlReady=false,recovering=false,periodicDetails=false,reaperJoined=false;
    explicit PlayerValues(const PlayerDiagnosticSnapshot& s)
        :transport(s.transport),network(s.network),protocol(s.protocol),media(s.mediaTelemetry),playback(s.playback),progress(s.media),inputGate(s.inputGate),
         cycle{s.pumpGapUs,s.previousPumpUs,s.pumpUs,s.transportUs,s.mediaBatchUs,s.prepareUs,s.resetUs,
               s.videoDrained,s.videoBefore,s.videoAfter,s.drained,0,0,0},
         videoWorkUs(s.videoWorkUs),networkRetirementMissed(s.networkRetirementMissed),controlDiscoveryRetirementMissed(s.controlDiscoveryRetirementMissed),controlReady(s.controlReady),recovering(s.recovering),periodicDetails(s.periodicDetails),reaperJoined(s.reaperJoined) {}
};
class PlayerRecord final : public ::fog::RunLog::DeferredRecord {
public:
    static constexpr std::size_t TerminalOutputBytes=48*1024;
    PlayerRecord(std::uint64_t elapsedMs,PlayerDiagnosticPhase phase,const PlayerDiagnosticSnapshot& snapshot,::stream::telemetry::Playback playback,OutputInput input={})
        :elapsedMs_(elapsedMs),phase_(phase),value_(project(snapshot,playback)),input_(input){}
    PlayerRecord(std::uint64_t elapsedMs,PlayerDiagnosticPhase phase,const PlayerDiagnosticSnapshot& snapshot)
        :PlayerRecord(elapsedMs,phase,snapshot,snapshot.playback){}
    std::size_t rawBytes()const noexcept override{return sizeof(*this);}
    std::size_t lineCount()const noexcept override{
        using P=PlayerDiagnosticPhase;
        switch(phase_){case P::Protocol:return 6;case P::Final:return 11;case P::Closed:return 7+(input_.playStation&&!value_.reaperJoined?1:0);
        case P::Periodic:return 8;case P::Metrics:return 6+(value_.periodicDetails?8:0)+(::fog::metrics::collects(::fog::metrics::DiagnosticClass::StageDistribution)?1:0);case P::SlowCycle:return 1;case P::DecodeError:return 4;
        case P::Control:return 1;}return 1;
    }
    std::size_t maxOutputBytes()const noexcept override{
        using P=PlayerDiagnosticPhase;
        switch(phase_){
        case P::Protocol:case P::Periodic:return 40*1024;
        case P::Final:case P::Closed:case P::Metrics:return TerminalOutputBytes;
        case P::DecodeError:return 32*1024;
        case P::SlowCycle:case P::Control:return 8192;
        }
        return 8192;
    }
    void format(::fog::RunLog::DeferredSink& sink)const override{
        using P=PlayerDiagnosticPhase;namespace n=::stream::telemetry::network;
        Writer out(sink,elapsedMs_);
        switch(phase_){
        case P::Protocol:
            network(out,n::Line::Dtls);network(out,n::Line::Rtcp);network(out,n::Line::Ice);transport(out,"Stream transport: ");
            out.line("Stream protocol: "+::stream::telemetry::protocol::protocolText(value_.protocol));
            out.line("Stream mutex: "+::stream::telemetry::protocol::mutexText(value_.protocol));break;
        case P::Final:
            out.line(::stream::telemetry::rumble::rumbleText(input_.rumble));
            network(out,n::Line::VideoFinal);network(out,n::Line::QueueFinal);
            out.line(::stream::telemetry::playbackText(value_.playback));transport(out,"Stream transport final: ");
            media(out,"Video decoder final: ",::fog::media::telemetry::Line::Video);
            media(out,"Audio playback final: ",::fog::media::telemetry::Line::Audio);
            out.line("Input latency: "+::stream::telemetry::inputText(value_.transport));
            media(out,"Client latency: ",::fog::media::telemetry::Line::Latency);
            input(out);stages(out,true);break;
        case P::Closed:
            transport(out,"Stream transport closed: ");network(out,n::Line::GapFinal);
            network(out,n::Line::DtlsFinal);network(out,n::Line::RtcpFinal);network(out,n::Line::IceFinal);
            out.line("Transport final protocol: "+::stream::telemetry::protocol::protocolText(value_.protocol));
            out.line("Stream mutex final: "+::stream::telemetry::protocol::mutexText(value_.protocol));
            if(input_.playStation&&!value_.reaperJoined)out.line("PS transport input closed: "+::stream::telemetry::protocol::inputText(value_.protocol));
            break;
        case P::Periodic:periodic(out);break;
        case P::Metrics:{
            if(value_.periodicDetails)periodic(out);
            const auto& p=value_.progress;
            out.line("Playback load: workers="+std::to_string(p.workers)+" generation="+std::to_string(p.workerGeneration)+
                " workerDecodeUs="+std::to_string(p.workerDecodeUs)+" workerConvertUs="+std::to_string(p.workerConvertUs)+" uiVideoUs="+std::to_string(value_.videoWorkUs));
            out.line(::stream::telemetry::playbackText(value_.playback));
            out.line("Input latency: "+::stream::telemetry::inputText(value_.transport));
            media(out,"Client latency: ",::fog::media::telemetry::Line::Latency);input(out);
            out.line(::stream::telemetry::rumble::rumbleText(input_.rumble));stages(out);break;}
        case P::SlowCycle:{
            const auto& c=value_.cycle;
            static constexpr const char* names[]={"pumpGapUs","previousPumpMeasuredUs","pumpMeasuredUs","transportUs","mediaBatchUs","prepareUs","decoderResetUs","videoDrained","videoBefore","videoAfter","drained"};
            std::string text="Playback slow cycle:";
            for(unsigned i=0;i<11;++i)text+=" "+std::string(names[i])+"="+std::to_string(c[i]);
            out.line(text+" outsideMeasuredPumpUs="+std::to_string(c[0]>c[1]?c[0]-c[1]:0));break;}
        case P::DecodeError:
            out.line("Video RTP at decode error: "+n::networkText(value_.network,n::Line::Video));
            out.line("Video gaps at decode error: "+n::networkText(value_.network,n::Line::Gap));
            out.line("Media queue at decode error: "+n::networkText(value_.network,n::Line::Queue));
            media(out,"Video decoder at error: ",::fog::media::telemetry::Line::Video);break;
        case P::Control:out.line("Player telemetry unavailable=control-compatibility");break;
        }
    }
private:
    static PlayerValues project(const PlayerDiagnosticSnapshot& snapshot,::stream::telemetry::Playback playback){
        PlayerValues values(snapshot);values.playback=playback;return values;
    }
    class Writer {
    public:
        Writer(::fog::RunLog::DeferredSink& sink,std::uint64_t elapsed):sink_(sink),elapsed_(elapsed){}
        void line(std::string text){
            if(written_++)sink_.nextLine();
            const auto prefix="["+std::to_string(elapsed_)+" ms] ";
            if(text.size()>8192-prefix.size())text.resize(8192-prefix.size());
            sink_.append(prefix);sink_.append(text);
        }
    private:
        ::fog::RunLog::DeferredSink& sink_;std::uint64_t elapsed_;std::size_t written_=0;
    };
    void periodic(Writer& out)const{
        namespace n=::stream::telemetry::network;
            const auto& p=value_.progress;
            out.line("Media video="+std::to_string(p.decoded)+" opus="+std::to_string(p.audio)+" hardware="+std::to_string(p.hardware)+
                " controlReady="+std::to_string(value_.controlReady)+" width="+std::to_string(p.width)+" height="+std::to_string(p.height)+
                " recovering="+std::to_string(value_.recovering)+" waitingIDR="+std::to_string(p.waiting));
            network(out,n::Line::Video);network(out,n::Line::Gap);network(out,n::Line::Queue);
            media(out,"Video decoder: ",::fog::media::telemetry::Line::Video);media(out,"Audio playback: ",::fog::media::telemetry::Line::Audio);
            network(out,n::Line::ServerPerformance);out.line("Control delivery: "+::stream::telemetry::protocol::controlDeliveryText(value_.protocol));    }
    void network(Writer& out,::stream::telemetry::network::Line kind)const{
        namespace n=::stream::telemetry::network;
        out.line(std::string(n::prefix(kind))+n::networkText(value_.network,kind));
    }
    void transport(Writer& out,const char* prefix)const{out.line(std::string(prefix)+::stream::telemetry::transportText(value_.transport)+" networkEventRetirementMissed="+std::to_string(value_.networkRetirementMissed)+" controlDiscoveryRetirementMissed="+std::to_string(value_.controlDiscoveryRetirementMissed));}
    void media(Writer& out,const char* prefix,::fog::media::telemetry::Line kind)const{
        namespace m=::fog::media::telemetry;
        out.line(std::string(prefix)+m::format(m::Record(value_.media,kind),m::processFormatNames()));
    }
    void stages(Writer& out,bool final=false)const{if(final||::fog::metrics::collects(::fog::metrics::DiagnosticClass::StageDistribution))out.line(::fog::media::telemetry::formatStages(value_.media));}
    void input(Writer& out)const{
        const auto& g=value_.inputGate;
        std::string text="Input suspension: enabled="+std::to_string(input_.enabled)+" providerSupported="+
            std::to_string(g.providerSuspensionSupported)+" "+input_.suspension.text(input_.capturedAtMs)+
            " gateScope=player-admission gateReasonSemantics=first-blocking gateClock=steady-ms gateInitialized="+std::to_string(g.initialized)+
            " gateFinal="+std::to_string(g.final)+" gateEpisodesIncludeStartupAndTerminal=1 gateCapturedAtMs="+std::to_string(g.capturedAtMs)+
            " gatePreparedCauseSemantics=last-suspend-latched gatePreparedCause="+inputPreparedGateCauseName(g.preparedGateCause)+
            " gatePreparedCauseAtMs="+std::to_string(g.preparedGateCauseAtMs)+
            " gateReason="+inputGateReasonName(g.reason)+" gateEpisodes="+std::to_string(g.episodes)+
            " gateCompleted="+std::to_string(g.completed)+" gateBlockedMs="+std::to_string(g.blockedMs)+
            " gateActiveMs="+std::to_string(g.activeMs)+" gateCursor="+std::to_string(g.sequence)+
            " gateOverwritten="+std::to_string(g.overwritten);
        for(std::size_t i=1;i<g.reasonMs.size();++i)text+=" gateReasonMs"+std::to_string(i)+"="+std::to_string(g.reasonMs[i]);
        const auto count=std::min(g.count,g.transitions.size());
        for(std::size_t i=0;i<count;++i) {
            const auto& e=g.transitions[i];
            text+=" gateEvent="+std::to_string(e.sequence)+":"+std::to_string(e.atMs)+":"+std::to_string(e.session)+":"+
                std::to_string(e.transport)+":"+std::to_string(e.generation)+":"+std::to_string(e.preparedAgeMs)+":"+
                std::to_string(e.previousReasonMs)+":"+inputGateReasonName(e.previous)+":"+inputGateReasonName(e.reason)+":"+
                inputPreparedGateCauseName(e.previousPreparedGateCause)+":"+inputPreparedGateCauseName(e.preparedGateCause)+":"+
                std::to_string(e.preparedGateCauseAtMs);
        }
        out.line(text);
    }
    const std::uint64_t elapsedMs_;
    const PlayerDiagnosticPhase phase_;
    const PlayerValues value_;
    const OutputInput input_;
};
static_assert(sizeof(PlayerRecord)<=16384);
class MenuImageRecord final : public ::fog::RunLog::DeferredRecord {
public:
    MenuImageRecord(std::uint64_t elapsedMs,const ::fog::media::telemetry::Snapshot& value):elapsedMs_(elapsedMs),value_(value){}
    std::size_t rawBytes()const noexcept override{return sizeof(*this);}
    std::size_t maxOutputBytes()const noexcept override{return 2112;}
    void format(::fog::RunLog::DeferredSink& sink)const override{
        auto text=std::string("Video image at menu: ")+::fog::media::telemetry::formatImage(value_);
        if(text.size()>2048)text.resize(2048);
        sink.append("["+std::to_string(elapsedMs_)+" ms] ");sink.append(text);
    }
private:const std::uint64_t elapsedMs_;const ::fog::media::telemetry::Snapshot value_;
};
static_assert(sizeof(MenuImageRecord)<=4096);
class StallRecord final : public ::fog::RunLog::DeferredRecord {
public:
    StallRecord(std::uint64_t elapsedMs,const ::fog::media::telemetry::StallEvent& value):elapsedMs_(elapsedMs),value_(value){}
    std::size_t rawBytes()const noexcept override{return sizeof(*this);}
    std::size_t maxOutputBytes()const noexcept override{return 2112;}
    void format(::fog::RunLog::DeferredSink& sink)const override{
        auto text=std::string("[Video watchdog] ")+::fog::media::telemetry::formatStall(value_);if(text.size()>2048)text.resize(2048);
        sink.append("["+std::to_string(elapsedMs_)+" ms] ");sink.append(text);
    }
private:const std::uint64_t elapsedMs_;const ::fog::media::telemetry::StallEvent value_;
};
static_assert(sizeof(StallRecord)<=4096);
class ControlDiscoveryRecord final : public ::fog::RunLog::DeferredRecord {
public:
    static constexpr std::size_t Capacity=4;
    ControlDiscoveryRecord(std::uint64_t elapsedMs,const ::stream::telemetry::network::ControlDiscoveryEvent* values,std::size_t count)
        :elapsedMs_(elapsedMs),count_(count){
        if(!values||!count||count>Capacity)throw std::invalid_argument("invalid control discovery batch");
        std::copy_n(values,count_,values_.begin());
    }
    std::size_t rawBytes()const noexcept override{return sizeof(*this);}
    std::size_t lineCount()const noexcept override{return count_;}
    std::size_t maxOutputBytes()const noexcept override{return count_*8192+count_-1;}
    void format(::fog::RunLog::DeferredSink& sink)const override{
        for(std::size_t i=0;i<count_;++i){
            if(i)sink.nextLine();
            const auto prefix="["+std::to_string(elapsedMs_)+" ms] Stream control: provider=mts ";
            auto text=::stream::telemetry::network::controlDiscoveryText(values_[i]);
            if(text.size()>8192-prefix.size())text.resize(8192-prefix.size());
            sink.append(prefix);sink.append(text);
        }
    }
private:
    const std::uint64_t elapsedMs_;const std::size_t count_;
    std::array<::stream::telemetry::network::ControlDiscoveryEvent,Capacity> values_{};
};
static_assert(sizeof(ControlDiscoveryRecord)<=::fog::RunLog::maxDeferredRawBytes);
class NetworkEventBatchRecord final : public ::fog::RunLog::DeferredRecord {
public:
    static constexpr std::size_t Capacity=32;
    NetworkEventBatchRecord(std::uint64_t elapsedMs,const ::stream::telemetry::network::NetworkEvent* events,std::size_t count,bool final)
        :elapsedMs_(elapsedMs),count_(count),final_(final){
        if(!events||!count||count>Capacity)throw std::invalid_argument("invalid network event batch");
        std::copy_n(events,count_,events_.begin());
    }
    std::size_t rawBytes()const noexcept override{return sizeof(*this);}
    std::size_t lineCount()const noexcept override{return count_;}
    std::size_t maxOutputBytes()const noexcept override{return count_*1980+count_*32+count_-1;}
    void format(::fog::RunLog::DeferredSink& sink)const override{
        namespace n=::stream::telemetry::network;
        for(std::size_t i=0;i<count_;++i){
            if(i)sink.nextLine();
            const auto& e=events_[i];
            auto text=n::networkEventText(e,final_);
            text+=n::values({{"journalSequence",e.journalSequence},{"journalMissed",e.journalMissed}});
            if(text.size()>1980)text.resize(1980);
            sink.append("["+std::to_string(elapsedMs_)+" ms] ");sink.append(text);
        }
    }
private:
    const std::uint64_t elapsedMs_;const std::size_t count_;const bool final_;
    std::array<::stream::telemetry::network::NetworkEvent,Capacity> events_{};
};
static_assert(sizeof(NetworkEventBatchRecord)<=16384);
}
