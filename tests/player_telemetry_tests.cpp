#include "player_fakes.hpp"
#include "features/streaming/application/player_diagnostics_formatter.hpp"
#include "features/streaming/application/ui_telemetry_logging.hpp"
#include "features/streaming/application/output_diagnostics_formatter.hpp"
#include "features/streaming/infrastructure/bounded_network_events.hpp"
#include "core/diagnostics/diagnostic_profile.hpp"
#include "core/constants/stage1_options.hpp"
#include "app/final_output_telemetry.hpp"
#include "app/stream_telemetry_logging.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include "maximum.hpp"

namespace {std::atomic<bool> failNextLogWrite{false};}
extern "C" ssize_t __real_write(int,const void*,std::size_t);
extern "C" ssize_t __wrap_write(int fd,const void* p,std::size_t count){
    if(failNextLogWrite.exchange(false)){errno=EIO;return -1;}
    return __real_write(fd,p,count);
}
namespace fog::media::telemetry {
const FormatNames& processFormatNames() noexcept {
    struct Dictionary {FormatNames names;Dictionary(){assert(names.setPixel(511,"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"));}};
    static const Dictionary dictionary;return dictionary.names;
}
}
namespace fogplay::diagnostics {
fog::RunLog runLog;
std::string safeText(std::string value,std::size_t maximum){return value.substr(0,maximum);}
void diagnostic(const std::string&,std::size_t){}
std::uint64_t diagnosticElapsedMs(){return 17;}
bool deferredDiagnostic(std::unique_ptr<const fog::RunLog::DeferredRecord> value,fog::RunLog::Admission admission)noexcept{return runLog.appendDeferred(std::move(value),admission);}
}
namespace fogplay {
using diagnostics::runLog;
using diagnostics::diagnostic;
using diagnostics::safeText;
using u64=std::uint64_t;
struct PadState {};
enum class Screen {Streaming};
inline std::vector<std::string> pumpTrace;
struct PlayerPhaseFake {
    bool room=true;
    void observe(std::chrono::steady_clock::time_point){pumpTrace.push_back("observe");room=false;}
    struct Before {bool media=false;};
    Before beforeMedia(int,std::chrono::steady_clock::time_point){pumpTrace.push_back("before");return {};}
    void cancel(){}
    struct Snapshot {stream::InputSuspension suspension;};
    Snapshot snapshot()const{return {};}
    void media(std::chrono::steady_clock::time_point){}
};
struct RentalPhaseFake {bool requested()const{return false;}};
struct ResetCounter {unsigned resets=0;void reset(){++resets;}};
struct ApplicationController {
    Screen screen_=Screen::Streaming;
    PlayerPhaseFake player_;
    RentalPhaseFake rentalSession_;
    fog::stream::InputSuspensionStats inputSuspensionStats_;
    bool inputFreezeWaiting_=false;
    void samplePlayStationInput(PadState&){pumpTrace.push_back("sample");}
    void logPlayStationInput(bool){pumpTrace.push_back("ps-log");assert(!player_.room);}
    bool handleStreamOverlay(PadState&,u64,u64){return false;}
    int playerAdmission()const{return 0;}
    void applyPlayerEvents(){}
    void applyPlayerEffects(){}
    void sendStreamInput(PadState&,u64,bool){}
    void pumpStream(PadState&,u64,u64);
    fog::stream::LoopTiming loopTiming_;
    std::chrono::steady_clock::time_point loopTimingWindowAt_=std::chrono::steady_clock::now(),loopTimingLifetimeAt_=loopTimingWindowAt_;
    fog::diagnostics::ThreadObservationSnapshot lastThreadObservations_;
    bool threadObservationsCaptured_=false;
    std::size_t nextThreadChunk_=0;
    std::uint64_t lastThreadCapturedElapsedMs_=0;
    std::chrono::steady_clock::time_point lastThreadCapturedAt_{};
    std::optional<fog::playstation::telemetry::Snapshot> pendingFinalPsTelemetry_;
    ResetCounter mouseButtons_,keyReleases_,gamepadSchedule_;
    void emitPendingFinalPsTelemetry() noexcept;
    void transportClosed() noexcept;
    void logOutputTimings(bool);
    bool streamPlayStation_=false;
    struct RumbleFake {stream::telemetry::rumble::Snapshot captureTelemetry()const{return {};}} rumble_;
    ResetCounter presentTiming_;
    stream::telemetry::Playback playbackSnapshot(stream::telemetry::Playback value,bool)const{return value;}
    void report(fog::stream::PlayerDiagnosticPhase,const fog::stream::PlayerDiagnosticSnapshot&)noexcept;
};
}
#include "output-glue.hpp"
namespace {
using namespace player_test;
using Phase=fog::stream::PlayerDiagnosticPhase;
namespace t=fog::stream::diagnostics;
namespace n=stream::telemetry::network;
// Production emits THREE terminal records: Final, FinalOutput(with optionalPS),
// Closed. Their real owned payloads plus maximum declared output+newline fit
// the one fixed outstanding reserve, even behind four ordinary records.
static_assert(2*(sizeof(t::PlayerRecord)+t::PlayerRecord::TerminalOutputBytes+1)+
    sizeof(fogplay::diagnostics::FinalOutputRecord)+fogplay::diagnostics::FinalOutputRecord::MaximumOutputBytes+1<=
    fog::RunLog::maxTerminalQueuedBytes-fog::RunLog::maxQueuedBytes);
std::string read(const std::string& path){std::ifstream file(path);return {std::istreambuf_iterator<char>(file),{}};}
struct Directory {
    std::string path;
    Directory(){char name[]="/tmp/fog-player-telemetry-XXXXXX";path=mkdtemp(name);}
    ~Directory(){std::filesystem::remove_all(path);}
};
struct GateRecord final:fog::RunLog::DeferredRecord {
    explicit GateRecord(Blocker& gate):gate_(gate){}
    std::size_t rawBytes()const noexcept override{return sizeof(*this);}
    std::size_t maxOutputBytes()const noexcept override{return 8;}
    void format(fog::RunLog::DeferredSink& sink)const override{gate_.block();sink.append("gate");}
    Blocker& gate_;
};
struct Observer final:fog::stream::PlayerDiagnosticsObserver {
    fog::RunLog* log=nullptr;
    std::vector<Phase> phases,failed;
    std::vector<fog::stream::PlayerDiagnosticSnapshot> values;
    std::uint64_t elapsed=17;
    unsigned batches=0,discarded=0,controlBatches=0,controlDiscarded=0;
    bool admitted=true;
    void report(Phase phase,const fog::stream::PlayerDiagnosticSnapshot& value)noexcept override{
        phases.push_back(phase);values.push_back(value);
        if(log&&phase!=Phase::Control)log->appendDeferred(std::make_unique<t::PlayerRecord>(elapsed,phase,value),phase==Phase::Final||phase==Phase::Closed?fog::RunLog::Admission::Terminal:fog::RunLog::Admission::Ordinary);
    }
    void captureFailed(Phase phase)noexcept override{failed.push_back(phase);if(log)log->appendDeferred({});}
    bool networkEvents(const n::NetworkEvent* events,std::size_t count,bool final)noexcept override{
        ++batches;
        return log?log->appendDeferred(std::make_unique<t::NetworkEventBatchRecord>(elapsed,events,count,final)):admitted;
    }
    void networkEventsDiscarded(unsigned count)noexcept override{discarded+=count;}
    bool controlDiscoveryEvents(const n::ControlDiscoveryEvent* events,std::size_t count)noexcept override{
        ++controlBatches;return log?log->appendDeferred(std::make_unique<t::ControlDiscoveryRecord>(elapsed,events,count)):admitted;
    }
    void controlDiscoveryDiscarded(unsigned count)noexcept override{controlDiscarded+=count;}
};
void phaseRecords(){
    Directory d;fog::RunLog log;assert(log.start(d.path));
    fog::stream::PlayerDiagnosticSnapshot value;
    value.transport.provider=stream::telemetry::Provider::Mts;
    value.transport.transport.availability=stream::telemetry::Availability::Available;
    value.network.provider=n::Provider::Mts;value.network.assembly.hasValue=true;
    value.network.mts.rtp.initialized=true;
    value.mediaTelemetry.video.source.availability=fog::media::telemetry::Availability::Available;
    value.mediaTelemetry.audio.source.availability=fog::media::telemetry::Availability::Available;
    for(auto phase:{Phase::Protocol,Phase::Final,Phase::Closed,Phase::Periodic,Phase::Metrics,Phase::SlowCycle,Phase::DecodeError}){
        assert(log.appendDeferred(std::make_unique<t::PlayerRecord>(731,phase,value)));
        assert(log.flush());
    }
    log.close();assert(log.stats().formatFailures==0&&log.stats().droppedLines==0);
    const auto text=read(d.path+"/last-run.log");
    for(auto prefix:{"Stream DTLS:","Stream RTCP:","Stream transport:","Video RTP final:","Media queue final:","Playback timing:","Stream transport final:","Video decoder final:","Audio playback final:","Input latency:","Client latency:","Input suspension:","Video gaps final:","Video RTP:","Video gaps:","Media queue:","Video decoder:","Audio playback:","Server performance:","Control delivery:","Playback load:","Playback slow cycle:","Video RTP at decode error:"})assert(text.find(std::string("[731 ms] ")+prefix)!=std::string::npos);
    assert(text.find("providerSupported=0")!=std::string::npos);
    assert(text.find("gateScope=player-admission gateReasonSemantics=first-blocking")!=std::string::npos);
    assert(text.find("gateEpisodesIncludeStartupAndTerminal=1")!=std::string::npos);
    assert(text.find("gatePreparedCauseSemantics=last-suspend-latched")!=std::string::npos);
    assert(text.find("[RunLog] deferred-format failure")==std::string::npos);
}
fog::stream::PlayerDiagnosticSnapshot maximumSnapshot(bool fh){
    fog::stream::PlayerDiagnosticSnapshot v;
        player_maximum::maximize(v.transport);player_maximum::maximize(v.network);player_maximum::maximize(v.protocol);
        player_maximum::maximize(v.mediaTelemetry);player_maximum::maximize(v.playback);
        v.transport.provider=fh?stream::telemetry::Provider::FogHub:stream::telemetry::Provider::Mts;
        v.transport.transport.availability=stream::telemetry::Availability::Available;
        v.transport.input.availability=v.transport.control.availability=v.transport.fh.core.availability=stream::telemetry::Availability::Available;
        v.network.provider=fh?n::Provider::FogHub:n::Provider::Mts;
        v.network.mtsQuality.automaticPolicy.reason=12;
        for(auto* o:{&v.network.assembly,&v.network.control,&v.network.history,&v.network.rtcp,&v.network.core,&v.network.dtls,&v.network.ice})o->state=n::State::FinalRetained;
        v.network.mtsIce.availability=n::IceCounters::Availability::Available;v.network.mtsIce.provenance=n::IceCounters::Provenance::FinalRetained;
        v.network.mtsIce.failureReason=n::IceCounters::FailureReason::ConnectivityTimeout;
        v.protocol.provider=fh?stream::telemetry::protocol::Provider::FogHub:stream::telemetry::protocol::Provider::Mts;
        v.protocol.state=stream::telemetry::protocol::State::FinalRetained;v.protocol.mts.operation=stream::telemetry::protocol::Operation::Op4;
        v.mediaTelemetry.captured=fog::media::telemetry::Capture::All;
        v.mediaTelemetry.video.source.availability=v.mediaTelemetry.audio.source.availability=fog::media::telemetry::Availability::FinalRetained;
        v.mediaTelemetry.video.codec=fog::media::telemetry::Codec::Hevc;
        v.mediaTelemetry.video.pixelFormat=v.mediaTelemetry.video.framePixelFormat=v.mediaTelemetry.video.cpuPixelFormat=511;
        for(auto& stage:v.mediaTelemetry.video.distributions)stage.availability=fog::media::telemetry::StageAvailability::Available;
        v.mediaTelemetry.presentation.uploadDistribution.availability=fog::media::telemetry::StageAvailability::Available;
        v.inputGate.initialized=v.inputGate.final=v.inputGate.providerSuspensionSupported=true;
        v.inputGate.capturedAtMs=v.inputGate.sequence=v.inputGate.overwritten=v.inputGate.episodes=v.inputGate.completed=v.inputGate.blockedMs=v.inputGate.activeMs=UINT64_MAX;
        v.inputGate.count=v.inputGate.transitions.size();v.inputGate.reason=fog::stream::InputGateReason::PreparedGate;
        v.inputGate.preparedGateCause=fog::stream::InputPreparedGateCause::ProviderFreeze;v.inputGate.preparedGateCauseAtMs=UINT64_MAX;
        for(auto& duration:v.inputGate.reasonMs)duration=UINT64_MAX;
        for(auto& event:v.inputGate.transitions) {
            event.sequence=event.atMs=event.session=event.transport=event.generation=event.preparedAgeMs=event.previousReasonMs=UINT64_MAX;
            event.previous=event.reason=fog::stream::InputGateReason::RecoveryDeadline;
            event.previousPreparedGateCause=event.preparedGateCause=fog::stream::InputPreparedGateCause::ProviderFreeze;
            event.preparedGateCauseAtMs=UINT64_MAX;
        }
        v.periodicDetails=true;
    return v;
}
void maximumSchema(){
    for(const bool fh:{false,true}){
        Directory d;fog::RunLog log;assert(log.start(d.path));const auto v=maximumSnapshot(fh);
        for(auto phase:{Phase::Protocol,Phase::Final,Phase::Closed,Phase::Periodic,Phase::Metrics,Phase::SlowCycle,Phase::DecodeError}){
            const auto before=log.stats().writtenLines;
            t::OutputInput context{{},UINT64_MAX,true,true,{}};player_maximum::maximize(context.rumble);context.rumble.mapping=stream::telemetry::rumble::Mapping::DualBand;
            auto record=std::make_unique<t::PlayerRecord>(UINT64_MAX,phase,v,v.playback,context);
            const auto expected=record->lineCount();const auto declared=record->maxOutputBytes(),raw=record->rawBytes();
            const auto bytesBefore=std::filesystem::file_size(d.path+"/last-run.log");
            assert(log.appendDeferred(std::move(record)));assert(log.flush());assert(log.stats().writtenLines==before+expected);
            const auto rendered=std::filesystem::file_size(d.path+"/last-run.log")-bytesBefore-1;
            std::cout<<"SIZE provider="<<(fh?"FH":"MTS")<<" phase="<<static_cast<int>(phase)<<" raw="<<raw<<" declared="<<declared<<" rendered="<<rendered<<"\n";
            assert(rendered<=declared);
        }
        log.close();assert(log.stats().formatFailures==0&&log.stats().droppedLines==0);
        const auto text=read(d.path+"/last-run.log");
        assert(text.find("PS transport input closed:")!=std::string::npos);
        assert(text.find("networkEventRetirementMissed=")!=std::string::npos);
    }
}
void eventInvalidDeclarations(){
    std::array<n::NetworkEvent,32> events;
    for(const auto count:{0u,33u}){bool rejected=false;try{t::NetworkEventBatchRecord bad(1,events.data(),count,false);}catch(const std::invalid_argument&){rejected=true;}assert(rejected);}
    bool rejected=false;try{t::NetworkEventBatchRecord bad(1,nullptr,1,false);}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
}
void eventMaximumAndFifo(){
    Directory d;fog::RunLog log;assert(log.start(d.path));Blocker gate;
    assert(log.appendDeferred(std::make_unique<GateRecord>(gate)));log.requestFlush();assert(gate.wait());
    {
        std::array<n::NetworkEvent,32> events;
        for(std::size_t i=0;i<events.size();++i){auto& e=events[i];e.journalSequence=i+1;e.journalMissed=UINT64_MAX;
            if(i%2){e.kind=n::EventKind::Core;e.core.kind=10;e.core.sequence=UINT64_MAX;e.core.connection=UINT64_MAX;e.core.wait=UINT64_MAX;e.core.timeUs=UINT64_MAX;e.core.frame=UINT64_MAX;e.core.value=UINT64_MAX;e.core.requests=UINT64_MAX;e.core.intact=UINT64_MAX;e.core.fecFailures=UINT64_MAX;e.core.totalsValid=UINT64_MAX;}
            else {e.rtp.reason=1;e.rtp.ms=UINT64_MAX;e.rtp.sequence=INT64_MIN;e.rtp.timestamp=UINT32_MAX;e.rtp.missing=UINT64_MAX;e.rtp.gap.last=INT64_MIN;e.rtp.gap.detectedMs=UINT64_MAX;e.rtp.gap.oldestMs=UINT64_MAX;e.rtp.gap.budgetMs=UINT64_MAX;e.rtp.gap.deadlineMs=UINT64_MAX;e.rtp.gap.overshootMs=UINT64_MAX;e.rtp.gap.firstNackMs=UINT64_MAX;e.rtp.gap.lastNackMs=UINT64_MAX;e.rtp.gap.nackAttempts=UINT64_MAX;e.rtp.gap.pendingFirst=INT64_MIN;e.rtp.gap.pendingLast=INT64_MAX;e.rtp.gap.pendingDepth=UINT64_MAX;}
        }
        assert(log.appendDeferred(std::make_unique<t::NetworkEventBatchRecord>(19,events.data(),events.size(),true)));
        for(auto& e:events)e.journalSequence=9999;
    }
    assert(log.append("Stream session=NEXT"));gate.release();assert(log.flush());log.close();
    const auto text=read(d.path+"/last-run.log");assert(log.stats().formatFailures==0&&log.stats().droppedLines==0);
    assert(text.find("journalSequence=32 journalMissed=18446744073709551615")<text.find("Stream session=NEXT"));
    assert(text.find("journalSequence=9999")==std::string::npos);
    assert(text.find("pendingDepth=18446744073709551615")!=std::string::npos);
    assert(text.find("fecFailedBlocks=18446744073709551615")!=std::string::npos);
}
void transitionEventMaximum(){
    Directory d;fog::RunLog log;assert(log.start(d.path));std::array<n::NetworkEvent,32> events;
    std::size_t maxLine=0;
    for(std::size_t i=0;i<events.size();++i){
        auto& e=events[i];e.journalSequence=e.journalMissed=UINT64_MAX;
        if(i%4==0){
            e.kind=n::EventKind::RouteSelected;auto route=maximumSnapshot(false).protocol.mts.route;route.localProtocol=route.remoteProtocol=3;e.transition.setRoute(route);
        }else if(i%4==1){
            e.kind=n::EventKind::Quality;stream::quality::NumericEvent q;player_maximum::maximize(q);
            q.state=9;q.reason=12;q.sampleFps=q.decodedFps=q.videoWorkMs=q.videoBusy=q.uiVideoMs=q.decoderVideoMs=q.uiVideoBusy=q.decoderVideoBusy=1e24;
            e.transition.setQuality(q);
        }else if(i%4==2){
            e.kind=n::EventKind::Fixed;n::FixedEvent fixed;fixed.kind=static_cast<n::FixedEventKind>((i/4)%8);fixed.ms=fixed.value=fixed.other=UINT64_MAX;fixed.known=fixed.enabled=fixed.queued=true;fixed.nativeWidth=fixed.nativeHeight=fixed.tableEntries=UINT32_MAX;e.transition.setFixed(fixed);
        }
        if(i%4==3){e.kind=n::EventKind::UdpPoll;n::UdpPollEvent udp;player_maximum::maximize(udp);e.transition.setUdp(udp);}
        const auto text=n::networkEventText(e,true)+n::values({{"journalSequence",e.journalSequence},{"journalMissed",e.journalMissed}});
        maxLine=std::max(maxLine,text.size());assert(text.size()<=1980);
    }
    assert(log.appendDeferred(std::make_unique<t::NetworkEventBatchRecord>(UINT64_MAX,events.data(),events.size(),true)));assert(log.flush());log.close();
    assert(log.stats().formatFailures==0&&log.stats().writtenLines==32);
    const auto text=read(d.path+"/last-run.log");assert(text.find("routeEvent=selected")!=std::string::npos&&text.find("decoderVideoBusy=")!=std::string::npos&&text.find("UDP poll event:")!=std::string::npos);
    std::cout<<"EVENT_MAX_LINE="<<maxLine<<" eventRaw="<<sizeof(n::NetworkEvent)<<" batchRaw="<<sizeof(t::NetworkEventBatchRecord)<<"\n";
}
struct BlockedPlayerRecord final:fog::RunLog::DeferredRecord {
    BlockedPlayerRecord(Blocker& gate,const fog::stream::PlayerDiagnosticSnapshot& value):gate_(gate),record_(53,Phase::Protocol,value){}
    std::size_t rawBytes()const noexcept override{return sizeof(*this);}
    std::size_t maxOutputBytes()const noexcept override{return record_.maxOutputBytes();}
    std::size_t lineCount()const noexcept override{return record_.lineCount();}
    void format(fog::RunLog::DeferredSink& sink)const override{gate_.block();record_.format(sink);}
    Blocker& gate_;t::PlayerRecord record_;
};
void coincidentCycleFourRecords(){
    for(const bool fh:{false,true}){
    Directory d;fog::RunLog log;assert(log.start(d.path));Blocker gate;
    const auto value=maximumSnapshot(fh);
    assert(log.appendDeferred(std::make_unique<BlockedPlayerRecord>(gate,value)));log.requestFlush();assert(gate.wait());
    assert(log.appendDeferred(std::make_unique<t::PlayerRecord>(53,Phase::Metrics,value)));
    namespace ui=stream::telemetry::ui;
    fog::stream::LoopTiming loop;
    const auto now=fog::stream::LoopTiming::Clock::now();
    fog::diagnostics::ThreadObservationSnapshot threads;threads.size=65;threads.epoch=threads.overflow=threads.persistentOverflow=UINT64_MAX;
    for(auto& row:threads.entries){row.role.fill('x');row.role.back()=0;row.core=row.idealCore=row.priority=INT32_MIN;row.affinity=row.observedEpoch=UINT64_MAX;row.affinityResult=row.priorityResult=INT64_MIN;row.queryErrors=UINT32_MAX;row.available=row.carried=true;}
    const auto chunk=ui::captureThreadChunk(threads,0,41,now);
    assert(log.appendDeferred(std::make_unique<t::OutputRecord>(ui::captureLoop(loop,ui::LoopScope::Window,now,now,53),chunk)));
    std::array<n::NetworkEvent,32> events;for(unsigned i=0;i<events.size();++i)events[i].journalSequence=i+1;
    assert(log.appendDeferred(std::make_unique<t::NetworkEventBatchRecord>(53,events.data(),events.size(),false)));
    assert(log.stats().deferredRecords==4&&log.stats().droppedDeferred==0);
    std::cout<<"CYCLE provider="<<(fh?"FH":"MTS")<<" residentBytes="<<log.stats().residentBytes<<" records="<<log.stats().deferredRecords<<"\n";
    assert(log.append("next-controller"));gate.release();assert(log.flush());log.close();
    assert(log.stats().formatFailures==0&&log.stats().droppedLines==0);
    const auto text=read(d.path+"/last-run.log");
    assert(text.find("[53 ms] Stream protocol:")<text.find("[53 ms] Media video="));
    assert(text.find("[53 ms] Media video=")<text.find("[53 ms] Playback timing:"));
    assert(text.find("[53 ms] Output loop window:")<text.find("[41 ms] Thread observation"));
    assert(text.find("journalSequence=32")<text.find("next-controller"));
    }
}
void ownersDieBeforeFormat(){
    Directory d;fog::RunLog log;assert(log.start(d.path));Blocker gate;
    assert(log.appendDeferred(std::make_unique<GateRecord>(gate)));log.requestFlush();assert(gate.wait());
    Observer observer;observer.log=&log;
    {
        Fixture f(true);f.owner.setDiagnosticsObserver(&observer);assert(f.owner.start(f.request(),{})==Player::StartResult::Started);
        f.client()->telemetry.provider=stream::telemetry::Provider::Mts;
        f.client()->telemetry.transport.availability=stream::telemetry::Availability::Available;
        f.client()->telemetry.mts.stages[0].calls=71;
        f.owner.closeAfterStopDispatch();assert(f.client()->destroyed==1&&f.media()->destroyed==1);
        assert(observer.phases==std::vector<Phase>({Phase::Final,Phase::Closed}));
        assert(log.append("Stream session=NEXT"));
        f.owner.setDiagnosticsObserver(nullptr);
    }
    gate.release();assert(log.flush());log.close();const auto text=read(d.path+"/last-run.log");
    assert(text.find("pumpCalls=71")!=std::string::npos);
    assert(text.find("[17 ms] Stream transport final:")<text.find("[17 ms] Stream transport closed:"));
    assert(text.find("Stream transport closed:")<text.find("Stream session=NEXT"));
    assert(log.stats().deferredRecords==0&&log.stats().formatFailures==0);
}
void actualEventAdmission(){
    for(const auto provider:{fog::ProviderId::Mts,fog::ProviderId::FogHub})for(const bool admitted:{true,false}){
        Observer observer;observer.admitted=admitted;Fixture f(true);f.owner.setDiagnosticsObserver(&observer);
        f.owner.start(f.request(provider),{});const auto state=f.client();
        assert(state->numericEventsAtStart);
        state->network.videoGeneration=101;f.media()->telemetry.mediaInstance=202;
        for(unsigned i=0;i<32;++i){n::NetworkEvent e;e.journalSequence=i+1;state->networkEvents.push_back(e);}
        f.now+=1s;f.before();assert(observer.batches==1&&state->networkEvents.empty());
        assert(state->eventAdmitted==(admitted?32u:0u)&&state->eventUncertain==(admitted?0u:32u));
        assert(state->legacyNetworkReads==0&&state->legacyEventPolls==0&&f.media()->legacyTelemetryReads==0);
        f.now+=1s;f.before();assert(observer.batches==1);
        n::NetworkEvent e;e.journalSequence=33;state->networkEvents.push_back(e);
        state->closeAction=[weak=std::weak_ptr<ClientState>(state)]{if(const auto state=weak.lock()){n::NetworkEvent late;late.journalSequence=34;state->networkEvents.push_back(late);}};
        f.owner.closeAfterStopDispatch();f.joined();assert(observer.batches==3&&state->networkEvents.empty());
        assert(state->eventAdmitted==(admitted?34u:0u)&&state->eventUncertain==(admitted?0u:34u));
        const auto final=std::find(observer.phases.begin(),observer.phases.end(),Phase::Final);assert(final!=observer.phases.end());
        const auto index=static_cast<std::size_t>(final-observer.phases.begin());
        assert(observer.values[index].network.videoGeneration==101&&observer.values[index].mediaTelemetry.mediaInstance==202);
        const auto closed=std::find(observer.phases.begin(),observer.phases.end(),Phase::Closed);assert(closed!=observer.phases.end());
        const auto closedIndex=static_cast<std::size_t>(closed-observer.phases.begin());
        assert(observer.values[closedIndex].network.eventCursor==34&&observer.values[closedIndex].network.eventMissed==(admitted?0u:34u));
        f.owner.setDiagnosticsObserver(nullptr);
    }
}
void unhealthyAdmissionIsNeverRetried(){
    Directory d;fog::RunLog log;assert(log.start(d.path));failNextLogWrite=true;
    assert(log.append("prime"));assert(!log.flush());assert(!log.archiveHealthy()||!log.mirrorHealthy());
    Observer observer;observer.log=&log;Fixture f(true);f.owner.setDiagnosticsObserver(&observer);f.owner.start(f.request(),{});
    n::NetworkEvent event;event.journalSequence=7;f.client()->networkEvents.push_back(event);
    f.now+=1s;f.before();assert(observer.batches==1&&f.client()->eventUncertain==1&&f.client()->eventAdmitted==0);
    const auto accepted=log.stats().acceptedDeferred;assert(accepted>=1);f.now+=1s;f.before();assert(observer.batches==1&&log.stats().acceptedDeferred==accepted);
    f.owner.closeAfterStopDispatch();f.owner.setDiagnosticsObserver(nullptr);assert(!log.flush());log.close();
    const auto mirror=read(d.path+"/last-run.log"),archive=read(log.archivePath());const auto& text=mirror.find("journalSequence=7")!=std::string::npos?mirror:archive;
    const auto first=text.find("journalSequence=7");assert(first!=std::string::npos&&text.find("journalSequence=7",first+1)==std::string::npos);
}
void fhBurstCadenceAndJoinedDiscard(){
    for(const auto provider:{fog::ProviderId::Mts,fog::ProviderId::FogHub}){
        Observer observer;Fixture f(true);f.owner.setDiagnosticsObserver(&observer);
        assert(f.owner.start(f.request(provider),{})==Player::StartResult::Started);
        const auto state=f.client();
        for(unsigned i=1;i<=84;++i){n::NetworkEvent e;e.journalSequence=i;state->networkEvents.push_back(e);}
        for(unsigned frame=0;frame<3;++frame){f.now+=17ms;f.before();}
        if(provider==fog::ProviderId::FogHub){
            assert(observer.batches==3&&state->eventAdmitted==84&&state->networkEvents.empty());
            const auto reports=observer.phases.size();f.now+=17ms;f.before();
            assert(observer.batches==3&&observer.phases.size()==reports);
        }else assert(observer.batches==0&&state->networkEvents.size()==84);
        // Disable observation before the unrelated fixture cleanup.
        f.owner.setDiagnosticsObserver(nullptr);f.owner.closeAfterStopDispatch();f.joined();
    }
    for(const bool admitted:{true,false}){
        Observer observer;observer.admitted=admitted;Fixture f(true);f.owner.setDiagnosticsObserver(&observer);
        assert(f.owner.start(f.request(fog::ProviderId::FogHub),{})==Player::StartResult::Started);
        const auto state=f.client();state->finalEventDiscardBudget=297;
        // This pre-close backlog exceeds one batch and must survive until core
        // join, when a late producer contributes another complete core ring.
        for(unsigned i=1;i<=84;++i){n::NetworkEvent e;e.journalSequence=i;state->networkEvents.push_back(e);}
        state->closeAction=[weak=std::weak_ptr<ClientState>(state)]{if(const auto s=weak.lock()){
            for(unsigned i=85;i<=340;++i){n::NetworkEvent e;e.journalSequence=i;s->networkEvents.push_back(e);}
        }};
        f.owner.closeAfterStopDispatch();f.joined();
        assert(observer.batches==2&&state->networkEvents.empty()&&observer.discarded==276);
        assert(state->eventAdmitted==(admitted?64u:0u)&&state->eventUncertain==(admitted?276u:340u));
        const auto closed=std::find(observer.phases.begin(),observer.phases.end(),Phase::Closed);assert(closed!=observer.phases.end());
        const auto index=static_cast<std::size_t>(closed-observer.phases.begin());
        assert(observer.values[index].network.eventCursor==340&&observer.values[index].network.eventMissed==(admitted?276u:340u));
        f.owner.setDiagnosticsObserver(nullptr);
    }
}
void fhBurstToRealLog(){
    Directory d;fog::RunLog log;assert(log.start(d.path));Observer observer;observer.log=&log;
    Fixture f(true);f.owner.setDiagnosticsObserver(&observer);
    assert(f.owner.start(f.request(fog::ProviderId::FogHub),{})==Player::StartResult::Started);
    const auto state=f.client();
    for(const unsigned burst:{84u,256u}){
        for(unsigned i=1;i<=burst;++i){n::NetworkEvent e;e.kind=n::EventKind::Core;e.provider=n::Provider::FogHub;
            e.journalSequence=i;e.core.sequence=i;e.core.kind=2;state->networkEvents.push_back(e);}
        const auto before=observer.batches;
        for(unsigned frame=0;frame<(burst+31)/32;++frame){
            f.now+=17ms;f.before();std::this_thread::sleep_for(17ms);
        }
        assert(observer.batches-before==(burst+31)/32&&state->networkEvents.empty());
        assert(log.flush()&&state->eventUncertain==0&&log.stats().droppedDeferred==0&&log.stats().droppedLines==0);
    }
    assert(state->eventAdmitted==340);
    f.owner.closeAfterStopDispatch();f.joined();f.owner.setDiagnosticsObserver(nullptr);assert(log.flush());log.close();
    assert(log.stats().formatFailures==0&&log.stats().droppedLines==0);
}
void controlDiscoveryRecords(){
    Directory d;fog::RunLog log;assert(log.start(d.path));Blocker gate;assert(log.appendDeferred(std::make_unique<GateRecord>(gate)));log.requestFlush();assert(gate.wait());
    {
        std::array<n::ControlDiscoveryEvent,4> values;
        for(auto& value:values){value.sequence=value.count=value.pending=value.firstMs=value.lastMs=value.discarded=UINT64_MAX;value.description.id=8;value.description.bytes=UINT64_MAX;value.description.unknownNames=UINT32_MAX;value.description.sample.count=64;
            for(auto& token:value.description.sample.tokens){token.kind=n::DiscoveryTokenKind::SignedValue;token.integer=INT64_MIN;}}
        assert(log.appendDeferred(std::make_unique<t::ControlDiscoveryRecord>(31,values.data(),values.size())));
        for(auto& value:values)value.sequence=1;
    }
    assert(log.append("next-controller"));gate.release();assert(log.flush());log.close();assert(log.stats().formatFailures==0);
    const auto text=read(d.path+"/last-run.log");assert(text.find("[31 ms] Stream control: provider=mts mtsControl")!=std::string::npos);
    assert(text.find("discoverySequence=18446744073709551615")<text.find("next-controller"));
    for(const auto count:{0u,5u}){n::ControlDiscoveryEvent value;bool rejected=false;try{t::ControlDiscoveryRecord bad(1,&value,count);}catch(const std::invalid_argument&){rejected=true;}assert(rejected);}
    bool rejected=false;try{t::ControlDiscoveryRecord bad(1,nullptr,1);}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
}
struct BlockedControlRecord final:fog::RunLog::DeferredRecord {
    BlockedControlRecord(Blocker& gate,const n::ControlDiscoveryEvent* values):gate_(gate),record_(31,values,4){}
    std::size_t rawBytes()const noexcept override{return sizeof(*this);}
    std::size_t maxOutputBytes()const noexcept override{return record_.maxOutputBytes();}
    std::size_t lineCount()const noexcept override{return record_.lineCount();}
    void format(fog::RunLog::DeferredSink& sink)const override{gate_.block();record_.format(sink);}
    Blocker& gate_;t::ControlDiscoveryRecord record_;
};
void finalPsUiFourRecords(){
    Directory d;fog::RunLog log;assert(log.start(d.path));Blocker gate;
    {
        std::array<n::ControlDiscoveryEvent,4> controls;for(auto& value:controls)value.count=77;
        assert(log.appendDeferred(std::make_unique<BlockedControlRecord>(gate,controls.data())));log.requestFlush();assert(gate.wait());
        std::array<n::NetworkEvent,32> events;for(unsigned i=0;i<events.size();++i)events[i].journalSequence=i+1;
        assert(log.appendDeferred(std::make_unique<t::NetworkEventBatchRecord>(31,events.data(),events.size(),true)));
        auto values=maximumSnapshot(false);assert(log.appendDeferred(std::make_unique<t::PlayerRecord>(31,Phase::Final,values),fog::RunLog::Admission::Terminal));
        fog::stream::LoopTiming loop;namespace ui=stream::telemetry::ui;const auto now=fog::stream::LoopTiming::Clock::now();
        const auto window=ui::captureLoop(loop,ui::LoopScope::FinalWindow,now,now,31),lifetime=ui::captureLoop(loop,ui::LoopScope::FinalLifetime,now,now,31);
        fog::playstation::telemetry::Snapshot ps;ps.final=true;ps.hasTransport=true;ps.elapsedMs=29;ps.gamepadAccepted=99;ps.transport=values.protocol;
        assert(fogplay::diagnostics::enqueueFinalOutput(log,window,lifetime,ps));
        std::cout<<"FINAL_CYCLE residentBytes="<<log.stats().residentBytes<<" records="<<log.stats().deferredRecords<<" finalUiRaw="<<sizeof(fogplay::diagnostics::FinalOutputRecord)<<"\n";
        assert(log.stats().deferredRecords==4&&log.stats().droppedDeferred==0);
        assert(log.appendDeferred(std::make_unique<t::PlayerRecord>(32,Phase::Closed,values),fog::RunLog::Admission::Terminal));
        assert(log.stats().deferredRecords==5&&log.stats().droppedDeferred==0);
        assert(log.stats().residentBytes<=fog::RunLog::maxTerminalQueuedBytes);
        std::cout<<"FINAL_CLOSED_CYCLE residentBytes="<<log.stats().residentBytes<<" records="<<log.stats().deferredRecords<<"\n";
        ps.gamepadAccepted=111;for(auto& event:events)event.journalSequence=999;
    }
    // Terminal residency can exceed the ordinary byte budget; the next
    // session starts only after this fixed outstanding burst is drained.
    gate.release();assert(log.flush());assert(log.append("Stream session=NEXT"));assert(log.flush());log.close();
    const auto text=read(d.path+"/last-run.log");assert(log.stats().formatFailures==0&&log.stats().droppedLines==0);
    assert(text.find("Stream control:")<text.find("Stream final event:"));
    assert(text.find("journalSequence=32")<text.find("Video RTP final:"));
    assert(text.find("Video RTP final:")<text.find("Output loop final window:"));
    assert(text.find("Output draw final lifetime:")<text.find("[29 ms] PS input final:"));
    assert(text.find("gamepadQueued=99")<text.find("[32 ms] Stream transport closed:"));
    assert(text.find("[32 ms] Stream transport closed:")<text.find("Stream session=NEXT"));
    assert(text.find("[32 ms] Stream ICE final:")!=std::string::npos);
    assert(text.find("gamepadQueued=111")==std::string::npos);
}
void controlDiscoveryScheduling(){
    for(const bool admitted:{true,false}){
        Observer observer;observer.admitted=admitted;Fixture f(true);f.owner.setDiagnosticsObserver(&observer);f.owner.start(f.request(),{});
        const auto state=f.client();state->discoveryEnabled=true;n::ControlDiscoveryEvent event;event.sequence=1;state->discoveryEvents.push_back(event);
        f.now+=16s;f.before();
        const bool details=fog::metrics::collects(fog::metrics::DiagnosticClass::PeriodicDetail);
        assert(observer.controlBatches==(details?0u:1u));
        if(details){assert(state->discoveryEvents.size()==1&&!f.owner.periodicDiagnosticsHaveRoom());f.media()->pending=true;f.owner.media(f.now);assert(!f.owner.periodicDiagnosticsHaveRoom());f.now+=1ms;f.before();assert(f.owner.periodicDiagnosticsHaveRoom());}
        assert(observer.controlBatches==1&&state->discoveryEvents.empty()&&state->legacyControlPolls==0);
        for(unsigned i=0;i<9;++i){event.sequence=i+2;state->discoveryEvents.push_back(event);}
        f.owner.closeAfterStopDispatch();assert(state->discoveryEvents.empty());
        assert(observer.controlBatches==2&&observer.controlDiscarded==5);
        assert(state->discoveryAdmitted==(admitted?5u:0u)&&state->discoveryUncertain==(admitted?5u:10u));
        f.owner.setDiagnosticsObserver(nullptr);
    }
}
void actualPumpOrder(){
    fogplay::pumpTrace.clear();fogplay::ApplicationController app;fogplay::PadState pad;
    app.pumpStream(pad,0,0);
    assert(fogplay::pumpTrace==std::vector<std::string>({"sample","observe","ps-log","before"}));
}
void actualTerminalPlayerGlue(){
    for(const bool fh:{false,true}) {
    Directory d;auto& log=fogplay::diagnostics::runLog;assert(log.start(d.path));Blocker gate;
    const auto value=maximumSnapshot(fh);
    // Match coincidentCycleFourRecords exactly, including its expensive writer
    // in-flight PlayerRecord and all outstanding worst-case output charges.
    assert(log.appendDeferred(std::make_unique<BlockedPlayerRecord>(gate,value)));log.requestFlush();assert(gate.wait());
    assert(log.appendDeferred(std::make_unique<t::PlayerRecord>(53,Phase::Metrics,value)));
    namespace ui=stream::telemetry::ui;
    fog::stream::LoopTiming loop;const auto now=fog::stream::LoopTiming::Clock::now();
    fog::diagnostics::ThreadObservationSnapshot threads;threads.size=65;threads.epoch=threads.overflow=threads.persistentOverflow=UINT64_MAX;
    for(auto& row:threads.entries){row.role.fill('x');row.role.back()=0;row.core=row.idealCore=row.priority=INT32_MIN;row.affinity=row.observedEpoch=UINT64_MAX;row.affinityResult=row.priorityResult=INT64_MIN;row.queryErrors=UINT32_MAX;row.available=row.carried=true;}
    const auto chunk=ui::captureThreadChunk(threads,0,41,now);
    assert(log.appendDeferred(std::make_unique<t::OutputRecord>(ui::captureLoop(loop,ui::LoopScope::Window,now,now,53),chunk)));
    std::array<n::NetworkEvent,32> events;for(unsigned i=0;i<events.size();++i)events[i].journalSequence=i+1;
    assert(log.appendDeferred(std::make_unique<t::NetworkEventBatchRecord>(53,events.data(),events.size(),false)));
    const auto ordinaryBytes=log.stats().residentBytes;
    assert(log.stats().deferredRecords==4&&log.stats().droppedLines==0);
    fogplay::ApplicationController app;
    fog::playstation::telemetry::Snapshot ps;ps.final=true;ps.hasTransport=true;ps.elapsedMs=13;ps.gamepadAccepted=55;ps.transport=value.protocol;
    app.pendingFinalPsTelemetry_=ps;app.streamPlayStation_=true;
    // Compiled verbatim production report: Final also emits FinalOutput withPS.
    app.report(Phase::Final,value);
    assert(log.stats().acceptedDeferred==6&&log.stats().deferredRecords==6&&log.stats().droppedLines==0);
    app.report(Phase::Closed,value);
    assert(log.stats().acceptedDeferred==7&&log.stats().deferredRecords==7&&log.stats().droppedLines==0);
    assert(log.stats().residentBytes<=fog::RunLog::maxTerminalQueuedBytes);
    std::cout<<"MAX_ORDINARY_PLUS_ALL_FINAL provider="<<(fh?"FH":"MTS")<<" ordinaryBytes="<<ordinaryBytes<<" totalBytes="<<log.stats().residentBytes<<" records="<<log.stats().deferredRecords<<"\n";
    assert(!fogplay::diagnostics::diagnosticPlayer(73,Phase::Closed,value,value.playback));
    assert(!fogplay::diagnostics::diagnosticPlayer(74,Phase::Protocol,value,value.playback));
    n::NetworkEvent event;event.journalSequence=987;
    // A final event batch remains Ordinary and cannot consume the reserve.
    assert(!fogplay::diagnostics::diagnosticNetworkEvents(75,&event,1,true));
    assert(log.stats().rejected.records==3&&log.stats().droppedLines==14);
    gate.release();assert(!log.flush());log.close();
    const auto text=read(d.path+"/last-run.log");
    assert(text.find("[17 ms] Video RTP final:")<text.find("[17 ms] Output loop final window:"));
    assert(text.find("[13 ms] PS input final:")<text.find("[17 ms] Stream transport closed:"));
    assert(text.find("[17 ms] Stream ICE final:")!=std::string::npos);
    assert(text.find("gamepadQueued=55")!=std::string::npos);
    assert(text.find("[73 ms]")==std::string::npos&&text.find("[74 ms]")==std::string::npos&&text.find("journalSequence=987")==std::string::npos);
    }
}
void actualOutputGlue(){
    for(const bool fallback:{false,true}){
        Directory d;auto& log=fogplay::diagnostics::runLog;assert(log.start(d.path));Blocker gate;
        assert(log.appendDeferred(std::make_unique<GateRecord>(gate)));log.requestFlush();assert(gate.wait());
        {
            fogplay::ApplicationController app;fog::playstation::telemetry::Snapshot ps;ps.final=true;ps.elapsedMs=13;ps.gamepadAccepted=55;
            app.pendingFinalPsTelemetry_=ps;
            if(fallback)app.transportClosed();else app.logOutputTimings(true);
            assert(!app.pendingFinalPsTelemetry_);
            const auto admitted=log.stats().acceptedDeferred;app.transportClosed();assert(log.stats().acceptedDeferred==admitted);
        }
        assert(log.append("Stream session=NEXT"));gate.release();assert(log.flush());log.close();
        const auto text=read(d.path+"/last-run.log");assert(text.find("[13 ms] PS input final:")<text.find("Stream session=NEXT"));
        assert(text.find("gamepadQueued=55")!=std::string::npos);
        if(!fallback)assert(text.find("[17 ms] Output draw final lifetime:")<text.find("[13 ms] PS input final:"));
        assert(log.stats().acceptedDeferred==2&&log.stats().formatFailures==0);
    }
    Directory d;auto& log=fogplay::diagnostics::runLog;assert(log.start(d.path));Blocker gate;
    assert(log.appendDeferred(std::make_unique<GateRecord>(gate)));log.requestFlush();assert(gate.wait());
    for(unsigned i=0;i<3;++i)assert(log.appendDeferred(std::make_unique<GateRecord>(gate)));
    {
        fogplay::ApplicationController app;fog::playstation::telemetry::Snapshot ps;ps.final=true;ps.elapsedMs=21;
        app.pendingFinalPsTelemetry_=ps;app.logOutputTimings(true);assert(!app.pendingFinalPsTelemetry_);
        const auto accepted=log.stats().acceptedDeferred;app.transportClosed();assert(log.stats().acceptedDeferred==accepted);
        assert(log.stats().droppedDeferred==0&&log.stats().droppedLines==0);
    }
    gate.release();assert(log.flush());log.close();assert(log.stats().acceptedDeferred==5);
}
void diagnosticFailuresDoNotChangeLifecycle(){
    for(const auto provider:{fog::ProviderId::Mts,fog::ProviderId::FogHub})for(const bool observed:{false,true}){
        Observer observer;Fixture f(true);if(observed)f.owner.setDiagnosticsObserver(&observer);
        f.owner.start(f.request(provider),{});const auto state=f.client();state->recovery=stream::RecoveryState::RetryableFailure;state->statusAllocationFailure=true;
        f.owner.beforeMedia({},f.now);f.joined();assert(!f.owner.snapshot().cancelled&&state->retries==1);
        if(observed)assert(observer.failed==(provider==fog::ProviderId::Mts?std::vector<Phase>({Phase::Final,Phase::Closed}):std::vector<Phase>({Phase::Final})));
        state->statusAllocationFailure=false;f.owner.setDiagnosticsObserver(nullptr);
    }
    {
        Observer observer;Fixture f;f.owner.setDiagnosticsObserver(&observer);f.owner.start(f.request(),{});
        f.client()->statusAllocationFailure=true;f.owner.observe(f.now);assert(!f.owner.snapshot().cancelled);
        assert(observer.failed==std::vector<Phase>({Phase::Protocol}));
        f.client()->statusAllocationFailure=false;f.before();f.frame();f.media()->videoFailures=1;f.client()->statusAllocationFailure=true;
        f.owner.media(f.now);assert(!f.owner.snapshot().cancelled&&f.owner.snapshot().videoRecovering&&f.media()->resets==1);
        assert(observer.failed.back()==Phase::DecodeError);f.client()->statusAllocationFailure=false;f.owner.setDiagnosticsObserver(nullptr);
    }
    {
        Directory d;fog::RunLog log;assert(log.start(d.path));Observer observer;observer.log=&log;Fixture f;
        f.owner.setDiagnosticsObserver(&observer);f.owner.start(f.request(),{});f.client()->statusAllocationFailure=true;
        f.owner.observe(f.now);assert(log.stats().droppedDeferred==1&&log.stats().droppedLines==1&&!f.owner.snapshot().cancelled);
        f.client()->statusAllocationFailure=false;f.owner.setDiagnosticsObserver(nullptr);assert(!log.flush());log.close();
    }
}
void directShutdownKeepsJoinedEvidence(){
    for(const bool refusedRetry:{false,true}){
        Observer observer;Fixture f(true);f.owner.setDiagnosticsObserver(&observer);f.owner.start(f.request(fog::ProviderId::FogHub),{});const auto state=f.client();
        state->closeAction=[weak=std::weak_ptr<ClientState>(state)]{if(const auto s=weak.lock()){n::NetworkEvent event;event.journalSequence=77;s->networkEvents.push_back(event);}};
        std::unique_ptr<Blocker> blocker;
        if(refusedRetry){blocker=std::make_unique<Blocker>();state->retryOk=false;state->retryAction=[&]{blocker->block();};state->recovery=stream::RecoveryState::RetryableFailure;f.before();assert(blocker->wait());blocker->release();}
        f.owner.shutdownAfterCancellation();assert(state->destroyed==1);
        assert(std::count(observer.phases.begin(),observer.phases.end(),Phase::Closed)==1);
        if(!refusedRetry)assert(observer.batches==1&&state->networkEvents.empty());
        else {const auto found=std::find(observer.phases.begin(),observer.phases.end(),Phase::Closed);const auto index=static_cast<std::size_t>(found-observer.phases.begin());assert(observer.values[index].retry&&!observer.values[index].allowed);}
        f.owner.setDiagnosticsObserver(nullptr);
    }
}
void menuImageParityAndOwnerDeath(){
    Directory d;fog::RunLog log;assert(log.start(d.path));Blocker gate;assert(log.appendDeferred(std::make_unique<GateRecord>(gate)));log.requestFlush();assert(gate.wait());
    std::string expected;
    {
        Fixture f(true);f.owner.start(f.request(),{});auto& image=f.media()->telemetry.image;
        image.samples=123;image.blackFrames=17;image.draws=44;image.nearBlack=true;image.width=1280;image.height=720;
        const auto value=f.owner.mediaView()->captureTelemetry();auto text=std::string("Video image at menu: ")+fog::media::telemetry::formatImage(value);if(text.size()>2048)text.resize(2048);
        expected="[27 ms] "+text+"\n";assert(log.appendDeferred(std::make_unique<t::MenuImageRecord>(27,value)));
        image.samples=999;f.owner.closeAfterStopDispatch();assert(f.media()->destroyed==1);
    }
    assert(log.append("next-controller"));gate.release();assert(log.flush());log.close();
    const auto text=read(d.path+"/last-run.log");assert(text=="gate\n"+expected+"next-controller\n");assert(log.stats().formatFailures==0);
}
void diagnosticDeadlineCrossing(){
    if(!fog::metrics::collects(fog::metrics::DiagnosticClass::PeriodicDetail))return;
    Observer observer;Fixture f(true);f.owner.setDiagnosticsObserver(&observer);f.owner.start(f.request(),{});
    f.client()->discoveryEnabled=true;f.now+=4999ms;f.before();assert(f.owner.periodicDiagnosticsHaveRoom());
    f.media()->pending=true;f.media()->prepareAction=[&]{f.now+=2ms;};f.owner.media(f.now);
    assert(std::count(observer.phases.begin(),observer.phases.end(),Phase::Metrics)==0);
    f.before();f.owner.media(f.now);assert(std::count(observer.phases.begin(),observer.phases.end(),Phase::Metrics)==1);
    f.owner.setDiagnosticsObserver(nullptr);
}
void lifecycleProfiles(){
    for(const auto provider:{fog::ProviderId::Mts,fog::ProviderId::FogHub}) {
        Observer observer;Fixture f(true);f.owner.setDiagnosticsObserver(&observer);f.owner.start(f.request(provider),{});
        f.before();assert(std::find(observer.phases.begin(),observer.phases.end(),Phase::Protocol)!=observer.phases.end());
        f.now+=16s;f.media()->pending=true;f.turn();
        const bool periodic=std::find(observer.phases.begin(),observer.phases.end(),Phase::Periodic)!=observer.phases.end()||
            std::any_of(observer.values.begin(),observer.values.end(),[](const auto& value){return value.periodicDetails;});
        assert(periodic==fog::metrics::collects(fog::metrics::DiagnosticClass::PeriodicDetail));
        observer.phases.clear();f.client()->error="retry";f.client()->retryable=true;f.before();f.joined();
        const auto final=std::find(observer.phases.begin(),observer.phases.end(),Phase::Final);
        const auto closed=std::find(observer.phases.begin(),observer.phases.end(),Phase::Closed);
        assert(final<closed&&closed!=observer.phases.end());
        const auto closedIndex=static_cast<std::size_t>(closed-observer.phases.begin());
        f.now+=2s;f.before();
        if(provider==fog::ProviderId::FogHub){auto effect=f.owner.takeEffect();while(effect&&!std::holds_alternative<Player::InspectionNeeded>(*effect))effect=f.owner.takeEffect();assert(effect);const auto request=std::get<Player::InspectionNeeded>(*effect);f.owner.resumeVerdict({request.purpose,Player::Verdict::Ready,request.expected});f.before();}
        f.before();const auto later=std::find(observer.phases.begin()+closedIndex+1,observer.phases.end(),Phase::Protocol);
        assert(later!=observer.phases.end());
        f.owner.closeAfterStopDispatch();f.joined();f.owner.setDiagnosticsObserver(nullptr);
    }
}
}
int main(){phaseRecords();maximumSchema();eventInvalidDeclarations();eventMaximumAndFifo();transitionEventMaximum();coincidentCycleFourRecords();ownersDieBeforeFormat();actualEventAdmission();unhealthyAdmissionIsNeverRetried();fhBurstCadenceAndJoinedDiscard();fhBurstToRealLog();controlDiscoveryRecords();finalPsUiFourRecords();controlDiscoveryScheduling();actualPumpOrder();actualTerminalPlayerGlue();actualOutputGlue();diagnosticFailuresDoNotChangeLifecycle();directShutdownKeepsJoinedEvidence();menuImageParityAndOwnerDeath();diagnosticDeadlineCrossing();lifecycleProfiles();std::cout<<"PASS actual Player typed telemetry lifecycle, immutable batch ownership, FIFO clocks, profile filters, full32 event bounds\n";}
