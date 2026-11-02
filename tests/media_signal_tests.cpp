#include "features/streaming/domain/media_signal.hpp"
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>
using namespace std::chrono_literals;
int main() {
    stream::MediaSignals signals;
    const auto audio = signals.audio.sequence();
    for (unsigned n = 0; n < 200; ++n) {
        const auto sequence = signals.video.sequence();
        // Notification between the empty poll and entering wait is retained.
        signals.video.notify();
        const auto before = std::chrono::steady_clock::now();
        signals.video.wait(sequence, 2s);
        assert(std::chrono::steady_clock::now() - before < 500ms);
    }
    assert(signals.audio.sequence() == audio);
    for (unsigned n = 0; n < 100; ++n) {
        std::promise<void> entering;
        const auto sequence = signals.video.sequence();
        auto consumer = std::async(std::launch::async, [&] {
            entering.set_value();
            signals.video.wait(sequence, 2s);
        });
        entering.get_future().wait();
        signals.video.notify();
        assert(consumer.wait_for(500ms) == std::future_status::ready);
        consumer.get();
    }
    const auto video = signals.video.sequence();
    auto waiter = std::async(std::launch::async, [&] { signals.audio.wait(audio, 2s); });
    signals.notifyAll(); // Shared stop/reset interrupts either consumer.
    assert(waiter.wait_for(500ms) == std::future_status::ready);
    waiter.get();
    assert(signals.video.sequence() != video);
    std::cout << "Media readiness: notify-before-wait, concurrent wait race, independent channels and stop/reset wake PASS\n";
}
