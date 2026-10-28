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
        { ApplicationController c;
          c.videoSettings_.quality = 4; c.videoSettings_.keyframeSeconds = 5;
          c.changeVideoSettings(changed(c),true);
          require(c.pending_.valid() && c.pairedVideoReconnectPending_ && c.paired_->polls == 0,
              "controller dispatches ownership inspection without running it on the UI thread");
          require(saved.quality == 4 && saved.keyframeSeconds == 5 && saved.bitrateMbps == 30,
              "FH changes save codec/bitrate while preserving shared MTS preferences");
          require(c.stream_ && c.disconnects == 0 && c.starts == 0 && c.finishes == 0,
              "healthy stream stays connected during ownership verification");
          auto latest = changed(c); latest.bitrateMbps = 40; c.changeVideoSettings(latest,true);
          require(c.dispatches == 1, "additional settings edits coalesce into the pending inspection");
          c.joinInspection();
          require(c.completePairedVideoReconnect() && c.starts == 1 && c.resumedOnly &&
              c.startedRental == c.streamRental_ && c.startedSettings.bitrateMbps == 40,
              "verified completion resumes only the exact original rental with latest preferences");
          require(c.disconnects == 1 && !c.finalDisconnect && c.finishes == 0 &&
              !c.pairedVideoReconnectPending_, "quality resume replaces transport without final rental teardown"); }
        for (int intent = 0; intent < 3; ++intent) {
            ApplicationController c; c.changeVideoSettings(changed(c),true);
            if (intent == 0) { c.stopAfterTask_ = true; c.paired_->requestStop(); }
            if (intent == 1) c.exitAfterStop_ = true;
            if (intent == 2) c.shutdownDone_ = true;
            c.joinInspection(); c.completePairedVideoReconnect();
            require(c.starts == 0 && c.disconnects == 0 && c.finishes == 1,
                "queued HOME, user stop or shutdown wins over stale ready inspection");
        }
        for (int bad = 0; bad < 4; ++bad) {
            ApplicationController c; c.changeVideoSettings(changed(c),true);
            if (bad == 0) c.paired_->value.rental.value = "other";
            if (bad == 1) c.paired_->value.stopRequested = true;
            if (bad == 2) c.paired_->value.state = fog::PairedCheckoutState::RecoveryRequired;
            if (bad == 3) c.stream_->lost = true;
            c.joinInspection(); c.completePairedVideoReconnect();
            require(c.starts == 0 && c.finishes == 1, "unverified rental, stop result or failed live stream cannot resume");
        }
        for (int failure = 0; failure < 3; ++failure) {
            ApplicationController c; c.changeVideoSettings(changed(c),true); c.joinInspection();
            if (failure == 0) c.disconnectAllowed = false;
            if (failure == 1) c.startAllowed = false;
            if (failure == 2) c.startThrows = true;
            c.completePairedVideoReconnect();
            require(c.finishes == 1 && (failure != 0 || c.starts == 0),
                "refused disconnect or failed resume always finishes the owned rental");
        }
        { ApplicationController c; c.changeVideoSettings(changed(c),true); c.joinInspection();
          c.pairedPending_ = false; c.sessionPending_ = false;
          require(!c.completePairedVideoReconnect() && c.streamCloses == 1 && c.starts == 0,
              "terminal ownership evidence closes old transport and returns to ordinary terminal handling"); }
        { ApplicationController c;
          c.start(Task::Other,[] { return TaskResult{}; });
          c.changeVideoSettings(changed(c),true);
          require(c.finishes == 0 && c.disconnects == 0 && !c.pairedVideoReconnectPending_ &&
              c.videoSaveStatus_.find(fog::constants::ui::Settings::FogHubReconnectBusyNotice) != std::string::npos,
              "unrelated pending task saves preferences and leaves healthy rental running"); }
        { ApplicationController c; c.dispatchThrows = true; c.changeVideoSettings(changed(c),true);
          require(c.finishes == 1 && !c.pairedVideoReconnectPending_, "inspection dispatch failure finishes and clears intent"); }
        { ApplicationController c; c.stream_->active = false; c.changeVideoSettings(changed(c),true);
          require(c.finishes == 0 && c.starts == 0 && c.disconnects == 0 && !c.pending_.valid(),
              "quality edit during healthy initial handshake never stops or replaces the rental");
          require(c.videoSaveStatus_.find(fog::constants::ui::Settings::FogHubConnectingSavedNotice) != std::string::npos,
              "deferred startup preference is explicitly marked for next connection"); }
        { ApplicationController c; c.changeVideoSettings(changed(c),true); c.joinInspection(); c.completePairedVideoReconnect();
          require(c.stream_ && !c.stream_->ready() && c.starts == 1, "successful resume enters asynchronous handshake");
          auto latest = changed(c); latest.bitrateMbps = 40; c.changeVideoSettings(latest,true);
          require(c.finishes == 0 && c.starts == 1 && c.disconnects == 1 && !c.pending_.valid(),
              "another quality edit during resume handshake never stops or resumes again");
          require(c.startedSettings.bitrateMbps == 30 && c.videoSettings_.bitrateMbps == 40 &&
              c.videoSaveStatus_.find(fog::constants::ui::Settings::FogHubConnectingSavedNotice) != std::string::npos,
              "controller distinguishes active handshake settings from explicitly saved next preferences"); }
        { ApplicationController c; saveThrows = true; c.changeVideoSettings(changed(c),true); saveThrows = false;
          require(c.videoSaveStatus_.find(fog::constants::ui::Settings::NotSaved) == 0 && c.pending_.valid(),
              "save failure is visible while the explicitly requested in-memory change can reconnect"); }
        { ApplicationController c; c.streamRental_.provider = fog::ProviderId::Mts;
          c.changeVideoSettings(changed(c),true);
          require(c.stream_->updates == 1 && !c.pending_.valid() && c.finishes == 0,
              "MTS continues to use its existing live settings request"); }
        for (const auto provider : {fog::ProviderId::Mts, fog::ProviderId::FogHub}) {
            ApplicationController c; c.streamRental_.provider = provider;
            auto fps = c.videoSettings_; fps.framesPerSecond = 60;
            c.changeVideoSettings(fps,true);
            require(saved.framesPerSecond == 60 && c.videoSettings_.framesPerSecond == 60 &&
                c.activeFramesPerSecond_ == 30 && c.stream_->updates == 0 &&
                !c.pending_.valid() && c.starts == 0 && c.finishes == 0,
                "FPS-only edit persists for next connection without interrupting either provider");
            require(c.videoSaveStatus_.find("60 FPS") != std::string::npos,
                "pending FPS is explicit while active stream stays at its negotiated rate");
            fps.bitrateMbps = 30; c.changeVideoSettings(fps,true);
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
          c.changeVideoSettings(next,true);
          require(saved.framesPerSecond == 30 && c.stream_->updatedSettings.framesPerSecond == 60 &&
              c.stream_->updatedSettings.quality == 2, "saving 30 never downgrades a live 60 FPS stream implicitly"); }
        require(saves > 0, "production settings persistence path exercised");
        std::cout << "Paired video settings controller: " << assertions << " assertions passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Paired video settings controller failed: " << error.what() << '\n'; return 1;
    }
}
