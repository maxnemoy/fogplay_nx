#include "core/constants/storage_paths.hpp"
#include "features/settings/presentation/settings_text.hpp"
#include "core/constants/ui_text.hpp"
#include "app/application_controller.hpp"

namespace uiText = ::fog::constants::ui;
namespace storagePaths = ::fog::constants::storage;

namespace fogplay {

void ApplicationController::changeVideoSettings(const fog::video::Settings& value, bool live) {
    videoSettings_ = value;
    bool saved = false;
    try {
        fog::video::saveSettings(storagePaths::path(storagePaths::VideoSettings), videoSettings_);
        saved = true;
    } catch (...) { diagnostic("Video settings save failed; no settings payload logged."); }
    videoSaveStatus_ = saved ? uiText::Settings::Saved : uiText::Settings::NotSaved;
    if (live) {
        const bool requested = stream_ && stream_->updateVideoSettings(videoSettings_);
        videoSaveStatus_ += requested ? uiText::Settings::RequestSentNotice :
            uiText::Settings::RequestNotSentNotice;
    }
    overlay_.setVideoSettings(videoSettings_);
    overlay_.setSaveStatus(videoSaveStatus_);
}

void ApplicationController::inputImageSettings(u64 keys) {
    if (keys & HidNpadButton_B) { screen_ = Screen::Settings; selection_ = 0; return; }
    if (keys & (HidNpadButton_Up | HidNpadButton_StickLUp)) selection_ = (selection_ + 3) % 4;
    if (keys & (HidNpadButton_Down | HidNpadButton_StickLDown)) selection_ = (selection_ + 1) % 4;
    int direction = 0;
    if (keys & (HidNpadButton_Left | HidNpadButton_StickLLeft)) direction = -1;
    else if (keys & (HidNpadButton_Right | HidNpadButton_StickLRight | HidNpadButton_A)) direction = 1;
    if (!direction) return;
    const std::array<fog::video::Field, 4> fields{
        fog::video::Field::Codec, fog::video::Field::Quality, fog::video::Field::Bitrate, fog::video::Field::Keyframes};
    auto value = videoSettings_;
    if (fog::video::adjust(value, fields[selection_], direction, fog::Media::supportsH265()))
        changeVideoSettings(value, false);
    else if (selection_ == 0) videoSaveStatus_ = uiText::Settings::H265Fallback;
}

void ApplicationController::drawImageSettings() {
    const auto area = ui_.content();
    const std::array<std::string, 4> labels{
        uiText::Settings::CodecPrefix + fog::ui::videoCodecLabel(videoSettings_),
        uiText::Settings::QualityPrefix + fog::ui::videoQualityLabel(videoSettings_),
        uiText::Settings::BitrateLimitPrefix + std::to_string(videoSettings_.bitrateMbps) + uiText::Common::MegabitsSuffix,
        uiText::Settings::KeyframesPrefix + fog::ui::videoKeyframeLabel(videoSettings_)};
    const std::array<std::string, 4> hints{
        fog::Media::supportsH265() ? uiText::Settings::H265ExperimentalNotice : uiText::Settings::H265Unavailable,
        uiText::Settings::QualityDescription,
        uiText::Settings::BitrateDescription,
        uiText::Settings::KeyframeDescription};
    for (std::size_t i = 0; i < labels.size(); ++i) {
        const int y = 145 + static_cast<int>(i) * 108;
        ui_.button(labels[i], {area.x, y, area.w, 57}, selection_ == i);
        ui_.text(hints[i], area.x + 14, y + 65, area.w - 28, muted);
    }
    if (!videoSaveStatus_.empty() && videoSaveStatus_ != uiText::Settings::AutoSaveNotice &&
        videoSaveStatus_ != uiText::Settings::Saved)
        ui_.text(videoSaveStatus_, area.x, 596, area.w, accent, false, 2);
}

void ApplicationController::inputConnectionSettings(u64 keys) {
    if (keys & HidNpadButton_B) { screen_ = Screen::Settings; selection_ = 2; return; }
    int direction = 0;
    if (keys & (HidNpadButton_Left | HidNpadButton_StickLLeft)) direction = -1;
    else if (keys & (HidNpadButton_Right | HidNpadButton_StickLRight | HidNpadButton_A)) direction = 1;
    auto next = connectionSettings_;
    if (!fog::connection::adjust(next, direction)) return;
    try {
        fog::connection::saveSettings(storagePaths::path(storagePaths::ConnectionSettings), next);
        connectionSettings_ = next;
        connectionSaveStatus_ = uiText::Settings::ConnectionSaved;
    } catch (...) {
        connectionSaveStatus_ = uiText::Settings::ConnectionSaveFailed;
        diagnostic("Connection settings save failed; no settings payload logged.");
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

}
