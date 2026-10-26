#pragma once
#include "core/constants/ui_text.hpp"
#include "features/settings/domain/connection_settings.hpp"
#include "features/settings/domain/video_settings.hpp"
#include <array>
#include <string>

namespace fog::ui {
inline std::string videoCodecLabel(const video::Settings& value) {
    return value.codec == ::stream::VideoCodec::H265 ? constants::ui::Settings::H265Codec : constants::ui::Settings::H264Codec;
}
inline std::string videoQualityLabel(const video::Settings& value) {
    namespace text = constants::ui::Settings;
    constexpr std::array<const char*, 5> labels{text::AutoQuality, text::LowQuality, text::MediumQuality, text::HighQuality, text::UltraQuality};
    return value.quality >= 0 && value.quality <= 4 ? labels[static_cast<std::size_t>(value.quality)] : text::AutoQuality;
}
inline std::string videoKeyframeLabel(const video::Settings& value) {
    return value.keyframeSeconds ? constants::ui::Settings::KeyframeIntervalPrefix + std::to_string(value.keyframeSeconds) +
        constants::ui::Settings::KeyframeIntervalSuffix : constants::ui::Settings::KeyframesOnLoss;
}
inline std::string connectionModeLabel(const connection::Settings& value) {
    if (value.mode == connection::Mode::Auto) return constants::ui::Settings::AutoQuality;
    if (value.mode == connection::Mode::Relay) return constants::ui::Settings::RelayMode;
    return constants::ui::Settings::NormalMode;
}
}
