#pragma once
#include "features/streaming/domain/auto_quality.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <optional>

namespace stream::quality {
inline std::optional<double> telemetryNumber(const nlohmann::json& data, const char* key,
                                             double low, double high, double divisor = 1) {
    const auto it = data.find(key);
    if (it == data.end() || !it->is_number()) return std::nullopt;
    const double value = it->get<double>();
    if (!std::isfinite(value) || value < low || value > high) return std::nullopt;
    return value / divisor;
}
inline std::optional<TelemetryUpdate> parseTelemetry(const nlohmann::json& data) {
    if (!data.is_object()) return std::nullopt;
    TelemetryUpdate update;
    const auto p = data.find("preset");
    if (p != data.end() && p->is_string()) {
        if (*p == "LOW") update.profile = Profile::Low;
        else if (*p == "MID") update.profile = Profile::Middle;
        else if (*p == "HIGH") update.profile = Profile::High;
        else if (*p == "ULTRA") update.profile = Profile::Ultra;
    }
    update.targetMbps = telemetryNumber(data, "target_bitrate", 0, 1000000000.0, 1000000.0);
    update.fps = telemetryNumber(data, "fps", 1, 240);
    const auto a = data.find("autoquality");
    if (a != data.end() && a->is_boolean()) update.automatic = a->get<bool>();
    const auto c = data.find("server_codec");
    if (c != data.end() && c->is_string()) {
        if (*c == "H264" || *c == "H.264" || *c == "h264") update.codec = "H264";
        else if (*c == "H265" || *c == "H.265" || *c == "h265" || *c == "HEVC" || *c == "hevc") update.codec = "H265";
    }
    return update;
}
inline void readTelemetry(Telemetry& telemetry, const nlohmann::json& data, std::uint64_t now) {
    if (auto update = parseTelemetry(data)) telemetry.apply(*update, now);
}
}
