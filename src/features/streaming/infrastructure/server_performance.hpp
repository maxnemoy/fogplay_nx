#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace stream {
// Diagnostic-only snapshots of the server's reported stats. Raw values have no
// inferred units and are not input acknowledgements or control-policy evidence.
// Caller serializes receive/diagnostics/reset and supplies monotonic milliseconds.
class ServerPerformance {
public:
    static constexpr std::uint64_t StaleAfterMs = 5000;
    static constexpr std::size_t MaxDiagnosticsBytes = 1600;
    static constexpr std::size_t MaxNumericStringBytes = 32;

    void receive(const nlohmann::json& data, std::uint64_t now) noexcept {
        if (!data.is_object()) return;
        for (std::size_t i = 0; i < Definitions.size(); ++i) {
            try {
                const auto item = data.find(Definitions[i].key);
                double value = 0;
                if (item != data.end() && numeric(*item, Definitions[i].maximum, value))
                    fields_[i] = {value, now, true};
            } catch (...) {
                // Optional diagnostics must not affect the stream or refresh old evidence.
            }
        }
    }

    void reset() noexcept { fields_ = {}; }

    std::string diagnostics(std::uint64_t now) const {
        std::string result = "serverPerf=reported-raw freshnessMs=" + std::to_string(StaleAfterMs);
        result.reserve(MaxDiagnosticsBytes);
        for (std::size_t i = 0; i < Definitions.size(); ++i) {
            const auto& field = fields_[i];
            const auto* key = Definitions[i].key;
            char value[32] = "unknown", age[32] = "unknown", entry[160];
            if (field.known) std::snprintf(value, sizeof(value), "%.9g", field.value);
            const bool ageKnown = field.known && now >= field.at;
            const auto elapsed = ageKnown ? now - field.at : 0;
            if (ageKnown) std::snprintf(age, sizeof(age), "%llu", static_cast<unsigned long long>(elapsed));
            const bool stale = !ageKnown || elapsed > StaleAfterMs;
            const int written = std::snprintf(entry, sizeof(entry), " %sKnown=%d %sRaw=%s %sAgeMs=%s %sStale=%d",
                key, field.known ? 1 : 0, key, value, key, age, key, stale ? 1 : 0);
            if (written > 0) result.append(entry, std::min({static_cast<std::size_t>(written),
                sizeof(entry) - 1, MaxDiagnosticsBytes - result.size()}));
        }
        return result;
    }
private:
    struct Definition { const char* key; double maximum; };
    inline static constexpr std::array<Definition, 11> Definitions{{
        {"cpu_usage", 100}, {"gpu_usage", 100}, {"ping", 1e9}, {"encode", 1e9},
        {"encodefull", 1e9}, {"grab", 1e9}, {"serverfps", 1000}, {"serverqueuefps", 1000},
        {"packetize", 1e9}, {"sendsamples_video", 1e9}, {"sendsamples_audio", 1e9}
    }};
    struct Field { double value = 0; std::uint64_t at = 0; bool known = false; };
    std::array<Field, Definitions.size()> fields_{};

    static bool numeric(const nlohmann::json& data, double maximum, double& result) {
        if (data.is_number()) result = data.get<double>();
        else if (data.is_string()) {
            const auto& text = data.get_ref<const std::string&>();
            if (text.empty() || text.size() > MaxNumericStringBytes || text.front() < '0' || text.front() > '9') return false;
            for (const unsigned char ch : text)
                if (!((ch >= '0' && ch <= '9') || ch == '.' || ch == 'e' || ch == 'E' || ch == '+' || ch == '-')) return false;
            // Strict JSON scalar parsing rejects leading zeros, incomplete
            // exponents, nonnumeric suffixes and overflow; input is at most 32 bytes.
            const auto number = nlohmann::json::parse(text, nullptr, false);
            if (!number.is_number()) return false;
            result = number.get<double>();
            if (result == 0) {
                for (const auto ch : text) {
                    if (ch == 'e' || ch == 'E') break;
                    if (ch >= '1' && ch <= '9') return false; // Underflow must not become a reported zero.
                }
            }
        } else return false;
        if (!std::isfinite(result) || result < 0 || result > maximum) return false;
        if (result == 0) result = 0; // Normalize numeric negative zero in diagnostic output.
        return true;
    }
};
}
