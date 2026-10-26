#pragma once
#include "features/streaming/infrastructure/quality_telemetry.hpp"
#include <array>
#include <cstdint>
#include <string>

namespace stream {
// No raw remote strings survive classification, except a size-bounded pong.
// Caller serializes access; stats retain individual arrival times and revisions.
class IncomingControl {
public:
    enum class Kind { Ignored, Stats, Ping, Close, Error, Invalid, Count };
    struct Message {
        Kind kind = Kind::Invalid;
        nlohmann::json data;
        std::string pong;
    };
    static Message classify(const std::string& text) {
        Message result;
        if (text.size() > 64 * 1024) return result;
        auto data = nlohmann::json::parse(text, nullptr, false);
        if (!data.is_object()) return result;
        const auto id = data.find("id");
        if (id == data.end()) { result.kind = Kind::Ignored; return result; }
        if (!id->is_string()) return result;
        if (*id == "stats") { result.kind = Kind::Stats; result.data = std::move(data); }
        else if (*id == "close") result.kind = Kind::Close;
        else if (*id == "error") result.kind = Kind::Error;
        else if (*id == "ping") {
            nlohmann::json pong{{"id", "pong"}};
            if (data.contains("ts")) pong["ts"] = data["ts"];
            result.pong = pong.dump();
            if (result.pong.size() > 64 * 1024) return result;
            result.kind = Kind::Ping;
        } else result.kind = Kind::Ignored;
        return result;
    }
    void receive(const Message& message, std::uint64_t now) {
        ++counts_[static_cast<std::size_t>(message.kind)];
        ++burst_; highBurst_ = std::max(highBurst_, burst_);
        if (!firstPending_) firstPending_ = now;
        if (message.kind == Kind::Stats) {
            if (statsPending_) ++statsCoalesced_;
            quality::readTelemetry(telemetry_, message.data, now);
            statsPending_ = true;
        }
    }
    bool takeStats(quality::Telemetry& result, std::uint64_t now) {
        if (firstPending_ && now >= firstPending_) delayHigh_ = std::max(delayHigh_, now - firstPending_);
        burst_ = 0; firstPending_ = 0;
        if (!statsPending_) return false;
        result = telemetry_; statsPending_ = false;
        return true;
    }
    void pongSent() { ++pongsSent_; }
    void pongFailed() { ++pongsFailed_; }
    std::string diagnostics() const {
        return " incomingIgnored=" + count(Kind::Ignored) + " incomingStats=" + count(Kind::Stats) +
            " incomingPing=" + count(Kind::Ping) + " incomingClose=" + count(Kind::Close) +
            " incomingError=" + count(Kind::Error) + " incomingInvalid=" + count(Kind::Invalid) +
            " incomingBurst=" + std::to_string(burst_) + " incomingBurstHigh=" + std::to_string(highBurst_) +
            " incomingDelayHighMs=" + std::to_string(delayHigh_) +
            " statsCoalesced=" + std::to_string(statsCoalesced_) +
            " pongSent=" + std::to_string(pongsSent_) + " pongFailed=" + std::to_string(pongsFailed_);
    }
private:
    std::string count(Kind kind) const { return std::to_string(counts_[static_cast<std::size_t>(kind)]); }
    quality::Telemetry telemetry_;
    std::array<std::uint64_t, static_cast<std::size_t>(Kind::Count)> counts_{};
    bool statsPending_ = false;
    std::uint64_t firstPending_ = 0, burst_ = 0, highBurst_ = 0;
    std::uint64_t delayHigh_ = 0, statsCoalesced_ = 0, pongsSent_ = 0, pongsFailed_ = 0;
};
}
