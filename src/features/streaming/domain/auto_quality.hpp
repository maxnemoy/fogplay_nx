#pragma once
// Deterministic application-thread Auto policy. Monotonic milliseconds; no RTC or platform dependencies.
#include <optional>
#include <algorithm>
#include <array>
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
struct TelemetryUpdate {
    std::optional<Profile> profile;
    std::optional<double> targetMbps, fps;
    std::optional<bool> automatic;
    std::optional<std::string> codec;
};
struct Telemetry {
    Field<Profile> profile;
    Field<double> targetMbps, fps;
    Field<bool> automatic;
    Field<std::string> codec;
    std::uint64_t count = 0, at = 0;
    void apply(const TelemetryUpdate& update, std::uint64_t now) {
        ++count; at = now;
        if (update.profile) profile.set(*update.profile, now);
        if (update.targetMbps) targetMbps.set(*update.targetMbps, now);
        if (update.fps) fps.set(*update.fps, now);
        if (update.automatic) automatic.set(*update.automatic, now);
        if (update.codec) codec.set(*update.codec, now);
    }
};

struct Sample {
    // Session totals: frames counts only new frames successfully presented.
    // Synchronous work includes decode/conversion/upload. With workerVideo,
    // workUs measures UI preparation/upload and decoderWorkUs measures the
    // independent worker's decode/conversion, including superseded frames.
    std::uint64_t frames = 0, decodedFrames = 0, supersededFrames = 0, workUs = 0, errors = 0;
    std::uint64_t decoderWorkUs = 0, videoGeneration = 0;
    bool workerVideo = false;
    // Audio loss counters exclude startup/recovery priming and intentional resets.
    std::uint64_t audioDropped = 0, audioUnderruns = 0;
    std::uint64_t audioResets = 0; // Intentional PCM clears interrupt headroom, not proof of overload.
    std::uint64_t missing = 0, faults = 0, recoveries = 0, pressure = 0;
    std::uint64_t receivedPackets = 0, repairedPackets = 0;
    std::size_t queuedVideo = 0;
    std::uint64_t oldestVideoMs = 0; // Residual queue after this playback slice, not a receive peak.
    bool recovering = false;
    int width = 0, height = 0;
};
enum class Action { None, Enhance, Degrade };
inline const char* command(Action value) { return value == Action::Enhance ? "enhancequality" : "degradequality"; }
enum class PauseCause { None, Loss, Superseded, Audio, Other };
inline const char* name(PauseCause value) {
    switch (value) {
        case PauseCause::Loss: return "loss";
        case PauseCause::Superseded: return "superseded";
        case PauseCause::Audio: return "audio";
        case PauseCause::Other: return "other";
        default: return "none";
    }
}
class Controller {
public:
    bool configureFramesPerSecond(int value) {
        if (value != 30 && value != 60) return false;
        if (framesPerSecond_ != value) {
            framesPerSecond_ = value;
            sampled_ = stable_ = false;
            queueWindows_ = deliveryWindows_ = audioWindows_ = 0;
        }
        return true;
    }
    void enable(bool enabled, std::uint64_t now) {
        if (enabled_ == enabled) return;
        enabled_ = enabled; pending_ = Action::None; sampled_ = false; stable_ = false;
        observed_ = Profile::Unknown;
        attemptsUp_ = attemptsDown_ = downgradeStreak_ = 0; nextUp_ = nextDown_ = now; lastCommand_ = now;
        lagging_ = false; lagSince_ = 0; queueWindows_ = deliveryWindows_ = audioWindows_ = 0; maxOldestVideoMs_ = 0;
        capCoalesced_ = 0;
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
        if (!server.profile.fresh(now)) queueWindows_ = deliveryWindows_ = audioWindows_ = 0;
        if (!sampled_ || now < lastObserve_ || now - lastObserve_ > 1000 ||
            sample.frames < base_.frames || sample.decodedFrames < base_.decodedFrames ||
            sample.supersededFrames < base_.supersededFrames || sample.audioDropped < base_.audioDropped ||
            sample.audioUnderruns < base_.audioUnderruns || sample.audioResets < base_.audioResets ||
            sample.workUs < base_.workUs || sample.decoderWorkUs < base_.decoderWorkUs || sample.errors < base_.errors ||
            sample.workerVideo != base_.workerVideo ||
            sample.missing < base_.missing || sample.faults < base_.faults ||
            sample.recoveries < base_.recoveries || sample.pressure < base_.pressure ||
            sample.receivedPackets < base_.receivedPackets || sample.repairedPackets < base_.repairedPackets) {
            base_ = sample; baseAt_ = lastObserve_ = now; sampled_ = true;
            stable_ = false; interrupted_ = sample.recovering; queueWindows_ = deliveryWindows_ = audioWindows_ = 0;
            lagging_ = false; lagSince_ = now; maxOldestVideoMs_ = sample.oldestVideoMs;
            workerVideo_ = sample.workerVideo;
            workMs_ = busy_ = uiWorkMs_ = uiBusy_ = decoderWorkMs_ = decoderBusy_ = 0;
            decide("sample-gap", now); return Action::None;
        }
        // Recovery generations retain cumulative work and loss counters. A
        // new decoder/reference chain invalidates upgrade headroom, but must
        // not restart the loss window: frequent resyncs are themselves damage
        // evidence and still need to trigger a downgrade.
        if (sample.videoGeneration != base_.videoGeneration) stable_ = false;
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
        const auto decoderWork = sample.decoderWorkUs - base_.decoderWorkUs;
        const auto decoded = sample.decodedFrames - base_.decodedFrames;
        const auto superseded = sample.supersededFrames - base_.supersededFrames;
        const auto audioDropped = sample.audioDropped - base_.audioDropped;
        const auto audioUnderruns = sample.audioUnderruns - base_.audioUnderruns;
        const auto audioResets = sample.audioResets - base_.audioResets;
        const auto errors = sample.errors - base_.errors, missing = sample.missing - base_.missing;
        const auto faults = sample.faults - base_.faults, pressure = sample.pressure - base_.pressure;
        const auto recoveries = sample.recoveries - base_.recoveries;
        const auto received = sample.receivedPackets - base_.receivedPackets;
        const auto repaired = sample.repairedPackets - base_.repairedPackets;
        fps_ = frames * 1000.0 / elapsed;
        decodedFps_ = decoded * 1000.0 / elapsed;
        uiWorkMs_ = decoded ? work / (1000.0 * decoded) : 0;
        uiBusy_ = work / (1000.0 * elapsed);
        decoderWorkMs_ = sample.workerVideo && decoded ? decoderWork / (1000.0 * decoded) : 0;
        decoderBusy_ = sample.workerVideo ? decoderWork / (1000.0 * elapsed) : 0;
        // These domains run concurrently. Adding their durations would invent
        // saturation of one thread and reject real headroom. Both must have
        // capacity, so the slower domain controls the existing thresholds.
        workMs_ = std::max(uiWorkMs_, decoderWorkMs_);
        busy_ = std::max(uiBusy_, decoderBusy_);
        workerVideo_ = sample.workerVideo;
        const bool dimensions = sample.width > 0 && sample.height > 0 && sample.width <= 1920 && sample.height <= 1080;
        const bool contiguous = !interrupted_ && elapsed <= 3000;
        const auto oldest = maxOldestVideoMs_; maxOldestVideoMs_ = 0;
        const bool sustainedQueue = lagging_ && now - lagSince_ >= 1000;
        base_ = sample; baseAt_ = now; interrupted_ = sample.recovering;
        if (!server.profile.fresh(now)) { stable_ = false; queueWindows_ = deliveryWindows_ = audioWindows_ = 0; decide("profile-stale", now); return Action::None; }
        if (pending_ != Action::None) { stable_ = false; queueWindows_ = deliveryWindows_ = audioWindows_ = 0; decide("await-confirmation", now); return Action::None; }
        if (!dimensions) { stable_ = false; queueWindows_ = deliveryWindows_ = audioWindows_ = 0; decide("dimensions", now); return Action::None; }
        const double expected = server.fps.fresh(now) ? std::min(static_cast<double>(framesPerSecond_), server.fps.value) : framesPerSecond_;
        const double frameBudgetScale = 30.0 / framesPerSecond_;
        queueWindows_ = sustainedQueue ? std::min(queueWindows_ + 1, 2u) : 0;
        // Low source FPS alone may be a static scene. A local delivery shortfall
        // needs decoded supply or substantial measured superseding, and must
        // persist across two complete windows outside startup/recovery gaps.
        const bool deliveryLoss = fps_ < expected * 0.8 &&
            ((decodedFps_ >= expected * 0.85 && frames < decoded * 0.8) ||
             (superseded >= 4 && superseded >= decoded * 0.2));
        deliveryWindows_ = contiguous && deliveryLoss ? std::min(deliveryWindows_ + 1, 2u) : 0;
        const bool audioLoss = audioDropped >= 2 || audioUnderruns >= 2;
        audioWindows_ = contiguous && !audioResets && audioLoss ? std::min(audioWindows_ + 1, 2u) : 0;
        const bool deliveryTrouble = deliveryWindows_ >= 2, audioTrouble = audioWindows_ >= 2;
        const bool overloaded = decoded && (busy_ >= 0.85 || (workMs_ >= 30 * frameBudgetScale && fps_ < expected * 0.8));
        const bool damaged = errors >= 2 || (errors && fps_ < expected * 0.8) ||
            missing >= 8 || recoveries >= 2 || pressure || queueWindows_ >= 2;
        // Source delivery can exceed the configured display cap. Replacing
        // those excess frames is intentional only when the cap's display slots
        // were actually filled. One boundary presentation and one pending frame
        // may straddle the cumulative-counter window; no wider FPS allowance.
        const auto capFrames = static_cast<std::uint64_t>(elapsed * framesPerSecond_ / 1000.0);
        const bool capDelivered = frames >= capFrames || (capFrames && frames == capFrames - 1);
        capCoalesced_ = capDelivered && decoded > capFrames && superseded <= decoded - capFrames + 2
            ? superseded : 0;
        // A manually selected ULTRA can remain active when Auto is enabled.
        // HIGH is the policy ceiling even when that ULTRA happens to decode
        // quickly; use the same confirmed, bounded downshift as overload.
        const bool aboveCeiling = server.profile.value == Profile::Ultra;
        if (aboveCeiling || overloaded || damaged || deliveryTrouble || audioTrouble) {
            stable_ = false;
            if (server.profile.value == Profile::Low) { decide("floor-low", now); return Action::None; }
            if (now - lastCommand_ < 5000 || now < nextDown_) { decide("down-cooldown", now); return Action::None; }
            if (attemptsDown_ >= 2) { decide("down-unconfirmed", now); return Action::None; }
            decide(aboveCeiling ? "above-auto-ceiling" : overloaded ? "video-work-overload" : damaged ? "transport-decode-trouble" :
                deliveryTrouble ? "video-delivery-loss" : "audio-playback-loss", now);
            return Action::Degrade;
        }
        const bool cleanTransport = !missing && !faults && !recoveries;
        // One repaired packet loss in a sufficiently large window is not a
        // capacity failure. It pauses good-time accumulation below rather than
        // becoming healthy evidence; repeated losses therefore never upgrade.
        const bool toleratedTransport = missing == 1 && faults <= 1 && recoveries <= 1 &&
            repaired >= 1 && received >= 500;
        const bool headroom = contiguous && !sample.recovering && !errors && (cleanTransport || toleratedTransport) &&
            !pressure && !audioResets &&
            // Brief young batches are normal, but every healthy window
            // must demonstrate that old queued video was actually drained.
            oldest <= 100 && !sustainedQueue && frames && fps_ >= expected * 0.85 && workMs_ <= 20 * frameBudgetScale && busy_ < 0.65 &&
            server.fps.fresh(now);
        const bool supersededExcess = superseded != capCoalesced_;
        const bool audioIssue = audioDropped || audioUnderruns;
        const bool healthy = headroom && cleanTransport && !supersededExcess && !audioIssue;
        if (!healthy) {
            // One small isolated loss pauses an existing experiment. It adds
            // no good time and cannot excuse work, age or audio evidence.
            const bool mild = headroom && superseded - capCoalesced_ <= std::max<std::uint64_t>(4, decoded / 20) &&
                audioDropped <= 1 && audioUnderruns <= 1;
            if (mild && stable_ && !headroomPaused_ && elapsed <= 3000) {
                recordHeadroom(now, static_cast<std::uint64_t>(elapsed), false);
                // Fixed cause so a tolerated packet loss is not confused with
                // superseded frames (both used to read only "headroom-paused").
                pauseCause_ = !cleanTransport ? PauseCause::Loss : supersededExcess ? PauseCause::Superseded :
                    audioIssue ? PauseCause::Audio : PauseCause::Other;
                ++pauseCounts_[static_cast<std::size_t>(pauseCause_)];
                headroomPaused_ = true; decide("headroom-paused", now); return Action::None;
            }
            if (contiguous) {
                // Not exclusive: a window may fail for several reasons at once.
                const bool load = workMs_ > 20 * frameBudgetScale || busy_ >= 0.65;
                const bool delivery = !frames || fps_ < expected * 0.85 || oldest > 100 || sustainedQueue || !server.fps.fresh(now);
                rejectLoss_ += !cleanTransport || sample.recovering || errors || pressure;
                rejectSuperseded_ += supersededExcess;
                rejectAudio_ += audioIssue || audioResets;
                rejectLoad_ += load;
                rejectDelivery_ += delivery;
            }
            stable_ = false; decide(contiguous ? "insufficient-headroom" : "recovery-window", now); return Action::None;
        }
        if (server.profile.value == Profile::High) {
            stable_ = false; decide("ceiling-high", now); return Action::None;
        }
        recordHeadroom(now, static_cast<std::uint64_t>(elapsed), true);
        headroomPaused_ = false;
        if (headroomGoodMs_ < 30000) { decide("stable-window", now); return Action::None; }
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
        char out[1280];
        std::snprintf(out, sizeof(out), " autoEnabled=%d autoReason=%s autoPending=%d autoTargetFps=%d sampleFps=%.2f decodedFps=%.2f videoWorkMs=%.2f videoBusy=%.3f autoUpAttempts=%u autoDownAttempts=%u autoDowngrades=%u autoNextUpMs=%llu autoQueueWindows=%u autoDeliveryWindows=%u autoAudioWindows=%u autoCapCoalesced=%llu autoGoodMs=%llu autoEvidenceMs=%llu autoPaused=%d workerVideo=%d uiVideoMs=%.2f decoderVideoMs=%.2f uiVideoBusy=%.3f decoderVideoBusy=%.3f"
            " autoPauseCause=%s autoPauseLoss=%llu autoPauseSuperseded=%llu autoPauseAudio=%llu autoPauseOther=%llu"
            " autoRejectLoss=%llu autoRejectSuperseded=%llu autoRejectAudio=%llu autoRejectLoad=%llu autoRejectDelivery=%llu",
            enabled_, reason_, pending(), framesPerSecond_, fps_, decodedFps_, workMs_, busy_, attemptsUp_, attemptsDown_, downgradeStreak_,
            static_cast<unsigned long long>(nextUp_), queueWindows_, deliveryWindows_, audioWindows_, static_cast<unsigned long long>(capCoalesced_),
            static_cast<unsigned long long>(stable_ ? headroomGoodMs_ : 0), static_cast<unsigned long long>(headroomSpan()), stable_ && headroomPaused_,
            workerVideo_, uiWorkMs_, decoderWorkMs_, uiBusy_, decoderBusy_, name(pauseCause_),
            static_cast<unsigned long long>(pauseCounts_[static_cast<std::size_t>(PauseCause::Loss)]),
            static_cast<unsigned long long>(pauseCounts_[static_cast<std::size_t>(PauseCause::Superseded)]),
            static_cast<unsigned long long>(pauseCounts_[static_cast<std::size_t>(PauseCause::Audio)]),
            static_cast<unsigned long long>(pauseCounts_[static_cast<std::size_t>(PauseCause::Other)]),
            static_cast<unsigned long long>(rejectLoss_), static_cast<unsigned long long>(rejectSuperseded_),
            static_cast<unsigned long long>(rejectAudio_), static_cast<unsigned long long>(rejectLoad_),
            static_cast<unsigned long long>(rejectDelivery_));
        return out;
    }
    bool pollEvent(std::string& out) {
        if (events_.empty()) return false;
        out = std::move(events_.front()); events_.pop_front(); return true;
    }
private:
    struct HeadroomWindow { std::uint64_t begin = 0, end = 0; bool healthy = false; };
    void recordHeadroom(std::uint64_t now, std::uint64_t duration, bool healthy) {
        if (!stable_) {
            headroom_.clear(); headroomGoodMs_ = 0;
            stable_ = true;
            duration = 0; // The first clean window establishes the baseline.
        }
        if (duration) headroom_.push_back({now - duration, now, healthy});
        const auto cutoff = now > 40000 ? now - 40000 : 0;
        while (!headroom_.empty() && headroom_.front().end <= cutoff) headroom_.pop_front();
        if (!headroom_.empty() && headroom_.front().begin < cutoff) headroom_.front().begin = cutoff;
        // Evaluations are >=2s apart: the 40s horizon retains at most 21
        // complete/partial windows, even through a long upgrade cooldown.
        headroomGoodMs_ = 0;
        for (const auto& window : headroom_)
            if (window.healthy) headroomGoodMs_ += window.end - window.begin;
    }
    std::uint64_t headroomSpan() const {
        return stable_ && !headroom_.empty() ? headroom_.back().end - headroom_.front().begin : 0;
    }
    void confirm(std::uint64_t now, const Telemetry& server) {
        if (server.profile.fresh(now) && server.profile.value != observed_) {
            const auto previous = observed_;
            observed_ = server.profile.value; attemptsUp_ = attemptsDown_ = 0; stable_ = false;
            // A preset transition invalidates every in-progress measurement:
            // even a mostly-HIGH window cannot become the first MID window.
            sampled_ = false; deliveryWindows_ = audioWindows_ = 0;
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
        if (server.profile.fresh(now) && server.profile.revision > pendingRevision_ && server.profile.at >= pendingAt_ && server.profile.value != from_) {
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
        char values[640];
        std::snprintf(values, sizeof(values), " sampleFps=%.2f decodedFps=%.2f videoWorkMs=%.2f videoBusy=%.3f downgrades=%u nextUpMs=%llu queueWindows=%u deliveryWindows=%u audioWindows=%u goodMs=%llu evidenceMs=%llu paused=%d workerVideo=%d uiVideoMs=%.2f decoderVideoMs=%.2f uiVideoBusy=%.3f decoderVideoBusy=%.3f", fps_, decodedFps_, workMs_, busy_, downgradeStreak_, static_cast<unsigned long long>(nextUp_), queueWindows_, deliveryWindows_, audioWindows_,
            static_cast<unsigned long long>(stable_ ? headroomGoodMs_ : 0), static_cast<unsigned long long>(headroomSpan()), stable_ && headroomPaused_,
            workerVideo_, uiWorkMs_, decoderWorkMs_, uiBusy_, decoderBusy_);
        events_.push_back(std::string("qualityEvent=") + state + " ms=" + std::to_string(now) +
            " reason=" + reason_ + " profile=" + name(observed_) + values);
    }
    bool enabled_ = false, sampled_ = false, stable_ = false, interrupted_ = false;
    std::uint64_t baseAt_ = 0, lastObserve_ = 0, lastCommand_ = 0;
    std::uint64_t nextUp_ = 0, nextDown_ = 0, pendingAt_ = 0, pendingRevision_ = 0;
    unsigned attemptsUp_ = 0, attemptsDown_ = 0, queueWindows_ = 0, deliveryWindows_ = 0, audioWindows_ = 0, downgradeStreak_ = 0;
    Profile observed_ = Profile::Unknown, from_ = Profile::Unknown;
    Action pending_ = Action::None;
    bool lagging_ = false;
    std::uint64_t lagSince_ = 0, maxOldestVideoMs_ = 0;
    std::uint64_t capCoalesced_ = 0;
    std::deque<HeadroomWindow> headroom_;
    std::uint64_t headroomGoodMs_ = 0;
    bool headroomPaused_ = false;
    PauseCause pauseCause_ = PauseCause::None;
    std::array<std::uint64_t, 5> pauseCounts_{};
    std::uint64_t rejectLoss_ = 0, rejectSuperseded_ = 0, rejectAudio_ = 0, rejectLoad_ = 0, rejectDelivery_ = 0;
    Sample base_;
    int framesPerSecond_ = 30;
    double fps_ = 0, decodedFps_ = 0, workMs_ = 0, busy_ = 0;
    double uiWorkMs_ = 0, uiBusy_ = 0, decoderWorkMs_ = 0, decoderBusy_ = 0;
    bool workerVideo_ = false;
    const char* reason_ = "manual";
    std::deque<std::string> events_;
};
}
