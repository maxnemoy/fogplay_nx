#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace stream {
// Caller serializes receive/poll. Only bounded, redacted descriptions survive;
// schema buckets replace a queue so remote event bursts cannot grow memory.
class IncomingControlDiagnostics {
public:
    struct Description { std::string key, sample; };
    static Description describe(const nlohmann::json& data, std::size_t bytes) {
        std::string id = "<missing>";
        bool rumble = false;
        if (data.is_object()) {
            const auto item = data.find("id");
            if (item != data.end()) {
                id = item->is_string() ? label(item->get_ref<const std::string&>()) : "<invalid>";
                rumble = id == "gamepad_notification";
            }
        }
        std::size_t schemaBudget = 12, sampleBudget = 12;
        const auto prefix = "id=" + id;
        Description result;
        result.key = prefix + " schema=" + shape(data, 0, schemaBudget, false, false);
        result.sample = prefix + " bytes=" + std::to_string(bytes) + " fields=" +
            shape(data, 0, sampleBudget, true, rumble);
        return result;
    }
    void receive(const Description& description, std::uint64_t nowMs) {
        ++received_;
        for (auto& bucket : buckets_) {
            if (bucket.key != description.key) continue;
            if (bucket.count != bucket.reported) ++coalesced_;
            ++bucket.count; bucket.last = nowMs;
            bucket.sample = bounded(description.sample, 800);
            return;
        }
        if (buckets_.size() == 32) { ++overflow_; return; }
        Bucket bucket;
        bucket.key = bounded(description.key, 800);
        bucket.sample = bounded(description.sample, 800);
        bucket.count = 1; bucket.first = bucket.last = nowMs;
        buckets_.push_back(std::move(bucket));
    }
    bool poll(std::string& event, std::uint64_t nowMs, bool force = false) {
        for (std::size_t n = 0; n < buckets_.size(); ++n) {
            const auto index = (cursor_ + n) % buckets_.size();
            auto& bucket = buckets_[index];
            if (bucket.count == bucket.reported || !due(bucket.emitted, bucket.emittedAt, nowMs, force)) continue;
            event = bounded("mtsControl " + bucket.sample + " count=" + std::to_string(bucket.count) +
                " pending=" + std::to_string(bucket.count - bucket.reported) +
                " firstMs=" + std::to_string(bucket.first) + " lastMs=" + std::to_string(bucket.last), 1024);
            bucket.reported = bucket.count; bucket.emittedAt = nowMs; ++bucket.emitted;
            cursor_ = (index + 1) % buckets_.size();
            return true;
        }
        if (overflow_ != overflowReported_ && due(overflowEmitted_, overflowAt_, nowMs, force)) {
            event = "mtsControl schemaOverflow=" + std::to_string(overflow_) +
                " pending=" + std::to_string(overflow_ - overflowReported_) + " schemas=32";
            overflowReported_ = overflow_; overflowAt_ = nowMs; ++overflowEmitted_;
            return true;
        }
        return false;
    }
    std::string diagnostics() const {
        return " incomingEvents=" + std::to_string(received_) + " incomingSchemas=" + std::to_string(buckets_.size()) +
            " incomingCoalesced=" + std::to_string(coalesced_) + " incomingSchemaOverflow=" + std::to_string(overflow_);
    }
private:
    struct Bucket {
        std::string key, sample;
        std::uint64_t count = 0, reported = 0, first = 0, last = 0, emittedAt = 0, emitted = 0;
    };
    static bool due(std::uint64_t emitted, std::uint64_t at, std::uint64_t now, bool force) {
        return force || emitted < 3 || (now >= at && now - at >= 5000);
    }
    static std::string bounded(const std::string& value, std::size_t limit) {
        return value.size() <= limit ? value : value.substr(0, limit - 3) + "...";
    }
    static std::string label(const std::string& value) {
        if (value.empty() || value.size() > 64) return "<invalid>";
        for (const unsigned char ch : value)
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.' || ch == ':')) return "<invalid>";
        return value;
    }
    static bool sensitive(const std::string& key) {
        std::string lower = key;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
            return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : static_cast<char>(ch);
        });
        for (const auto* word : {"token", "cookie", "password", "auth", "url", "sdp", "candidate", "ip",
                 "address", "host", "session", "hash", "email", "phone", "pin", "text", "clipboard", "body"})
            if (lower.find(word) != std::string::npos) return true;
        return false;
    }
    static const char* type(const nlohmann::json& value) {
        if (value.is_discarded()) return "invalid";
        if (value.is_null()) return "null";
        if (value.is_boolean()) return "bool";
        if (value.is_number_integer()) return "integer";
        if (value.is_number()) return "number";
        if (value.is_string()) return "string";
        return "binary";
    }
    static std::string shape(const nlohmann::json& value, unsigned depth, std::size_t& budget,
                             bool sample, bool rumble) {
        if (value.is_object()) {
            if (depth >= 3) return "object{...}";
            std::string out = "{";
            bool first = true;
            for (auto item = value.begin(); item != value.end(); ++item) {
                if (!budget) { out += first ? "..." : ",..."; break; }
                --budget;
                if (!first) out += ',';
                first = false;
                const auto key = label(item.key());
                out += key + ':';
                if (key == "<invalid>" || sensitive(key)) out += "<redacted>";
                else if (sample && rumble && depth == 0 && (key == "index" || key == "large_motor" || key == "small_motor") &&
                    item->is_number_integer() && *item >= 0 && *item <= 65535) out += std::to_string(item->get<unsigned>());
                else out += shape(item.value(), depth + 1, budget, sample, rumble);
            }
            return bounded(out + '}', 640);
        }
        if (value.is_array()) {
            if (depth >= 3) return "array[...]";
            std::string out = "[";
            for (std::size_t n = 0; n < std::min<std::size_t>(2, value.size()); ++n) {
                if (n) out += ',';
                out += shape(value[n], depth + 1, budget, sample, rumble);
            }
            if (value.size() > 2) out += ",...";
            return bounded(out + ']', 640);
        }
        return type(value);
    }
    std::vector<Bucket> buckets_;
    std::size_t cursor_ = 0;
    std::uint64_t received_ = 0, coalesced_ = 0, overflow_ = 0;
    std::uint64_t overflowReported_ = 0, overflowEmitted_ = 0, overflowAt_ = 0;
};
}
