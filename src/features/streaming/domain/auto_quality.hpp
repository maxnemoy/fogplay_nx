#pragma once
// Deterministic application-thread Auto policy. Monotonic milliseconds; no RTC or platform dependencies.
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>

namespace stream::quality {
enum class Profile { Unknown, Low, Middle, High, Ultra };
inline const char* name(Profile value) {
    switch (value) {
    case Profile::Low: return "LOW";
    case Profile::Middle: return "MID";
    case Profile::High: return "HIGH";
    case Profile::Ultra: return "ULTRA";
    default: return "unknown";
    }
}
template<class T> struct Field {
    T value{};
    std::uint64_t at = 0, revision = 0;
    bool known = false;
    void set(T next, std::uint64_t now) { value = next; at = now; known = true; ++revision; }
    bool fresh(std::uint64_t now) const { return known && now >= at && now - at <= 6000; }
    std::int64_t age(std::uint64_t now) const { return known && now >= at ? static_cast<std::int64_t>(now - at) : -1; }
};
struct Telemetry {
    Field<Profile> profile;
    Field<double> targetMbps, fps;
    Field<bool> automatic;
    Field<std::string> codec;
    std::uint64_t count = 0, at = 0;
    void read(const nlohmann::json& data, std::uint64_t now) {
        if (!data.is_object()) return;
        ++count; at = now;
        const auto p = data.find("preset");
        if (p != data.end() && p->is_string()) {
            // Official preset enum: LOW / MID / HIGH / ULTRA.
            // Never preserve/log arbitrary remote strings.
            if (*p == "LOW") profile.set(Profile::Low, now);
            else if (*p == "MID") profile.set(Profile::Middle, now);
            else if (*p == "HIGH") profile.set(Profile::High, now);
            else if (*p == "ULTRA") profile.set(Profile::Ultra, now);
        }
        readNumber(data, "target_bitrate", 0, 1000000000.0, targetMbps, now, 1000000.0);
        readNumber(data, "fps", 1, 240, fps, now);
        const auto a = data.find("autoquality");
        if (a != data.end() && a->is_boolean()) automatic.set(a->get<bool>(), now);
        const auto c = data.find("server_codec");
        if (c != data.end() && c->is_string()) {
            if (*c == "H264" || *c == "H.264" || *c == "h264") codec.set("H264", now);
            else if (*c == "H265" || *c == "H.265" || *c == "h265" || *c == "HEVC" || *c == "hevc") codec.set("H265", now);
        }
    }
private:
    static void readNumber(const nlohmann::json& data, const char* key, double low, double high,
                           Field<double>& field, std::uint64_t now, double divisor = 1) {
        const auto it = data.find(key);
        if (it == data.end() || !it->is_number()) return;
        const double value = it->get<double>();
        if (std::isfinite(value) && value >= low && value <= high) field.set(value / divisor, now);
    }
};
struct Sample {
    std::uint64_t frames = 0, workUs = 0, errors = 0;
    std::uint64_t missing = 0, faults = 0, recoveries = 0, pressure = 0;
    std::size_t queuedVideo = 0;
    std::uint64_t oldestVideoMs = 0; // Residual queue after this playback slice, not a receive peak.
    bool recovering = false;
    int width = 0, height = 0;
};
enum class Action { None, Enhance, Degrade };
inline const char* command(Action value) { return value == Action::Enhance ? "enhancequality" : "degradequality"; }
class Controller {
public:
    void enable(bool enabled, std::uint64_t now) {
        if (enabled_ == enabled) return;
        enabled_ = enabled; pending_ = Action::None; sampled_ = false; stable_ = false;
        observed_ = Profile::Unknown;
        attemptsUp_ = attemptsDown_ = downgradeStreak_ = 0; nextUp_ = nextDown_ = now; lastCommand_ = now;
        lagging_ = false; lagSince_ = 0; queueWindows_ = 0; maxOldestVideoMs_ = 0;
        reason_ = enabled ? "startup" : "manual";
        event(enabled ? "enabled" : "disabled", now);
    }
    // Every frame, after draining. Counters are cumulative for this session.
    Action observe(std::uint64_t now, const Sample& sample, const Telemetry& server) {
        if (!enabled_) return Action::None;
        confirm(now, server);
        // Check freshness on every playback observation, including between 2s
        // evaluations: an update at the window boundary cannot hide stale gaps.
        if (!server.profile.fresh(now) || !server.fps.fresh(now)) stable_ = false;
        if (!server.profile.fresh(now)) queueWindows_ = 0;
        if (!sampled_ || now < lastObserve_ || now - lastObserve_ > 1000 ||
            sample.frames < base_.frames || sample.workUs < base_.workUs || sample.errors < base_.errors ||
            sample.missing < base_.missing || sample.faults < base_.faults ||
            sample.recoveries < base_.recoveries || sample.pressure < base_.pressure) {
            base_ = sample; baseAt_ = lastObserve_ = now; sampled_ = true;
            stable_ = false; interrupted_ = sample.recovering; queueWindows_ = 0;
            lagging_ = false; lagSince_ = now; maxOldestVideoMs_ = sample.oldestVideoMs;
            decide("sample-gap", now); return Action::None;
        }
        lastObserve_ = now;
        maxOldestVideoMs_ = std::max(maxOldestVideoMs_, sample.oldestVideoMs);
        // A batch can be deep immediately after reception without being slow.
        // Demand both age and depth continuously; isolated boundary peaks do
        // not count as a whole overloaded two-second window.
        const bool lag = sample.queuedVideo >= 4 && sample.oldestVideoMs >= 100;
        if (!lag || !lagging_) lagSince_ = now;
        lagging_ = lag;
        interrupted_ |= sample.recovering || sample.width <= 0 || sample.height <= 0 ||
            sample.width > 1920 || sample.height > 1080 || sample.width != base_.width || sample.height != base_.height;
        if (now - baseAt_ < 2000) return Action::None;
        const double elapsed = static_cast<double>(now - baseAt_);
        const auto frames = sample.frames - base_.frames, work = sample.workUs - base_.workUs;
        const auto errors = sample.errors - base_.errors, missing = sample.missing - base_.missing;
        const auto faults = sample.faults - base_.faults, pressure = sample.pressure - base_.pressure;
        const auto recoveries = sample.recoveries - base_.recoveries;
        fps_ = frames * 1000.0 / elapsed;
        workMs_ = frames ? work / (1000.0 * frames) : 0;
        busy_ = work / (1000.0 * elapsed);
        const bool dimensions = sample.width > 0 && sample.height > 0 && sample.width <= 1920 && sample.height <= 1080;
        const bool contiguous = !interrupted_ && elapsed <= 3000;
        const auto oldest = maxOldestVideoMs_; maxOldestVideoMs_ = 0;
        const bool sustainedQueue = lagging_ && now - lagSince_ >= 1000;
        base_ = sample; baseAt_ = now; interrupted_ = sample.recovering;
        if (!server.profile.fresh(now)) { stable_ = false; queueWindows_ = 0; decide("profile-stale", now); return Action::None; }
        if (pending_ != Action::None) { stable_ = false; queueWindows_ = 0; decide("await-confirmation", now); return Action::None; }
        if (!dimensions) { stable_ = false; queueWindows_ = 0; decide("dimensions", now); return Action::None; }
        const double expected = server.fps.fresh(now) ? std::min(30.0, server.fps.value) : 30.0;
        queueWindows_ = sustainedQueue ? std::min(queueWindows_ + 1, 2u) : 0;
        // Low FPS alone may be a static source. Demand measured local work, errors,
        // repeated transport damage or persistent queued video before decreasing.
        const bool overloaded = frames && (busy_ >= 0.85 || (workMs_ >= 30 && fps_ < expected * 0.8));
        const bool damaged = errors >= 2 || (errors && fps_ < expected * 0.8) ||
            missing >= 8 || recoveries >= 2 || pressure || queueWindows_ >= 2;
        if (overloaded || damaged) {
            stable_ = false;
            if (server.profile.value == Profile::Low) { decide("floor-low", now); return Action::None; }
            if (now - lastCommand_ < 5000 || now < nextDown_) { decide("down-cooldown", now); return Action::None; }
            if (attemptsDown_ >= 2) { decide("down-unconfirmed", now); return Action::None; }
            decide(overloaded ? "video-work-overload" : "transport-decode-trouble", now);
            return Action::Degrade;
        }
        const bool healthy = contiguous && !sample.recovering && !errors && !missing && !faults && !pressure &&
            // Brief young batches are normal, but every healthy window
            // must demonstrate that old queued video was actually drained.
            oldest <= 100 && !sustainedQueue && frames && fps_ >= expected * 0.85 && workMs_ <= 20 && busy_ < 0.65 &&
            server.fps.fresh(now);
        if (!healthy) { stable_ = false; decide(contiguous ? "insufficient-headroom" : "recovery-window", now); return Action::None; }
        if (server.profile.value == Profile::High || server.profile.value == Profile::Ultra) {
            stable_ = false; decide("ceiling-high", now); return Action::None;
        }
        if (!stable_) { stable_ = true; stableAt_ = now; }
        if (now - stableAt_ < 30000) { decide("stable-window", now); return Action::None; }
        if (now - lastCommand_ < 30000 || now < nextUp_) { decide("up-cooldown", now); return Action::None; }
        if (attemptsUp_ >= 2) { decide("up-unconfirmed", now); return Action::None; }
        decide("stable-headroom", now); return Action::Enhance;
    }
    // Only call after a command has been accepted/queued by the real channel.
    void sent(Action action, std::uint64_t now, const Telemetry& server) {
        if (action == Action::None || !enabled_) return;
        pending_ = action; pendingAt_ = lastCommand_ = now; from_ = server.profile.value;
        pendingRevision_ = server.profile.revision; stable_ = false;
        if (action == Action::Enhance) ++attemptsUp_; else ++attemptsDown_;
        event(action == Action::Enhance ? "enhance-pending" : "degrade-pending", now);
    }
    void sendFailed(std::uint64_t now) { stable_ = false; nextUp_ = std::max(nextUp_, now + 30000); nextDown_ = now + 30000; event("send-failed", now); }
    const char* reason() const { return reason_; }
    bool pending() const { return pending_ != Action::None; }
    double fps() const { return fps_; }
    std::string diagnostics() const {
        char out[400];
        std::snprintf(out, sizeof(out), " autoEnabled=%d autoReason=%s autoPending=%d sampleFps=%.2f videoWorkMs=%.2f videoBusy=%.3f autoUpAttempts=%u autoDownAttempts=%u autoDowngrades=%u autoNextUpMs=%llu autoQueueWindows=%u",
            enabled_, reason_, pending(), fps_, workMs_, busy_, attemptsUp_, attemptsDown_, downgradeStreak_,
            static_cast<unsigned long long>(nextUp_), queueWindows_);
        return out;
    }
    bool pollEvent(std::string& out) {
        if (events_.empty()) return false;
        out = std::move(events_.front()); events_.pop_front(); return true;
    }
private:
    void confirm(std::uint64_t now, const Telemetry& server) {
        if (server.profile.fresh(now) && server.profile.value != observed_) {
            const auto previous = observed_;
            observed_ = server.profile.value; attemptsUp_ = attemptsDown_ = 0; stable_ = false;
            if (previous != Profile::Unknown && observed_ < previous) {
                // Keep the failed capacity experiment for the whole session.
                // A healthy MID immediately after a bad HIGH must not cause
                // the same upgrade/downshift loop every thirty seconds.
                downgradeStreak_ = std::min(downgradeStreak_ + 1, 4u);
                const std::uint64_t hold = std::min<std::uint64_t>(600000, 120000ull << (downgradeStreak_ - 1));
                nextUp_ = std::max(nextUp_, now + hold);
                event("downgrade-hold", now);
            }
            event("profile-observed", now);
        }
        if (pending_ == Action::None) return;
        if (server.profile.fresh(now) && server.profile.revision > pendingRevision_ && server.profile.value != from_) {
            const bool right = pending_ == Action::Enhance ? server.profile.value > from_ : server.profile.value < from_;
            event(right ? "confirmed" : "rejected-profile", now);
            pending_ = Action::None; stable_ = false;
            // Even rejected changes require a new full stable window before upgrade.
            nextUp_ = std::max(nextUp_, now + 30000); nextDown_ = now + 5000; return;
        }
        if (now - pendingAt_ >= 10000) {
            const auto attempts = pending_ == Action::Enhance ? attemptsUp_ : attemptsDown_;
            if (pending_ == Action::Enhance) nextUp_ = std::max(nextUp_, now + 30000 * attempts);
            else nextDown_ = now + 30000 * attempts;
            event("timeout-unconfirmed", now); pending_ = Action::None; stable_ = false;
        }
    }
    void decide(const char* reason, std::uint64_t now) {
        if (reason_ != std::string(reason)) { reason_ = reason; event("decision", now); }
    }
    void event(const char* state, std::uint64_t now) {
        if (events_.size() >= 32) events_.pop_front();
        char values[240];
        std::snprintf(values, sizeof(values), " sampleFps=%.2f videoWorkMs=%.2f videoBusy=%.3f downgrades=%u nextUpMs=%llu queueWindows=%u", fps_, workMs_, busy_, downgradeStreak_, static_cast<unsigned long long>(nextUp_), queueWindows_);
        events_.push_back(std::string("qualityEvent=") + state + " ms=" + std::to_string(now) +
            " reason=" + reason_ + " profile=" + name(observed_) + values);
    }
    bool enabled_ = false, sampled_ = false, stable_ = false, interrupted_ = false;
    std::uint64_t baseAt_ = 0, lastObserve_ = 0, stableAt_ = 0, lastCommand_ = 0;
    std::uint64_t nextUp_ = 0, nextDown_ = 0, pendingAt_ = 0, pendingRevision_ = 0;
    unsigned attemptsUp_ = 0, attemptsDown_ = 0, queueWindows_ = 0, downgradeStreak_ = 0;
    Profile observed_ = Profile::Unknown, from_ = Profile::Unknown;
    Action pending_ = Action::None;
    bool lagging_ = false;
    std::uint64_t lagSince_ = 0, maxOldestVideoMs_ = 0;
    Sample base_;
    double fps_ = 0, workMs_ = 0, busy_ = 0;
    const char* reason_ = "manual";
    std::deque<std::string> events_;
};
}
