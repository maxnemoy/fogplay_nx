#pragma once
#include "features/streaming/domain/rumble.hpp"
#include "features/streaming/infrastructure/incoming_control_diagnostics.hpp"
#include "features/streaming/infrastructure/quality_telemetry.hpp"
#include <array>
#include <cstdint>
#include <string>

namespace stream {
// Payload strings are excluded from diagnostics; only bounded event/field names
// and explicitly allowed motor values survive, plus a size-bounded pong.
// Caller serializes access; stats retain individual arrival times and revisions.
class IncomingControl {
public:
    enum class Kind { Ignored, Stats, Ping, Rumble, InvalidRumble, Close, Error, Invalid, Count };
    static const char* kindName(Kind kind) {
        switch (kind) {
            case Kind::Ignored: return "ignored";
            case Kind::Stats: return "stats";
            case Kind::Ping: return "ping";
            case Kind::Rumble: return "rumble";
            case Kind::InvalidRumble: return "invalid-rumble";
            case Kind::Close: return "close";
            case Kind::Error: return "error";
            default: return "invalid";
        }
    }
    struct Message {
        Kind kind = Kind::Invalid;
        nlohmann::json data;
        std::string pong;
        Rumble rumble;
        IncomingControlDiagnostics::Description diagnostic;
    };
    static Message classify(const std::string& text) {
        Message result;
        if (text.size() > 64 * 1024) {
            try { result.diagnostic = {"oversized", "event=<oversized> bytes=" + std::to_string(text.size())}; }
            catch (...) {} // Optional logging cannot turn into a transport failure.
            return result;
        }
        auto data = nlohmann::json::parse(text, nullptr, false);
        try { result.diagnostic = IncomingControlDiagnostics::describe(data, text.size()); }
        catch (...) {}
        if (!data.is_object()) return result;
        const auto id = data.find("id");
        if (id == data.end()) { result.kind = Kind::Ignored; return result; }
        if (!id->is_string()) return result;
        if (*id == "stats") { result.kind = Kind::Stats; result.data = std::move(data); }
        else if (*id == "gamepad_notification") {
            result.kind = Kind::InvalidRumble;
            unsigned index = 0, large = 0, small = 0;
            if (!readByte(data, "index", index) || index != 0 ||
                !readByte(data, "large_motor", large) || !readByte(data, "small_motor", small)) return result;
            result.rumble.controller = static_cast<std::uint16_t>(index);
            result.rumble.lowFrequency = static_cast<std::uint16_t>(large * 257);
            result.rumble.highFrequency = static_cast<std::uint16_t>(small * 257);
            // MTS sends no duration. Match the Android receiver's bounded pulse.
            result.rumble.durationMs = std::max(large, small) * 20;
            result.kind = Kind::Rumble;
        } else if (*id == "close") result.kind = Kind::Close;
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
            " incomingRumble=" + count(Kind::Rumble) + " incomingInvalidRumble=" + count(Kind::InvalidRumble) +
            " incomingBurst=" + std::to_string(burst_) + " incomingBurstHigh=" + std::to_string(highBurst_) +
            " incomingDelayHighMs=" + std::to_string(delayHigh_) +
            " statsCoalesced=" + std::to_string(statsCoalesced_) +
            " pongSent=" + std::to_string(pongsSent_) + " pongFailed=" + std::to_string(pongsFailed_);
    }
private:
    static bool readByte(const nlohmann::json& data, const char* key, unsigned& value) {
        const auto item = data.find(key);
        if (item == data.end() || !item->is_number_integer()) return false;
        // Compare before conversion so huge unsigned integers cannot wrap.
        if (*item < 0 || *item > 255) return false;
        value = item->get<unsigned>();
        return true;
    }
    std::string count(Kind kind) const { return std::to_string(counts_[static_cast<std::size_t>(kind)]); }
    quality::Telemetry telemetry_;
    std::array<std::uint64_t, static_cast<std::size_t>(Kind::Count)> counts_{};
    bool statsPending_ = false;
    std::uint64_t firstPending_ = 0, burst_ = 0, highBurst_ = 0;
    std::uint64_t delayHigh_ = 0, statsCoalesced_ = 0, pongsSent_ = 0, pongsFailed_ = 0;
};
}
