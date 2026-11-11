#include "core/constants/storage_paths.hpp"
#include "features/settings/presentation/settings_text.hpp"
#include "features/settings/presentation/rumble_text.hpp"
#include "features/settings/presentation/controller_text.hpp"
#include "features/settings/presentation/controller_settings_view.hpp"
#include "core/constants/ui_text.hpp"
#include "app/application_controller.hpp"

namespace uiText = ::fog::constants::ui;
namespace storagePaths = ::fog::constants::storage;

namespace fogplay {

void ApplicationController::inputControllerSettings(u64 keys) {
    if (keys & HidNpadButton_B) { screen_ = Screen::Settings; selection_ = 1; return; }
    if (keys & (HidNpadButton_Up | HidNpadButton_StickLUp | HidNpadButton_Down | HidNpadButton_StickLDown))
        selection_ = (selection_ + 1) % 2;
    if (!(keys & (HidNpadButton_Left | HidNpadButton_StickLLeft | HidNpadButton_Right | HidNpadButton_StickLRight | HidNpadButton_A))) return;
    auto next = controllerSettings_;
    if (selection_ == 0) next.profile = next.profile == ::stream::ControllerProfile::Xbox ?
        ::stream::ControllerProfile::DualSense : ::stream::ControllerProfile::Xbox;
    else next.xbox = !next.xbox;
    controllerSettings_ = next;
    ui_.setXboxLayout(next.xbox);
    controllerSaveStatus_ = uiText::Settings::Saving;
    try {
        settingsSaves_.submit(fog::settings::Key::Controller, [next] {
            fog::controller::saveSettings(storagePaths::path(storagePaths::ControllerSettings), next);
        }, [this](bool saved) {
            controllerSaveStatus_ = saved ? fog::controller::text::Saved : fog::controller::text::SaveFailed;
            if (!saved) diagnostic("Controller settings save failed; active preferences remain in memory.");
        });
    } catch (...) {
        controllerSaveStatus_ = fog::controller::text::SaveFailed;
        diagnostic("Controller settings save dispatch failed; active preferences remain in memory.");
    }
}

void ApplicationController::drawControllerSettings() {
    fog::ui::controllerSettings(ui_, controllerSettings_, selection_, controllerSaveStatus_);
}

void ApplicationController::inputImageSettings(u64 keys) {
    if (keys & HidNpadButton_B) { screen_ = Screen::Settings; selection_ = 0; return; }
    if (keys & (HidNpadButton_Up | HidNpadButton_StickLUp)) selection_ = (selection_ + 4) % 5;
    if (keys & (HidNpadButton_Down | HidNpadButton_StickLDown)) selection_ = (selection_ + 1) % 5;
    int direction = 0;
    if (keys & (HidNpadButton_Left | HidNpadButton_StickLLeft)) direction = -1;
    else if (keys & (HidNpadButton_Right | HidNpadButton_StickLRight | HidNpadButton_A)) direction = 1;
    if (!direction) return;
    const std::array<fog::video::Field, 5> fields{
        fog::video::Field::Codec, fog::video::Field::Quality, fog::video::Field::Bitrate,
        fog::video::Field::Keyframes, fog::video::Field::FramesPerSecond};
    auto value = videoSettings_;
    if (fog::video::adjust(value, fields[selection_], direction, playerMediaFactory_.supportsH265()))
        changeVideoSettings(value, false);
    else if (selection_ == 0) videoSaveStatus_ = uiText::Settings::H265Fallback;
}

void ApplicationController::drawImageSettings() {
    const auto area = ui_.content();
    const std::array<std::string, 5> labels{
        uiText::Settings::CodecPrefix + fog::ui::videoCodecLabel(videoSettings_),
        uiText::Settings::QualityPrefix + fog::ui::videoQualityLabel(videoSettings_),
        uiText::Settings::BitrateLimitPrefix + std::to_string(videoSettings_.bitrateMbps) + uiText::Common::MegabitsSuffix,
        uiText::Settings::KeyframesPrefix + fog::ui::videoKeyframeLabel(videoSettings_),
        uiText::Settings::FrameRatePrefix + fog::ui::videoFrameRateLabel(videoSettings_)};
    const std::array<std::string, 5> hints{
        playerMediaFactory_.supportsH265() ? uiText::Settings::H265ExperimentalNotice : uiText::Settings::H265Unavailable,
        uiText::Settings::QualityDescription,
        uiText::Settings::BitrateDescription,
        uiText::Settings::KeyframeDescription,
        uiText::Settings::FrameRateDescription};
    for (std::size_t i = 0; i < labels.size(); ++i) {
        const int y = 145 + static_cast<int>(i) * 84;
        ui_.button(labels[i], {area.x, y, area.w, 49}, selection_ == i);
        ui_.text(hints[i], area.x + 14, y + 54, area.w - 28, muted);
    }
    ui_.text(uiText::Settings::FogHubVideoDescription, area.x, 578, area.w, muted);
    if (!videoSaveStatus_.empty() && videoSaveStatus_ != uiText::Settings::AutoSaveNotice &&
        videoSaveStatus_ != uiText::Settings::Saved)
        ui_.text(videoSaveStatus_, area.x, 610, area.w, accent, false, 2);
}

void ApplicationController::inputConnectionSettings(u64 keys) {
    if (keys & HidNpadButton_B) { screen_ = Screen::Settings; selection_ = 2; return; }
    int direction = 0;
    if (keys & (HidNpadButton_Left | HidNpadButton_StickLLeft)) direction = -1;
    else if (keys & (HidNpadButton_Right | HidNpadButton_StickLRight | HidNpadButton_A)) direction = 1;
    auto next = connectionSettings_;
    if (!fog::connection::adjust(next, direction)) return;
    connectionSettings_ = next;
    connectionSaveStatus_ = uiText::Settings::Saving;
    try {
        settingsSaves_.submit(fog::settings::Key::Connection, [next] {
            fog::connection::saveSettings(storagePaths::path(storagePaths::ConnectionSettings), next);
        }, [this](bool saved) {
            connectionSaveStatus_ = saved ? uiText::Settings::ConnectionSaved : uiText::Settings::ConnectionSaveFailed;
            if (!saved) diagnostic("Connection settings save failed; active preferences remain in memory.");
        });
    } catch (...) {
        connectionSaveStatus_ = uiText::Settings::ConnectionSaveFailed;
        diagnostic("Connection settings save dispatch failed; active preferences remain in memory.");
    }
}

void ApplicationController::drawConnectionSettings() {
    const auto area = ui_.content();
    ui_.button(uiText::Settings::ConnectionModePrefix + fog::ui::connectionModeLabel(connectionSettings_), {area.x, 154, area.w, 57}, true);
    ui_.text(uiText::Settings::NormalConnectionDescription, area.x + 14, 240, area.w - 28, muted);
    ui_.text(uiText::Settings::AutoConnectionDescription, area.x + 14, 310, area.w - 28, muted);
    ui_.text(uiText::Settings::ReconnectCountingDescription, area.x + 14, 365, area.w - 28, muted, false, 2);
    ui_.text(uiText::Settings::TurnConnectionDescription, area.x + 14, 440, area.w - 28, muted);
    if (!connectionSaveStatus_.empty() && connectionSaveStatus_ != uiText::Settings::ConnectionAutoSaveNotice &&
        connectionSaveStatus_ != uiText::Settings::ConnectionSaved)
        ui_.text(connectionSaveStatus_, area.x, 560, area.w, accent, false, 2);
}

void ApplicationController::changeRumbleSettings(const fog::rumble::Settings& value) {
    if (!fog::rumble::valid(value)) return;
    rumbleSettings_ = value;
    const auto nowMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    rumble_.setMode(value.mode, nowMs);
    rumbleSaveStatus_ = uiText::Settings::Saving;
    try {
        settingsSaves_.submit(fog::settings::Key::Rumble, [value] {
            fog::rumble::saveSettings(storagePaths::path(storagePaths::RumbleSettings), value);
        }, [this](bool saved) {
            rumbleSaveStatus_ = saved ? fog::rumble::text::Saved : fog::rumble::text::SaveFailed;
            overlay_.setRumbleStatus(rumbleSaveStatus_);
            if (!saved) diagnostic("Rumble settings save failed; active preferences remain in memory.");
        });
    } catch (...) {
        rumbleSaveStatus_ = fog::rumble::text::SaveFailed;
        diagnostic("Rumble settings save dispatch failed; active preferences remain in memory.");
    }
    overlay_.setRumbleSettings(rumbleSettings_);
    overlay_.setRumbleStatus(rumbleSaveStatus_);
}

void ApplicationController::inputRumbleSettings(u64 keys) {
    if (keys & HidNpadButton_B) { screen_ = Screen::Settings; selection_ = 5; return; }
    int direction = 0;
    if (keys & (HidNpadButton_Left | HidNpadButton_StickLLeft)) direction = -1;
    else if (keys & (HidNpadButton_Right | HidNpadButton_StickLRight | HidNpadButton_A)) direction = 1;
    auto next = rumbleSettings_;
    if (fog::rumble::adjust(next, direction)) changeRumbleSettings(next);
}

void ApplicationController::drawRumbleSettings() {
    const auto area = ui_.content();
    ui_.button(fog::rumble::text::label(rumbleSettings_.mode), {area.x, 154, area.w, 57}, true);
    ui_.text(fog::rumble::text::description(rumbleSettings_.mode), area.x + 14, 240, area.w - 28, muted, false, 3);
    ui_.text(fog::rumble::text::Shared, area.x + 14, 360, area.w - 28, muted, false, 2);
    if (!rumbleSaveStatus_.empty()) ui_.text(rumbleSaveStatus_, area.x, 530, area.w, accent, false, 3);
}

}
