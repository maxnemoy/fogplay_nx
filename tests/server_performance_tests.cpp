#include "features/streaming/infrastructure/server_performance.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using Json = nlohmann::json;
using Performance = stream::ServerPerformance;

namespace {
constexpr std::array<const char*, 11> Keys{"cpu_usage", "gpu_usage", "ping", "encode", "encodefull", "grab",
    "serverfps", "serverqueuefps", "packetize", "sendsamples_video", "sendsamples_audio"};
std::string token(const std::string& line, const std::string& key) {
    const auto begin = line.find(key + "=");
    assert(begin != std::string::npos);
    const auto value = begin + key.size() + 1;
    const auto end = line.find(' ', value);
    return line.substr(value, end == std::string::npos ? end : end - value);
}
void unknown(const Performance& stats, std::uint64_t now) {
    const auto line = stats.diagnostics(now);
    assert(line.size() <= Performance::MaxDiagnosticsBytes);
    for (const auto* key : Keys) {
        assert(token(line, std::string(key) + "Known") == "0");
        assert(token(line, std::string(key) + "Raw") == "unknown");
        assert(token(line, std::string(key) + "AgeMs") == "unknown");
        assert(token(line, std::string(key) + "Stale") == "1");
    }
}
void sparseAndFreshness() {
    Performance stats;
    unknown(stats, 0);
    stats.receive(Json{{"cpu_usage", 75.25}, {"encode", "4.5"}, {"grab", "0.25"}}, 0);
    auto line = stats.diagnostics(100);
    assert(token(line, "cpu_usageKnown") == "1" && token(line, "cpu_usageRaw") == "75.25");
    assert(token(line, "encodeKnown") == "1" && token(line, "encodeRaw") == "4.5");
    assert(token(line, "grabRaw") == "0.25" && token(line, "gpu_usageKnown") == "0");
    assert(token(line, "encodeAgeMs") == "100" && token(line, "encodeStale") == "0");
    stats.receive(Json{{"serverfps", 59.94}, {"encode", "4.5 private-token"}, {"cpu_usage", 101}}, 5000);
    line = stats.diagnostics(5000);
    assert(token(line, "cpu_usageAgeMs") == "5000" && token(line, "cpu_usageStale") == "0");
    assert(token(line, "serverfpsAgeMs") == "0" && token(line, "serverfpsStale") == "0");
    line = stats.diagnostics(5001);
    assert(token(line, "cpu_usageStale") == "1" && token(line, "encodeStale") == "1");
    assert(token(line, "serverfpsAgeMs") == "1" && token(line, "serverfpsStale") == "0");
    stats.receive(Json{{"serverfps", 60}, {"unlisted", 1}}, 9000);
    line = stats.diagnostics(9000);
    assert(token(line, "encodeAgeMs") == "9000" && token(line, "encodeRaw") == "4.5");
    assert(token(line, "serverfpsAgeMs") == "0" && token(line, "serverfpsRaw") == "60");
    stats.receive(Json{{"encode", "6.25"}}, 10000);
    line = stats.diagnostics(10001);
    assert(token(line, "encodeAgeMs") == "1" && token(line, "encodeStale") == "0");
    assert(token(line, "grabAgeMs") == "10001" && token(line, "grabStale") == "1");
    line = stats.diagnostics(9999);
    assert(token(line, "encodeKnown") == "1" && token(line, "encodeAgeMs") == "unknown" &&
        token(line, "encodeStale") == "1");
}
void allowedFormsAndLimits() {
    for (const auto& value : {Json(0), Json(100), Json(100u), Json(1.25), Json(-0.0), Json("0"),
             Json("100"), Json("1.25"), Json("1e2"), Json("1E+2"), Json("1e-2"),
             Json("0.000000000000000000000000000001")}) {
        Performance stats; stats.receive(Json{{"cpu_usage", value}}, 10);
        assert(token(stats.diagnostics(10), "cpu_usageKnown") == "1");
    }
    for (const auto* key : Keys) {
        const double maximum = std::string(key) == "cpu_usage" || std::string(key) == "gpu_usage" ? 100 :
            std::string(key) == "serverfps" || std::string(key) == "serverqueuefps" ? 1000 : 1e9;
        Performance stats;
        stats.receive(Json{{key, maximum}}, 1);
        assert(token(stats.diagnostics(1), std::string(key) + "Known") == "1");
        stats.receive(Json{{key, maximum + 1}}, 2);
        assert(token(stats.diagnostics(2), std::string(key) + "AgeMs") == "1");
        assert(std::stod(token(stats.diagnostics(2), std::string(key) + "Raw")) == maximum);
        stats.receive(Json{{key, -1}}, 3);
        assert(token(stats.diagnostics(3), std::string(key) + "AgeMs") == "2");
    }
}
void malformedAndNonfinite() {
    const std::vector<Json> invalid{Json(), Json(true), Json(false), Json::array({1}), Json{{"value", 1}},
        Json(-1), Json(1e10), Json(std::numeric_limits<double>::infinity()),
        Json(-std::numeric_limits<double>::infinity()), Json(std::numeric_limits<double>::quiet_NaN()),
        Json(""), Json(" 1"), Json("1 "), Json("1\n"), Json("+1"), Json("-1"), Json("-0"),
        Json("01"), Json("00.5"), Json(".5"), Json("1."), Json("1e"), Json("1e+"), Json("1e999"), Json("1e-999"),
        Json("0x10"), Json("nan"), Json("NaN"), Json("inf"), Json("Infinity"), Json("1,25"),
        Json("1 2"), Json("1+2"), Json("1.2.3"), Json("1e2private"), Json("1ms"), Json("1%"),
        Json("\"1\""), Json(std::string("1\0", 2)), Json("١"), Json("１"), Json(std::string(33, '1'))};
    for (const auto& value : invalid) {
        Performance stats;
        stats.receive(Json{{"encode", value}}, 100);
        assert(token(stats.diagnostics(100), "encodeKnown") == "0");
        stats.receive(Json{{"encode", 1.5}}, 200);
        stats.receive(Json{{"encode", value}}, 300);
        const auto line = stats.diagnostics(400);
        assert(token(line, "encodeRaw") == "1.5" && token(line, "encodeAgeMs") == "200");
    }
    for (const auto& value : {Json(), Json(3), Json("server-private"), Json::array({Json{{"cpu_usage", 3}}}),
             Json::parse("{malformed", nullptr, false)}) {
        Performance stats; stats.receive(value, 1); unknown(stats, 2);
    }
}
void whitelistAndBounds() {
    static_assert(sizeof(Performance) <= Keys.size() * 32, "State stays fixed-size with at most eleven fields");
    Performance stats;
    Json secrets{{"cpu_usage", "35 secret-token"}, {"gpu_usage", "NVIDIA private-hardware"},
        {"address", "192.0.2.123"}, {"session", "private-session"}, {"token", "private-token"},
        {"cpu_name", "private-cpu"}, {"cpu_usage_private", 87654321}, {"CPU_USAGE", 50}};
    for (int i = 0; i < 1000; ++i) secrets["unknown_" + std::to_string(i)] = std::string(100, 'x');
    stats.receive(secrets, 0);
    unknown(stats, 1);
    auto line = stats.diagnostics(1);
    for (const auto* forbidden : {"secret", "private", "NVIDIA", "192.0.2.123", "session", "address", "token", "unknown_", "87654321"})
        assert(line.find(forbidden) == std::string::npos);
    Json all = Json::object();
    for (const auto* key : Keys) all[key] = 1.23456789e-300;
    stats.receive(all, 0);
    line = stats.diagnostics(std::numeric_limits<std::uint64_t>::max());
    assert(line.size() <= Performance::MaxDiagnosticsBytes);
    for (const auto* key : Keys) {
        assert(token(line, std::string(key) + "Known") == "1");
        assert(token(line, std::string(key) + "AgeMs") == "18446744073709551615");
        assert(token(line, std::string(key) + "Stale") == "1");
    }
    for (unsigned char ch : line) assert(ch >= 32 && ch <= 126);
    stats.reset(); unknown(stats, 99999);
}
}

int main() {
    sparseAndFreshness();
    allowedFormsAndLimits();
    malformedAndNonfinite();
    whitelistAndBounds();
    std::cout << "Server performance PASS: eleven whitelisted raw fields, independent freshness/reset, bounded numeric strings/ranges, malformed/nonfinite/overflow rejection, secret redaction and output cap\n";
}
