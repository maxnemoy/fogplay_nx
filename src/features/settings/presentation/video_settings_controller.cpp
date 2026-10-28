#include "core/constants/storage_paths.hpp"
#include "features/settings/infrastructure/video_settings_storage.hpp"
#include "app/application_controller.hpp"

namespace uiText = ::fog::constants::ui;
namespace storagePaths = ::fog::constants::storage;

namespace fogplay {

void ApplicationController::changeVideoSettings(const fog::video::Settings& value, bool live) {
    if (!fog::video::valid(value)) return;
    const bool foghub = live && streamRental_.provider == fog::ProviderId::FogHub;
    const auto previous = videoSettings_;
    if (foghub) {
        // The FH overlay owns codec/bitrate only. Its normalized transport
        // settings must not erase the shared MTS quality or PLI preferences.
        videoSettings_.codec = value.codec;
        videoSettings_.bitrateMbps = value.bitrateMbps;
    } else {
        videoSettings_ = value;
    }
    bool saved = false;
    try {
        fog::video::saveSettings(storagePaths::path(storagePaths::VideoSettings), videoSettings_);
        saved = true;
    } catch (...) { diagnostic("Video settings save failed; no settings payload logged."); }
    videoSaveStatus_ = saved ? uiText::Settings::Saved : uiText::Settings::NotSaved;
    if (foghub) {
        if (previous.codec != videoSettings_.codec || previous.bitrateMbps != videoSettings_.bitrateMbps) {
            if (requestPairedVideoReconnect()) videoSaveStatus_ += uiText::Settings::FogHubReconnectNotice;
        }
    } else if (live) {
        const bool requested = stream_ && stream_->updateVideoSettings(videoSettings_);
        videoSaveStatus_ += requested ? uiText::Settings::RequestSentNotice :
            uiText::Settings::RequestNotSentNotice;
    }
    overlay_.setVideoSettings(videoSettings_);
    overlay_.setSaveStatus(videoSaveStatus_);
}

bool ApplicationController::requestPairedVideoReconnect() {
    // Additional edits while ownership verification runs share its one result;
    // the new transport picks up the latest saved codec/bitrate preferences.
    if (pairedVideoReconnectPending_) return true;
    if (pending_.valid()) {
        // A different UI operation must not interrupt a healthy paid stream.
        // Preferences are already saved; the user can retry after it completes.
        videoSaveStatus_ += uiText::Settings::FogHubReconnectBusyNotice;
        return false;
    }
    if (screen_ != Screen::Streaming || !sessionPending_ || !pairedPending_ ||
        !paired_ || !pairedServices_ || stopAfterTask_ ||
        exitAfterStop_ || shutdownDone_ || streamRental_.empty() ||
        streamRental_.provider != fog::ProviderId::FogHub || !stream_ ||
        stream_->disconnected() || !stream_->error().empty()) {
        stopAfterTask_ = true;
        if (paired_) paired_->requestStop();
        beginFinish("paired-quality-reconnect-unavailable");
        return false;
    }
    if (!stream_->ready()) {
        // Startup and a successful same-rental resume both include a healthy
        // handshake. An edit at this boundary must not stop the paid rental.
        videoSaveStatus_ += uiText::Settings::FogHubConnectingSavedNotice;
        return false;
    }
    streamInputGate_.suspendVideo();
    neutralizeStreamInput();
    pairedVideoReconnectPending_ = true;
    try {
        // poll verifies the original account, exact recorded rental/computer
        // and the original paired endpoint. It never creates or pairs again.
        start(Task::PairedPoll,[this] { paired_->poll(); return std::monostate{}; });
    } catch (...) {
        pairedVideoReconnectPending_ = false;
        beginFinish("paired-quality-inspection-dispatch-failed");
        return false;
    }
    status_ = uiText::Settings::FogHubReconnectStatus;
    diagnostic("FH quality reconnect: exact-rental verification requested; no rental creation or pairing.");
    return true;
}

bool ApplicationController::completePairedVideoReconnect() {
    if (!pairedVideoReconnectPending_) return false;
    pairedVideoReconnectPending_ = false;
    if (!pairedPending_) {
        // Fresh terminal evidence wins over the requested transport change.
        closeStream();
        return false;
    }
    const auto& result = paired_->result();
    const bool ready = result.state == fog::PairedCheckoutState::Ready &&
        !result.stopRequested && result.rental == streamRental_ &&
        screen_ == Screen::Streaming && sessionPending_ &&
        !stopAfterTask_ && !exitAfterStop_ && !shutdownDone_ &&
        stream_ && stream_->ready() && !stream_->disconnected() && stream_->error().empty();
    if (!ready) { beginFinish("paired-quality-verification-refused"); return true; }
    if (!closeTransport(false)) { beginFinish("paired-quality-disconnect-refused"); return true; }
    try {
        // Retain rental ownership, recovery journal and recent-launch attribution.
        // resumeOnly forbids launching an app, creating or pairing another rental.
        if (startTransport(true)) {
            status_ = uiText::Settings::FogHubReconnectStatus;
            diagnostic("FH quality reconnect: verified same-rental resume requested.");
            return true;
        }
    } catch (...) {
        diagnostic("FH quality reconnect: transport setup failed; stopping owned rental.");
    }
    beginFinish("paired-quality-resume-failed");
    return true;
}

}
