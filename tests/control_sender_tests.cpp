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
