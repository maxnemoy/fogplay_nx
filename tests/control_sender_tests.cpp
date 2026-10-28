#include "features/streaming/infrastructure/async_control_sender.hpp"
#include <atomic>
#include <cassert>
#include <future>
#include <iostream>
#include <mutex>
#include <vector>
using Sender = stream::AsyncControlSender;
using Mode = stream::ControlQueue::Mode;
using namespace std::chrono_literals;
struct Fake {
    std::mutex mutex;
    std::condition_variable changed;
    bool block = false, entered = false;
    std::vector<std::string> sent;
    void send(const std::string& text) {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true; changed.notify_all();
        changed.wait(lock, [this] { return !block; });
        sent.push_back(text); changed.notify_all();
    }
    void awaitEntry() {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(lock, 2s, [this] { return entered; }));
    }
    void unblock() { std::lock_guard<std::mutex> lock(mutex); block = false; changed.notify_all(); }
    void awaitSize(std::size_t size) {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(lock, 2s, [this, size] { return sent.size() >= size; }));
    }
};
int main() {
    {
        // All physical buttons, including both digital triggers, remain ordered
        // while only the newest analog snapshot within each held state survives.
        Fake fake; fake.block = true;
        Sender sender([] { return true; }, [&fake](const auto& text) { fake.send(text); });
        sender.allow(true);
        assert(sender.enqueueGamepad("in-flight", std::uint32_t{0})); fake.awaitEntry();
        assert(sender.enqueueGamepad("button-down", std::uint32_t{1}));
        for (unsigned n = 0; n < 100; ++n)
            assert(sender.enqueueGamepad("held-" + std::to_string(n), std::uint32_t{1}));
        assert(sender.enqueueGamepad("trigger-down", std::uint32_t{1} | (std::uint32_t{1} << 18)));
        assert(sender.enqueueGamepad("button-up", std::uint32_t{1} << 18));
        assert(sender.enqueueGamepad("trigger-up", std::uint32_t{0}));
        const auto queued = sender.snapshot();
        assert(queued.inFlight && queued.pending == 6 && queued.buttonQueuedEdges == 4 && queued.buttonRejectedEdges == 0);
        assert(queued.psFrames == 0 && queued.gamepadCoalesced == 99);
        fake.unblock(); fake.awaitSize(6); sender.stop();
        assert((fake.sent == std::vector<std::string>{"in-flight", "button-down", "held-99", "trigger-down", "button-up", "trigger-up"}));
    }
    {
        // Each of the 18 buttons and two triggers must preserve a short tap.
        Fake fake;
        Sender sender([] { return true; }, [&fake](const auto& text) { fake.send(text); });
        sender.block(true); sender.allow(true);
        std::vector<std::string> expected;
        for (unsigned bit = 0; bit < 20; ++bit) {
            const auto down = "down-" + std::to_string(bit), up = "up-" + std::to_string(bit);
            assert(sender.enqueueGamepad(down, std::uint32_t{1} << bit));
            assert(sender.enqueueGamepad(up, std::uint32_t{0}));
            expected.push_back(down); expected.push_back(up);
        }
        assert(sender.snapshot().pending == 40 && sender.snapshot().buttonQueuedEdges == 40);
        sender.block(false); fake.awaitSize(40); sender.stop();
        assert(fake.sent == expected);
    }
    {
        // A full ordered/release budget still has room for a neutral gamepad.
        Fake fake;
        Sender sender([] { return true; }, [&fake](const auto& text) { fake.send(text); });
        sender.block(true); sender.allow(true);
        assert(sender.enqueueGamepad("button-down", std::uint32_t{1}));
        for (unsigned n = 0; n < 46; ++n) assert(sender.enqueue("ordered"));
        for (unsigned n = 0; n < 16; ++n) assert(sender.enqueue("release", Mode::Release, std::to_string(n)));
        assert(sender.snapshot().pending == 63);
        assert(sender.enqueueGamepad("neutral", std::uint32_t{0}));
        assert(sender.snapshot().pending == 64 && !sender.snapshot().stalled);
        // An analog-only sample may be retried once room exists. Losing an edge
        // instead publishes a stall, so the application cannot silently miss it.
        assert(!sender.enqueueGamepad("periodic-neutral", std::uint32_t{0}));
        assert(!sender.snapshot().stalled);
        assert(!sender.enqueueGamepad("overflow-down", std::uint32_t{1}));
        assert(sender.snapshot().stalled && sender.snapshot().buttonRejectedEdges == 1 && sender.snapshot().pending == 64);
        sender.stop(); assert(fake.sent.empty());
    }
    {
        // Analog snapshots from separate held-button segments must not consume
        // the neutral slot. Preserve every edge while reclaiming a stale axis sample.
        Fake fake;
        Sender sender([] { return true; }, [&fake](const auto& text) { fake.send(text); });
        sender.block(true); sender.allow(true);
        std::uint32_t buttons = 0xfffff;
        assert(sender.enqueueGamepad("all-down", buttons));
        std::vector<std::string> expected{"all-down"};
        for (unsigned n = 0; n < 46; ++n) {
            const auto text = "ordered-" + std::to_string(n);
            assert(sender.enqueue(text)); expected.push_back(text);
        }
        assert(sender.enqueueGamepad("redundant-before-release", buttons));
        for (unsigned bit = 0; bit < 8; ++bit) {
            buttons &= ~(std::uint32_t{1} << bit);
            const auto edge = "release-" + std::to_string(bit), analog = "analog-" + std::to_string(bit);
            assert(sender.enqueueGamepad(edge, buttons)); expected.push_back(edge);
            assert(sender.enqueueGamepad(analog, buttons)); expected.push_back(analog);
        }
        assert(sender.snapshot().pending == 64);
        assert(sender.enqueueGamepad("full-neutral", std::uint32_t{0})); expected.push_back("full-neutral");
        assert(sender.snapshot().pending == 64 && !sender.snapshot().stalled && sender.snapshot().gamepadCoalesced == 1);
        sender.block(false); fake.awaitSize(expected.size()); sender.stop();
        assert(fake.sent == expected);
    }
    {
        // Byte saturation has the same neutral guarantee; oversize rejection
        // must not erase accepted states before admission is known to succeed.
        stream::ControlQueue queue;
        const std::string large(64 * 1024, 'x');
        assert(queue.push("down", Mode::GamepadEdge, {}, 0));
        assert(queue.push(large, Mode::Ordered, {}, 0));
        assert(queue.push(large, Mode::Ordered, {}, 0));
        assert(queue.push(large, Mode::Gamepad, {}, 0));
        assert(queue.push("up-one", Mode::GamepadRelease, {}, 0));
        assert(queue.push(std::string(32 * 1024, 'y'), Mode::Gamepad, {}, 0));
        const auto count = queue.size(), bytes = queue.bytes();
        assert(!queue.push(large + "invalid", Mode::GamepadNeutral, {}, 0));
        assert(queue.size() == count && queue.bytes() == bytes);
        assert(queue.push(large, Mode::GamepadNeutral, {}, 0));
        assert(queue.bytes() <= stream::ControlQueue::MaxBytes && queue.coalesced() == 1);
        std::vector<Mode> modes;
        while (!queue.empty()) { modes.push_back(queue.front().mode); queue.pop(0); }
        assert((modes == std::vector<Mode>{Mode::GamepadEdge, Mode::Ordered, Mode::Ordered,
            Mode::GamepadRelease, Mode::Gamepad, Mode::GamepadNeutral}));
    }
    {
        Fake old, fresh;
        Sender previous([] { return true; }, [&old](const auto& text) { old.send(text); });
        previous.block(true); previous.allow(true);
        assert(previous.enqueueGamepad("stale-down", std::uint32_t{1})); previous.stop("old-neutral");
        assert(old.sent.empty());
        Sender next([] { return true; }, [&fresh](const auto& text) { fresh.send(text); });
        next.allow(true);
        assert(next.enqueueGamepad("fresh-neutral", std::uint32_t{0})); fresh.awaitSize(1);
        assert(next.snapshot().buttonQueuedEdges == 0);
        assert(next.enqueueGamepad("fresh-down", std::uint32_t{1})); fresh.awaitSize(2); next.stop("fresh-up");
        assert((fresh.sent == std::vector<std::string>{"fresh-neutral", "fresh-down", "fresh-up"}));
    }
    {
        Fake fake;
        Sender sender([] { return true; }, [&fake](const auto& text) { fake.send(text); });
        sender.block(true); sender.allow(true);
        assert(sender.enqueueNative("finger0-down", Mode::Ordered, "touch:0", "finger0-up", 1));
        assert(sender.enqueueNative("finger1-down", Mode::Ordered, "touch:1", "finger1-up", 1));
        for (unsigned n = 0; n < 100; ++n) {
            assert(sender.enqueueNative("finger0-move-" + std::to_string(n), Mode::TouchMove, "touch:0", "finger0-up", 2));
            assert(sender.enqueueNative("finger1-move-" + std::to_string(n), Mode::TouchMove, "touch:1", "finger1-up", 2));
            assert(sender.enqueueNative("accel-" + std::to_string(n), Mode::Motion, "sensor:1", "accel-neutral", 4));
            assert(sender.enqueueNative("gyro-" + std::to_string(n), Mode::Motion, "sensor:2", "gyro-zero", 5));
        }
        assert(sender.enqueueNative("finger0-up", Mode::Release, "touch:0", {}, 3));
        assert(sender.enqueueNative("finger1-up", Mode::Release, "touch:1", {}, 3));
        assert(sender.snapshot().pending == 8);
        const auto queued = sender.snapshot();
        assert(queued.nativeQueued[1] == 2 && queued.nativeQueued[2] == 200 && queued.nativeQueued[4] == 100);
        assert(queued.nativeCoalesced[2] == 198 && queued.nativeCoalesced[4] == 99 && queued.nativeCoalesced[5] == 99);
        sender.block(false); fake.awaitSize(8); sender.stop();
        assert((fake.sent == std::vector<std::string>{"finger0-down", "finger1-down", "finger0-move-99", "finger1-move-99", "accel-99", "gyro-99", "finger0-up", "finger1-up", "accel-neutral", "gyro-zero"}));
        const auto sent = sender.snapshot();
        assert(sent.nativeSent[1] == 2 && sent.nativeSent[2] == 2 && sent.nativeSent[3] == 2);
        assert(sent.nativeSent[4] == 2 && sent.nativeSent[5] == 2 && sent.nativeFailed[4] == 0);
    }
    {
        // Moves after a lift/new press must not erase the preceding gesture.
        Fake fake;
        Sender sender([] { return true; }, [&fake](const auto& text) { fake.send(text); });
        sender.block(true); sender.allow(true);
        assert(sender.enqueueNative("down1", Mode::Ordered, "touch:0", "up1", 1));
        assert(sender.enqueueNative("move1", Mode::TouchMove, "touch:0", "up1", 2));
        assert(sender.enqueueNative("up1", Mode::Release, "touch:0", {}, 3));
        assert(sender.enqueueNative("down2", Mode::Ordered, "touch:0", "up2", 1));
        assert(sender.enqueueNative("move2", Mode::TouchMove, "touch:0", "up2", 2));
        assert(sender.snapshot().pending == 5);
        sender.block(false); fake.awaitSize(5); sender.stop();
        assert((fake.sent == std::vector<std::string>{"down1", "move1", "up1", "down2", "move2", "up2"}));
        assert(sender.snapshot().nativeSent[3] == 2); // Explicit first lift plus final shutdown lift.
    }
    {
        Sender sender([] { return true; }, [](const auto&) { throw std::runtime_error("transport failure"); });
        sender.allow(true); assert(sender.enqueueNative("gyro", Mode::Motion, "sensor:2", "gyro-zero", 5));
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (sender.snapshot().failure == Sender::Failure::None && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        sender.stop();
        const auto failed = sender.snapshot();
        assert(failed.nativeQueued[5] == 1 && failed.nativeAttempt[5] == 1 && failed.nativeFailed[5] == 1 && failed.nativeSent[5] == 0);
    }
    {
        Fake fake;
        Sender sender([] { return true; }, [&fake](const auto& text) { fake.send(text); });
        sender.block(true); sender.allow(true);
        assert(sender.enqueueGamepad("touch-down", true));
        for (unsigned n = 0; n < 100; ++n) assert(sender.enqueueGamepad("held-" + std::to_string(n), true));
        assert(sender.enqueueGamepad("touch-up", false));
        for (unsigned n = 0; n < 100; ++n) assert(sender.enqueueGamepad("released-" + std::to_string(n), false));
        assert(sender.enqueueGamepad("short-down", true));
        assert(sender.enqueueGamepad("short-up", false));
        const auto queued = sender.snapshot();
        assert(queued.pending == 6 && queued.psAccepted == 204 && queued.psRejected == 0);
        assert(queued.touchQueuedDown == 2 && queued.touchQueuedUp == 2 && queued.gamepadCoalesced == 198);
        assert(queued.touchAttemptDown == 0 && queued.touchAttemptUp == 0);
        sender.block(false); fake.awaitSize(6); sender.stop();
        assert((fake.sent == std::vector<std::string>{"touch-down", "held-99", "touch-up", "released-99", "short-down", "short-up"}));
        const auto sent = sender.snapshot();
        assert(sent.touchAttemptDown == 2 && sent.touchAttemptUp == 2);
        assert(sent.touchSentDown == 2 && sent.touchSentUp == 2 && sent.touchFailedDown == 0 && sent.touchDropped == 0);
        assert(sent.lastQueuedSeq == 204 && sent.lastSentSeq == 204);
    }
    {
        // Reserve release capacity even if commands fill the ordinary budget.
        Fake fake;
        Sender sender([] { return true; }, [&fake](const auto& text) { fake.send(text); });
        sender.block(true); sender.allow(true);
        assert(sender.enqueueGamepad("touch-down", true));
        for (unsigned n = 0; n < 46; ++n) assert(sender.enqueue("command"));
        assert(!sender.enqueue("full"));
        assert(sender.enqueueGamepad("touch-up", false));
        assert(!sender.enqueueGamepad("rejected-down", true));
        const auto queued = sender.snapshot();
        assert(queued.touchQueuedDown == 1 && queued.touchQueuedUp == 1 && queued.touchRejectedDown == 1);
        assert(queued.psAccepted == 2 && queued.psRejected == 1 && queued.pending == 48);
        sender.stop();
        assert(sender.snapshot().touchDropped == 2 && fake.sent.empty());
    }
    {
        Fake fake;
        Sender sender([] { return true; }, [&fake](const auto& text) { fake.send(text); });
        sender.allow(true); assert(sender.enqueueGamepad("touch-down", true)); fake.awaitSize(1);
        sender.stop("neutral");
        assert((fake.sent == std::vector<std::string>{"touch-down", "neutral"}));
        const auto sent = sender.snapshot();
        assert(sent.touchSentDown == 1 && sent.touchSentUp == 1 && sent.lastSentSeq == 2);
    }
    {
        Sender sender([] { return true; }, [](const auto&) { throw std::runtime_error("transport failure"); });
        sender.allow(true); assert(sender.enqueueGamepad("touch-down", true));
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (sender.snapshot().failure == Sender::Failure::None && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        sender.stop();
        const auto failed = sender.snapshot();
        assert(failed.touchAttemptDown == 1 && failed.touchFailedDown == 1 && failed.touchSentDown == 0);
        assert(failed.psAccepted == 1 && failed.lastQueuedSeq == 1 && failed.lastAttemptSeq == 1 && failed.lastSentSeq == 0);
    }
    {
        Fake fake; fake.block = true;
        Sender sender([] { return true; }, [&fake](const auto& s) { fake.send(s); });
        sender.allow(true);
        assert(sender.enqueue("down", Mode::Ordered, "key:A", "up")); fake.awaitEntry();
        const auto begin = std::chrono::steady_clock::now();
        for (unsigned n = 0; n < 1000; ++n) assert(sender.enqueue(std::to_string(n), Mode::Gamepad));
        assert(sender.enqueue("queued-down", Mode::Ordered, "key:B", "queued-up"));
        assert(std::chrono::steady_clock::now() - begin < 200ms);
        sender.advance(15001);
        const auto status = sender.snapshot();
        assert(status.inFlight && status.stalled && status.inFlightMs >= 15000 && status.pending == 3);
        // stop seals immediately, but cannot destroy a channel under a synchronous
        // in-flight call. Verify the join and exactly the held-state releases.
        const auto sealAt = std::chrono::steady_clock::now();
        sender.requestStop("neutral");
        assert(std::chrono::steady_clock::now() - sealAt < 200ms);
        assert(sender.snapshot().pending == 1); // Only the already in-flight press remains.
        auto stopped = std::async(std::launch::async, [&] { sender.stop(); });
        assert(stopped.wait_for(20ms) == std::future_status::timeout);
        assert(!sender.enqueue("late"));
        fake.unblock(); assert(stopped.wait_for(2s) == std::future_status::ready); stopped.get();
        assert((fake.sent == std::vector<std::string>{"down", "up", "neutral"}));
        assert(sender.snapshot().sendCalls == 3 && sender.snapshot().sendMaxUs >= 20000);
    }
    {
        Fake fake;
        std::atomic<unsigned> pongs{0};
        Sender sender([] { return true; }, [&fake](const auto& s) { fake.send(s); }, [&pongs](bool sent) { assert(sent); ++pongs; });
        sender.block(true); sender.allow(true);
        assert(sender.enqueue("pad-old", Mode::Gamepad)); assert(sender.enqueue("pad-new", Mode::Gamepad));
        assert(sender.enqueue("move-before", Mode::MouseMove)); assert(sender.enqueue("click", Mode::Ordered));
        assert(sender.enqueue("move-old", Mode::MouseMove)); assert(sender.enqueue("move-new", Mode::MouseMove));
        assert(sender.enqueue("down", Mode::Ordered, "key:A", "up"));
        assert(sender.enqueue("up", Mode::Release, "key:A")); assert(sender.enqueue("up", Mode::Release, "key:A"));
        assert(sender.enqueue("pong-old", Mode::Pong)); assert(sender.enqueue("pong-new", Mode::Pong));
        assert(sender.snapshot().pending == 8);
        sender.block(false); fake.awaitSize(8); sender.stop();
        assert((fake.sent == std::vector<std::string>{"pong-old", "pong-new", "pad-new", "move-before", "click", "move-new", "down", "up"}));
        assert(pongs == 2 && sender.snapshot().pending == 0);
    }
    {
        Fake fake;
        Sender sender([] { return true; }, [&fake](const auto& s) { fake.send(s); });
        sender.block(true); sender.allow(true);
        for (unsigned n = 0; n < 47; ++n) assert(sender.enqueue("press", Mode::Ordered));
        assert(!sender.enqueue("saturated", Mode::Ordered));
        for (unsigned n = 0; n < 16; ++n) assert(sender.enqueue("release", Mode::Release, std::to_string(n)));
        assert(sender.enqueue("neutral", Mode::Gamepad)); assert(sender.enqueue("pong", Mode::Pong));
        assert(sender.snapshot().pending == 65 && sender.snapshot().rejected == 1);
        sender.advance(15001); assert(sender.snapshot().stalled);
        sender.stop(); assert(fake.sent.empty() && sender.snapshot().pending == 0);
    }
    {
        Fake fake;
        Sender sender([] { return true; }, [&fake](const auto& s) { fake.send(s); });
        sender.block(true); sender.allow(true);
        assert(sender.enqueue("ordered"));
        for (unsigned n = 0; n < 1000; ++n) assert(sender.enqueue("pong" + std::to_string(n), Mode::Pong));
        assert(sender.snapshot().pending == 17 && sender.snapshot().pongCoalesced == 984);
        sender.block(false); fake.awaitSize(17); sender.stop();
        assert(fake.sent[4] == "ordered" && fake.sent.back() == "pong999");
    }
    {
        Fake old, fresh;
        Sender sender([] { return true; }, [&old](const auto& s) { old.send(s); });
        assert(sender.enqueue("old")); sender.stop();
        Sender replacement([] { return true; }, [&fresh](const auto& s) { fresh.send(s); });
        replacement.allow(true); assert(replacement.enqueue("new")); fresh.awaitSize(1); replacement.stop();
        assert(old.sent.empty() && fresh.sent == std::vector<std::string>{"new"});
    }
    {
        Fake probe, output; probe.block = true;
        Sender sender([&probe] { probe.send("probe"); return true; }, [&output](const auto& s) { output.send(s); });
        sender.allow(true); assert(sender.enqueue("never-sent", Mode::Ordered, "key:A", "up"));
        probe.awaitEntry();
        const auto sealedAt = std::chrono::steady_clock::now();
        sender.requestStop("neutral", false);
        assert(std::chrono::steady_clock::now() - sealedAt < 200ms);
        assert(sender.snapshot().transportActive && !sender.snapshot().inFlight && sender.snapshot().pending == 0);
        assert(!sender.enqueue("old-generation"));
        probe.unblock(); sender.stop();
        assert(output.sent.empty() && !sender.snapshot().transportActive);
    }
    {
        Fake output; output.block = true;
        Sender sender([] { return true; }, [&output](const auto& s) { output.send(s); });
        sender.allow(true); assert(sender.enqueue("ambiguous-down", Mode::Ordered, "key:A", "must-not-release-on-retry"));
        output.awaitEntry(); assert(sender.enqueue("queued-old-state"));
        sender.requestStop("must-not-send-neutral-on-retry", false);
        assert(sender.snapshot().transportActive && sender.snapshot().inFlight && sender.snapshot().pending == 1);
        output.unblock(); sender.stop();
        assert(output.sent == std::vector<std::string>{"ambiguous-down"});
    }
    {
        std::atomic<unsigned> probes{0}, sends{0};
        Sender sender([&probes] { ++probes; return true; }, [&sends](const auto&) { ++sends; });
        sender.allow(true); sender.requestStop("unused-neutral", false);
        assert(!sender.snapshot().transportActive); sender.stop();
        assert(probes == 0 && sends == 0);
    }
    {
        Fake output;
        Sender sender([] { return true; }, [&output](const auto& s) { output.send(s); return false; });
        sender.allow(true); assert(sender.enqueue("accepted-in-rtc-buffer")); output.awaitSize(1); sender.stop();
        assert(output.sent.size() == 1 && sender.snapshot().failure == Sender::Failure::None);
    }
    {
        Sender sender([] { return true; }, [](const auto&) { throw std::runtime_error("secret transport detail"); });
        sender.allow(true); assert(sender.enqueue("failure"));
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (sender.snapshot().failure == Sender::Failure::None && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        assert(sender.snapshot().failure == Sender::Failure::Transport);
        assert(!sender.enqueue("retry must be a new generation")); sender.stop();
    }
    std::cout << "Async control sender: blocked-send publication, ordered/coalesced inputs, pong priority, saturation/stall, held releases, generation shutdown and sanitized failure passed\n";
}
