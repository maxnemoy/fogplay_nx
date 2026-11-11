#include "app/stream_telemetry_logging.hpp"
#include "app/final_output_telemetry.hpp"
#include "features/streaming/application/ui_telemetry_logging.hpp"
#include "core/diagnostics/thread_observation.hpp"
#include "core/constants/ui_text.hpp"
#include "app/application_controller.hpp"
#include "features/streaming/domain/stick_deadzone.hpp"
#include "core/constants/stage1_options.hpp"
#include "features/playstation/domain/controller.hpp"

namespace uiText = ::fog::constants::ui;
namespace fogplay {
using Player = fog::stream::Player;

stream::telemetry::Playback ApplicationController::playbackSnapshot(stream::telemetry::Playback value,bool takeLogTiming) const {
    const auto enqueue = takeLogTiming ? takeDiagnosticTiming() : diagnosticTimingSnapshot();
    value.stages[1] = {presentTiming_.calls,presentTiming_.totalUs,presentTiming_.maxUs,0};
    value.stages[7] = {enqueue.calls,enqueue.totalUs,enqueue.maxUs,0};
    const auto log = runLog.stats();
    value.logQueued=log.queuedLines; value.logDropped=log.droppedLines; value.logWritten=log.writtenLines;
    value.logBatches=log.batches; value.logBatchLastUs=log.lastBatchUs; value.logBatchMaxUs=log.maxBatchUs;
    return value;
}
std::string ApplicationController::playbackTimings(bool takeLogTiming) const {
    return stream::telemetry::playbackText(playbackSnapshot(player_.timingsSnapshot(),takeLogTiming));
}
std::unique_ptr<stream::StreamingClient> ApplicationController::create(fog::ProviderId provider) {
    auto* services = providerServices(provider);
    if (!services) throw std::runtime_error("Stream provider unavailable; stopping rental");
    return services->stream();
}
Player::Admission ApplicationController::playerAdmission() const {
    return {screen_ == Screen::Streaming && appletGetFocusState() == AppletFocusState_InFocus,
        rentalSession_.requested(),shutdownDone_ || exitAfterStop_,pending_.valid(),
        rentalSession_.state().pending,rentalSession_.state().pairedPending};
}
void ApplicationController::transportStarting(stream::ControllerProfile profile,stream::StreamInputSink&) {
    mouseButtons_.reset(); keyReleases_.reset(); gamepadSchedule_.reset(); loopTiming_.resetPresentationCounter();
    if (profile == stream::ControllerProfile::DualSense) {
        psSensors_.start(); psTouchTrace_ = {}; psNativeInput_ = {}; psTouchLastSubmitted_ = false;
        psLogAtMs_ = psGamepadAccepted_ = psGamepadRejected_ = 0;
        psGyroPeak_ = psAccelPeak_ = 0; psMotionMovedFrames_ = 0;
        diagnostic("PS touch reset: pending both contacts UP for new transport");
    }
}
void ApplicationController::transportPumped(stream::StreamInputSink&) {
    retryMouseReleases(); retryKeyReleases(); pumpTypedText(); retryGamepadRelease();
    if (player_.snapshot().provider == fog::ProviderId::FogHub) overlay_.setVideoChangesEnabled(player_.snapshot().ready);
}
void ApplicationController::videoSuspended(fog::stream::VideoSuspension reason) {
    if (reason != fog::stream::VideoSuspension::InputFreeze) loopTiming_.suspendPresentation();
    suspendVideoInput();
}
void ApplicationController::videoPrepared(bool waiting) { streamInputGate_.videoPrepared(waiting); }
void ApplicationController::beforeTransportClose(stream::StreamInputSink*) noexcept {
    stopRumble();
    if (streamPlayStation_) {
        try { releasePlayStationInput(); } catch (...) {}
        psSensors_.stop(); try { logPlayStationInput(true); } catch (...) {}
    }
    releaseMouse(); retryKeyReleases(); typedText_.clear(); touchWasDown_ = false;
    loopTiming_.suspendPresentation();
    inputSuspensionStats_.observe({},static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()));
    inputFreezeWaiting_ = false;
}
void ApplicationController::transportClosed() noexcept {
    emitPendingFinalPsTelemetry();
    mouseButtons_.reset(); keyReleases_.reset(); gamepadSchedule_.reset();
}
void ApplicationController::closeStream() noexcept {
    player_.closeAfterStopDispatch();
    artwork_.setPaused(false); steamArtwork_.setPaused(false); streamPlayStation_ = false;
    recentLaunch_.close(); overlay_.close(); overlayNotice_.clear(); streamInputGate_.reset();
}
void ApplicationController::pollTransportTeardown() {
    player_.pollTeardown(); applyPlayerEvents(); applyPlayerEffects();
}
void ApplicationController::applyPlayerEffects() {
    while (const auto effect = player_.takeEffect()) {
        if (const auto* stop = std::get_if<Player::StopRequired>(&*effect)) {
            rentalSession_.request(); beginFinish(Player::stopReasonName(stop->reason));
        } else {
            const auto& inspection = std::get<Player::InspectionNeeded>(*effect);
            bool admitted = false;
            try {
                admitted = inspection.purpose == Player::Purpose::Transport ?
                    startRental(fog::RentalSessionCoordinator::InspectTransport{inspection.expected}) :
                    startRental(fog::RentalSessionCoordinator::InspectQuality{inspection.expected});
            } catch (...) { admitted = false; }
            if (!admitted) player_.inspectionDispatchFailed();
        }
    }
    if (const auto error = player_.takeFailure()) {
        try { std::rethrow_exception(error); } catch (const std::exception& value) {
            status_=safeText(value.what()); diagnostic(status_);
            if(screen_==Screen::Checkout)checkoutStatus_=status_;
        }
        catch (...) { diagnostic("Player failure; details unavailable."); }
    }
}
bool ApplicationController::completePlayerInspection(const fog::RentalSessionCoordinator::Effects& effects) {
    const bool inspectionResult = effects.operation == fog::RentalSessionCoordinator::Operation::TransportInspection ||
        effects.operation == fog::RentalSessionCoordinator::Operation::QualityInspection;
    if (!player_.snapshot().inspection && !inspectionResult) return false;
    if (!rentalSession_.state().pairedPending) { player_.closeResolvedRental(); closeStream(); return false; }
    // The outer loop deliberately skips playback turns after stop/focus intent.
    // A joined inspection must therefore dispatch that queued stop here; only
    // restart decisions are deferred to the post-overlay beforeMedia phase.
    if (rentalSession_.requested() || exitAfterStop_ || shutdownDone_ || appletGetFocusState() != AppletFocusState_InFocus) {
        beginFinish("inspection-stop-queued"); return true;
    }
    if (!player_.snapshot().inspection) return false;
    const auto purpose = effects.operation == fog::RentalSessionCoordinator::Operation::TransportInspection ?
        Player::Purpose::Transport : Player::Purpose::Quality;
    Player::ResumeVerdict verdict{purpose,Player::Verdict::Refused,{}};
    if (effects.resume) {
        verdict.expected=effects.resume->expected;
        verdict.purpose=effects.resume->purpose == fog::RentalSessionCoordinator::ResumePurpose::Transport ? Player::Purpose::Transport : Player::Purpose::Quality;
        verdict.verdict=effects.resume->status == fog::RentalSessionCoordinator::ResumeStatus::Ready ? Player::Verdict::Ready :
            effects.resume->status == fog::RentalSessionCoordinator::ResumeStatus::Retry ? Player::Verdict::Retry : Player::Verdict::Refused;
    }
    player_.resumeVerdict(std::move(verdict)); // Store only. Overlay stop runs before beforeMedia.
    return true;
}
void ApplicationController::applyPlayerEvents() {
    while (const auto event = player_.takeEvent()) {
        switch (event->kind) {
            case Player::EventKind::TransportStarted: {
                const auto playback = player_.snapshot();
                overlay_.setProvider(playback.provider); overlay_.setVideoChangesEnabled(playback.provider == fog::ProviderId::Mts);
                overlay_.setVideoSettings(playback.activeVideo); overlay_.setSaveStatus(videoSaveStatus_);
                diagnostic(std::string("Controller profile: ")+(playback.controller==stream::ControllerProfile::DualSense?"dualsense; PlayStation touch/motion enabled":"xbox")+
                    "; provider="+(playback.provider==fog::ProviderId::Mts?"mts":"moonlight"));
                diagnostic(playback.provider==fog::ProviderId::Mts?std::string("Transport ICE policy: ")+(playback.relay?"relay":"all"):"Moonlight direct transport; RTC relay policy unavailable");
                if(playback.progress.workers)diagnostic(std::string(playback.provider==fog::ProviderId::Mts?"MTS":"FH")+" media workers started: independent video/audio; paced presentation fps="+std::to_string(playback.framesPerSecond));
                if(playback.provider==fog::ProviderId::Mts)diagnostic("Stage3 options mts_media_workers="+std::to_string(playback.mtsMediaWorkers));
                break;
            }
            case Player::EventKind::FirstPrepared: diagnostic(event->flag ? "First video frame prepared (hardware)." : "First video frame prepared (software)."); break;
            case Player::EventKind::Prepared: rememberDecodedGame(); break;
            case Player::EventKind::RecoverySucceeded: status_=uiText::Stream::RestoredMenuHint; diagnostic("Transport recovery succeeded; attemptsIn120s="+std::to_string(event->value)); break;
            case Player::EventKind::RecoveryScheduled: status_=uiText::Stream::ReconnectStatus; diagnostic("Transport recovery scheduled; attemptsIn120s="+std::to_string(event->value)+" limit=3 rollingWindow=120s recoveryDeadline=90s"); break;
            case Player::EventKind::QualityResume: status_=uiText::Settings::FogHubReconnectStatus; diagnostic("FH quality reconnect: verified same-rental resume requested."); break;
            case Player::EventKind::DecoderReset: diagnostic("Video recovery begin; reset="+std::to_string(event->value)+" limit=3/60s deadline=10s keyframeRequested="+std::to_string(event->flag)); break;
            case Player::EventKind::Generation: diagnostic("Video chain resync: generation="+std::to_string(event->value)); break;
            case Player::EventKind::TeardownScheduled: diagnostic("Moonlight teardown scheduled; UI remains active."); break;
            case Player::EventKind::TransportBreak:
                diagnostic("Transport break recorded; recentBreaks="+std::to_string(event->value));
                if(event->flag)diagnostic("Auto TURN selected after two breaks in 120s; retained until rental closes.");
                break;
            default: break;
        }
    }
}
void ApplicationController::pumpStream(PadState& pad,u64 held,u64 down) {
    samplePlayStationInput(pad);
    const auto refreshSlot=std::chrono::steady_clock::now();
    player_.observe(refreshSlot);
    logPlayStationInput(false);
    bool overlayHandled=false;
    if (screen_ == Screen::Streaming) {
        overlayHandled=handleStreamOverlay(pad,held,down);
        if (screen_ != Screen::Streaming || rentalSession_.requested()) { player_.cancel(); return; }
    }
    const auto before=player_.beforeMedia(playerAdmission(),std::chrono::steady_clock::now());
    applyPlayerEvents(); applyPlayerEffects();
    if (!before.media || rentalSession_.requested()) return;
    const auto playback=player_.snapshot();
    inputSuspensionStats_.observe(fog::stage1::inputFreezeGate ? playback.suspension : stream::InputSuspension{},
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()));
    inputFreezeWaiting_=fog::stage1::inputFreezeGate && playback.suspension.suspended();
    if (fog::stage1::earlyInput) sendStreamInput(pad,held,overlayHandled);
    player_.media(refreshSlot);
    applyPlayerEvents(); applyPlayerEffects();
    if (!fog::stage1::earlyInput) sendStreamInput(pad,held,overlayHandled);
}

void ApplicationController::emitPendingFinalPsTelemetry() noexcept {
    if(!pendingFinalPsTelemetry_)return;
    const auto value=*pendingFinalPsTelemetry_;pendingFinalPsTelemetry_.reset();
    fog::playstation::telemetry::enqueue(runLog,value);
}
void ApplicationController::logOutputTimings(bool final) {
    const auto now=std::chrono::steady_clock::now();
    const auto elapsed=diagnostics::diagnosticElapsedMs();
    namespace t=stream::telemetry::ui;
    if(final) {
        const auto window=t::captureLoop(loopTiming_,t::LoopScope::FinalWindow,now,loopTimingWindowAt_,elapsed);
        const auto lifetime=t::captureLoop(loopTiming_,t::LoopScope::FinalLifetime,now,loopTimingLifetimeAt_,elapsed);
        auto ps=std::move(pendingFinalPsTelemetry_);pendingFinalPsTelemetry_.reset();
        diagnostics::enqueueFinalOutput(runLog,window,lifetime,std::move(ps));
    } else {
        const auto loop=t::captureLoop(loopTiming_,t::LoopScope::Window,now,loopTimingWindowAt_,elapsed);
        loopTiming_.clearWindow();loopTimingWindowAt_=now;
        const auto threads=fog::diagnostics::captureThreadObservations();
        if(!threadObservationsCaptured_||threads!=lastThreadObservations_) {
            lastThreadObservations_=threads;threadObservationsCaptured_=true;nextThreadChunk_=0;
            lastThreadCapturedElapsedMs_=elapsed;lastThreadCapturedAt_=now;
        }
        std::optional<t::ThreadChunk> chunk;
        if(nextThreadChunk_<t::threadChunkCount(lastThreadObservations_)) {
            chunk=t::captureThreadChunk(lastThreadObservations_,nextThreadChunk_,lastThreadCapturedElapsedMs_,lastThreadCapturedAt_);
            // Process-global observations may span frames/sessions. Never retry
            // an uncertain admission; every chunk owns the same frozen registry.
            ++nextThreadChunk_;
        }
        diagnostics::diagnosticOutput(loop,std::move(chunk));
    }
}
void ApplicationController::startStream(const fog::RentalId& rental, bool createdFromCatalog,
                                      const fog::EntityId& computer) {
    emitPendingFinalPsTelemetry();
    if (fog::stage1::pauseArtwork) {
        artwork_.setPaused(true);
        steamArtwork_.setPaused(true);
    }
    streamPlayStation_ = (rental.provider == fog::ProviderId::Mts && fog::isPlayStationComputerId(computer)) ||
        controllerSettings_.profile == ::stream::ControllerProfile::DualSense;
    latency_.cancel();
    recentLaunch_.stream(createdFromCatalog);
    loopTiming_.reset(); loopTimingWindowAt_ = loopTimingLifetimeAt_ = std::chrono::steady_clock::now();
    inputSuspensionStats_ = {}; inputFreezeWaiting_ = false;
    presentTiming_ = {}; resetDiagnosticTiming();
    ++sessionSequence_;
    diagnostic("Stream session=" + std::to_string(sessionSequence_) +
        (createdFromCatalog ? " origin=catalog catalogGameId=" + safeText(selectedGameId_.value) +
         " catalogGame=" + safeText(selectedGameName_) : " origin=existing-rental catalogGame=unknown"));
    screen_ = Screen::Streaming;
    overlay_.close(); streamInputGate_.reset(); streamInputGate_.overlayTransition(); touchWasDown_ = touchDrag_ = false;
    pointerX_ = 640.0f; pointerY_ = 360.0f;
    mouseAt_ = std::chrono::steady_clock::now();
    overlayNotice_.clear();
    try {
        diagnostic("Stream setup begins; preparing video/audio and signaling.");
        diagnostic(std::string("Video preferences: codec=") + ::stream::videoCodecName(videoSettings_.codec) +
            " capMbps=" + std::to_string(videoSettings_.bitrateMbps) + " quality=" + std::to_string(videoSettings_.quality) +
            " fps=" + std::to_string(videoSettings_.framesPerSecond) +
            " pliSeconds=" + std::to_string(videoSettings_.keyframeSeconds));
        armExitHandling();
        fog::stream::Player::Start playback;
        playback.rental = rental; playback.video = videoSettings_; playback.connection = connectionSettings_;
        playback.controller = streamPlayStation_ ? stream::ControllerProfile::DualSense : stream::ControllerProfile::Xbox;
        const auto result = player_.start(std::move(playback), playerAdmission());
        applyPlayerEvents(); applyPlayerEffects();
        if (result == fog::stream::Player::StartResult::Refused) { beginFinish("player-start-refused"); return; }
        if (result != fog::stream::Player::StartResult::Started) return;
        status_ = uiText::Stream::ConnectingMenuHint;
        diagnostic("Stream connection requested; recovery journal saved.");
    } catch (...) { beginFinish(); throw; }
}
void ApplicationController::sendStreamInput(PadState& pad, u64 held, bool overlayHandled) {
    const auto playback = player_.snapshot();
    auto* sink = player_.inputSink();
    if (screen_ != Screen::Streaming || rentalSession_.requested() || exitAfterStop_ || !sink || !playback.media) return;
    const auto now = std::chrono::steady_clock::now();
    const bool inputAllowed = playback.inputAllowed && streamInputGate_.gamepadAllowed(overlayHandled);
    dispatchPlayStationInput(inputAllowed);
    if (!inputAllowed || (streamPlayStation_ && !psNativeInput_.contactsInitialized())) return;
    const auto inputNowMs=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
    const bool psTouching = streamPlayStation_ && psNativeInput_.click(inputNowMs,psTouchLastSubmitted_);
    if (!playback.inputAllowed) return;
    stream::Gamepad gamepad;
    const std::array<u64,16> buttons{HidNpadButton_B,HidNpadButton_A,HidNpadButton_Y,HidNpadButton_X,
        HidNpadButton_L,HidNpadButton_R,HidNpadButton_ZL,HidNpadButton_ZR,HidNpadButton_Minus,HidNpadButton_Plus,
        HidNpadButton_StickL,HidNpadButton_StickR,HidNpadButton_Up,HidNpadButton_Down,HidNpadButton_Left,HidNpadButton_Right};
    const auto remoteHeld = streamPlayStation_ ? held : fog::controller::mapStreamButtons(held,controllerSettings_,faceMasks);
    for (std::size_t i=0; i<buttons.size(); ++i) gamepad.buttons[i] = (remoteHeld & buttons[i]) ? 1 : 0;
    if (streamPlayStation_) {
        fog::playstation::applyExtraButtons(gamepad.buttons,psTouching);
    }
    const auto left = padGetStickPos(&pad,0), right = padGetStickPos(&pad,1);
    const auto leftAxes = fog::stage1::radialDeadzone ? fog::stream::radialStick(left.x,left.y) : fog::stream::cursorStick(left.x,left.y);
    const auto rightAxes = fog::stage1::radialDeadzone ? fog::stream::radialStick(right.x,right.y) : fog::stream::cursorStick(right.x,right.y);
    gamepad.axes = {leftAxes.x,-leftAxes.y,rightAxes.x,-rightAxes.y};
    gamepad.leftTrigger = (held & HidNpadButton_ZL) ? 1.0f : 0.0f;
    gamepad.rightTrigger = (held & HidNpadButton_ZR) ? 1.0f : 0.0f;
    if (!gamepadSchedule_.due(gamepad, now)) return;
    gamepad.sampledAtUs = padSampledAtUs_;
    const bool accepted = sink->sendGamepad(gamepad);
    if (accepted) gamepadSchedule_.accepted(gamepad, now);
    if (streamPlayStation_) {
        if (accepted) { ++psGamepadAccepted_; psTouchLastSubmitted_ = psTouching; }
        else ++psGamepadRejected_;
    }
}

bool ApplicationController::networkEvents(const stream::telemetry::network::NetworkEvent* events,std::size_t count,bool final) noexcept {
    return diagnostics::diagnosticNetworkEvents(diagnostics::diagnosticElapsedMs(),events,count,final);
}
void ApplicationController::networkEventsDiscarded(unsigned count) noexcept {
    while(count--)runLog.appendDeferred({});
}
bool ApplicationController::controlDiscoveryEvents(const stream::telemetry::network::ControlDiscoveryEvent* values,std::size_t count) noexcept {
    return diagnostics::diagnosticControlDiscovery(diagnostics::diagnosticElapsedMs(),values,count);
}
void ApplicationController::controlDiscoveryDiscarded(unsigned count) noexcept {
    while(count--)runLog.appendDeferred({});
}
void ApplicationController::captureFailed(fog::stream::PlayerDiagnosticPhase) noexcept {runLog.appendDeferred({});}
void ApplicationController::report(fog::stream::PlayerDiagnosticPhase phase,const fog::stream::PlayerDiagnosticSnapshot& captured) noexcept {
    using Phase=fog::stream::PlayerDiagnosticPhase;
    try {
        const auto elapsed=diagnostics::diagnosticElapsedMs();
        if(phase==Phase::Control) {
            for(const auto& line:captured.controls)diagnostic("Stream control: "+line,2048);
            for(const auto& line:captured.videoEvents)diagnostic(std::string(captured.finalFlush?"Stream final event: ":"Video RTP event: ")+line,2048);
            return;
        }
        if(phase==Phase::Protocol) {
            if(captured.stateChanged)diagnostic("Stream stage: "+captured.status);
            if(!captured.error.empty())diagnostic("Stream error: "+safeText(captured.error));
        } else if(phase==Phase::Closed) {
            if(captured.reaperJoined)diagnostic("Moonlight teardown joined; retry="+std::to_string(captured.retry)+" allowed="+std::to_string(captured.allowed));
        } else if(phase==Phase::DecodeError) {
            std::string detail=captured.hasPacket?"Video decode error: ":"Video presentation error: ";
            detail+=safeText(captured.error,1800);
            if(captured.hasPacket)detail+=" rtpTimestamp="+std::to_string(captured.rtpTimestamp)+" rtpFirstSeq="+std::to_string(captured.firstSequence)+" rtpLastSeq="+std::to_string(captured.lastSequence);
            diagnostic(detail,2048);
        }
        const auto playback=(phase==Phase::Final||phase==Phase::Metrics)?playbackSnapshot(captured.playback,phase==Phase::Metrics):captured.playback;
        diagnostics::diagnosticPlayer(elapsed,phase,captured,playback,{inputSuspensionStats_,static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()),fog::stage1::inputFreezeGate,streamPlayStation_,(phase==Phase::Final||phase==Phase::Metrics)?rumble_.captureTelemetry():stream::telemetry::rumble::Snapshot{}});
        if(phase==Phase::Final||phase==Phase::Metrics)logOutputTimings(phase==Phase::Final);
        if(phase==Phase::Metrics)presentTiming_={};
    } catch (...) {} // Diagnostics never interrupt rental cleanup.
}

}
