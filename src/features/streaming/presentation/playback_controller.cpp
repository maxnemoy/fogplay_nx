#include "app/application_controller.hpp"
#include "features/streaming/domain/playback_drain.hpp"

namespace fogplay {

std::string ApplicationController::playbackTimings() const {
    const auto log = runLog.stats();
    return "Playback timing: " + drainTiming_.text("drain") + " " + presentTiming_.text("present") +
        " " + transportPumpTiming_.text("transportPump") + " " + videoFeedTiming_.text("videoFeed") +
        " " + decoderResetTiming_.text("decoderReset") + " " + audioFeedTiming_.text("audioFeed") + " " + prepareTiming_.text("prepare") +
        " " + diagnosticTiming.text("logEnqueue") + " pumpGapMaxUs=" + std::to_string(pumpGapMaxUs_) +
        " outsideMeasuredPumpMaxUs=" + std::to_string(outsideMeasuredPumpMaxUs_) +
        " videoQueueWaitMaxMs=" + std::to_string(videoQueueWaitMaxMs_) +
        " audioQueueWaitMaxMs=" + std::to_string(audioQueueWaitMaxMs_) +
        " slowCycles=" + std::to_string(slowPlaybackCycles_) +
        " batchVideoTotal=" + std::to_string(drainVideoTotal_) + " batchVideoMax=" + std::to_string(drainVideoMax_) +
        " frameBudgetYields=" + std::to_string(drainVideoBudgetHits_) +
        " budgetYields=" + std::to_string(drainBudgetHits_) + " catchupWindows=" + std::to_string(catchupWindows_) +
        " logQueued=" + std::to_string(log.queuedLines) + " logDropped=" + std::to_string(log.droppedLines) +
        " logWritten=" + std::to_string(log.writtenLines) + " logBatches=" + std::to_string(log.batches) +
        " logBatchLastUs=" + std::to_string(log.lastBatchUs) + " logBatchMaxUs=" + std::to_string(log.maxBatchUs);
}

void ApplicationController::logOutputTimings(bool final) {
    using Timing = fog::stream::LoopTiming;
    const auto now = std::chrono::steady_clock::now();
    const auto active = loopTiming_.activeText(now);
    // Samples belong to the window in which their scope finishes. Nested draw
    // stages overlap the outer draw/loop; these durations must not be summed.
    for (const bool lifetime : {false, true}) {
        if (lifetime && !final) break;
        const auto& snapshot = lifetime ? loopTiming_.lifetime : loopTiming_.window;
        const auto spanMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            now-(lifetime ? loopTimingLifetimeAt_ : loopTimingWindowAt_)).count();
        const std::string scope = final ? (lifetime ? "final lifetime" : "final window") : "window";
        std::string loop = "Output loop " + scope + ": spanMs=" + std::to_string(spanMs) + " " + active;
        for (const auto stage : {Timing::Stage::Loop, Timing::Stage::Events, Timing::Stage::Poll,
            Timing::Stage::Pump, Timing::Stage::Input, Timing::Stage::Draw})
            loop += " " + snapshot.at(stage).text(Timing::name(stage));
        diagnostic(loop,4096);
        std::string draw = "Output draw " + scope + ":";
        for (const auto stage : {Timing::Stage::Artwork, Timing::Stage::MediaDraw, Timing::Stage::Overlay, Timing::Stage::Present})
            draw += " " + snapshot.at(stage).text(Timing::name(stage));
        draw += " " + snapshot.newPresentGap.text("newPresentGap") +
            " newPresentAnchors=" + std::to_string(snapshot.anchors) +
            " suppressedNewFrames=" + std::to_string(snapshot.suppressedFrames) +
            " presentContinuityBreaks=" + std::to_string(snapshot.continuityBreaks);
        diagnostic(draw,4096);
    }
    if (!final) { loopTiming_.clearWindow(); loopTimingWindowAt_ = now; }
}

bool ApplicationController::closeTransport(bool final) noexcept {
    bool retryAllowed = true;
    if (final) releaseMouse();
    else mouseLeft_ = mouseRight_ = touchDrag_ = touchWasDown_ = false;
    videoRecovery_.reset();
    videoLossWaiting_ = false;
    loopTiming_.suspendPresentation();
    if (stream_) {
        try {
            std::string event;
            for (unsigned i = 0; i < 96 && stream_->pollVideoDiagnostic(event); ++i)
                diagnostic("Stream final event: " + event,2048);
            diagnostic("Video RTP final: " + stream_->videoDiagnostics(),4096);
            diagnostic("Media queue final: " + stream_->queueDiagnostics(),2048);
            diagnostic(playbackTimings(),2048);
            logOutputTimings(true);
            diagnostic("Stream transport final: " + stream_->transportTimings(),4096);
            if (media_) {
                diagnostic("Video decoder final: " + media_->diagnostics(),2048);
                diagnostic("Audio playback final: " + media_->audioDiagnostics(),2048);
            }
        } catch (...) { retryAllowed = false; } // Cleanup must continue; an uncertain retry must stop.
        try {
            if (final) stream_->close();
            else if (!stream_->disconnectForRetry()) {
                retryAllowed = false;
                diagnostic("Transport retry cancelled: " + safeText(stream_->error()));
                diagnostic("Transport final protocol: " + stream_->diagnostics(),2048);
            }
        } catch (...) { retryAllowed = false; }
        try {
            diagnostic("Stream transport closed: " + stream_->transportTimings(),4096);
            diagnostic("Video gaps final: " + stream_->gapDiagnostics(),4096);
            diagnostic("Stream DTLS final: " + stream_->dtlsDiagnostics(),8192);
            diagnostic("Stream RTCP final: " + stream_->rtcpDiagnostics(),4096);
            diagnostic("Stream mutex final: " + stream_->mutexDiagnostics(),4096);
        } catch (...) {} // Diagnostic failure cannot prevent rental termination.
        // A refused retry can still own an in-flight RTC call. Keep it alive
        // until beginFinish dispatches the rental stop, then performs final close.
        if (!final && !retryAllowed) return false;
        stream_.reset();
    }
    media_.reset();
    return final || retryAllowed;
}

void ApplicationController::closeStream() noexcept {
    transportReconnect_.cancel();
    closeTransport(true);
    streamHash_.clear();
    relaySelection_.reset();
    transportReconnect_.reset();
    recentLaunch_.close();
    overlay_.close();
    overlayNotice_.clear();
    streamInputGate_.reset();
}

bool ApplicationController::startTransport() {
    // Transport replacement must not adopt, create, or reset rental ownership.
    videoRecovery_.reset();
    videoLossWaiting_ = false; firstFramePresented_ = false;
    loopTiming_.resetPresentationCounter();
    videoWorkUs_ = videoDecodeErrors_ = lastVideoGeneration_ = 0;
    media_ = std::make_unique<fog::Media>(rt_.renderer,stream::VideoCodec::H264,true);
    stream_ = std::make_unique<stream::Client>();
    if (!stream_->configureRelay(relaySelection_.relay()))
        throw std::runtime_error("Invalid connection configuration; stopping rental");
    diagnostic(std::string("Transport ICE policy: ") + (relaySelection_.relay() ? "relay" : "all"));
    if (!stream_->configureVideo(videoSettings_))
        throw std::runtime_error("Invalid video configuration; stopping rental");
    overlay_.setVideoSettings(videoSettings_);
    overlay_.setSaveStatus(videoSaveStatus_);
    lastStreamState_.clear(); lastStreamDiagnostics_.clear();
    std::array<unsigned char,5> random{}; randomGet(random.data(),random.size());
    std::string suffix;
    for (auto value : random) suffix += "0123456789abcdefghijklmnopqrstuvwxyz"[value % 36];
    const bool started = stream_->start(streamHash_, suffix, "romfs:/cacert.pem");
    streamStarted_ = lastVideo_ = lastMediaLog_ = lastVideoEventLog_ = std::chrono::steady_clock::now();
    return started;
}

void ApplicationController::retryTransport(const char* reason) {
    const bool wasRelay = relaySelection_.relay();
    if (relaySelection_.broken(std::chrono::steady_clock::now()))
        diagnostic("Transport break recorded; recentBreaks=" + std::to_string(relaySelection_.breaks()));
    if (!wasRelay && relaySelection_.relay()) diagnostic("Auto TURN selected after two breaks in 120s; retained until rental closes.");
    if (screen_ != Screen::Streaming || !sessionPending_ || stopAfterTask_ ||
        exitAfterStop_ || streamHash_.empty() || pending_.valid() ||
        !transportReconnect_.retry(std::chrono::steady_clock::now())) {
        beginFinish("transport-reconnect-exhausted");
        return;
    }
    streamInputGate_.suspendVideo();
    if (!closeTransport(false)) {
        beginFinish("transport-retry-terminal"); return;
    }
    status_ = "Связь потеряна. Повторяем подключение к той же аренде; оплата продолжается.";
    diagnostic("Transport recovery scheduled; reason=" + std::string(reason) +
        " attemptsIn120s=" + std::to_string(transportReconnect_.attempts()) + " limit=3 rollingWindow=120s recoveryDeadline=90s");
}

void ApplicationController::startStream(const std::string& hash, bool createdFromCatalog) {
    latency_.cancel();
    recentLaunch_.stream(createdFromCatalog);
    relaySelection_.reset(connectionSettings_);
    sessionPending_ = true;
    transportReconnect_.reset();
    loopTiming_.reset(); loopTimingWindowAt_ = loopTimingLifetimeAt_ = std::chrono::steady_clock::now();
    drainTiming_ = {}; presentTiming_ = {}; diagnosticTiming = {};
    lastPumpAt_ = {}; lastProtocolLog_ = {}; pumpGapMaxUs_ = 0; outsideMeasuredPumpMaxUs_ = 0; drainBudgetHits_ = 0; catchupWindows_ = 0;
    transportPumpTiming_ = {}; videoFeedTiming_ = {}; decoderResetTiming_ = {}; audioFeedTiming_ = {}; prepareTiming_ = {};
    drainVideoTotal_ = drainVideoMax_ = drainVideoBudgetHits_ = 0;
    videoQueueWaitMaxMs_ = audioQueueWaitMaxMs_ = lastPumpWorkUs_ = slowPlaybackCycles_ = 0;
    lastSlowPlaybackLog_ = {};
    ++sessionSequence_;
    diagnostic("Stream session=" + std::to_string(sessionSequence_) +
        (createdFromCatalog ? " origin=catalog catalogGameId=" + std::to_string(selectedGameId_) +
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
            " pliSeconds=" + std::to_string(videoSettings_.keyframeSeconds));
        armExitHandling();
        streamHash_ = hash;
        if (!startTransport()) {
            if (stream_->retryableError()) { retryTransport("initial-start"); return; }
            throw std::runtime_error("Signaling startup failed; stopping rental");
        }
        status_ = "Подключение. Минус + Плюс — меню трансляции.";
        diagnostic("Stream connection requested; recovery journal saved.");
    } catch (...) { beginFinish(); throw; }
}

void ApplicationController::recoverVideoError(const fog::VideoDecodeError& error, const stream::Packet* packet) {
    loopTiming_.suspendPresentation();
    ++videoDecodeErrors_;
    std::string detail = packet ? "Video decode error: " : "Video presentation error: ";
    detail += error.what();
    if (packet) detail += " rtpTimestamp=" + std::to_string(packet->timestamp) +
        " rtpFirstSeq=" + std::to_string(packet->firstSequence) + " rtpLastSeq=" + std::to_string(packet->lastSequence);
    diagnostic(detail,2048);
    diagnostic("Video RTP at decode error: " + stream_->videoDiagnostics(),4096);
    diagnostic("Video gaps at decode error: " + stream_->gapDiagnostics(),4096);
    diagnostic("Media queue at decode error: " + stream_->queueDiagnostics(),2048);
    diagnostic("Video decoder at error: " + media_->diagnostics(),2048);
    if (!error.recoverable()) throw error;
    if (!videoRecovery_.begin(std::chrono::steady_clock::now()))
        throw std::runtime_error("Video recovery deadline/reset budget exhausted; stopping rental");
    suspendVideoInput();
    stream_->setDecoderWaitingForKeyframe(true);
    const auto resetStarted = std::chrono::steady_clock::now();
    media_->resetVideoDecoder();
    decoderResetTiming_.record(resetStarted);
    videoWorkUs_ += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now()-resetStarted).count());
    const bool requested = stream_->requestVideoKeyframe();
    diagnostic("Video recovery begin; reset=" + std::to_string(videoRecovery_.recentResets()) +
        " limit=3/60s deadline=10s keyframeRequested=" + std::to_string(requested));
}

void ApplicationController::feedStreamAudio(const stream::Packet& packet) {
    const auto now = std::chrono::steady_clock::now();
    if (videoRecovery_.expired(now))
        throw std::runtime_error("Video recovery deadline reached; stopping rental");
    const auto ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
    audioQueueWaitMaxMs_ = std::max(audioQueueWaitMaxMs_, ms >= packet.enqueueMs ? ms-packet.enqueueMs : 0);
    const auto before = media_->audioPackets();
    media_->audio(packet.data.data(), packet.data.size());
    audioFeedTiming_.record(now);
    if (!before) diagnostic("First Opus packet decoded and queued.");
}
void ApplicationController::drainStreamAudio() {
    const auto started = std::chrono::steady_clock::now();
    stream::Packet packet;
    // Independent bounded audio slice, including when video exhausts its budget.
    for (unsigned n = 0; n < 12; ++n) {
        if (n && std::chrono::steady_clock::now()-started >= std::chrono::milliseconds(3)) break;
        if (!stream_->pollAudio(packet)) break;
        feedStreamAudio(packet);
    }
}
void ApplicationController::pumpStream(PadState& pad, u64 held, u64 down) {
    // Menu/finish intent wins over a due retry, including backoff with no Client.
    bool overlayHandled = false;
    const bool wasRecovering = transportReconnect_.recovering();
    if (wasRecovering && screen_ == Screen::Streaming) {
        overlayHandled = handleStreamOverlay(pad, held, down);
        if (screen_ != Screen::Streaming || stopAfterTask_ || streamHash_.empty()) return;
    }
    const auto pumpAt = std::chrono::steady_clock::now();
    std::uint64_t pumpGapUs = 0;
    const auto previousPumpWorkUs = lastPumpWorkUs_;
    std::uint64_t transportWorkUs = 0;
    // Consume queued terminal/remote-close events before any timeout can replace
    // this transport; a local deadline must never turn them into another retry.
    if (stream_) {
        if (lastPumpAt_ != std::chrono::steady_clock::time_point{}) {
            const auto gap = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(pumpAt-lastPumpAt_).count());
            pumpGapUs = gap;
            pumpGapMaxUs_ = std::max(pumpGapMaxUs_, gap);
            outsideMeasuredPumpMaxUs_ = std::max(outsideMeasuredPumpMaxUs_, gap > previousPumpWorkUs ? gap-previousPumpWorkUs : 0);
        }
        lastPumpAt_ = pumpAt;
        const auto transportStarted = std::chrono::steady_clock::now();
        stream_->pump();
        transportPumpTiming_.record(transportStarted);
        transportWorkUs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now()-transportStarted).count());
        const auto streamState = stream_->status();
        const bool stateChanged = streamState != lastStreamState_;
        if (stateChanged) {
            lastStreamState_ = streamState;
            diagnostic("Stream stage: " + streamState);
        }
        // Build the mutex-protected snapshot only when a log is actually due.
        if (stateChanged || !stream_->error().empty() || pumpAt-lastProtocolLog_ >= std::chrono::seconds(5)) {
            const auto details = stream_->diagnostics();
            if (details != lastStreamDiagnostics_) {
                lastStreamDiagnostics_ = details;
                diagnostic("Stream protocol: " + details,2048);
            }
            diagnostic("Stream DTLS: " + stream_->dtlsDiagnostics(),8192);
            diagnostic("Stream RTCP: " + stream_->rtcpDiagnostics(),4096);
            diagnostic("Stream transport: " + stream_->transportTimings(),4096);
            diagnostic("Stream mutex: " + stream_->mutexDiagnostics(),4096);
            lastProtocolLog_ = pumpAt;
        }
        if (!stream_->error().empty()) {
            diagnostic("Stream error: " + safeText(stream_->error()));
            if (stream_->retryableError()) retryTransport("transport-error");
            else beginFinish("transport-fatal");
            return;
        }
        if (stream_->disconnected() && !transportReconnect_.recovering()) {
            transportReconnect_.disconnected(pumpAt);
            suspendVideoInput();
            diagnostic("Transport disconnected; waiting up to 5s before replacement; rental remains active.");
        }
    }
    const auto recoveryAction = transportReconnect_.poll(std::chrono::steady_clock::now());
    if (recoveryAction == fog::stream::TransportReconnect::Action::Stop) {
        beginFinish("transport-reconnect-deadline"); return;
    }
    if (recoveryAction == fog::stream::TransportReconnect::Action::CloseTransport) {
        const bool wasRelay = relaySelection_.relay();
        if (relaySelection_.broken(std::chrono::steady_clock::now()))
            diagnostic("Transport break recorded after unresolved grace; recentBreaks=" + std::to_string(relaySelection_.breaks()));
        if (!wasRelay && relaySelection_.relay()) diagnostic("Auto TURN selected after two breaks in 120s; retained until rental closes.");
        streamInputGate_.suspendVideo();
        if (!closeTransport(false)) {
            beginFinish("transport-retry-terminal"); return;
        }
    }
    if (recoveryAction == fog::stream::TransportReconnect::Action::StartTransport) {
        diagnostic("Transport reconnect attempt=" + std::to_string(transportReconnect_.attempts()) + "/3 rollingWindow=120s");
        if (!startTransport()) {
            if (stream_->retryableError()) retryTransport("retry-start");
            else beginFinish("transport-start-fatal");
            return;
        }
    }
    if (!stream_) return;
    const bool lossWaiting = stream_->waitingForVideoKeyframe();
    if (lossWaiting && !videoLossWaiting_) {
        loopTiming_.suspendPresentation();
        suspendVideoInput();
    }
    videoLossWaiting_ = lossWaiting;
    const auto diagnosticNow = std::chrono::steady_clock::now();
    // SD lines are durable: drain a bounded event ring at most once per second,
    // not on each RTP packet. Five-second counters include suppressed events.
    if (diagnosticNow - lastVideoEventLog_ >= std::chrono::seconds(1)) {
        std::string event;
        if (stream_->pollVideoDiagnostic(event)) diagnostic("Video RTP event: " + event);
        lastVideoEventLog_ = diagnosticNow;
    }
    if (videoRecovery_.expired(std::chrono::steady_clock::now()))
        throw std::runtime_error("Video recovery deadline reached; stopping rental");
    drainStreamAudio();
    stream::Packet packet;
    int drained = 0;
    const auto decoderResetBeforeUs = decoderResetTiming_.totalUs;
    const auto drainStarted = std::chrono::steady_clock::now();
    const auto queuedBeforeDrain = stream_->queuedVideoPackets();
    fog::stream::PlaybackDrainBudget drainBudget(queuedBeforeDrain, stream_->queuedVideoAgeMs());
    if (drainBudget.catchup()) ++catchupWindows_;
    // Keep compressed references in FIFO order. Bound both work time and the
    // number of video frames before input, presentation, and audio get a turn.
    while (true) {
        const auto elapsedUs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now()-drainStarted).count());
        const auto yield = drainBudget.yield(elapsedUs);
        if (yield != fog::stream::PlaybackDrainBudget::Yield::None) {
            if (yield == fog::stream::PlaybackDrainBudget::Yield::Video) ++drainVideoBudgetHits_;
            if (yield == fog::stream::PlaybackDrainBudget::Yield::Time) ++drainBudgetHits_;
            break;
        }
        if (!stream_->poll(packet)) break;
        ++drained;
        drainBudget.consumed(packet.kind == stream::Kind::Video);
        const auto dequeueMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        const auto waitMs = dequeueMs >= packet.enqueueMs ? dequeueMs-packet.enqueueMs : 0;
        auto& waitMaximum = packet.kind == stream::Kind::Video ? videoQueueWaitMaxMs_ : audioQueueWaitMaxMs_;
        waitMaximum = std::max(waitMaximum, waitMs);
        if (videoRecovery_.expired(std::chrono::steady_clock::now()))
            throw std::runtime_error("Video recovery deadline reached; stopping rental");
        if (packet.kind == stream::Kind::Video) {
            try {
                media_->setVideoCodec(stream_->selectedVideoCodec());
                if (packet.videoGeneration != lastVideoGeneration_) {
                    loopTiming_.suspendPresentation();
                    const auto resetStarted = std::chrono::steady_clock::now();
                    media_->resetVideoDecoder();
                    decoderResetTiming_.record(resetStarted);
                    const auto resetUs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now()-resetStarted).count());
                    videoWorkUs_ += resetUs;
                    lastVideoGeneration_ = packet.videoGeneration;
                    suspendVideoInput();
                    diagnostic("Video chain resync: decoder reset generation=" + std::to_string(lastVideoGeneration_));
                }
            } catch (const fog::VideoDecodeError& error) {
                recoverVideoError(error,&packet);
                continue;
            }
            const auto workStarted = std::chrono::steady_clock::now();
            try { media_->video(packet.data.data(), packet.data.size()); }
            catch (const fog::VideoDecodeError& error) {
                videoFeedTiming_.record(workStarted);
                videoWorkUs_ += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - workStarted).count());
                recoverVideoError(error,&packet);
                continue;
            }
            videoFeedTiming_.record(workStarted);
            videoWorkUs_ += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - workStarted).count());
            if (videoRecovery_.expired(std::chrono::steady_clock::now()))
                throw std::runtime_error("Video recovery deadline reached; stopping rental");
        } else {
            feedStreamAudio(packet);
        }
    }
    drainStreamAudio();
    drainVideoTotal_ += drainBudget.video();
    drainVideoMax_ = std::max(drainVideoMax_, static_cast<std::uint64_t>(drainBudget.video()));
    const auto presentationStarted = std::chrono::steady_clock::now();
    bool prepared = false;
    try { prepared = media_->preparePresentation(); }
    catch (const fog::VideoDecodeError& error) {
        videoWorkUs_ += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now()-presentationStarted).count());
        recoverVideoError(error,nullptr);
    }
    prepareTiming_.record(presentationStarted);
    const auto prepareWorkUs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now()-presentationStarted).count());
    if (prepared) {
        videoWorkUs_ += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now()-presentationStarted).count());
        if (videoRecovery_.expired(std::chrono::steady_clock::now()))
            throw std::runtime_error("Video recovery deadline reached; stopping rental");
        lastVideo_ = std::chrono::steady_clock::now();
        rememberDecodedGame();
        if (videoRecovery_.active()) {
            diagnostic("Video recovery succeeded; frames=" + std::to_string(media_->videoFrames()) +
                " width=" + std::to_string(media_->videoWidth()) + " height=" + std::to_string(media_->videoHeight()));
            videoRecovery_.decoded();
            stream_->setDecoderWaitingForKeyframe(false);
        }
        if (transportReconnect_.recovering() && stream_->ready() && !stream_->disconnected() &&
            transportReconnect_.prepared(std::chrono::steady_clock::now())) {
            diagnostic("Transport recovery succeeded; attemptsIn120s=" + std::to_string(transportReconnect_.attempts()));
            status_ = "Связь восстановлена. Минус + Плюс — меню трансляции.";
        }
        if (!transportReconnect_.recovering() && stream_->ready() && !stream_->disconnected())
            relaySelection_.connected();
        // Queue recovery may finish before the decoder produces an image. Resume
        // only after this generation has successfully prepared a visible frame.
        videoLossWaiting_ = stream_->waitingForVideoKeyframe();
        streamInputGate_.videoPrepared(transportReconnect_.recovering() || videoRecovery_.active() || videoLossWaiting_ || media_->waitingForVideoKeyframe());
        if (!firstFramePresented_) {
            diagnostic(media_->hardwareVideo() ? "First video frame prepared (hardware)." : "First video frame prepared (software).");
            firstFramePresented_ = true;
        }
    }
    drainTiming_.record(drainStarted);
    const auto measuredAt = std::chrono::steady_clock::now();
    lastPumpWorkUs_ = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(measuredAt-pumpAt).count());
    // One alert per second at most; the five-second aggregates count all stalls.
    // The previous measured work distinguishes an app-thread gap from slow media work.
    if (pumpGapUs >= 50000 || lastPumpWorkUs_ >= 50000) {
        ++slowPlaybackCycles_;
        if (measuredAt-lastSlowPlaybackLog_ >= std::chrono::seconds(1)) {
            diagnostic("Playback slow cycle: pumpGapUs=" + std::to_string(pumpGapUs) +
                " outsideMeasuredPumpUs=" + std::to_string(pumpGapUs > previousPumpWorkUs ? pumpGapUs-previousPumpWorkUs : 0) +
                " previousPumpMeasuredUs=" + std::to_string(previousPumpWorkUs) +
                " pumpMeasuredUs=" + std::to_string(lastPumpWorkUs_) +
                " transportUs=" + std::to_string(transportWorkUs) +
                " mediaBatchUs=" + std::to_string(static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(measuredAt-drainStarted).count())) +
                " prepareUs=" + std::to_string(prepareWorkUs) +
                " decoderResetUs=" + std::to_string(decoderResetTiming_.totalUs-decoderResetBeforeUs) +
                " videoDrained=" + std::to_string(drainBudget.video()) +
                " videoBefore=" + std::to_string(queuedBeforeDrain) +
                " videoAfter=" + std::to_string(stream_->queuedVideoPackets()) +
                " drained=" + std::to_string(drained), 1024);
            lastSlowPlaybackLog_ = measuredAt;
        }
    }
    // Presentation counts come from the preceding completed RenderPresent, never
    // repeated drawing of an old texture or decoding alone.
    stream_->observePlayback({media_->presentedFrames(), media_->videoFrames(), media_->supersededFrames(),
        videoWorkUs_, videoDecodeErrors_, media_->audioUnderruns(), media_->audioQueueResets(),
        transportReconnect_.recovering() || videoRecovery_.active() || videoLossWaiting_ || media_->waitingForVideoKeyframe(),
        media_->videoWidth(), media_->videoHeight()});
    const auto now = std::chrono::steady_clock::now();
    if (now-lastMediaLog_ >= std::chrono::seconds(5)) {
        diagnostic("Media video=" + std::to_string(media_->videoFrames()) +
            " opus=" + std::to_string(media_->audioPackets()) +
            " hardware=" + std::to_string(media_->hardwareVideo()) +
            " controlReady=" + std::to_string(stream_->ready()) +
            " width=" + std::to_string(media_->videoWidth()) + " height=" + std::to_string(media_->videoHeight()) +
            " recovering=" + std::to_string(videoRecovery_.active()) +
            " waitingIDR=" + std::to_string(media_->waitingForVideoKeyframe()));
        diagnostic("Video RTP: " + stream_->videoDiagnostics(),4096);
        diagnostic("Video gaps: " + stream_->gapDiagnostics(),4096);
        diagnostic("Media queue: " + stream_->queueDiagnostics(),2048);
        diagnostic("Video decoder: " + media_->diagnostics(),2048);
        diagnostic("Audio playback: " + media_->audioDiagnostics(),2048);
        logOutputTimings(false);
        const auto timings = playbackTimings();
        drainTiming_ = {}; presentTiming_ = {}; diagnosticTiming = {};
        pumpGapMaxUs_ = 0; outsideMeasuredPumpMaxUs_ = 0; drainBudgetHits_ = 0; catchupWindows_ = 0;
        transportPumpTiming_ = {}; videoFeedTiming_ = {}; decoderResetTiming_ = {}; audioFeedTiming_ = {}; prepareTiming_ = {};
        drainVideoTotal_ = drainVideoMax_ = drainVideoBudgetHits_ = 0;
        videoQueueWaitMaxMs_ = audioQueueWaitMaxMs_ = slowPlaybackCycles_ = 0;
        diagnostic(timings,2048);
        lastMediaLog_ = now;
    }
    if (videoRecovery_.expired(now)) throw std::runtime_error("Video recovery deadline reached; stopping rental");
    if (!transportReconnect_.recovering() && (!firstFramePresented_ || !stream_->ready()) &&
        now-streamStarted_ >= std::chrono::seconds(30)) {
        retryTransport("startup-timeout"); return;
    }
    if (!transportReconnect_.recovering() && !videoRecovery_.active() &&
        now-lastVideo_ >= std::chrono::seconds(firstFramePresented_ ? 10 : 30)) {
        retryTransport("media-timeout"); return;
    }
    if (screen_ != Screen::Streaming) return;
    if (!wasRecovering) overlayHandled = handleStreamOverlay(pad, held, down);
    if (!streamInputGate_.gamepadAllowed(overlayHandled)) return;
    if (transportReconnect_.recovering() || videoRecovery_.active() || videoLossWaiting_ || now-lastInput_ < std::chrono::milliseconds(30) || !stream_->ready()) return;
    stream::Gamepad gamepad;
    const std::array<u64,16> buttons{HidNpadButton_B,HidNpadButton_A,HidNpadButton_Y,HidNpadButton_X,
        HidNpadButton_L,HidNpadButton_R,HidNpadButton_ZL,HidNpadButton_ZR,HidNpadButton_Minus,HidNpadButton_Plus,
        HidNpadButton_StickL,HidNpadButton_StickR,HidNpadButton_Up,HidNpadButton_Down,HidNpadButton_Left,HidNpadButton_Right};
    const auto remoteHeld = fog::controller::mapStreamButtons(held,controllerSettings_,faceMasks);
    for (std::size_t i=0; i<buttons.size(); ++i) gamepad.buttons[i] = (remoteHeld & buttons[i]) ? 1 : 0;
    const auto left = padGetStickPos(&pad,0), right = padGetStickPos(&pad,1);
    gamepad.axes = {axis(left.x),-axis(left.y),axis(right.x),-axis(right.y)};
    gamepad.leftTrigger = (held & HidNpadButton_ZL) ? 1.0f : 0.0f;
    gamepad.rightTrigger = (held & HidNpadButton_ZR) ? 1.0f : 0.0f;
    stream_->sendGamepad(gamepad); lastInput_ = now;
}

}
