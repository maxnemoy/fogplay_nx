#include "features/streaming/domain/auto_quality.hpp"
#include <cassert>
#include <iostream>
#include <limits>
#include <vector>
using namespace stream::quality;
using Json = nlohmann::json;
struct Harness {
    Controller controller;
    Telemetry server;
    Sample sample;
    std::uint64_t now = 0;
    std::vector<Action> actions;
    Profile profile = Profile::Low;
    bool refresh = true, overload = false, staticSource = false, send = true;
    unsigned presentedPerTick = 3, decodedPerTick = 3, supersededPerTick = 0;
    unsigned audioDroppedPerTick = 0, audioUnderrunsPerTick = 0;
    Harness() {
        sample.width = 1920; sample.height = 1080;
        server.read(Json{{"preset", "LOW"}, {"fps", 30}}, now);
        controller.enable(true, now);
        controller.observe(now, sample, server);
    }
    // Literal wire fixtures from the official Android preset enum; deliberately
    // independent of the production name()/parser to catch vocabulary mistakes.
    static const char* wireProfile(Profile value) {
        switch (value) {
        case Profile::Low: return "LOW";
        case Profile::Middle: return "MID";
        case Profile::High: return "HIGH";
        case Profile::Ultra: return "ULTRA";
        default: return "unknown";
        }
    }
    void advance(unsigned seconds) { ticks(seconds * 10); }
    void ticks(unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            now += 100;
            if (!staticSource) {
                sample.frames += presentedPerTick; sample.decodedFrames += decodedPerTick;
                sample.supersededFrames += supersededPerTick;
                sample.workUs += overload ? 100000 : decodedPerTick * 3000;
            }
            sample.audioDropped += audioDroppedPerTick; sample.audioUnderruns += audioUnderrunsPerTick;
            if (refresh && now % 2000 == 0) server.read(Json{{"preset", wireProfile(profile)}, {"fps", 30}}, now);
            const auto action = controller.observe(now, sample, server);
            if (action != Action::None) {
                actions.push_back(action);
                if (send) controller.sent(action, now, server); else controller.sendFailed(now);
            }
        }
    }
    bool event(const std::string& needle) {
        std::string out; bool found = false;
        while (controller.pollEvent(out)) if (out.find(needle) != std::string::npos) found = true;
        return found;
    }
};
int main() {
    {
        Telemetry actualWire;
        actualWire.read(Json{{"preset","LOW"},{"fps",30}}, 100);
        actualWire.read(Json{{"preset","MID"},{"target_bitrate",3732000},{"fps",30}}, 200);
        assert(actualWire.profile.value == Profile::Middle);
        assert(std::string(name(actualWire.profile.value)) == "MID");
        assert(actualWire.profile.revision == 2 && actualWire.targetMbps.value == 3.732);
        actualWire.read(Json{{"preset","MID"}}, 7000);
        assert(actualWire.profile.revision == 3 && actualWire.profile.fresh(7000));
        assert(actualWire.profile.age(7000) == 0);
        actualWire.read(Json{{"preset","MIDDLE"}}, 13001);
        assert(actualWire.profile.revision == 3 && !actualWire.profile.fresh(13001));
        actualWire.read(Json{{"preset","HIGH"}}, 14000);
        assert(actualWire.profile.value == Profile::High && actualWire.profile.revision == 4);
    }
    Telemetry telemetry;
    telemetry.read(Json{{"preset","MID"},{"target_bitrate",2488000},{"fps",30},{"autoquality",true},{"server_codec","HEVC"}}, 100);
    assert(telemetry.profile.value == Profile::Middle && telemetry.targetMbps.value == 2.488);
    assert(telemetry.automatic.value && telemetry.codec.value == "H265" && telemetry.count == 1);
    const auto revision = telemetry.profile.revision;
    telemetry.read(Json{{"preset",3},{"target_bitrate","private-credential"},{"fps",Json::array({"secret"})},
                        {"autoquality",42},{"server_codec","private-sdp"}}, 6000);
    telemetry.read(Json{{"preset","LOW\nprivate"},{"target_bitrate",-1},{"fps",1e9},{"server_codec",7}}, 6500);
    telemetry.read(Json{{"unrelated","secret"}}, 7000);
    assert(telemetry.profile.revision == revision && telemetry.profile.age(7000) == 6900);
    assert(!telemetry.profile.fresh(7000) && !telemetry.targetMbps.fresh(7000));
    assert(telemetry.targetMbps.age(7000) == 6900 && telemetry.count == 4 && telemetry.at == 7000);
    assert(telemetry.codec.value == "H265" && telemetry.fps.value == 30);
    telemetry.read(Json{{"target_bitrate",std::numeric_limits<double>::infinity()},{"fps",false},{"preset",nullptr}}, 8000);
    assert(telemetry.targetMbps.revision == 1 && telemetry.fps.revision == 1);
    telemetry.read(Json::array(), 9000); assert(telemetry.count == 5);
    {
        Harness h; h.advance(31); assert(h.actions.empty()); h.advance(1);
        assert(h.actions == std::vector<Action>{Action::Enhance} && h.controller.pending());
        assert(h.server.profile.value == Profile::Low); // Request cannot optimistically change observed profile.
        h.profile = Profile::Middle; h.advance(2);
        assert(!h.controller.pending() && h.event("qualityEvent=confirmed"));
        h.advance(29); assert(h.actions.size() == 1); h.advance(3);
        assert(h.actions.size() == 2); h.profile = Profile::High; h.advance(2);
        h.advance(80); assert(h.actions.size() == 2 && std::string(h.controller.reason()) == "ceiling-high");
    }
    {
        Harness h; h.profile = Profile::High; h.overload = true; h.advance(6);
        assert(h.actions == std::vector<Action>{Action::Degrade});
        h.profile = Profile::Middle; h.advance(2); assert(h.event("qualityEvent=confirmed"));
        h.advance(4); assert(h.actions.size() == 1); h.advance(2); assert(h.actions.size() == 2);
    }
    {
        Harness h; h.advance(200); assert(h.actions.size() == 2 && !h.controller.pending());
        assert(h.event("timeout-unconfirmed") && std::string(h.controller.reason()) == "up-unconfirmed");
        h.profile = Profile::Middle; h.advance(34); assert(h.actions.size() == 3);
    }
    {
        Harness h; h.advance(32); h.profile = Profile::Ultra; h.advance(2);
        // A larger observed profile confirms direction; Auto never enhances HIGH/ULTRA.
        assert(h.event("qualityEvent=confirmed")); h.advance(90); assert(h.actions.size() == 1);
        h.overload = true; h.advance(6); assert(h.actions.back() == Action::Degrade);
    }
    {
        Harness h; h.profile = Profile::High; h.advance(32); assert(h.actions.empty());
        h.profile = Profile::Ultra; h.advance(32); assert(h.actions.empty());
        h.staticSource = true; h.advance(40); assert(h.actions.empty()); // Low FPS alone is not a fault.
    }
    {
        Harness h; h.advance(20); h.refresh = false; h.advance(30); assert(h.actions.empty());
        h.refresh = true; h.advance(31); assert(h.actions.empty()); h.advance(3); assert(h.actions.size() == 1);
    }
    {
        Harness h; h.advance(20); h.now += 1500; h.advance(30); assert(h.actions.empty());
        h.advance(4); assert(h.actions.size() == 1);
    }
    {
        Harness h; h.advance(20); h.sample.recovering = true; h.advance(2);
        h.sample.recovering = false; h.advance(30); assert(h.actions.empty()); h.advance(4); assert(h.actions.size() == 1);
    }
    {
        Harness h; h.sample.width = 3840; h.advance(90); assert(h.actions.empty());
        h.sample.width = 1920; h.advance(30); assert(h.actions.empty()); h.advance(4); assert(h.actions.size() == 1);
    }
    {
        Harness h; h.advance(10); ++h.sample.missing; h.sample.faults += 20; ++h.sample.recoveries; h.advance(20);
        assert(h.actions.empty()); // Isolated loss cannot trigger repeated downgrades.
        h.advance(20); assert(h.actions.size() == 1 && h.actions.back() == Action::Enhance);
    }
    {
        Harness h; h.profile = Profile::Middle; h.advance(6); h.sample.errors += 2; h.advance(2);
        assert(h.actions == std::vector<Action>{Action::Degrade}); h.advance(8); assert(h.actions.size() == 1);
    }
    {
        Harness h; h.advance(32); h.controller.enable(false,h.now); h.advance(60);
        assert(!h.controller.pending() && h.actions.size() == 1);
        h.controller.enable(true,h.now); h.advance(31); assert(h.actions.size() == 1);
        h.advance(3); assert(h.actions.size() == 2);
        assert(h.controller.diagnostics().find("private") == std::string::npos);
    }
    {
        Harness h; h.profile = Profile::Middle; h.advance(34); h.profile = Profile::Low; h.advance(2);
        assert(h.event("rejected-profile") && !h.controller.pending());
    }
    {
        Harness h; h.send = false; h.advance(33); assert(h.actions.size() == 1);
        h.advance(29); assert(h.actions.size() == 1); h.advance(4); assert(h.actions.size() == 2);
    }
    {
        Harness h; h.server.fps.known = false; h.refresh = false;
        for (unsigned i=0;i<45;++i) {
            h.server.read(Json{{"preset","LOW"}},h.now); h.advance(2);
        }
        assert(h.actions.empty()); // Fresh profile alone cannot refresh missing FPS.
    }
    {
        Harness h; h.refresh = false;
        for (unsigned i=0;i<45;++i) {
            h.server.read(Json{{"fps",30},{"preset","private"}},h.now); h.advance(2);
        }
        assert(h.actions.empty() && h.server.profile.age(h.now) == 90000);
    }
    {
        Harness h; h.profile = Profile::Middle; h.sample.queuedVideo = 5; h.sample.oldestVideoMs = 150; h.advance(6);
        assert(h.actions == std::vector<Action>{Action::Degrade});
    }
    {
        Harness h; h.advance(20); h.sample.frames = 0; h.sample.workUs = 0;
        h.advance(30); assert(h.actions.empty()); h.advance(4); assert(h.actions.size()==1);
    }
    {
        Harness h; h.profile = Profile::Middle; h.advance(5);
        // Exactly one boundary sample must belong to one window, even when
        // the following sample immediately reports an empty drained queue.
        h.now += 900; h.sample.frames += 27; h.sample.decodedFrames += 27; h.sample.workUs += 81000;
        h.server.read(Json{{"preset","MID"},{"fps",30}},h.now);
        h.controller.observe(h.now,h.sample,h.server);
        h.now += 100; h.sample.frames += 3; h.sample.decodedFrames += 3; h.sample.workUs += 9000; h.sample.queuedVideo = 4;
        const auto action = h.controller.observe(h.now,h.sample,h.server);
        assert(action == Action::None);
        h.sample.queuedVideo = 0; h.advance(2);
        assert(h.actions.empty());
    }
    {
        Harness h; h.sample.queuedVideo = 3; h.advance(32);
        assert(h.actions == std::vector<Action>{Action::Enhance}); // Normal small receive batches.
    }
    {
        Harness h; h.refresh = false;
        for (unsigned i=0;i<20;++i) {
            if (h.now % 8000 == 0) h.server.read(Json{{"preset","LOW"},{"fps",30}},h.now);
            h.advance(2);
        }
        assert(h.actions.empty()); // Updates at 8s boundaries cannot hide intermediate stale periods.
        h.refresh = true; h.advance(31); assert(h.actions.empty()); h.advance(3); assert(h.actions.size() == 1);
    }
    {
        // Deep but young receive batches are not a sustained playback backlog.
        Harness h; h.profile = Profile::Middle; h.sample.queuedVideo = 20; h.sample.oldestVideoMs = 8;
        h.advance(34); assert(h.actions == std::vector<Action>{Action::Enhance});
    }
    {
        // An aged boundary spike immediately drained must not stand in for
        // four seconds of pressure, even when repeated in adjacent windows.
        Harness h; h.profile = Profile::Middle;
        for (unsigned i = 0; i < 8; ++i) {
            h.sample.queuedVideo = 8; h.sample.oldestVideoMs = 200; h.advance(1);
            h.sample.queuedVideo = 0; h.sample.oldestVideoMs = 0; h.advance(1);
        }
        assert(h.actions.empty());
        h.advance(30); assert(h.actions.empty()); h.advance(4);
        assert(h.actions == std::vector<Action>{Action::Enhance});
    }
    {
        // High depth alone and age alone cannot provoke a downgrade. Age
        // without drainage still prevents a speculative quality increase.
        Harness h; h.profile = Profile::Middle; h.sample.queuedVideo = 1; h.sample.oldestVideoMs = 500;
        h.advance(90); assert(h.actions.empty());
    }
    {
        // After measured HIGH overload and a confirmed downgrade, wait two
        // minutes before trying HIGH again, then four after the next failure.
        Harness h; h.profile = Profile::High; h.overload = true; h.advance(6);
        assert(h.actions == std::vector<Action>{Action::Degrade});
        h.profile = Profile::Middle; h.overload = false; h.advance(2);
        assert(h.event("downgrade-hold") && !h.controller.pending());
        h.advance(118); assert(h.actions.size() == 1);
        h.advance(4); assert(h.actions.size() == 2 && h.actions.back() == Action::Enhance);
        h.profile = Profile::High; h.advance(2); h.overload = true; h.advance(6);
        assert(h.actions.size() == 3 && h.actions.back() == Action::Degrade);
        h.profile = Profile::Middle; h.overload = false; h.advance(2);
        h.advance(238); assert(h.actions.size() == 3);
        h.advance(4); assert(h.actions.size() == 4 && h.actions.back() == Action::Enhance);
        assert(h.controller.diagnostics().find("autoDowngrades=2") != std::string::npos);
    }
    {
        // A backend-initiated downshift is also evidence against an immediate
        // retry; a failed command must not erase the stronger downgrade hold.
        Harness h; h.profile = Profile::High; h.advance(4);
        h.profile = Profile::Middle; h.advance(2); h.controller.sendFailed(h.now);
        h.advance(118); assert(h.actions.empty()); h.advance(4);
        assert(h.actions == std::vector<Action>{Action::Enhance});
    }
    {
        // Merely receiving a new same-profile revision is no confirmation.
        Harness h; h.advance(32); assert(h.controller.pending());
        h.advance(9); assert(h.controller.pending()); h.advance(1);
        assert(!h.controller.pending() && h.event("timeout-unconfirmed"));
    }
    {
        // Repeated capacity failures cap the hold at ten minutes; explicit
        // manual/Auto switching starts a fresh policy experiment.
        Harness h; h.staticSource = true; h.profile = Profile::High; h.advance(2);
        for (unsigned i = 0; i < 5; ++i) {
            h.profile = Profile::Middle; h.advance(2);
            if (i != 4) { h.profile = Profile::High; h.advance(2); }
        }
        assert(h.actions.empty());
        const auto expected = "autoNextUpMs=" + std::to_string(h.now + 600000);
        assert(h.controller.diagnostics().find(expected) != std::string::npos);
        assert(h.controller.diagnostics().find("autoDowngrades=4") != std::string::npos);
        h.controller.enable(false, h.now); h.controller.enable(true, h.now);
        assert(h.controller.diagnostics().find("autoDowngrades=0") != std::string::npos);
    }
    {
        // A changed preset carried by an old timestamp is not a confirmation.
        Harness h; h.advance(32); assert(h.controller.pending());
        h.server.profile.set(Profile::Middle, h.now - 7000);
        h.controller.observe(h.now, h.sample, h.server);
        assert(h.controller.pending() && !h.event("qualityEvent=confirmed"));
    }
    {
        // A delayed but still fresh pre-command update cannot confirm a command.
        Harness h; h.advance(32); assert(h.controller.pending());
        h.server.profile.set(Profile::Middle, h.now - 1);
        h.controller.observe(h.now, h.sample, h.server);
        assert(h.controller.pending() && !h.event("qualityEvent=confirmed"));
    }
    {
        // Manual profile changes must not be attributed to the next Auto run.
        Harness h; h.profile = Profile::High; h.advance(4);
        h.controller.enable(false, h.now);
        h.profile = Profile::Low; h.advance(2);
        h.controller.enable(true, h.now); h.advance(34);
        assert(h.actions == std::vector<Action>{Action::Enhance});
        assert(h.controller.diagnostics().find("autoDowngrades=0") != std::string::npos);
    }
    {
        // Fast decoding is not smooth delivery: count only new presentations.
        Harness h; h.profile = Profile::High; h.advance(6);
        h.presentedPerTick = 1; h.supersededPerTick = 2;
        h.advance(2); assert(h.actions.empty());
        assert(h.controller.fps() == 10);
        assert(h.controller.diagnostics().find("decodedFps=30.00") != std::string::npos);
        assert(h.controller.diagnostics().find("videoWorkMs=3.00") != std::string::npos);
        h.advance(2); assert(h.actions == std::vector<Action>{Action::Degrade});
        assert(std::string(h.controller.reason()) == "video-delivery-loss");
        h.profile = Profile::Middle; h.presentedPerTick = 3; h.supersededPerTick = 0;
        h.advance(2); assert(!h.controller.pending() && h.event("qualityEvent=confirmed"));
        h.advance(118); assert(h.actions.size() == 1);
        h.advance(4); assert(h.actions.size() == 2 && h.actions.back() == Action::Enhance);
    }
    {
        // A renderer that presents nothing must still show local loss even if
        // superseding instrumentation reports no replacement yet.
        Harness h; h.profile = Profile::High; h.advance(6);
        h.presentedPerTick = 0; h.advance(4);
        assert(h.actions == std::vector<Action>{Action::Degrade});
    }
    {
        // Low-rate source content with matching delivery is not local loss.
        Harness h; h.profile = Profile::High; h.presentedPerTick = h.decodedPerTick = 1;
        h.advance(100); assert(h.actions.empty());
        h.staticSource = true; h.advance(100); assert(h.actions.empty());
    }
    {
        // One bad interval, including a large replacement burst, must recover
        // without lowering quality. It also restarts the upgrade stable window.
        Harness h; h.profile = Profile::Middle; h.advance(20);
        h.presentedPerTick = 1; h.supersededPerTick = 2; h.advance(2);
        h.presentedPerTick = 3; h.supersededPerTick = 0; h.advance(30);
        assert(h.actions.empty()); h.advance(4);
        assert(h.actions == std::vector<Action>{Action::Enhance});
    }
    {
        // A single lost local frame in each window is below downgrade limits,
        // but never establishes the completely healthy headroom for an upgrade.
        Harness h; h.profile = Profile::Middle;
        for (unsigned i = 0; i < 40; ++i) { ++h.sample.supersededFrames; h.advance(2); }
        assert(h.actions.empty());
    }
    {
        // Sustained local audio loss matters even with full video throughput.
        for (const auto counter : {&Sample::audioDropped, &Sample::audioUnderruns}) {
            Harness h; h.profile = Profile::High; h.advance(6);
            h.sample.*counter += 10; h.advance(2); assert(h.actions.empty());
            h.sample.*counter += 10; h.advance(2);
            assert(h.actions == std::vector<Action>{Action::Degrade});
            assert(std::string(h.controller.reason()) == "audio-playback-loss");
        }
    }
    {
        // A lone startup/recovery-sized audio burst cannot lower the preset.
        Harness h; h.profile = Profile::Middle; h.advance(20);
        h.sample.audioDropped += 100; h.sample.audioUnderruns += 20; h.advance(2);
        h.advance(30); assert(h.actions.empty()); h.advance(4);
        assert(h.actions == std::vector<Action>{Action::Enhance});
    }
    {
        // Even small repeated audio loss blocks a speculative upgrade.
        Harness h; h.profile = Profile::Middle;
        for (unsigned i = 0; i < 40; ++i) { ++h.sample.audioDropped; h.advance(2); }
        assert(h.actions.empty());
    }
    {
        // Deliberate PCM clears are recovery evidence, never an independent
        // overload signal, but cannot be part of a clean upgrade window.
        Harness h; h.profile = Profile::Middle;
        for (unsigned i = 0; i < 40; ++i) { h.sample.audioResets += 10; h.advance(2); }
        assert(h.actions.empty()); h.advance(30); assert(h.actions.empty());
        h.advance(4); assert(h.actions == std::vector<Action>{Action::Enhance});
    }
    {
        // Partial counter resets must rebaseline; unsigned subtraction must
        // neither turn the reset into loss nor preserve earlier headroom.
        for (const auto counter : {&Sample::decodedFrames, &Sample::supersededFrames,
                                   &Sample::audioDropped, &Sample::audioUnderruns, &Sample::audioResets}) {
            Harness h; h.sample.*counter = 10; h.advance(20);
            h.sample.*counter = 0; h.advance(30); assert(h.actions.empty());
            h.advance(4); assert(h.actions == std::vector<Action>{Action::Enhance});
        }
    }
    {
        // Recovery intervals do not count toward the two-window loss signal.
        Harness h; h.profile = Profile::High; h.advance(6);
        h.presentedPerTick = 1; h.supersededPerTick = 2; h.sample.recovering = true;
        h.audioDroppedPerTick = 1; h.advance(10); assert(h.actions.empty());
        h.sample.recovering = false; h.advance(2); assert(h.actions.empty());
        h.advance(2); assert(h.actions.empty()); h.advance(2);
        assert(h.actions == std::vector<Action>{Action::Degrade});
    }
    {
        // A gap between loss windows cancels their continuity.
        Harness h; h.profile = Profile::High; h.advance(6);
        h.presentedPerTick = 1; h.supersededPerTick = 2; h.advance(2);
        h.now += 1500; h.advance(2); assert(h.actions.empty());
        h.advance(2); assert(h.actions.empty()); h.advance(2);
        assert(h.actions == std::vector<Action>{Action::Degrade});
    }
    {
        // A loss window at one preset cannot complete the downgrade signal for
        // a different preset, including a backend-initiated downshift.
        Harness h; h.profile = Profile::High; h.advance(6);
        h.presentedPerTick = 1; h.supersededPerTick = 2; h.advance(2);
        h.profile = Profile::Middle; h.advance(2); assert(h.actions.empty());
        h.advance(2); assert(h.actions.empty());
        h.advance(2); assert(h.actions == std::vector<Action>{Action::Degrade});
    }
    {
        // Observe MID 100ms before the old HIGH window ends. Neither delivery
        // nor audio loss may use that mostly-HIGH window as MID evidence.
        for (const bool audio : {false, true}) {
            Harness h; h.profile = Profile::High; h.advance(6);
            h.presentedPerTick = audio ? 3 : 1; h.supersededPerTick = audio ? 0 : 2;
            h.audioDroppedPerTick = audio ? 1 : 0;
            h.ticks(19); assert(h.now == 7900 && h.actions.empty());
            h.profile = Profile::Middle;
            h.server.read(Json{{"preset", "MID"}, {"fps", 30}}, h.now);
            assert(h.controller.observe(h.now, h.sample, h.server) == Action::None);
            h.ticks(21); assert(h.now == 10000 && h.actions.empty());
            h.ticks(18); assert(h.now == 11800 && h.actions.empty());
            h.ticks(1); assert(h.actions == std::vector<Action>{Action::Degrade});
        }
    }
    {
        // Intentional audio recovery clears interrupt repeated audio trouble.
        Harness h; h.profile = Profile::High; h.advance(6);
        h.audioDroppedPerTick = 1; h.advance(2); assert(h.actions.empty());
        ++h.sample.audioResets; h.advance(2); assert(h.actions.empty());
        h.advance(2); assert(h.actions.empty()); h.advance(2);
        assert(h.actions == std::vector<Action>{Action::Degrade});
    }
    {
        // Persistent delivery trouble still obeys the existing bounded retry
        // and unconfirmed-command timeouts; it cannot spam downgrade requests.
        Harness h; h.profile = Profile::High;
        h.presentedPerTick = 1; h.supersededPerTick = 2; h.advance(200);
        assert((h.actions == std::vector<Action>{Action::Degrade, Action::Degrade}));
        assert(!h.controller.pending() && std::string(h.controller.reason()) == "down-unconfirmed");
    }
    std::cout << "Auto quality: sanitized per-field telemetry, freshness, contiguous stability, aged residual pressure, progressive downgrade holds, overload, displayed/decoded delivery, sustained audio loss, counter resets, static source, confirmation/rejection, bounded retries, mode changes, HIGH ceiling passed\n";
}
