#include "core/constants/storage_paths.hpp"
#include "features/settings/infrastructure/video_settings_storage.hpp"
#include "features/settings/presentation/settings_text.hpp"
#include "app/application_controller.hpp"

namespace uiText = ::fog::constants::ui;
namespace storagePaths = ::fog::constants::storage;

namespace fogplay {

void ApplicationController::changeVideoSettings(const fog::video::Settings& value, bool live) {
    if (!fog::video::valid(value)) return;
    const bool foghub = live && player_.snapshot().provider == fog::ProviderId::FogHub;
    const auto previous = videoSettings_;
    if (foghub) {
        // The FH overlay owns codec/bitrate/FPS. Its normalized transport
        // settings must not erase the shared MTS quality or PLI preferences.
        videoSettings_.codec = value.codec;
        videoSettings_.bitrateMbps = value.bitrateMbps;
        videoSettings_.framesPerSecond = value.framesPerSecond;
    } else {
        videoSettings_ = value;
    }
    videoSaveStatus_ = uiText::Settings::Saving;
    try {
        settingsSaves_.submit(fog::settings::Key::Video, [value = videoSettings_] {
            fog::video::saveSettings(storagePaths::path(storagePaths::VideoSettings), value);
        }, [this](bool saved) {
            // Preserve notices about active transport/FPS while replacing only
            // the persistence prefix owned by this latest settings revision.
            const std::string pending = uiText::Settings::Saving;
            const auto notice = videoSaveStatus_.compare(0, pending.size(), pending) == 0 ?
                videoSaveStatus_.substr(pending.size()) : std::string{};
            videoSaveStatus_ = std::string(saved ? uiText::Settings::Saved : uiText::Settings::NotSaved) + notice;
            overlay_.setSaveStatus(videoSaveStatus_);
            if (!saved) diagnostic("Video settings save failed; active preferences remain in memory.");
        });
    } catch (...) {
        videoSaveStatus_ = uiText::Settings::NotSaved;
        diagnostic("Video settings save dispatch failed; active preferences remain in memory.");
    }
    if (live) {
        const auto result = player_.changeVideo(videoSettings_,playerAdmission(),std::chrono::steady_clock::now());
        using Result = fog::stream::Player::QualityResult;
        if (foghub) {
            if (result == Result::Inspection || result == Result::Coalesced) videoSaveStatus_ += uiText::Settings::FogHubReconnectNotice;
            else if (result == Result::Busy) videoSaveStatus_ += uiText::Settings::FogHubReconnectBusyNotice;
            else if (result == Result::Connecting) videoSaveStatus_ += uiText::Settings::FogHubConnectingSavedNotice;
        } else {
            auto active = videoSettings_, old = previous;
            active.framesPerSecond = old.framesPerSecond = player_.snapshot().framesPerSecond;
            if (active != old) videoSaveStatus_ += result == Result::Applied ? uiText::Settings::RequestSentNotice : uiText::Settings::RequestNotSentNotice;
        }
        applyPlayerEffects();
    }
    if (live && videoSettings_.framesPerSecond != player_.snapshot().framesPerSecond)
        videoSaveStatus_ += " · " + fog::ui::videoFrameRateLabel(videoSettings_) + uiText::Settings::FrameRateNextConnectionSuffix;
    overlay_.setVideoSettings(videoSettings_);
    overlay_.setSaveStatus(videoSaveStatus_);
}

}
