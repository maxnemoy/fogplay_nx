#include "player_fakes.hpp"
#include <iostream>

using namespace player_test;
using fog::ProviderId;
static Player::StopReason stop(Fixture& fixture) {
    const auto effect=fixture.owner.takeEffect();
    assert(effect && std::holds_alternative<Player::StopRequired>(*effect));
    return std::get<Player::StopRequired>(*effect).reason;
}
static Player::InspectionNeeded inspection(Fixture& fixture,Player::Purpose purpose) {
    const auto effect=fixture.owner.takeEffect();
    assert(effect && std::holds_alternative<Player::InspectionNeeded>(*effect));
    const auto value=std::get<Player::InspectionNeeded>(*effect);assert(value.purpose==purpose);return value;
}
static void startAndMilestones() {
    for(const auto provider:{ProviderId::Mts,ProviderId::FogHub})for(const bool workers:{false,true})for(const int fps:{30,60}) {
        Fixture f(workers);auto request=f.request(provider);request.video.framesPerSecond=fps;
        request.video.codec=::stream::VideoCodec::H265;request.controller=::stream::ControllerProfile::DualSense;
        request.connection.mode=fog::connection::Mode::Relay;f.medias.hevc=false;
        assert(f.owner.start(request,{})==Player::StartResult::Started);
        assert(f.transports.calls==1&&f.client()->connection.rental==request.rental&&!f.client()->connection.resumeOnly);
        assert(f.client()->profile==request.controller&&f.client()->relay==(provider==ProviderId::Mts));
        assert(f.media()->fps==fps&&f.owner.snapshot().framesPerSecond==fps);
        assert(f.media()->progress.workers==(provider==ProviderId::FogHub||workers));
        if(provider==ProviderId::FogHub)assert(f.client()->settings.codec==::stream::VideoCodec::H264&&f.client()->settings.quality==0&&f.client()->settings.keyframeSeconds==0);
        assert(!f.owner.snapshot().firstPrepared&&!f.owner.snapshot().inputAllowed);
        if(f.media()->progress.workers){f.media()->progress.decoded=1;f.media()->pending=true;}
        else f.frame();
        const auto before=f.before();assert(before.media&&!before.inputAllowed);
        const auto prepared=f.owner.media(f.now);assert(prepared.prepared&&prepared.firstPrepared);
        assert(f.owner.snapshot().firstPrepared&&f.owner.snapshot().progress.presented==0&&f.owner.snapshot().progress.uploaded==1);
        auto* view=f.owner.presentation();assert(view&&view->draw());
        assert(f.owner.snapshot().progress.presented==0);view->present();f.owner.presentationCompleted(f.now);
        assert(f.owner.snapshot().progress.presented==1);view->draw();view->present();f.owner.presentationCompleted(f.now);
        assert(f.owner.snapshot().progress.presented==1&&f.before().inputAllowed);
        if(provider==ProviderId::Mts){f.owner.media(f.now);assert(f.client()->observations&&f.client()->observed.presentedFrames==1);}
        auto changed=request.video;changed.framesPerSecond=fps==30?60:30;f.owner.changeVideo(changed,{},f.now);
        assert(f.owner.snapshot().framesPerSecond==fps);
        f.owner.cancel();assert(!f.owner.snapshot().inputAllowed&&f.client()->closes==0);
        f.trace.add("rental-stop-dispatch");f.owner.closeAfterStopDispatch();f.joined();
        const auto trace=f.trace.copy();
        const auto dispatch=std::find(trace.begin(),trace.end(),"rental-stop-dispatch");
        const auto join=std::find(dispatch,trace.end(),"media-worker-join");
        const auto release=std::find(join,trace.end(),"input-release");
        const auto close=std::find(release,trace.end(),"final-close");
        assert(dispatch<join&&join<release&&release<close&&close<trace.end());
        assert(!f.owner.snapshot().ownsRental&&!f.owner.inputSink()&&!f.owner.presentation());
        assert(f.owner.start(request,{})==Player::StartResult::Started);f.owner.closeResolvedRental();f.joined();
    }
    for(int refusal=0;refusal<6;++refusal) {
        Fixture denied;auto request=denied.request(fog::ProviderId::FogHub);Player::Admission admission;
        if(refusal==0)admission.stopRequested=true;
        if(refusal==1)admission.active=false;
        if(refusal==2)admission.shuttingDown=true;
        if(refusal==3)admission.serviceBusy=true;
        if(refusal==4)admission.rentalPending=false;
        if(refusal==5)admission.pairedPending=false;
        assert(denied.owner.start(request,admission)==Player::StartResult::Refused&&denied.transports.calls==0);
    }
    for(const bool mediaFailure:{false,true}) {
        Fixture f;f.medias.fail=mediaFailure;f.transports.fail=!mediaFailure;
        assert(f.owner.start(f.request(),{})==Player::StartResult::StopRequired&&stop(f)==Player::StopReason::InitialStart);
        assert(f.owner.takeFailure());f.owner.closeAfterStopDispatch();
    }
    Fixture failed;auto client=std::make_shared<ClientState>();client->startOk=false;failed.transports.next.push_back(client);
    assert(failed.owner.start(failed.request(),{})==Player::StartResult::StopRequired&&!failed.media()->progress.workers);
    assert(client->destroyed==0&&client->closes==0);failed.owner.closeAfterStopDispatch();assert(client->closes==1);
}
static void retainedRetryAndAdmission() {
    Fixture f;assert(f.owner.start(f.request(),{})==Player::StartResult::Started);
    f.frame();f.turn();auto original=f.client();
    original->error="retryable";original->retryable=true;assert(!f.before().media);
    assert(original->retries==1&&f.owner.snapshot().retained&&f.owner.snapshot().attempts==0);
    f.now+=2s;f.before();assert(f.transports.calls==1&&original->starts==2&&original->connection.resumeOnly&&f.owner.snapshot().attempts==1);
    f.frame();f.owner.media(f.now);assert(!f.owner.snapshot().recovering);
    original->disconnected=true;f.before();assert(f.owner.snapshot().recovering&&original->retries==1);
    f.now+=4s;f.before();assert(original->retries==1);original->disconnected=false;
    f.frame();f.owner.media(f.now);assert(!f.owner.snapshot().recovering);
    original->recovery=::stream::RecoveryState::TerminalFailure;f.now+=30s;
    f.before();assert(stop(f)==Player::StopReason::FatalTransport&&original->starts==2&&original->closes==0);
    f.owner.closeAfterStopDispatch();
    Fixture due;due.owner.start(due.request(),{});due.client()->error="retryable";due.client()->retryable=true;
    due.before();due.now+=2s;Player::Admission stopped;stopped.stopRequested=true;due.before(stopped);
    assert(due.client()->starts==1&&due.transports.calls==1);due.owner.closeAfterStopDispatch();
    Fixture refused(true);refused.owner.start(refused.request(),{});auto held=refused.client();held->retryOk=false;held->error="retryable";held->retryable=true;
    refused.before();assert(stop(refused)==Player::StopReason::RetryTerminal&&held->destroyed==0&&held->closes==0&&refused.owner.inputSink());
    assert(!refused.owner.snapshot().progress.workers);
    refused.owner.closeAfterStopDispatch();assert(held->closes==1&&held->destroyed==1);
}
static Player::InspectionNeeded beginFhRecovery(Fixture& f) {
    f.owner.start(f.request(ProviderId::FogHub),{});f.client()->error="retryable";f.client()->retryable=true;
    f.before();f.joined();f.now+=2s;assert(!f.before().media);return inspection(f,Player::Purpose::Transport);
}
static void fogHubRecovery() {
    for(const auto result:{Player::Verdict::Ready,Player::Verdict::Retry,Player::Verdict::Refused}) {
        Fixture f;const auto needed=beginFhRecovery(f);assert(f.transports.calls==1);
        f.owner.resumeVerdict({needed.purpose,result,needed.expected});
        assert(f.transports.calls==1);f.before();
        if(result==Player::Verdict::Ready){assert(f.transports.calls==2&&f.client()->connection.resumeOnly);f.media()->pending=true;f.owner.media(f.now);assert(!f.owner.snapshot().recovering);}
        else if(result==Player::Verdict::Retry){assert(f.transports.calls==1&&!f.owner.snapshot().inspection);f.now+=2s;f.before();inspection(f,Player::Purpose::Transport);}
        else assert(stop(f)==Player::StopReason::PreflightRefused&&f.transports.calls==1);
    }
    for(const int failure:{0,1,2,3}) {
        Fixture f;const auto needed=beginFhRecovery(f);
        auto verdict=Player::ResumeVerdict{needed.purpose,Player::Verdict::Ready,needed.expected};
        if(failure==0)verdict.expected.value="wrong";
        if(failure==1)verdict.purpose=Player::Purpose::Quality;
        f.owner.resumeVerdict(verdict);
        if(failure==2)f.now+=30s;
        if(failure==3){Player::Admission a;a.stopRequested=true;f.before(a);}else f.before();
        assert(f.transports.calls==1&&!f.owner.snapshot().inputAllowed);
        if(failure!=3)stop(f);
    }
    Fixture blocked;auto state=std::make_shared<ClientState>();Blocker blocker;state->retryAction=[&]{blocker.block();};blocked.transports.next.push_back(state);
    blocked.owner.start(blocked.request(ProviderId::FogHub),{});state->error="retryable";state->retryable=true;blocked.before();
    const bool entered=blocker.wait();blocked.now+=3s;blocked.before();assert(entered&&blocked.transports.calls==1&&blocked.owner.snapshot().teardown);
    blocked.owner.cancel();blocker.release();blocked.joined();blocked.before();assert(blocked.transports.calls==1);
    blocked.owner.closeAfterStopDispatch();blocked.joined();
    Fixture refused;auto reject=std::make_shared<ClientState>();reject->retryOk=false;refused.transports.next.push_back(reject);
    refused.owner.start(refused.request(ProviderId::FogHub),{});reject->error="retry";reject->retryable=true;refused.before();refused.joined();
    assert(stop(refused)==Player::StopReason::RetryTerminal&&reject->closes==0&&reject->destroyed==0);
    refused.owner.closeAfterStopDispatch();refused.joined();assert(reject->closes==1&&reject->destroyed==1);
    Fixture cancelledRefusal;auto delayed=std::make_shared<ClientState>();Blocker returned;
    delayed->retryOk=false;delayed->retryAction=[&]{returned.block();};cancelledRefusal.transports.next.push_back(delayed);
    cancelledRefusal.owner.start(cancelledRefusal.request(ProviderId::FogHub),{});delayed->error="retry";delayed->retryable=true;
    cancelledRefusal.before();const bool returnedEntered=returned.wait();cancelledRefusal.owner.cancel();returned.release();cancelledRefusal.joined();
    assert(returnedEntered&&delayed->closes==0&&delayed->destroyed==0&&cancelledRefusal.owner.inputSink());
    Player::Admission busyStop;busyStop.stopRequested=true;busyStop.serviceBusy=true;cancelledRefusal.before(busyStop);
    assert(delayed->closes==0&&delayed->destroyed==0);cancelledRefusal.owner.closeAfterStopDispatch();cancelledRefusal.joined();
    assert(delayed->closes==1&&delayed->destroyed==1);
}
static void qualityCoalescing() {
    Fixture f;f.owner.start(f.request(ProviderId::FogHub),{});auto old=f.client();
    auto preferences=f.request().video;preferences.bitrateMbps+=1;
    assert(f.owner.changeVideo(preferences,{},f.now)==Player::QualityResult::Inspection);
    const auto needed=inspection(f,Player::Purpose::Quality);assert(old->retries==0&&old->destroyed==0);
    preferences.bitrateMbps+=2;preferences.framesPerSecond=30;
    assert(f.owner.changeVideo(preferences,{},f.now)==Player::QualityResult::Coalesced);
    f.owner.resumeVerdict({needed.purpose,Player::Verdict::Ready,needed.expected});assert(old->retries==0);
    f.before();f.joined();assert(f.transports.calls==1);f.before();
    assert(f.transports.calls==2&&f.client()->settings.bitrateMbps==preferences.bitrateMbps&&f.client()->settings.framesPerSecond==30&&f.client()->connection.resumeOnly);
    assert(f.owner.snapshot().attempts==0);
    for(const bool afterClose:{false,true}) {
        Fixture cancelled;cancelled.owner.start(cancelled.request(ProviderId::FogHub),{});
        auto p=cancelled.request().video;p.bitrateMbps++;
        cancelled.owner.changeVideo(p,{},cancelled.now);auto check=inspection(cancelled,Player::Purpose::Quality);
        cancelled.owner.resumeVerdict({check.purpose,Player::Verdict::Ready,check.expected});
        if(afterClose){cancelled.before();cancelled.joined();}
        Player::Admission stopAdmission;stopAdmission.stopRequested=true;cancelled.before(stopAdmission);
        assert(cancelled.transports.calls==1);cancelled.owner.closeAfterStopDispatch();cancelled.joined();
    }
    Fixture timed;auto state=std::make_shared<ClientState>();Blocker blocked;state->retryAction=[&]{blocked.block();};timed.transports.next.push_back(state);
    timed.owner.start(timed.request(ProviderId::FogHub),{});auto p=timed.request().video;p.bitrateMbps++;
    timed.owner.changeVideo(p,{},timed.now);auto check=inspection(timed,Player::Purpose::Quality);timed.owner.resumeVerdict({check.purpose,Player::Verdict::Ready,check.expected});
    timed.before();const bool entered=blocked.wait();timed.now+=15s;timed.before();
    assert(entered&&stop(timed)==Player::StopReason::QualityCloseDeadline&&timed.transports.calls==1);
    blocked.release();timed.joined();timed.owner.closeAfterStopDispatch();
}
static void mediaGenerationAndBudgets() {
    Fixture f;f.owner.start(f.request(),{});f.media()->videoAction=[&]{f.now+=5ms;};const auto slot=f.now;
    f.frame();f.frame();f.frame();auto before=f.before();assert(!before.inputAllowed);auto result=f.owner.media(slot);
    assert(result.prepared&&f.media()->slot==slot&&f.now>slot&&f.media()->feeds==3&&f.client()->packets.empty());
    assert(f.owner.timings().stages[3].totalUs==15000&&f.owner.snapshot().videoWorkUs==15000);
    f.client()->generation=1;assert(!f.before().inputAllowed);f.frame(1);f.owner.media(f.now);
    assert(f.media()->resets==1&&f.owner.snapshot().observedGeneration==1&&f.before().inputAllowed);
    f.client()->lossWaiting=true;assert(!f.before().inputAllowed);f.client()->lossWaiting=false;
    assert(!f.before().inputAllowed);f.frame(1);f.owner.media(f.now);assert(f.before().inputAllowed);
    f.client()->suspension.localWaiting=true;assert(!f.before().inputAllowed);
    assert(f.input.suspensions.back()==fog::stream::VideoSuspension::InputFreeze);
    f.client()->suspension={};assert(!f.before().inputAllowed);f.frame(1);f.owner.media(f.now);assert(f.before().inputAllowed);
    Fixture worker(true);worker.owner.start(worker.request(),{});worker.media()->pending=true;worker.media()->progress.decoded=1;worker.turn();
    worker.media()->workerGeneration=2;worker.media()->progress.workerGeneration=2;worker.media()->progress.waiting=true;
    assert(!worker.before().inputAllowed);worker.media()->pending=true;worker.media()->prepareAllowed=false;
    assert(!worker.owner.media(worker.now).prepared&&!worker.owner.snapshot().inputAllowed);
    worker.media()->prepareAllowed=true;worker.media()->progress.waiting=false;worker.media()->progress.workerDecodeUs=200;worker.media()->progress.workerConvertUs=100;
    worker.turn();assert(worker.before().inputAllowed&&worker.client()->observed.decoderWorkUs==300&&worker.client()->observed.videoGeneration==2);
    Fixture audio;audio.owner.start(audio.request(),{});audio.client()->audio.push_back({});audio.media()->audioFailure=true;
    audio.turn();assert(stop(audio)==Player::StopReason::MediaFailure&&audio.owner.takeFailure()&&audio.client()->closes==0);
}
static void errorsAndDeadlines() {
    Fixture f;f.owner.start(f.request(),{});f.media()->videoFailures=1;f.frame();f.turn();
    assert(f.owner.snapshot().videoRecovering&&f.owner.snapshot().decodeErrors==1&&f.media()->resets==1&&f.client()->plis==1&&!f.owner.snapshot().inputAllowed);
    f.frame();f.turn();assert(!f.owner.snapshot().videoRecovering&&f.owner.snapshot().firstPrepared);
    for(unsigned i=0;i<2;++i){f.media()->videoFailures=1;f.frame();f.turn();f.frame();f.turn();}
    f.media()->videoFailures=1;f.frame();f.turn();assert(stop(f)==Player::StopReason::MediaFailure&&f.owner.snapshot().decoderResets==3);
    Fixture timeout;timeout.owner.start(timeout.request(),{});timeout.media()->prepareFailures=1;timeout.turn();timeout.now+=10s;
    timeout.media()->pending=true;timeout.turn();assert(stop(timeout)==Player::StopReason::MediaFailure&&!timeout.owner.snapshot().firstPrepared);
    Fixture initial;initial.owner.start(initial.request(),{});initial.now+=30s;initial.turn();assert(initial.owner.snapshot().retained&&initial.client()->retries==1);
    Fixture terminal;terminal.owner.start(terminal.request(),{});terminal.client()->pumpAction=[&]{terminal.client()->recovery=::stream::RecoveryState::TerminalFailure;};
    terminal.now+=30s;terminal.turn();assert(stop(terminal)==Player::StopReason::FatalTransport&&terminal.client()->retries==0);
    Fixture busy;busy.owner.start(busy.request(),{});busy.now+=30s;Player::Admission admission;admission.serviceBusy=true;busy.turn(admission);
    assert(stop(busy)==Player::StopReason::Exhausted&&busy.client()->retries==0);
}
static void immutableDiagnostics() {
    struct Observer final : fog::stream::PlayerDiagnosticsObserver {
        std::vector<std::pair<fog::stream::PlayerDiagnosticPhase,fog::stream::PlayerDiagnosticSnapshot>> captures;
        void report(fog::stream::PlayerDiagnosticPhase phase,const fog::stream::PlayerDiagnosticSnapshot& value)noexcept override {
            captures.emplace_back(phase,value);
        }
    } observer;
    Fixture f(true);f.owner.setDiagnosticsObserver(&observer);f.owner.start(f.request(),{});
    f.client()->telemetry.sequence=11;f.before();
    const auto first=std::find_if(observer.captures.begin(),observer.captures.end(),[](const auto& value){return value.first==fog::stream::PlayerDiagnosticPhase::Protocol;});
    assert(first!=observer.captures.end());const auto captured=first->second;
    f.client()->telemetry.sequence=22;f.media()->progress.workerDecodeUs=600;f.media()->progress.workerConvertUs=200;
    f.media()->progress.workerGeneration=9;f.media()->progress.decoded=1;f.media()->pending=true;
    f.now+=5s;f.turn();
    assert(captured.transport.sequence==11&&captured.media.uploaded==0);
    const auto metrics=std::find_if(observer.captures.begin(),observer.captures.end(),[](const auto& value){return value.first==fog::stream::PlayerDiagnosticPhase::Metrics;});
    assert(metrics!=observer.captures.end()&&metrics->second.transport.sequence==22&&metrics->second.media.uploaded==1);
    assert(metrics->second.media.workerDecodeUs==600&&metrics->second.media.workerConvertUs==200&&metrics->second.media.workerGeneration==9);
    f.owner.closeAfterStopDispatch();
    assert(std::any_of(observer.captures.begin(),observer.captures.end(),[](const auto& value){return value.first==fog::stream::PlayerDiagnosticPhase::Final;}));
    assert(std::any_of(observer.captures.begin(),observer.captures.end(),[](const auto& value){return value.first==fog::stream::PlayerDiagnosticPhase::Closed;}));
    f.owner.setDiagnosticsObserver(nullptr);
}
static void inputGateEpisodes() {
    using fog::stream::InputGateReason;
    fog::stream::InputGateDiagnostics gates;
    gates.observe(InputGateReason::None,100,1,1,1,0);
    for(unsigned i=1;i<=10;++i)gates.observe(i%2?InputGateReason::Loss:InputGateReason::None,100+i*50,1,1,1,0);
    auto bounded=gates.snapshot(600,false);
    assert(bounded.count==8&&bounded.sequence==11&&bounded.overwritten==3);
    assert(bounded.transitions.front().sequence==4&&bounded.transitions.back().sequence==11);
    assert(bounded.episodes==5&&bounded.completed==5&&bounded.blockedMs==250&&bounded.activeMs==0);
    gates.observe(InputGateReason::Decoder,650,1,1,2,50);
    gates.observe(InputGateReason::Generation,700,1,1,3,100);
    gates.finish(750);
    const auto final=gates.snapshot(1000,false);
    assert(final.final&&final.reason==InputGateReason::Generation&&final.activeMs==100&&final.blockedMs==350);
    assert(final.reasonMs[static_cast<unsigned>(InputGateReason::Decoder)]==50);
    assert(final.reasonMs[static_cast<unsigned>(InputGateReason::Generation)]==50);
    assert(final.capturedAtMs==750&&final.completed==5); // Final is not a fabricated allow event.
    gates.observe(InputGateReason::None,1100,1,1,3,0);
    assert(gates.snapshot(1200,false).sequence==final.sequence);

    fog::stream::InputGateDiagnostics monotonic;
    monotonic.observe(InputGateReason::Loss,100,1,1,1,0);
    monotonic.observe(InputGateReason::Loss,90,1,1,1,0);
    assert(monotonic.snapshot(110,false).blockedMs==10);

    using fog::stream::InputPreparedGateCause;
    fog::stream::InputGateDiagnostics latched;
    latched.observe(InputGateReason::PreparedGate,200,1,1,2,0,InputPreparedGateCause::Loss,190);
    latched.observe(InputGateReason::PreparedGate,210,1,1,2,10,InputPreparedGateCause::Decoder,205);
    const auto changedCause=latched.snapshot(210,false);
    assert(changedCause.episodes==1&&changedCause.sequence==2&&changedCause.preparedGateCause==InputPreparedGateCause::Decoder);
    assert(changedCause.transitions.back().sequence==0); // Only two entries are populated.
    assert(changedCause.transitions[1].previousPreparedGateCause==InputPreparedGateCause::Loss&&changedCause.transitions[1].preparedGateCauseAtMs==205);
    latched.observe(InputGateReason::None,220,1,1,2,0,InputPreparedGateCause::Loss,190);
    const auto opened=latched.snapshot(220,false);
    assert(opened.completed==1&&opened.blockedMs==20&&opened.preparedGateCause==InputPreparedGateCause::None&&opened.preparedGateCauseAtMs==0);

    Fixture f;
    f.owner.start(f.request(),{});f.frame();f.turn();f.before();
    const auto healthy=f.owner.snapshot().inputGate;
    assert(healthy.initialized&&healthy.reason==InputGateReason::None&&!healthy.providerSuspensionSupported);
    f.client()->lossWaiting=true;f.before();
    const auto loss=f.owner.snapshot().inputGate;
    assert(loss.reason==InputGateReason::PreparedGate&&loss.preparedGateCause==InputPreparedGateCause::Loss&&!f.owner.snapshot().inputAllowed);
    assert(loss.preparedGateCauseAtMs==static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(f.now.time_since_epoch()).count()));
    f.now+=123ms;f.before();
    const auto blocked=f.owner.snapshot().inputGate;
    assert(blocked.blockedMs-loss.blockedMs==123&&blocked.activeMs==123);
    f.client()->lossWaiting=false;f.before();
    const auto stillClosed=f.owner.snapshot().inputGate;
    assert(stillClosed.reason==InputGateReason::PreparedGate&&stillClosed.preparedGateCause==InputPreparedGateCause::Loss);
    assert(stillClosed.preparedGateCauseAtMs==loss.preparedGateCauseAtMs&&!f.owner.snapshot().lossWaiting); // Cause is historical, not a live loss assertion.
    f.frame();f.turn();f.before();
    const auto resumed=f.owner.snapshot().inputGate;
    assert(resumed.reason==InputGateReason::None&&resumed.preparedGateCause==InputPreparedGateCause::None&&resumed.completed==loss.completed+1&&f.owner.snapshot().inputAllowed);
    assert(loss.reason==InputGateReason::PreparedGate&&loss.activeMs==0); // Value-owned history remains frozen.
    f.owner.closeAfterStopDispatch();
    const auto closed=f.owner.snapshot().inputGate;
    f.now+=2s;
    assert(closed.final&&closed.reason==InputGateReason::Cancelled);
    assert(f.owner.snapshot().inputGate.blockedMs==closed.blockedMs);
}
int main() {
    startAndMilestones();retainedRetryAndAdmission();fogHubRecovery();qualityCoalescing();mediaGenerationAndBudgets();errorsAndDeadlines();immutableDiagnostics();inputGateEpisodes();
    std::cout<<"PASS actual Player owner: private clients/media, single reaper, both-provider lifecycle/recovery, post-input verdicts, worker/sync generations, bounded media, distinct prepared/presented, deadlines/errors\n";
}
