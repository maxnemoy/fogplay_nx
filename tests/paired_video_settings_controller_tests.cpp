#include "app/application_controller.hpp"
#include <iostream>

namespace {
int assertions = 0, saves = 0;
bool saveThrows = false;
fog::video::Settings saved;
void require(bool condition,const char* message) {
    ++assertions;
    if (!condition) throw std::runtime_error(message);
}
fog::video::Settings changed(fogplay::ApplicationController& controller) {
    auto value = controller.videoSettings_;
    value.codec = stream::VideoCodec::H264; value.bitrateMbps = 30;
    value.quality = 0; value.keyframeSeconds = 0;
    return value;
}
}
namespace fog::video {
void saveSettings(const std::string&,const Settings& settings) {
    ++saves;
    if (saveThrows) throw std::runtime_error("synthetic storage failure");
    saved = settings;
}
}
int main() {
    using namespace fogplay;
    try {
        { ApplicationController c(std::chrono::hours(1));
          const auto before = saves;
          auto next = changed(c); next.framesPerSecond = 60;
          c.changeVideoSettings(next,true);
          require(fog::stage1::asyncSettings ?
              saves == before && c.videoSaveStatus_.find(fog::constants::ui::Settings::Saving) == 0 :
              saves == before + 1 && c.videoSaveStatus_.find(fog::constants::ui::Settings::Saved) == 0,
              "controller save status matches asynchronous or synchronous A/B profile");
          require(c.videoSettings_.bitrateMbps == 30 && c.pending_.valid(),
              "active preferences and verified reconnect intent do not wait for storage");
          c.settingsSaves_.flush();
          require(saves == before + 1 && c.videoSaveStatus_.find(fog::constants::ui::Settings::Saved) == 0 &&
              c.videoSaveStatus_.find(fog::constants::ui::Settings::FogHubReconnectNotice) != std::string::npos &&
              c.videoSaveStatus_.find("60 FPS") != std::string::npos && c.overlay_.saved == c.videoSaveStatus_,
              "owner completion reports durable save and retains live reconnect and FPS notices"); }
        { ApplicationController c;
          c.videoSettings_.quality = 4; c.videoSettings_.keyframeSeconds = 5;
          c.changeVideoSettings(changed(c),true); c.settingsSaves_.flush();
          require(c.pending_.valid() && c.pairedVideoReconnectPending_ && c.paired_->polls == 0,
              "controller dispatches ownership inspection without running it on the UI thread");
          require(saved.quality == 4 && saved.keyframeSeconds == 5 && saved.bitrateMbps == 30,
              "FH changes save codec/bitrate while preserving shared MTS preferences");
          require(c.stream_ && c.disconnects == 0 && c.starts == 0 && c.finishes == 0,
              "healthy stream stays connected during ownership verification");
          auto latest = changed(c); latest.bitrateMbps = 40; c.changeVideoSettings(latest,true); c.settingsSaves_.flush();
          require(c.dispatches == 1, "additional settings edits coalesce into the pending inspection");
          c.joinInspection();
          require(c.completePairedVideoReconnect() && c.starts == 1 && c.resumedOnly &&
              c.startedRental == c.streamRental_ && c.startedSettings.bitrateMbps == 40,
              "verified completion resumes only the exact original rental with latest preferences");
          require(c.disconnects == 1 && !c.finalDisconnect && c.finishes == 0 &&
              !c.pairedVideoReconnectPending_, "quality resume replaces transport without final rental teardown"); }
        { ApplicationController c; c.deferClose = true;
          c.changeVideoSettings(changed(c),true); c.settingsSaves_.flush(); c.joinInspection();
          require(c.completePairedVideoReconnect() && c.transportTeardown_.pending() &&
              c.pairedVideoResumeAfterClose_ && !c.stream_ && c.closingStream_ && c.starts == 0,
              "verified quality change waits for joined old transport before resume");
          auto latest = changed(c); latest.bitrateMbps = 40;
          c.changeVideoSettings(latest,true); c.settingsSaves_.flush();
          require(c.starts == 0 && c.finishes == 0 && c.dispatches == 1 &&
              c.pairedVideoResumeAfterClose_, "additional quality edits coalesce while old transport closes");
          c.completeDeferredClose();
          require(c.starts == 0 && c.pairedVideoResumeAfterClose_,
              "mailbox completion leaves resume deferred until current frame input is handled");
          c.dispatchDeferredResume();
          require(!c.transportTeardown_.pending() && !c.closingStream_ && c.starts == 1 &&
              c.resumedOnly && c.startedRental == c.streamRental_ && c.startedSettings.bitrateMbps == 40,
              "joined quality teardown resumes exact rental with latest settings");
          c.completeDeferredClose();
          c.dispatchDeferredResume();
          require(c.starts == 1, "consumed close completion cannot resume twice"); }
        for (int cancellation = 0; cancellation < 7; ++cancellation) {
            ApplicationController c; c.deferClose = true;
            c.changeVideoSettings(changed(c),true); c.settingsSaves_.flush(); c.joinInspection();
            c.completePairedVideoReconnect();
            require(c.transportTeardown_.pending() && c.starts == 0, "cancellation fixture owns blocked close");
            c.dispatchDeferredResume();
            require(c.starts == 0 && c.pairedVideoResumeAfterClose_,
                "resume dispatch waits for old transport callback join");
            c.completeDeferredClose();
            require(c.starts == 0 && c.pairedVideoResumeAfterClose_,
                "joined mailbox does not race current frame cancellation input");
            if (cancellation == 0) c.stopAfterTask_ = true;
            if (cancellation == 1) c.exitAfterStop_ = true;
            if (cancellation == 2) c.shutdownDone_ = true;
            if (cancellation == 3) c.pairedPending_ = false;
            if (cancellation == 4) c.sessionPending_ = false;
            if (cancellation == 5) c.streamRental_ = {};
            if (cancellation == 6) c.screen_ = Screen::Recovery;
            c.dispatchDeferredResume();
            require(c.starts == 0 && !c.pairedVideoResumeAfterClose_ && !c.transportTeardown_.pending(),
                "late close completion cannot undo stop, shutdown or lost rental ownership");
        }
        { ApplicationController c; c.deferClose = true;
          c.changeVideoSettings(changed(c),true); c.settingsSaves_.flush(); c.joinInspection();
          c.completePairedVideoReconnect(); c.closeStream(); c.completeDeferredClose(); c.dispatchDeferredResume();
          require(c.starts == 0 && !c.pairedVideoResumeAfterClose_,
              "ordinary final close cancels deferred quality resume"); }
        for (int intent = 0; intent < 3; ++intent) {
            ApplicationController c; c.changeVideoSettings(changed(c),true); c.settingsSaves_.flush();
            if (intent == 0) { c.stopAfterTask_ = true; c.paired_->requestStop(); }
            if (intent == 1) c.exitAfterStop_ = true;
            if (intent == 2) c.shutdownDone_ = true;
            c.joinInspection(); c.completePairedVideoReconnect();
            require(c.starts == 0 && c.disconnects == 0 && c.finishes == 1,
                "queued HOME, user stop or shutdown wins over stale ready inspection");
        }
        for (int bad = 0; bad < 4; ++bad) {
            ApplicationController c; c.changeVideoSettings(changed(c),true); c.settingsSaves_.flush();
            if (bad == 0) c.paired_->value.rental.value = "other";
            if (bad == 1) c.paired_->value.stopRequested = true;
            if (bad == 2) c.paired_->value.state = fog::PairedCheckoutState::RecoveryRequired;
            if (bad == 3) c.stream_->lost = true;
            c.joinInspection(); c.completePairedVideoReconnect();
            require(c.starts == 0 && c.finishes == 1, "unverified rental, stop result or failed live stream cannot resume");
        }
        for (int failure = 0; failure < 3; ++failure) {
            ApplicationController c; c.changeVideoSettings(changed(c),true); c.settingsSaves_.flush(); c.joinInspection();
            if (failure == 0) c.disconnectAllowed = false;
            if (failure == 1) c.startAllowed = false;
            if (failure == 2) c.startThrows = true;
            c.completePairedVideoReconnect();
            require(c.finishes == 1 && (failure != 0 || c.starts == 0),
                "refused disconnect or failed resume always finishes the owned rental");
        }
        { ApplicationController c; c.changeVideoSettings(changed(c),true); c.settingsSaves_.flush(); c.joinInspection();
          c.pairedPending_ = false; c.sessionPending_ = false;
          require(!c.completePairedVideoReconnect() && c.streamCloses == 1 && c.starts == 0,
              "terminal ownership evidence closes old transport and returns to ordinary terminal handling"); }
        { ApplicationController c;
          c.start(Task::Other,[] { return TaskResult{}; });
          c.changeVideoSettings(changed(c),true); c.settingsSaves_.flush();
          require(c.finishes == 0 && c.disconnects == 0 && !c.pairedVideoReconnectPending_ &&
              c.videoSaveStatus_.find(fog::constants::ui::Settings::FogHubReconnectBusyNotice) != std::string::npos,
              "unrelated pending task saves preferences and leaves healthy rental running"); }
        { ApplicationController c; c.dispatchThrows = true; c.changeVideoSettings(changed(c),true); c.settingsSaves_.flush();
          require(c.finishes == 1 && !c.pairedVideoReconnectPending_, "inspection dispatch failure finishes and clears intent"); }
        { ApplicationController c; c.stream_->active = false; c.changeVideoSettings(changed(c),true); c.settingsSaves_.flush();
          require(c.finishes == 0 && c.starts == 0 && c.disconnects == 0 && !c.pending_.valid(),
              "quality edit during healthy initial handshake never stops or replaces the rental");
          require(c.videoSaveStatus_.find(fog::constants::ui::Settings::FogHubConnectingSavedNotice) != std::string::npos,
              "deferred startup preference is explicitly marked for next connection"); }
        { ApplicationController c; c.changeVideoSettings(changed(c),true); c.settingsSaves_.flush(); c.joinInspection(); c.completePairedVideoReconnect();
          require(c.stream_ && !c.stream_->ready() && c.starts == 1, "successful resume enters asynchronous handshake");
          auto latest = changed(c); latest.bitrateMbps = 40; c.changeVideoSettings(latest,true); c.settingsSaves_.flush();
          require(c.finishes == 0 && c.starts == 1 && c.disconnects == 1 && !c.pending_.valid(),
              "another quality edit during resume handshake never stops or resumes again");
          require(c.startedSettings.bitrateMbps == 30 && c.videoSettings_.bitrateMbps == 40 &&
              c.videoSaveStatus_.find(fog::constants::ui::Settings::FogHubConnectingSavedNotice) != std::string::npos,
              "controller distinguishes active handshake settings from explicitly saved next preferences"); }
        { ApplicationController c; saveThrows = true; c.changeVideoSettings(changed(c),true); c.settingsSaves_.flush(); saveThrows = false;
          require(c.videoSaveStatus_.find(fog::constants::ui::Settings::NotSaved) == 0 && c.pending_.valid(),
              "save failure is visible while the explicitly requested in-memory change can reconnect"); }
        { ApplicationController c; c.streamRental_.provider = fog::ProviderId::Mts;
          c.changeVideoSettings(changed(c),true); c.settingsSaves_.flush();
          require(c.stream_->updates == 1 && !c.pending_.valid() && c.finishes == 0,
              "MTS continues to use its existing live settings request"); }
        for (const auto provider : {fog::ProviderId::Mts, fog::ProviderId::FogHub}) {
            ApplicationController c; c.streamRental_.provider = provider;
            auto fps = c.videoSettings_; fps.framesPerSecond = 60;
            c.changeVideoSettings(fps,true); c.settingsSaves_.flush();
            require(saved.framesPerSecond == 60 && c.videoSettings_.framesPerSecond == 60 &&
                c.activeFramesPerSecond_ == 30 && c.stream_->updates == 0 &&
                !c.pending_.valid() && c.starts == 0 && c.finishes == 0,
                "FPS-only edit persists for next connection without interrupting either provider");
            require(c.videoSaveStatus_.find("60 FPS") != std::string::npos,
                "pending FPS is explicit while active stream stays at its negotiated rate");
            fps.bitrateMbps = 30; c.changeVideoSettings(fps,true); c.settingsSaves_.flush();
            if (provider == fog::ProviderId::Mts) {
                require(c.stream_->updates == 1 && c.stream_->updatedSettings.framesPerSecond == 30 &&
                    c.stream_->updatedSettings.bitrateMbps == 30 && saved.framesPerSecond == 60,
                    "MTS live bitrate edit preserves active FPS and saved next-connection FPS");
            } else {
                c.joinInspection(); c.completePairedVideoReconnect();
                require(c.starts == 1 && c.resumedOnly && c.startedSettings.framesPerSecond == 60,
                    "explicit FH bitrate reconnect applies the latest saved FPS to the same rental");
            }
        }
        { ApplicationController c; c.streamRental_.provider = fog::ProviderId::Mts;
          c.activeFramesPerSecond_ = c.videoSettings_.framesPerSecond = 60;
          auto next = c.videoSettings_; next.framesPerSecond = 30; next.quality = 2;
          c.changeVideoSettings(next,true); c.settingsSaves_.flush();
          require(saved.framesPerSecond == 30 && c.stream_->updatedSettings.framesPerSecond == 60 &&
              c.stream_->updatedSettings.quality == 2, "saving 30 never downgrades a live 60 FPS stream implicitly"); }
        require(saves > 0, "production settings persistence path exercised");
        std::cout << "Paired video settings controller: " << assertions << " assertions passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Paired video settings controller failed: " << error.what() << '\n'; return 1;
    }
}
