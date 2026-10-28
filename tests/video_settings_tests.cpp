#include "features/settings/domain/video_settings.hpp"
#include "features/settings/infrastructure/video_settings_storage.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
namespace {
int assertions = 0;
void require(bool value, const char* message) {
    ++assertions;
    if (!value) throw std::runtime_error(message);
}
bool rejects(const std::string& path) {
    try { fog::video::loadSettings(path); return false; } catch (...) { return true; }
}
}
int main() {
    using namespace fog::video;
    const auto dir = std::filesystem::temp_directory_path() / ("fog-video-settings-" + std::to_string(getpid()));
    std::filesystem::create_directory(dir);
    const auto path = (dir / "video-settings.json").string();
    try {
        const Settings defaults;
        require(loadSettings(path) == defaults, "absent file uses defaults");
        require(defaults.codec == ::stream::VideoCodec::H265 && defaults.bitrateMbps == 20 &&
            defaults.quality == 0 && defaults.keyframeSeconds == 0 && defaults.framesPerSecond == 30, "documented defaults");
        Settings savedH264;
        savedH264.codec = ::stream::VideoCodec::H264;
        saveSettings(path, savedH264);
        require(loadSettings(path).codec == ::stream::VideoCodec::H264, "saved H264 preserved despite new HEVC default");
        Settings selected;
        selected.codec = ::stream::VideoCodec::H265;
        for (const int fps : FrameRates) for (const int bitrate : Bitrates) for (const int interval : KeyframeIntervals) for (int quality = 0; quality < 5; ++quality) {
            selected.bitrateMbps = bitrate; selected.keyframeSeconds = interval; selected.quality = quality;
            selected.framesPerSecond = fps;
            saveSettings(path, selected);
            require(loadSettings(path) == selected, "roundtrip supported choices");
        }
        auto content = nlohmann::json::parse(std::ifstream(path));
        require(content.at("version") == 2 && content.at("framesPerSecond") == 60, "new saves use version two with frame rate");
        auto legacy = content; legacy["version"] = 1; legacy.erase("framesPerSecond");
        std::ofstream(path) << legacy.dump();
        auto migrated = selected; migrated.framesPerSecond = 30;
        require(loadSettings(path) == migrated, "legacy file keeps all preferences and defaults to thirty FPS");
        saveSettings(path, loadSettings(path));
        require(nlohmann::json::parse(std::ifstream(path)).at("version") == 2, "legacy preferences migrate on next save");
        auto missing = content; missing.erase("framesPerSecond");
        std::ofstream(path) << missing.dump(); require(rejects(path), "version two requires explicit frame rate");
        auto legacyExtra = content; legacyExtra["version"] = 1;
        std::ofstream(path) << legacyExtra.dump(); require(rejects(path), "legacy version rejects new or unknown keys");
        const std::pair<const char*, nlohmann::json> invalid[]{
            {"version", 3}, {"version", 2.0}, {"codec", "AV1"}, {"codec", 1},
            {"bitrateMbps", 0}, {"bitrateMbps", 21}, {"bitrateMbps", 20.0},
            {"bitrateMbps", 4294967316ULL}, {"keyframeSeconds", -1}, {"keyframeSeconds", 1},
            {"keyframeSeconds", 4294967296ULL}, {"quality", -1}, {"quality", 5}, {"quality", true},
            {"quality", 4294967296ULL}, {"framesPerSecond", 0}, {"framesPerSecond", 45},
            {"framesPerSecond", 30.0}, {"framesPerSecond", true}, {"framesPerSecond", "60"},
            {"framesPerSecond", 4294967326ULL}};
        for (const auto& entry : invalid) {
            auto corrupt = content; corrupt[entry.first] = entry.second; std::ofstream(path) << corrupt.dump();
            require(rejects(path), "invalid or overflowing values rejected");
        }
        for (const auto& bytes : {"{", "null", "[]", "{}", ""}) {
            std::ofstream(path) << bytes; require(rejects(path), "malformed file rejected");
        }
        auto extra = content; extra["token"] = "invalid-extra-field";
        std::ofstream(path) << extra.dump(); require(rejects(path), "unknown fields rejected");
        std::ofstream(path) << std::string(1025, ' '); require(rejects(path), "bounded storage payload");
        std::filesystem::remove(path);
        std::filesystem::create_directory(path); require(rejects(path), "directory rejected");
        std::filesystem::remove(path);
        saveSettings(path + ".bak", selected);
        require(loadSettings(path) == selected && !std::filesystem::exists(path + ".bak"), "backup recovered");
        Settings bad; bad.bitrateMbps = 22;
        bool failed = false;
        try { saveSettings(path, bad); } catch (...) { failed = true; }
        require(failed && loadSettings(path) == selected, "invalid save preserves file");
        failed = false;
        try { saveSettings((dir / "missing" / "video-settings.json").string(), defaults); } catch (...) { failed = true; }
        require(failed, "save failure observable");
        Settings value;
        value.codec = ::stream::VideoCodec::H264;
        require(!adjust(value, Field::Codec, 1, false), "unsupported H265 cannot be selected");
        require(adjust(value, Field::Codec, 1) && value.codec == ::stream::VideoCodec::H265, "available H265 opt-in");
        require(adjust(value, Field::Quality, -1) && value.quality == 4, "quality backward wrap");
        require(adjust(value, Field::Bitrate, -1) && value.bitrateMbps == 15, "bitrate choice backward");
        require(adjust(value, Field::Keyframes, -1) && value.keyframeSeconds == 10, "keyframe choice backward");
        require(adjust(value, Field::FramesPerSecond, 1) && value.framesPerSecond == 60, "frame rate switches to sixty");
        require(adjust(value, Field::FramesPerSecond, -1) && value.framesPerSecond == 30, "frame rate switches back to thirty");
        auto otherFps = value; otherFps.framesPerSecond = 60;
        require(otherFps != value, "frame rate participates in equality");
        otherFps.framesPerSecond = 45; require(!valid(otherFps), "only documented frame rates valid");
        require(!adjust(value, Field::Bitrate, 0) && !adjust(bad, Field::Quality, 1), "invalid and zero changes ignored");
        std::filesystem::remove_all(dir);
        std::cout << "Video settings tests passed: " << assertions << " assertions\n"; return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(dir);
        std::cerr << "Video settings tests failed: " << error.what() << '\n'; return 1;
    }
}
