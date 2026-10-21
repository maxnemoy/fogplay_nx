#include "app/application_controller.hpp"

namespace fogplay {

std::string ApplicationController::playbackTimings() const {
    const auto log = runLog.stats();
    return "Playback timing: " + drainTiming_.text("drain") + " " + presentTiming_.text("present") +
        " " + diagnosticTiming.text("logEnqueue") + " pumpGapMaxUs=" + std::to_string(pumpGapMaxUs_) +
        " budgetYields=" + std::to_string(drainBudgetHits_) + " catchupWindows=" + std::to_string(catchupWindows_) +
        " logQueued=" + std::to_string(log.queuedLines) + " logDropped=" + std::to_string(log.droppedLines) +
        " logWritten=" + std::to_string(log.writtenLines) + " logBatches=" + std::to_string(log.batches) +
        " logBatchLastUs=" + std::to_string(log.lastBatchUs) + " logBatchMaxUs=" + std::to_string(log.maxBatchUs);
}

void ApplicationController::closeStream() noexcept {
    recentLaunch_.close();
    releaseMouse();
    overlay_.close();
    videoRecovery_.reset();
    videoLossWaiting_ = false;
    if (stream_) {
        try {
            diagnostic("Video RTP final: " + stream_->videoDiagnostics(),2048);
            diagnostic(playbackTimings(),2048);
            if (media_) diagnostic("Video decoder final: " + media_->diagnostics(),2048);
        } catch (...) {} // Diagnostics must never prevent rental shutdown.
        try { stream_->close(); } catch (...) {}
        stream_.reset();
    }
    media_.reset();
}

void ApplicationController::startStream(const std::string& hash, bool createdFromCatalog) {
    latency_.cancel();
    recentLaunch_.stream(createdFromCatalog);
    sessionPending_ = true;
    videoRecovery_.reset();
    videoLossWaiting_ = false; firstFramePresented_ = false;
    videoWorkUs_ = videoDecodeErrors_ = lastVideoGeneration_ = 0;
    drainTiming_ = {}; presentTiming_ = {}; diagnosticTiming = {};
    lastPumpAt_ = {}; lastProtocolLog_ = {}; pumpGapMaxUs_ = 0; drainBudgetHits_ = 0; catchupWindows_ = 0;
    ++sessionSequence_;
    diagnostic("Stream session=" + std::to_string(sessionSequence_) +
        (createdFromCatalog ? " origin=catalog catalogGameId=" + std::to_string(selectedGameId_) +
         " catalogGame=" + safeText(selectedGameName_) : " origin=existing-rental catalogGame=unknown"));
    screen_ = Screen::Streaming;
    overlay_.close(); suppressStreamInput_ = true; touchWasDown_ = touchDrag_ = false;
    pointerX_ = 640.0f; pointerY_ = 360.0f;
    mouseAt_ = std::chrono::steady_clock::now();
    overlayNotice_.clear();
    try {
        diagnostic("Stream setup begins; preparing video/audio and signaling.");
        diagnostic(std::string("Video preferences: codec=") + ::stream::videoCodecName(videoSettings_.codec) +
            " capMbps=" + std::to_string(videoSettings_.bitrateMbps) + " quality=" + std::to_string(videoSettings_.quality) +
            " pliSeconds=" + std::to_string(videoSettings_.keyframeSeconds));
        armExitHandling();
        media_ = std::make_unique<fog::Media>(rt_.renderer,stream::VideoCodec::H264,true);
        stream_ = std::make_unique<stream::Client>();
        if (!stream_->configureVideo(videoSettings_))
            throw std::runtime_error("Invalid video configuration; stopping rental");
        overlay_.setVideoSettings(videoSettings_);
        overlay_.setSaveStatus(videoSaveStatus_);
        lastStreamState_.clear(); lastStreamDiagnostics_.clear();
        std::array<unsigned char,5> random{}; randomGet(random.data(),random.size());
        std::string suffix;
        for (auto value : random) suffix += "0123456789abcdefghijklmnopqrstuvwxyz"[value % 36];
        if (!stream_->start(hash, suffix, "romfs:/cacert.pem"))
            throw std::runtime_error("Signaling startup failed; stopping rental");
        streamStarted_ = lastVideo_ = lastMediaLog_ = lastVideoEventLog_ = std::chrono::steady_clock::now();
        status_ = "Подключение. Минус + Плюс — меню трансляции.";
        diagnostic("Stream connection requested; recovery journal saved.");
    } catch (...) { beginFinish(); throw; }
}

void ApplicationController::recoverVideoError(const fog::VideoDecodeError& error, const stream::Packet* packet) {
    ++videoDecodeErrors_;
    std::string detail = packet ? "Video decode error: " : "Video presentation error: ";
    detail += error.what();
    if (packet) detail += " rtpTimestamp=" + std::to_string(packet->timestamp) +
        " rtpFirstSeq=" + std::to_string(packet->firstSequence) + " rtpLastSeq=" + std::to_string(packet->lastSequence);
    diagnostic(detail,2048);
    diagnostic("Video RTP at decode error: " + stream_->videoDiagnostics(),2048);
    diagnostic("Video decoder at error: " + media_->diagnostics(),2048);
    if (!error.recoverable()) throw error;
    if (!videoRecovery_.begin(std::chrono::steady_clock::now()))
        throw std::runtime_error("Video recovery deadline/reset budget exhausted; stopping rental");
    overlayTransition();
    stream_->setDecoderWaitingForKeyframe(true);
    media_->resetVideoDecoder();
    const bool requested = stream_->requestVideoKeyframe();
    diagnostic("Video recovery begin; reset=" + std::to_string(videoRecovery_.recentResets()) +
        " limit=3/60s deadline=10s keyframeRequested=" + std::to_string(requested));
}

void ApplicationController::pumpStream(PadState& pad, u64 held, u64 down) {
    if (!stream_) return;
    const auto pumpAt = std::chrono::steady_clock::now();
    if (lastPumpAt_ != std::chrono::steady_clock::time_point{}) {
        const auto gap = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(pumpAt-lastPumpAt_).count());
        pumpGapMaxUs_ = std::max(pumpGapMaxUs_, gap);
    }
    lastPumpAt_ = pumpAt;
    stream_->pump();
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
        lastProtocolLog_ = pumpAt;
    }
    if (!stream_->error().empty()) {
        diagnostic("Stream error: " + safeText(stream_->error()));
        throw std::runtime_error("Stream failed; stopping rental");
    }
    const bool lossWaiting = stream_->waitingForVideoKeyframe();
    if (lossWaiting && !videoLossWaiting_) overlayTransition();
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
    stream::Packet packet;
    int drained = 0;
    const auto drainStarted = std::chrono::steady_clock::now();
    const bool catchup = stream_->queuedVideoPackets() >= 2;
    const auto drainBudget = std::chrono::milliseconds(catchup ? 20 : 8);
    if (catchup) ++catchupWindows_;
    // Yield to input/presentation after a small batch. Never drop compressed
    // reference frames here: the receive queue owns IDR-safe resynchronization.
    while (drained < 12) {
        if (drained && std::chrono::steady_clock::now()-drainStarted >= drainBudget) {
            ++drainBudgetHits_; break;
        }
        if (!stream_->poll(packet)) break;
        ++drained;
        if (videoRecovery_.expired(std::chrono::steady_clock::now()))
            throw std::runtime_error("Video recovery deadline reached; stopping rental");
        if (packet.kind == stream::Kind::Video) {
            const auto workStarted = std::chrono::steady_clock::now();
            try {
                media_->setVideoCodec(stream_->selectedVideoCodec());
                if (packet.videoGeneration != lastVideoGeneration_) {
                    media_->resetVideoDecoder();
                    lastVideoGeneration_ = packet.videoGeneration;
                    overlayTransition();
                    diagnostic("Video chain resync: decoder reset generation=" + std::to_string(lastVideoGeneration_));
                }
                media_->video(packet.data.data(), packet.data.size());
            }
            catch (const fog::VideoDecodeError& error) {
                videoWorkUs_ += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - workStarted).count());
                recoverVideoError(error,&packet);
                continue;
            }
            videoWorkUs_ += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - workStarted).count());
            if (videoRecovery_.expired(std::chrono::steady_clock::now()))
                throw std::runtime_error("Video recovery deadline reached; stopping rental");
        } else {
            const auto before = media_->audioPackets();
            media_->audio(packet.data.data(), packet.data.size());
            if (!before) diagnostic("First Opus packet decoded and queued.");
        }
    }
    const auto presentationStarted = std::chrono::steady_clock::now();
    bool prepared = false;
    try { prepared = media_->preparePresentation(); }
    catch (const fog::VideoDecodeError& error) {
        videoWorkUs_ += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now()-presentationStarted).count());
        recoverVideoError(error,nullptr);
    }
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
        if (!firstFramePresented_) {
            diagnostic(media_->hardwareVideo() ? "First video frame prepared (hardware)." : "First video frame prepared (software).");
            firstFramePresented_ = true;
        }
    }
    drainTiming_.record(drainStarted);
    // Counters describe successful decoding and the full video processing cost
    // (including transfer/upload); all observation remains on the app thread.
    stream_->observePlayback(media_->videoFrames(), videoWorkUs_, videoDecodeErrors_,
        videoRecovery_.active() || videoLossWaiting_ || media_->waitingForVideoKeyframe(),
        media_->videoWidth(), media_->videoHeight());
    const auto now = std::chrono::steady_clock::now();
    if (now-lastMediaLog_ >= std::chrono::seconds(5)) {
        diagnostic("Media video=" + std::to_string(media_->videoFrames()) +
            " opus=" + std::to_string(media_->audioPackets()) +
            " hardware=" + std::to_string(media_->hardwareVideo()) +
            " controlReady=" + std::to_string(stream_->ready()) +
            " width=" + std::to_string(media_->videoWidth()) + " height=" + std::to_string(media_->videoHeight()) +
            " recovering=" + std::to_string(videoRecovery_.active()) +
            " waitingIDR=" + std::to_string(media_->waitingForVideoKeyframe()));
        diagnostic("Video RTP: " + stream_->videoDiagnostics(),2048);
        diagnostic("Video decoder: " + media_->diagnostics(),2048);
        const auto timings = playbackTimings();
        drainTiming_ = {}; presentTiming_ = {}; diagnosticTiming = {};
        pumpGapMaxUs_ = 0; drainBudgetHits_ = 0; catchupWindows_ = 0;
        diagnostic(timings,2048);
        lastMediaLog_ = now;
    }
    if (videoRecovery_.expired(now)) throw std::runtime_error("Video recovery deadline reached; stopping rental");
    if (!videoRecovery_.active() && now-lastVideo_ > std::chrono::seconds(firstFramePresented_ ? 10 : 35))
        throw std::runtime_error("Video timeout; stopping rental");
    if (screen_ != Screen::Streaming || handleStreamOverlay(pad, held, down)) return;
    if (videoRecovery_.active() || videoLossWaiting_ || now-lastInput_ < std::chrono::milliseconds(30) || !stream_->ready()) return;
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
