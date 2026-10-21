#pragma once
#include "features/streaming/domain/video_codec.hpp"
#include <algorithm>
#include <array>
#include <string>

namespace fog::video {
struct Settings {
    ::stream::VideoCodec codec = ::stream::VideoCodec::H264;
    int bitrateMbps = 20;
    int keyframeSeconds = 0;
    int quality = 0;
};
enum class Field { Codec, Quality, Bitrate, Keyframes };
inline constexpr std::array<int, 6> Bitrates{5, 10, 15, 20, 30, 40};
inline constexpr std::array<int, 4> KeyframeIntervals{0, 2, 5, 10};
inline bool valid(const Settings& value) {
    return (value.codec == ::stream::VideoCodec::H264 || value.codec == ::stream::VideoCodec::H265) &&
        std::find(Bitrates.begin(), Bitrates.end(), value.bitrateMbps) != Bitrates.end() &&
        std::find(KeyframeIntervals.begin(), KeyframeIntervals.end(), value.keyframeSeconds) != KeyframeIntervals.end() &&
        value.quality >= 0 && value.quality <= 4;
}
inline bool operator==(const Settings& a, const Settings& b) {
    return a.codec == b.codec && a.bitrateMbps == b.bitrateMbps &&
        a.keyframeSeconds == b.keyframeSeconds && a.quality == b.quality;
}
inline bool operator!=(const Settings& a, const Settings& b) { return !(a == b); }
inline bool adjust(Settings& value, Field field, int direction, bool allowH265 = true) {
    if (!valid(value) || direction == 0) return false;
    const auto before = value;
    const int step = direction > 0 ? 1 : -1;
    switch (field) {
        case Field::Codec:
            if (allowH265) value.codec = value.codec == ::stream::VideoCodec::H264 ?
                ::stream::VideoCodec::H265 : ::stream::VideoCodec::H264;
            break;
        case Field::Quality: value.quality = (value.quality + step + 5) % 5; break;
        case Field::Bitrate: {
            const auto index = std::find(Bitrates.begin(), Bitrates.end(), value.bitrateMbps) - Bitrates.begin();
            value.bitrateMbps = Bitrates[static_cast<std::size_t>((index + step + 6) % 6)];
            break;
        }
        case Field::Keyframes: {
            const auto index = std::find(KeyframeIntervals.begin(), KeyframeIntervals.end(), value.keyframeSeconds) - KeyframeIntervals.begin();
            value.keyframeSeconds = KeyframeIntervals[static_cast<std::size_t>((index + step + 4) % 4)];
            break;
        }
    }
    return value != before;
}
inline std::string codecLabel(const Settings& value) {
    return value.codec == ::stream::VideoCodec::H265 ? "H.265 / HEVC · экспериментально" : "H.264";
}
inline std::string qualityLabel(const Settings& value) {
    const std::array<const char*, 5> labels{"Авто", "Низкое", "Среднее", "Высокое", "Ультра"};
    return value.quality >= 0 && value.quality <= 4 ? labels[static_cast<std::size_t>(value.quality)] : "Авто";
}
inline std::string keyframeLabel(const Settings& value) {
    return value.keyframeSeconds ? "Запрос каждые " + std::to_string(value.keyframeSeconds) + " с" : "Только при потере (рекомендуется)";
}
// Missing file uses defaults. Invalid/unreadable files throw a fixed safe error;
// the caller retains defaults and displays a warning. Authentication is separate.
}
