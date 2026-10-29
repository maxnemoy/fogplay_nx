#include "features/streaming/infrastructure/control_delivery.hpp"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using stream::ControlDelivery;
static void unknownAndAge() {
    ControlDelivery delivery;
    const auto empty = delivery.snapshot();
    assert(!empty.known && !empty.blocked && empty.currentBytes == 0 && empty.maxBytes == 0);
    const auto unknown = delivery.diagnostics(10000);
    assert(unknown.find("controlBufferedKnown=0") != std::string::npos);
    assert(unknown.find("controlBufferedAgeMs=-1 controlBufferedStale=1") != std::string::npos);
    delivery.sample(100, 1000);
    delivery.sample(50, 1100);
    const auto sample = delivery.snapshot();
    assert(sample.known && sample.currentBytes == 50 && sample.maxBytes == 100 && sample.sampledAt == 1100);
    assert(delivery.diagnostics(1200).find("controlBufferedAgeMs=100 controlBufferedStale=0") != std::string::npos);
    assert(delivery.diagnostics(1099).find("controlBufferedAgeMs=-1 controlBufferedStale=1") != std::string::npos);
}
static void thresholdAndEpisodes() {
    ControlDelivery delivery;
    delivery.sample(ControlDelivery::BlockedBytes, 10);
    assert(!delivery.snapshot().blocked);
    delivery.sample(ControlDelivery::BlockedBytes + 1, 20);
    assert(delivery.snapshot().blocked && delivery.snapshot().blockedSince == 20);
    delivery.sample(100000, 40);
    const auto blocked = delivery.snapshot();
    assert(blocked.blockedSince == 20 && blocked.currentBytes == 100000 && blocked.maxBytes == 100000);
    assert(delivery.diagnostics(70).find("controlBufferedBlocked=1 controlBufferedBlockedMs=50") != std::string::npos);
    delivery.sample(ControlDelivery::BlockedBytes, 80);
    const auto clear = delivery.snapshot();
    assert(!clear.blocked && clear.blockedSince == 0 && clear.maxBytes == 100000);
    assert(delivery.diagnostics(90).find("controlBufferedBlocked=0 controlBufferedBlockedMs=0") != std::string::npos);
    delivery.sample(70000, 100);
    assert(delivery.snapshot().blockedSince == 100);
    delivery.sample(70000, 50); // Regressed clocks cannot extend the preceding episode.
    assert(delivery.snapshot().blockedSince == 50);
    assert(delivery.diagnostics(60).find("controlBufferedBlockedMs=10") != std::string::npos);
}
static void staleAndBounds() {
    ControlDelivery delivery;
    delivery.sample(70000, 1000);
    delivery.sample(80000, 1500);
    const auto fresh = delivery.diagnostics(6500);
    assert(fresh.find("controlBufferedAgeMs=5000 controlBufferedStale=0") != std::string::npos);
    assert(fresh.find("controlBufferedBlockedMs=5500") != std::string::npos);
    const auto stale = delivery.diagnostics(6501);
    assert(stale.find("controlBufferedAgeMs=5001 controlBufferedStale=1") != std::string::npos);
    assert(stale.find("controlBufferedBlockedMs=500") != std::string::npos);
    assert(delivery.diagnostics(100000).find("controlBufferedBlockedMs=500") != std::string::npos);
    assert(delivery.diagnostics(1499).find("controlBufferedBlockedMs=0") != std::string::npos);
    delivery.sample(80000, 10000);
    assert(delivery.snapshot().blockedSince == 10000);
    assert(delivery.diagnostics(10020).find("controlBufferedBlockedMs=20") != std::string::npos);
    delivery.sample(std::numeric_limits<std::size_t>::max(), 0);
    const auto text = delivery.diagnostics(std::numeric_limits<std::uint64_t>::max());
    assert(text.size() < 384 && text.find("controlBufferedBlockedMs=0") != std::string::npos);
    assert(text.find('\n') == std::string::npos && text.find("controlBufferedBytes=") != std::string::npos);
}
static void concurrentSnapshots() {
    ControlDelivery delivery;
    std::atomic<bool> start{false}, done{false}, valid{true};
    std::thread writer([&] {
        while (!start.load()) std::this_thread::yield();
        for (std::uint64_t n = 1; n <= 50000; ++n)
            delivery.sample(n % 2 ? ControlDelivery::BlockedBytes + 1 : 0, n);
        done = true;
    });
    std::vector<std::thread> readers;
    for (unsigned reader = 0; reader < 4; ++reader) readers.emplace_back([&] {
        while (!start.load()) std::this_thread::yield();
        do {
            const auto state = delivery.snapshot();
            if (state.known) {
                const bool odd = state.sampledAt % 2;
                if (state.blocked != odd || state.currentBytes != (odd ? ControlDelivery::BlockedBytes + 1 : 0) ||
                    state.maxBytes != ControlDelivery::BlockedBytes + 1 || state.blockedSince != (odd ? state.sampledAt : 0)) valid = false;
            }
            if (delivery.diagnostics(50000).size() >= 384) valid = false;
        } while (!done.load());
    });
    start = true;
    writer.join();
    for (auto& reader : readers) reader.join();
    assert(valid && delivery.snapshot().sampledAt == 50000 && !delivery.snapshot().blocked);
}
int main() {
    unknownAndAge(); thresholdAndEpisodes(); staleAndBounds(); concurrentSnapshots();
    std::cout << "Control delivery: unknown/fresh/stale samples, strict 64KiB threshold, bounded episode age, clock rollback and concurrent snapshots passed\n";
}
