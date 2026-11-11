#include "features/streaming/application/player.hpp"
#include "features/streaming/application/transport_teardown.hpp"
#include "features/streaming/domain/transport_reconnect.hpp"
#include "features/streaming/domain/video_recovery.hpp"
#include "features/streaming/domain/playback_drain.hpp"
#include "core/constants/stage1_options.hpp"
#include "core/constants/stage3_options.hpp"
#include "core/diagnostics/diagnostic_profile.hpp"
#include <algorithm>
#include <deque>
#include <utility>

namespace fog::stream {
namespace {
std::uint64_t micros(Player::Time end,Player::Time begin) {
    return end >= begin ? static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end-begin).count()) : 0;
}
std::uint64_t milliseconds(Player::Time time) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count());
}
void record(::stream::telemetry::Timing& timing,Player::Time start,Player::Time end) {
    const auto elapsed=micros(end,start);
    ++timing.calls; timing.totalUs+=elapsed; timing.maxUs=std::max(timing.maxUs,elapsed);
    if (elapsed >= 50000) ++timing.over50ms;
}
}
struct Player::Impl {
    PlayerTransportFactory& transports;
    PlayerMediaFactory& medias;
    PlayerInputEffects& input;
    Options options;
    Clock clock;
    // Constructed before the first paid start. This is the single existing
    // TransportTeardown algorithm; no second executor or retry worker exists.
    TransportTeardown<::stream::StreamingClient> teardown;
    std::unique_ptr<::stream::StreamingClient> client,retained;
    std::unique_ptr<PlayerMedia> decoder;
    PlayerMedia::Progress progress;
    TransportReconnect reconnect;
    connection::RelaySelection relay;
    VideoRecovery videoRecovery;
    Start start;
    video::Settings activeVideo;
    bool cancelled=false,shutdown=false,stopEmitted=false,cleanupAuthorized=false,firstPrepared=false,lossWaiting=false,inputFreeze=false;
    bool qualityClosing=false,mediaAdmitted=false,preparedGate=false;
    Admission admitted;
    PlayerDiagnosticsObserver* observer=nullptr;
    std::optional<InspectionNeeded> inspection;
    std::optional<ResumeVerdict> verdict;
    std::deque<Effect> effects;
    std::deque<Event> events;
    std::exception_ptr failure;
    Time started{},lastVideo{},closeStarted{},lastPump{},lastProtocol{},lastMediaLog{},lastMetricsLog{},lastVideoEvent{},lastSlowLog{};
    std::string lastStatus;
    std::uint64_t pumpGapUs=0,previousPumpUs=0,transportUs=0;
    std::uint64_t generation=0,videoWorkUs=0,decodeErrors=0,lastPumpWorkUs=0;
    ::stream::telemetry::Playback timing;
    std::uint64_t networkRetirementMissed=0,controlDiscoveryRetirementMissed=0;
    InputGateDiagnostics inputGate;
    InputPreparedGateCause preparedGateCause=InputPreparedGateCause::None;
    std::uint64_t preparedGateCauseAtMs=0;
    std::uint64_t sessionEpoch=0,transportEpoch=0;
    unsigned plannedDiagnosticBurst=0,plannedDiagnosticBase=0;
    bool diagnosticMetricsDue=false,diagnosticPeriodicDue=false,diagnosticNetworkDue=false,diagnosticProtocolDue=false;
    Impl(PlayerTransportFactory& t,PlayerMediaFactory& m,PlayerInputEffects& i,Options o,Clock c)
        : transports(t),medias(m),input(i),options(o),clock(std::move(c)) {}
    PlayerDiagnosticSnapshot diagnosticSnapshot() const {
        PlayerDiagnosticSnapshot value;value.networkRetirementMissed=networkRetirementMissed;value.controlDiscoveryRetirementMissed=controlDiscoveryRetirementMissed;value.playback=timing;value.videoWorkUs=videoWorkUs;
        value.recovering=videoRecovery.active();
        value.inputGate=inputGate.snapshot(milliseconds(clock()),client&&client->supportsInputSuspension());
        if(client) {
            value.transport=client->captureTelemetry(::stream::telemetry::Capture::All);
            value.network=client->captureNetworkTelemetry();
            value.protocol=client->captureProtocolTelemetry();
            value.status=client->status();value.error=client->error();value.controlReady=client->ready();
        }
        if(decoder) {value.media=decoder->progress();value.mediaTelemetry=decoder->captureTelemetry();}
        return value;
    }
    bool report(PlayerDiagnosticPhase phase,bool periodicDetails=false) noexcept {
        if(!observer)return true;
        try {auto value=diagnosticSnapshot();value.periodicDetails=periodicDetails;observer->report(phase,value);return true;}catch(...){observer->captureFailed(phase);return false;}
    }
    void reportNetworkEvents(::stream::StreamNetworkTelemetrySource& source,bool final,bool discard=true) noexcept {
        if(!observer)return;
        std::array<::stream::telemetry::network::NetworkEvent,32> events;
        const auto count=source.peekNetworkEvents(events.data(),events.size());
        if(count) {
            const bool accepted=observer->networkEvents(events.data(),count,final);
            for(std::size_t i=0;i<count;++i)if(!source.retireNetworkEvent(events[i].journalSequence,accepted))++networkRetirementMissed;
        }
        ::stream::telemetry::network::NetworkEvent event;
        if(final&&discard) {
            unsigned dropped=0;
            const auto budget=source.networkEventFinalDiscardBudget();
            for(unsigned attempts=0;attempts<budget&&source.peekNetworkEvent(event);++attempts) {
                if(source.retireNetworkEvent(event.journalSequence,false))++dropped;
                else ++networkRetirementMissed;
            }
            if(dropped)observer->networkEventsDiscarded(dropped);
        }
    }
    void resetDiagnosticPlan()noexcept {
        plannedDiagnosticBurst=plannedDiagnosticBase=0;
        diagnosticMetricsDue=diagnosticPeriodicDue=diagnosticNetworkDue=diagnosticProtocolDue=false;
    }
    void planDiagnostics(Time now)noexcept {
        const bool details=::fog::metrics::collects(::fog::metrics::DiagnosticClass::PeriodicDetail);
        diagnosticMetricsDue=details&&now-lastMetricsLog>=std::chrono::seconds(5);
        diagnosticPeriodicDue=details&&now-lastMediaLog>=std::chrono::seconds(stage1::sparseDiagnostics?15:5);
        // FH transfers the core ring on demand into one immutable <=32-entry
        // batch per pump. Keep the existing four-record diagnostic plan; never
        // submit an entire core burst as several simultaneous log records.
        diagnosticNetworkDue=start.rental.provider==ProviderId::FogHub||now-lastVideoEvent>=std::chrono::seconds(1);
        diagnosticProtocolDue=details&&now-lastProtocol>=std::chrono::seconds(stage1::sparseDiagnostics?15:5);
        try {
            const bool protocol=client->status()!=lastStatus||!client->error().empty()||diagnosticProtocolDue;
            plannedDiagnosticBase=unsigned(protocol)+(diagnosticMetricsDue?2u:unsigned(diagnosticPeriodicDue))+unsigned(diagnosticNetworkDue);
        }catch(...){plannedDiagnosticBase=4;}
        plannedDiagnosticBurst=plannedDiagnosticBase;
        if(client->supportsControlDiscoveryTelemetry()&&plannedDiagnosticBurst<4)++plannedDiagnosticBurst;
    }
    void reportControlDiscovery(bool final)noexcept {
        if(!observer||!client||!client->supportsControlDiscoveryTelemetry()||(!final&&plannedDiagnosticBase>=4))return;
        std::array<::stream::telemetry::network::ControlDiscoveryEvent,4> values;
        const auto count=client->peekControlDiscoveryEvents(values.data(),values.size(),final);
        if(count) {
            const bool accepted=observer->controlDiscoveryEvents(values.data(),count);
            for(std::size_t i=0;i<count;++i)if(!client->retireControlDiscoveryEvent(values[i].sequence,accepted))++controlDiscoveryRetirementMissed;
        }
        if(final) {
            unsigned dropped=0;
            // 32 buckets plus overflow can each retain one frozen and one
            // newer count: <=66 retirements after sealing, bounded by128.
            for(unsigned batches=0;batches<32;++batches) {
                const auto pending=client->peekControlDiscoveryEvents(values.data(),values.size(),true);
                if(!pending)break;
                for(std::size_t i=0;i<pending;++i){if(client->retireControlDiscoveryEvent(values[i].sequence,false))++dropped;else ++controlDiscoveryRetirementMissed;}
            }
            if(dropped)observer->controlDiscoveryDiscarded(dropped);
        }
    }
    void reportControls(bool final=false) noexcept {
        if(!observer||!client)return;
        try {
            PlayerDiagnosticSnapshot value;value.finalFlush=final;std::string event;
            if(client->supportsControlDiscoveryTelemetry())reportControlDiscovery(final);
            else for(unsigned n=0;n<40&&client->pollControlDiagnostic(event,final);++n)value.controls.push_back(std::move(event));
            if(final) {
                for(unsigned n=0;n<96&&client->pollUnmigratedNetworkEvent(event);++n)value.videoEvents.push_back(std::move(event));
                // FH still has live core producers until the reaper joins.
                // Preserve that backlog for the joined-close accounting pass.
                reportNetworkEvents(*client,true,start.rental.provider!=ProviderId::FogHub);
            } else if(diagnosticNetworkDue) {
                if(client->pollUnmigratedNetworkEvent(event))value.videoEvents.push_back(std::move(event));
                reportNetworkEvents(*client,false);lastVideoEvent=clock();
            }
            if(!value.controls.empty()||!value.videoEvents.empty())observer->report(PlayerDiagnosticPhase::Control,value);
        }catch(...){}
    }
    void event(EventKind kind,std::uint64_t value=0,bool flag=false) noexcept {
        // Events are fixed local facts, not remote payloads. Bound storage if a
        // shell temporarily stops consuming optional UI milestone notices.
        try {
            if (events.size() == 128) events.pop_front();
            events.push_back({kind,value,flag});
        } catch (...) {} // Optional UI notices cannot interrupt lifecycle cleanup.
    }
    bool allowed(Admission a) const {
        return a.active && !a.stopRequested && !a.shuttingDown && a.rentalPending && !cancelled && !shutdown && !start.rental.empty();
    }
    bool recoveryAllowed(Admission a) const { return allowed(a) && !a.serviceBusy; }
    void stop(StopReason reason) {
        cancelled=true; mediaAdmitted=false; reconnect.cancel();
        inspection.reset(); verdict.reset(); qualityClosing=false;
        effects.erase(std::remove_if(effects.begin(),effects.end(),[](const Effect& effect) {
            return std::holds_alternative<InspectionNeeded>(effect);
        }),effects.end());
        if (!stopEmitted) { effects.emplace_back(StopRequired{reason}); stopEmitted=true; }
    }
    void failed(std::exception_ptr error,StopReason reason=StopReason::MediaFailure) {
        failure=error; stop(reason);
    }
    void suspend(VideoSuspension reason) {
        preparedGate=false;
        switch(reason) {
            case VideoSuspension::Loss:preparedGateCause=InputPreparedGateCause::Loss;break;
            case VideoSuspension::Decoder:preparedGateCause=InputPreparedGateCause::Decoder;break;
            case VideoSuspension::Generation:preparedGateCause=InputPreparedGateCause::Generation;break;
            case VideoSuspension::InputFreeze:preparedGateCause=InputPreparedGateCause::ProviderFreeze;break;
            case VideoSuspension::Transport:preparedGateCause=InputPreparedGateCause::Transport;break;
        }
        preparedGateCauseAtMs=milliseconds(clock());
        input.videoSuspended(reason);
    }
    void resetVideo() {
        videoRecovery.reset(); lossWaiting=false; inputFreeze=false; firstPrepared=false;
        generation=videoWorkUs=decodeErrors=0; preparedGate=false;
        preparedGateCause=InputPreparedGateCause::None;preparedGateCauseAtMs=0;
    }
    bool startTransport(bool resumeOnly) {
        if (teardown.pending()) throw std::runtime_error("Previous stream is still stopping");
        resetVideo();resetDiagnosticPlan();++transportEpoch;
        decoder=medias.create();
        if (!decoder) throw std::runtime_error("Stream media unavailable; stopping rental");
        client=retained ? std::move(retained) : transports.create(start.rental.provider);
        if (!client) throw std::runtime_error("Stream transport unavailable; stopping rental");
        if (!client->configureController(start.controller)) throw std::runtime_error("Invalid controller configuration; stopping rental");
        input.transportStarting(start.controller,*client);
        const bool mts=start.rental.provider == ProviderId::Mts;
        if (!client->configureRelay(mts && relay.relay())) throw std::runtime_error("Invalid connection configuration; stopping rental");
        activeVideo=start.video;
        if (!mts) {
            if (!medias.supportsH265()) activeVideo.codec=::stream::VideoCodec::H264;
            activeVideo.quality=0; activeVideo.keyframeSeconds=0;
        }
        if (!client->configureVideo(activeVideo)) throw std::runtime_error("Invalid video configuration; stopping rental");
        decoder->setPresentationFrameRate(activeVideo.framesPerSecond);
        client->enableNetworkEvents(observer!=nullptr);
        const bool accepted=client->start({start.rental,resumeOnly});
        if (accepted && (!mts || options.mtsMediaWorkers))
            decoder->startWorkers(*client,activeVideo.framesPerSecond,mts ? PlayerMedia::WorkerPolicy::Mts : PlayerMedia::WorkerPolicy::FogHub);
        progress=decoder->progress();
        started=lastVideo=lastMediaLog=lastMetricsLog=lastVideoEvent=clock();lastStatus.clear();
        observeInputGate(started,progress);
        event(EventKind::TransportStarted,static_cast<std::uint64_t>(activeVideo.framesPerSecond),resumeOnly);
        return accepted;
    }
    bool closeTransport(bool final) noexcept {
        mediaAdmitted=false;
        // The owner/reaper is constructed before the shell's remaining UI
        // members. Constructor unwinding with no transport must not call input
        // hooks on shell members that have not been constructed yet.
        if (!decoder && !client) return true;
        observeInputGate(clock(),progress);
        if(final)inputGate.finish(milliseconds(clock()));
        if (decoder) { decoder->stopWorkers(); progress.workers=false; }
        input.beforeTransportClose(client.get());
        videoRecovery.reset(); lossWaiting=false; inputFreeze=false;
        bool retryAllowed=true;
        if (client) {
            reportControls(true);
            report(PlayerDiagnosticPhase::Final);
            if (start.rental.provider == ProviderId::FogHub) {
                if (!teardown.begin(client,!final)) return false;
                closeStarted=clock();
                event(EventKind::TeardownScheduled);
            }
            if (client) {
                try { if (final) client->close(); else retryAllowed=client->disconnectForRetry(); }
                catch (...) { retryAllowed=false; }
                reportControlDiscovery(true);
                reportNetworkEvents(*client,true);
                report(PlayerDiagnosticPhase::Closed);
                // A refused retry can still own an in-flight call. Hold it
                // until shell dispatches rental stop and authorizes final close.
                if (!final && !retryAllowed) return false;
                if (!final && start.rental.provider == ProviderId::Mts) retained=std::move(client);
                else client.reset();
            }
        }
        input.transportClosed();
        decoder.reset();
        event(EventKind::TransportClosed,0,final);
        return final || retryAllowed;
    }
    void close() noexcept {
        inspection.reset(); verdict.reset(); qualityClosing=false; reconnect.cancel();
        closeTransport(true);
        retained.reset();
        start.rental={}; relay.reset(); reconnect.reset();
    }
    void reportJoinedClose(::stream::StreamingClient& source,bool retry,bool allowed)noexcept {
        if(observer) {
            try {
                PlayerDiagnosticSnapshot value;value.reaperJoined=true;value.retry=retry;value.allowed=allowed;
                reportNetworkEvents(source,true);
                value.protocol=source.captureProtocolTelemetry();
                value.network=source.captureNetworkTelemetry();
                value.networkRetirementMissed=networkRetirementMissed;
                value.transport=source.captureTelemetry(::stream::telemetry::Capture::Transport);
                observer->report(PlayerDiagnosticPhase::Closed,value);
            }catch(...){observer->captureFailed(PlayerDiagnosticPhase::Closed);}
        }
    }
    void observeTeardown() {
        auto result=teardown.take();
        if (!result.client) return;
        reportJoinedClose(*result.client,result.retry,result.allowed);
        event(EventKind::TeardownJoined,0,result.allowed);
        if (!result.allowed) {
            // Keep the rejected client on the owner until stop-dispatch/close.
            // Never send it straight into another blocking final teardown.
            client=std::move(result.client);
            if (!cleanupAuthorized && !start.rental.empty()) {
                if (!cancelled && !shutdown) stop(StopReason::RetryTerminal);
                // cancel is intent only. In particular, a busy shared service
                // slot has not yet admitted stop and cannot authorize this join.
            } else { teardown.begin(client,false); closeStarted=clock(); }
        }
        // An allowed completed retry is only consumed. No restart before the
        // current frame's overlay/stop and fresh owner admission.
    }
    void retry(Admission a,Time now,StopReason refusal) {
        if (!recoveryAllowed(a) || !reconnect.retry(now)) { stop(StopReason::Exhausted); return; }
        suspend(VideoSuspension::Transport);
        if (start.rental.provider == ProviderId::Mts) {
            const bool wasRelay=relay.relay();
            if(relay.broken(now))event(EventKind::TransportBreak,relay.breaks(),!wasRelay&&relay.relay());
        }
        if (!closeTransport(false)) { stop(refusal); return; }
        event(EventKind::RecoveryScheduled,reconnect.attempts());
    }
    bool handleTransport(Admission a,Time now) {
        if (!client) return true;
        switch (client->recoveryState()) {
            case ::stream::RecoveryState::TerminalFailure: stop(StopReason::FatalTransport); return false;
            case ::stream::RecoveryState::RetryableFailure: retry(a,now,StopReason::RetryTerminal); return false;
            case ::stream::RecoveryState::Disconnected:
                if (!reconnect.recovering()) { reconnect.disconnected(now); suspend(VideoSuspension::Transport); }
                break;
            case ::stream::RecoveryState::Connected: break;
        }
        return true;
    }
    void completeTransport(Verdict prepared,Admission a,Time now) {
        const auto action=reconnect.poll(now);
        if (!recoveryAllowed(a) || client || !reconnect.connecting() || action != TransportReconnect::Action::Continue) {
            stop(StopReason::PreflightExpired); return;
        }
        if (prepared == Verdict::Retry) { retry(a,now,StopReason::RetryTerminal); return; }
        if (prepared != Verdict::Ready) { stop(StopReason::PreflightRefused); return; }
        try {
            if (startTransport(true)) return;
            if (client && client->recoveryState() == ::stream::RecoveryState::RetryableFailure) {
                retry(a,clock(),StopReason::RetryTerminal); return;
            }
        } catch (...) { failure=std::current_exception(); }
        stop(StopReason::StartFatal);
    }
    void completeQuality(Verdict prepared,Admission a,Time now) {
        if (prepared != Verdict::Ready || !allowed(a) || a.serviceBusy || !a.pairedPending || !client || !client->ready() ||
            client->disconnected() || !client->error().empty()) { stop(StopReason::QualityRefused); return; }
        qualityClosing=true;
        if (!closeTransport(false)) { stop(StopReason::QualityDisconnect); return; }
        // FH normally uses the reaper; a synchronous test/client still resumes
        // only from the same post-overlay beforeMedia turn below.
        (void)now;
    }
    bool advance(Admission a,Time now) {
        if (qualityClosing && now-closeStarted >= std::chrono::seconds(15)) { stop(StopReason::QualityCloseDeadline); return false; }
        if (teardown.pending()) {
            if (reconnect.deadlineExpired(now)) stop(StopReason::Deadline);
            return false;
        }
        if (verdict && inspection) {
            const auto value=*verdict; const auto expected=*inspection;
            verdict.reset(); inspection.reset();
            if (value.expected != start.rental || value.expected != expected.expected || value.purpose != expected.purpose) {
                stop(expected.purpose == Purpose::Quality ? StopReason::QualityRefused : StopReason::PreflightRefused); return false;
            }
            if (value.purpose == Purpose::Transport) completeTransport(value.verdict,a,now);
            else completeQuality(value.verdict,a,now);
            if (cancelled || teardown.pending()) return false;
        }
        if (qualityClosing) {
            qualityClosing=false;
            if (!allowed(a) || a.serviceBusy || !a.pairedPending) { stop(StopReason::QualityResume); return false; }
            try { if (startTransport(true)) { event(EventKind::QualityResume); return false; } }
            catch (...) { failure=std::current_exception(); }
            stop(StopReason::QualityResume); return false;
        }
        switch (reconnect.poll(now)) {
            case TransportReconnect::Action::Stop: stop(StopReason::Deadline); return false;
            case TransportReconnect::Action::CloseTransport: retry(a,now,StopReason::RetryTerminal); return false;
            case TransportReconnect::Action::StartTransport:
                if (!recoveryAllowed(a) || client || !reconnect.connecting()) { stop(StopReason::PreflightUnavailable); return false; }
                if (start.rental.provider == ProviderId::FogHub) {
                    inspection=InspectionNeeded{Purpose::Transport,start.rental}; effects.emplace_back(*inspection);
                } else completeTransport(Verdict::Ready,a,now);
                break;
            case TransportReconnect::Action::Continue: break;
        }
        return client && !cancelled;
    }
    void recover(const PlayerVideoError& error,const ::stream::Packet* packet=nullptr) {
        ++decodeErrors;
        if(observer) {
            try {
                auto value=diagnosticSnapshot();value.error=error.what();value.hasPacket=bool(packet);
                if(packet){value.rtpTimestamp=packet->timestamp;value.firstSequence=packet->firstSequence;value.lastSequence=packet->lastSequence;}
                observer->report(PlayerDiagnosticPhase::DecodeError,value);
            }catch(...){observer->captureFailed(PlayerDiagnosticPhase::DecodeError);} // Diagnostics cannot consume decoder recovery.
        }
        if (!error.recoverable()) throw error;
        if (!videoRecovery.begin(clock())) throw std::runtime_error("Video recovery deadline/reset budget exhausted; stopping rental");
        suspend(VideoSuspension::Decoder);
        client->setDecoderWaitingForKeyframe(true);
        const auto began=clock(); decoder->resetVideoDecoder();
        record(timing.stages[4],began,clock()); videoWorkUs+=micros(clock(),began);
        event(EventKind::DecoderReset,videoRecovery.recentResets(),client->requestVideoKeyframe());
    }
    void checkVideoDeadline() const {
        if (videoRecovery.expired(clock())) throw std::runtime_error("Video recovery deadline reached; stopping rental");
    }
    void audio(const ::stream::Packet& packet) {
        checkVideoDeadline(); const auto now=clock();
        const auto ms=milliseconds(now);
        timing.audioQueueWaitMaxMs=std::max(timing.audioQueueWaitMaxMs,ms >= packet.enqueueMs ? ms-packet.enqueueMs : 0);
        decoder->audio(packet); record(timing.stages[5],now,clock());
    }
    void drainAudio() {
        if (decoder->workersRunning()) return;
        const auto began=clock(); ::stream::Packet packet;
        for (unsigned n=0;n<12;++n) {
            if (n && clock()-began >= std::chrono::milliseconds(3)) break;
            if (!client->pollAudio(packet)) break;
            audio(packet);
        }
    }
    InputGateReason inputGateReason(Time now,const PlayerMedia::Progress& progress) const {
        // Preserve the admission predicate's short-circuit order. This is the
        // first blocking condition, not a claim that other reasons are absent.
        if(cancelled)return InputGateReason::Cancelled;
        if(!client||!decoder)return InputGateReason::Resources;
        if(!firstPrepared)return InputGateReason::FirstPrepared;
        if(!preparedGate)return InputGateReason::PreparedGate;
        if(client->waitingForVideoKeyframe())return InputGateReason::Assembly;
        if(decoder->waitingForVideoKeyframe())return InputGateReason::Decoder;
        if(options.inputFreezeGate&&client->inputSuspended())return InputGateReason::ProviderFreeze;
        if(progress.workers?decoder->workerVideoGeneration()!=generation:client->currentVideoGeneration()!=generation)return InputGateReason::Generation;
        if(client->disconnected()||client->recoveryState()!=::stream::RecoveryState::Connected)return InputGateReason::Transport;
        if(videoRecovery.expired(now))return InputGateReason::RecoveryDeadline;
        if(now-lastVideo>=std::chrono::seconds(10))return InputGateReason::PreparedAge;
        if(reconnect.recovering())return InputGateReason::Reconnect;
        if(videoRecovery.active())return InputGateReason::VideoRecovery;
        if(lossWaiting)return InputGateReason::Loss;
        if(!client->ready())return InputGateReason::ControlReady;
        return InputGateReason::None;
    }
    bool inputAllowed(Time now,const PlayerMedia::Progress& progress) const {return inputGateReason(now,progress)==InputGateReason::None;}
    void recordInputGate(InputGateReason reason,Time now) noexcept {
        inputGate.observe(reason,milliseconds(now),sessionEpoch,transportEpoch,generation,firstPrepared?micros(now,lastVideo)/1000:0,
            preparedGateCause,preparedGateCauseAtMs);
    }
    void observeInputGate(Time now,const PlayerMedia::Progress& progress) noexcept {
        try {recordInputGate(inputGateReason(now,progress),now);}catch(...){} // Optional evidence cannot change input or recovery.
    }
    MediaTurn media(Time refreshSlot) {
        MediaTurn result;
        decoder->checkAudioRecovery(); drainAudio();
        ::stream::Packet packet;
        const auto resetBefore=timing.stages[4].totalUs;
        const auto began=clock();
        const auto queuedBefore=client->queuedVideoPackets();
        const auto policy=start.rental.provider == ProviderId::FogHub ? PlaybackDrainPolicy::BoundedQueue :
            options.mtsCatchup ? (options.mtsCatchupAgeGuard ? PlaybackDrainPolicy::PresentationFirst : PlaybackDrainPolicy::ImmediatePresentationFirst) :
            PlaybackDrainPolicy::LegacyPresentationFirst;
        PlaybackDrainBudget budget(queuedBefore,client->queuedVideoAgeMs(),policy,activeVideo.framesPerSecond);
        if (!decoder->workersRunning() && budget.catchup()) ++timing.catchupWindows;
        while (!decoder->workersRunning()) {
            const auto yield=budget.yield(micros(clock(),began));
            if (yield != PlaybackDrainBudget::Yield::None) {
                if (yield == PlaybackDrainBudget::Yield::Video) ++timing.frameBudgetYields;
                if (yield == PlaybackDrainBudget::Yield::Time) ++timing.budgetYields;
                break;
            }
            if (!client->poll(packet)) break;
            budget.consumed(packet.kind == ::stream::Kind::Video);
            const auto dequeue=milliseconds(clock());
            auto& wait=packet.kind == ::stream::Kind::Video ? timing.videoQueueWaitMaxMs : timing.audioQueueWaitMaxMs;
            wait=std::max(wait,dequeue >= packet.enqueueMs ? dequeue-packet.enqueueMs : 0);
            checkVideoDeadline();
            if (packet.kind == ::stream::Kind::Audio) { audio(packet); continue; }
            try {
                decoder->setVideoCodec(client->selectedVideoCodec());
                if (packet.videoGeneration != generation) {
                    const auto resetAt=clock(); decoder->resetVideoDecoder();
                    record(timing.stages[4],resetAt,clock()); videoWorkUs+=micros(clock(),resetAt);
                    generation=packet.videoGeneration; suspend(VideoSuspension::Generation); event(EventKind::Generation,generation);
                }
            } catch (const PlayerVideoError& error) { recover(error,&packet); continue; }
            const auto videoAt=clock();
            try { decoder->video(packet); }
            catch (const PlayerVideoError& error) {
                record(timing.stages[3],videoAt,clock()); videoWorkUs+=micros(clock(),videoAt); recover(error,&packet); continue;
            }
            record(timing.stages[3],videoAt,clock()); videoWorkUs+=micros(clock(),videoAt); checkVideoDeadline();
        }
        drainAudio(); timing.batchVideoTotal+=budget.video();
        timing.batchVideoMax=std::max(timing.batchVideoMax,static_cast<std::uint64_t>(budget.video()));
        const auto preparing=clock();
        try { result.prepared=decoder->prepare(refreshSlot); }
        catch (const PlayerVideoError& error) { videoWorkUs+=micros(clock(),preparing); recover(error); }
        record(timing.stages[6],preparing,clock());
        const auto prepareUs=micros(clock(),preparing);
        if (result.prepared) {
            videoWorkUs+=micros(clock(),preparing); checkVideoDeadline(); lastVideo=clock();
            client->setDecoderWaitingForKeyframe(false);
            if (videoRecovery.active()) videoRecovery.decoded();
            if (reconnect.recovering() && client->ready() && !client->disconnected() && reconnect.prepared(clock()))
                event(EventKind::RecoverySucceeded,reconnect.attempts());
            if (!reconnect.recovering() && client->ready() && !client->disconnected()) relay.connected();
            lossWaiting=client->waitingForVideoKeyframe();
            const auto progress=decoder->progress();
            const bool waiting=reconnect.recovering() || videoRecovery.active() || lossWaiting || progress.waiting ||
                (options.inputFreezeGate && client->inputSuspended());
            preparedGate=!waiting;
            if(preparedGate){preparedGateCause=InputPreparedGateCause::None;preparedGateCauseAtMs=0;}
            input.videoPrepared(waiting);
            if (!firstPrepared) { firstPrepared=true; result.firstPrepared=true; event(EventKind::FirstPrepared,0,progress.hardware); }
            event(EventKind::Prepared);
        }
        record(timing.stages[0],began,clock());
        const auto measuredAt=clock();lastPumpWorkUs=micros(measuredAt,refreshSlot);
        progress=decoder->progress();
        if (start.rental.provider == ProviderId::Mts)
            client->observePlayback({progress.presented,progress.decoded,progress.superseded,videoWorkUs,decodeErrors,
                progress.audioUnderruns,progress.audioResets,reconnect.recovering() || videoRecovery.active() || lossWaiting || progress.waiting,
                progress.width,progress.height,progress.workerDecodeUs+progress.workerConvertUs,progress.workerGeneration,progress.workers});
        const auto now=clock();
        if (pumpGapUs >= 50000 || lastPumpWorkUs >= 50000) {
            ++timing.slowCycles;
            if(observer&&now-lastSlowLog>=std::chrono::seconds(1)) {
                PlayerDiagnosticSnapshot value;value.pumpGapUs=pumpGapUs;value.previousPumpUs=previousPumpUs;
                value.pumpUs=lastPumpWorkUs;value.transportUs=transportUs;value.mediaBatchUs=micros(measuredAt,began);
                value.prepareUs=prepareUs;value.resetUs=timing.stages[4].totalUs-resetBefore;
                value.videoBefore=queuedBefore;value.videoDrained=budget.video();value.drained=budget.packets();value.videoAfter=client->queuedVideoPackets();
                observer->report(PlayerDiagnosticPhase::SlowCycle,value);lastSlowLog=now;
            }
        }
        if(::fog::metrics::collects(::fog::metrics::DiagnosticClass::PeriodicDetail)) {
            const bool periodic=diagnosticPeriodicDue,metrics=diagnosticMetricsDue;
            diagnosticPeriodicDue=diagnosticMetricsDue=false;
            if(metrics) {report(PlayerDiagnosticPhase::Metrics,periodic);timing={};lastMetricsLog=now;}
            else if(periodic)report(PlayerDiagnosticPhase::Periodic);
            if(periodic)lastMediaLog=now;
        }
        checkVideoDeadline();
        return result;
    }
};

Player::Player(PlayerTransportFactory& t,PlayerMediaFactory& m,PlayerInputEffects& i,Clock c)
    : Player(t,m,i,Options{stage3::mtsMediaWorkers,stage1::inputFreezeGate,stage1::mtsCatchup,stage1::mtsCatchupAgeGuard},std::move(c)) {}
Player::Player(PlayerTransportFactory& t,PlayerMediaFactory& m,PlayerInputEffects& i,Options o,Clock c)
    : impl_(std::make_unique<Impl>(t,m,i,o,std::move(c))) {}
Player::~Player() { shutdownAfterCancellation(); }
Player::StartResult Player::start(Start value,Admission admission) {
    auto& m=*impl_;
    if (m.shutdown || m.client || m.retained || m.decoder || m.teardown.pending() || !m.start.rental.empty() ||
        value.rental.empty() || !admission.active || admission.stopRequested || admission.shuttingDown || admission.serviceBusy || !admission.rentalPending ||
        (value.rental.provider == ProviderId::FogHub && !admission.pairedPending))
        return StartResult::Refused;
    m.inputGate={};++m.sessionEpoch;m.transportEpoch=0;
    m.cancelled=false; m.stopEmitted=false; m.cleanupAuthorized=false; m.start=std::move(value);
    m.effects.clear(); m.events.clear(); m.failure={}; m.inspection.reset(); m.verdict.reset(); m.qualityClosing=false;
    m.reconnect.reset(); m.relay.reset(m.start.connection); m.timing={}; m.lastPump={}; m.lastPumpWorkUs=0;
    try {
        if (m.startTransport(false)) return StartResult::Started;
        if (m.client && m.client->retryableError()) {
            m.retry(admission,m.clock(),StopReason::RetryTerminal);
            return m.cancelled ? StartResult::StopRequired : StartResult::Recovering;
        }
        throw std::runtime_error("Signaling startup failed; stopping rental");
    } catch (...) { m.failure=std::current_exception(); }
    m.stop(StopReason::InitialStart); return StartResult::StopRequired;
}
void Player::observe(Time slot) {
    auto& m=*impl_; m.mediaAdmitted=false;m.resetDiagnosticPlan();m.observeTeardown();
    if (!m.client || m.cancelled || m.shutdown) return;
    m.pumpGapUs=0;m.previousPumpUs=m.lastPumpWorkUs;
    if (m.lastPump != Time{}) {
        const auto gap=micros(slot,m.lastPump);
        m.pumpGapUs=gap;
        m.timing.pumpGapMaxUs=std::max(m.timing.pumpGapMaxUs,gap);
        m.timing.outsideMeasuredPumpMaxUs=std::max(m.timing.outsideMeasuredPumpMaxUs,gap > m.lastPumpWorkUs ? gap-m.lastPumpWorkUs : 0);
    }
    m.lastPump=slot;
    try {
        const auto began=m.clock(); m.client->pump();
        m.planDiagnostics(m.clock());
        m.input.transportPumped(*m.client);m.reportControls();
        record(m.timing.stages[2],began,m.clock());
        m.transportUs=micros(m.clock(),began);
        if(m.observer) {
            try {
            const auto status=m.client->status();const bool changed=status!=m.lastStatus;
            const bool due=changed||!m.client->error().empty()||m.diagnosticProtocolDue;
            m.lastStatus=status;
            if(due) {
                auto value=m.diagnosticSnapshot();value.stateChanged=changed;
                m.observer->report(PlayerDiagnosticPhase::Protocol,value);m.lastProtocol=slot;
            }
            }catch(...){m.observer->captureFailed(PlayerDiagnosticPhase::Protocol);} // Pump succeeded; only observation failed.
        }
    } catch (...) { m.failed(std::current_exception()); }
}
void Player::resumeVerdict(ResumeVerdict value) {
    if (impl_->inspection && !impl_->cancelled && !impl_->shutdown) impl_->verdict=std::move(value);
}
void Player::pollTeardown() { impl_->observeTeardown(); }
void Player::inspectionDispatchFailed() { if (impl_->inspection) impl_->stop(StopReason::InspectionDispatch); }
Player::BeforeMedia Player::beforeMedia(Admission admission,Time now) {
    auto& m=*impl_; m.mediaAdmitted=false;
    if (!m.allowed(admission)) {
        if (admission.stopRequested || admission.shuttingDown) cancel();
        else if (!m.start.rental.empty() && !m.cancelled) m.stop(StopReason::Admission);
        return {};
    }
    try {
        if (!m.handleTransport(admission,now) || !m.advance(admission,m.clock()) || !m.decoder) return {};
        const bool waiting=m.client->waitingForVideoKeyframe();
        if (waiting && !m.lossWaiting) { m.suspend(VideoSuspension::Loss); m.event(EventKind::Loss); }
        m.lossWaiting=waiting;
        const auto freeze=m.options.inputFreezeGate ? m.client->inputSuspension() : ::stream::InputSuspension{};
        if (freeze.suspended() && !m.inputFreeze) { m.suspend(VideoSuspension::InputFreeze); m.event(EventKind::InputFreeze); }
        m.inputFreeze=freeze.suspended(); m.checkVideoDeadline();
        if (m.decoder->workersRunning()) {
            PlayerMedia::WorkerError error;
            if (m.decoder->takeWorkerError(error) && error.error) {
                try { std::rethrow_exception(error.error); } catch (const PlayerVideoError& failure) { m.recover(failure,error.hasPacket?&error.packet:nullptr); }
            }
            const auto generation=m.decoder->workerVideoGeneration();
            if (generation && generation != m.generation) {
                m.generation=generation; m.suspend(VideoSuspension::Generation); m.event(EventKind::Generation,generation);
            }
        }
        m.mediaAdmitted=true;
        m.admitted=admission;
        m.progress=m.decoder->progress();
        const auto gateAt=m.clock();
        const auto reason=m.inputGateReason(gateAt,m.progress);
        m.recordInputGate(reason,gateAt);
        return {true,reason==InputGateReason::None};
    } catch (...) { m.failed(std::current_exception()); return {}; }
}
Player::MediaTurn Player::media(Time slot) {
    auto& m=*impl_;
    if (!m.mediaAdmitted || m.cancelled || !m.client || !m.decoder) return {};
    m.mediaAdmitted=false;
    try {
        auto result=m.media(slot);
        const auto now=m.clock();
        if (!m.reconnect.recovering() && (!m.firstPrepared || !m.client->ready()) && now-m.started >= std::chrono::seconds(30))
            m.retry(m.admitted,now,StopReason::RetryTerminal);
        else if (!m.reconnect.recovering() && !m.videoRecovery.active() && now-m.lastVideo >= std::chrono::seconds(m.firstPrepared ? 10 : 30))
            m.retry(m.admitted,now,StopReason::RetryTerminal);
        return result;
    } catch (...) { m.failed(std::current_exception()); return {}; }
}
void Player::presentationCompleted(Time now) {
    // The Media endpoint counted the completed draw/present itself. Reading its
    // current value never manufactures a prepared/uploaded generation.
    if (impl_->decoder) impl_->progress.presented=impl_->decoder->presentedFrames();
    (void)now;
}
Player::QualityResult Player::changeVideo(video::Settings settings,Admission a,Time now) {
    auto& m=*impl_;
    const auto previous=m.start.video; m.start.video=settings;
    if (m.start.rental.empty()) return QualityResult::Saved;
    if (m.start.rental.provider == ProviderId::Mts) {
        auto active=settings,old=previous; active.framesPerSecond=old.framesPerSecond=m.activeVideo.framesPerSecond;
        if (active == old) return QualityResult::Saved;
        if (m.client && m.client->updateVideoSettings(active)) { m.activeVideo=active; return QualityResult::Applied; }
        return QualityResult::Saved;
    }
    if (previous.codec == settings.codec && previous.bitrateMbps == settings.bitrateMbps) return QualityResult::Saved;
    if (m.reconnect.recovering() || (m.inspection && m.inspection->purpose == Purpose::Transport)) return QualityResult::Connecting;
    if (m.inspection || m.qualityClosing) return QualityResult::Coalesced;
    if (a.serviceBusy) return QualityResult::Busy;
    if (!m.allowed(a) || !a.pairedPending || !m.client || m.client->disconnected() || !m.client->error().empty()) {
        m.stop(StopReason::QualityUnavailable); return QualityResult::StopRequired;
    }
    if (!m.client->ready()) return QualityResult::Connecting;
    m.suspend(VideoSuspension::Transport);
    m.inspection=InspectionNeeded{Purpose::Quality,m.start.rental}; m.effects.emplace_back(*m.inspection);
    (void)now;
    return QualityResult::Inspection;
}
void Player::cancel() noexcept {
    auto& m=*impl_; m.cancelled=true; m.mediaAdmitted=false; m.reconnect.cancel(); m.inspection.reset(); m.verdict.reset(); m.qualityClosing=false;
    m.effects.erase(std::remove_if(m.effects.begin(),m.effects.end(),[](const Effect& effect) {
        return std::holds_alternative<InspectionNeeded>(effect);
    }),m.effects.end());
}
void Player::closeAfterStopDispatch() noexcept { impl_->cleanupAuthorized=true; cancel(); impl_->close(); }
void Player::closeResolvedRental() noexcept { impl_->cleanupAuthorized=true; cancel(); impl_->close(); }
void Player::shutdownAfterCancellation() noexcept {
    if (!impl_ || impl_->shutdown) return;
    impl_->shutdown=true; impl_->cleanupAuthorized=true; cancel(); impl_->close();
    auto joined=impl_->teardown.finishRetained();
    if(joined.client) {
        impl_->reportJoinedClose(*joined.client,joined.retry,joined.allowed);
        impl_->event(EventKind::TeardownJoined,0,joined.allowed);
    }
}
Player::Snapshot Player::snapshot() const {
    const auto& m=*impl_; Snapshot result;
    result.provider=m.start.rental.provider; result.ownsRental=!m.start.rental.empty();
    result.transport=bool(m.client); result.media=bool(m.decoder); result.teardown=m.teardown.pending(); result.retained=bool(m.retained);
    result.cancelled=m.cancelled; result.shutdown=m.shutdown; result.recovering=m.reconnect.recovering(); result.connecting=m.reconnect.connecting();
    result.videoRecovering=m.videoRecovery.active(); result.firstPrepared=m.firstPrepared; result.lossWaiting=m.lossWaiting; result.inputFreeze=m.inputFreeze;
    result.inspection=m.inspection.has_value(); result.qualityClosing=m.qualityClosing; result.attempts=m.reconnect.attempts(); result.decoderResets=m.videoRecovery.recentResets();
    result.relay=m.relay.relay();result.mtsMediaWorkers=m.options.mtsMediaWorkers;
    result.observedGeneration=m.generation; result.lastVideoAgeMs=micros(m.clock(),m.lastVideo)/1000;result.transportAgeMs=micros(m.clock(),m.started)/1000;
    result.videoWorkUs=m.videoWorkUs; result.decodeErrors=m.decodeErrors; result.framesPerSecond=m.activeVideo.framesPerSecond;
    result.controller=m.start.controller; result.activeVideo=m.activeVideo;
    if (m.decoder) { result.progress=m.progress; result.progress.waiting=m.decoder->waitingForVideoKeyframe(); }
    if (m.client) { result.ready=m.client->ready(); result.disconnected=m.client->disconnected(); result.connected=m.client->recoveryState()==::stream::RecoveryState::Connected; result.suspension=m.client->inputSuspension(); }
    result.inputAllowed=m.inputAllowed(m.clock(),result.progress);
    result.inputGate=m.inputGate.snapshot(milliseconds(m.clock()),m.client&&m.client->supportsInputSuspension());
    return result;
}
::stream::telemetry::Playback Player::timings(bool reset) { auto result=impl_->timing; if (reset) impl_->timing={}; return result; }
::stream::telemetry::Playback Player::timingsSnapshot() const { return impl_->timing; }
std::optional<Player::Effect> Player::takeEffect() {
    if (impl_->effects.empty()) return {};
    auto value=std::move(impl_->effects.front()); impl_->effects.pop_front(); return value;
}
std::optional<Player::Event> Player::takeEvent() {
    if (impl_->events.empty()) return {};
    auto value=impl_->events.front(); impl_->events.pop_front(); return value;
}
std::exception_ptr Player::takeFailure() { auto error=impl_->failure; impl_->failure={}; return error; }
void Player::setDiagnosticsObserver(PlayerDiagnosticsObserver* observer) noexcept { impl_->observer=observer; }
std::string Player::qualityStatus() const { return impl_->client?impl_->client->qualityStatus():std::string{}; }
bool Player::periodicDiagnosticsHaveRoom() const noexcept {return impl_->plannedDiagnosticBurst<4;}
std::string Player::inputDiagnostics() const { return impl_->client?impl_->client->inputDiagnostics():std::string{}; }
bool Player::requestVideoKeyframe() { return impl_->client&&impl_->client->requestVideoKeyframe(); }
::stream::StreamInputSink* Player::inputSink() noexcept { return impl_->client.get(); }
const ::stream::StreamTelemetrySource* Player::telemetrySource() const noexcept { return impl_->client.get(); }
MediaPresentation* Player::presentation() noexcept { return impl_->decoder.get(); }
const PlayerMedia* Player::mediaView() const noexcept { return impl_->decoder.get(); }
const char* Player::stopReasonName(StopReason value) noexcept {
    switch (value) {
        case StopReason::Admission: return "playback-admission-refused";
        case StopReason::InitialStart: return "initial-start-fatal";
        case StopReason::FatalTransport: return "transport-fatal";
        case StopReason::Exhausted: return "transport-reconnect-exhausted";
        case StopReason::RetryTerminal: return "transport-retry-terminal";
        case StopReason::Deadline: return "transport-reconnect-deadline";
        case StopReason::PreflightUnavailable: return "transport-reconnect-unavailable";
        case StopReason::PreflightExpired: return "transport-reconnect-preflight-expired";
        case StopReason::PreflightRefused: return "transport-preflight-refused";
        case StopReason::StartFatal: return "transport-start-fatal";
        case StopReason::InspectionDispatch: return "reconnect-inspection-dispatch-failed";
        case StopReason::QualityUnavailable: return "paired-quality-reconnect-unavailable";
        case StopReason::QualityRefused: return "paired-quality-verification-refused";
        case StopReason::QualityDisconnect: return "paired-quality-disconnect-refused";
        case StopReason::QualityCloseDeadline: return "paired-quality-close-deadline";
        case StopReason::QualityResume: return "paired-quality-resume-failed";
        case StopReason::MediaFailure: return "media-failure";
    }
    return "player-stop";
}
}
