#include "features/streaming/infrastructure/async_ice_snapshot.hpp"
#include <atomic>
#include <cassert>
#include <iostream>
#include <memory>

using Clock = std::chrono::steady_clock;
struct Sample {
    int proof = 0;
    explicit Sample(int value) : proof(value) {}
    struct Timing { std::uint64_t calls = 1, totalUs = 7, maxUs = 7; } diagnosticTiming, pairTiming;
};
using Worker = stream::AsyncIceSnapshot<Sample>;
struct Block {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false, released = false;
    void wait() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true; condition.notify_all();
        condition.wait(lock, [this] { return released; });
    }
    void waitEntered() {
        std::unique_lock<std::mutex> lock(mutex);
        assert(condition.wait_for(lock, std::chrono::seconds(2), [this] { return entered; }));
    }
    void release() { std::lock_guard<std::mutex> lock(mutex); released = true; condition.notify_all(); }
};
struct Transport {
    std::atomic<unsigned>& destroyed;
    ~Transport() { ++destroyed; }
};
static void idle(Worker& worker) {
    const auto began = Clock::now();
    for (;;) {
        const auto state = worker.state();
        if (!state.active && !state.pending) return;
        assert(Clock::now() - began < std::chrono::seconds(2));
        std::this_thread::yield();
    }
}
int main() {
    {
        Worker worker;
        auto block = std::make_shared<Block>();
        std::atomic<unsigned> destroyed{0};
        auto transport = std::make_shared<Transport>(Transport{destroyed});
        destroyed = 0;
        const auto generation = worker.state().revision;
        assert(worker.request(generation, [block, transport] { block->wait(); return Sample{1}; }));
        block->waitEntered(); transport.reset();
        const auto began = Clock::now();
        for (unsigned n = 0; n < 10000; ++n) { assert(worker.state().active); assert(!worker.take()); }
        assert(Clock::now() - began < std::chrono::milliseconds(200));
        worker.invalidate();
        const auto fresh = worker.state().revision;
        assert(fresh != generation);
        assert(worker.request(fresh, [] { return Sample{2}; }));
        assert(!worker.request(fresh, [] { return Sample{3}; })); // One bounded pending read.
        assert(!destroyed); // Strong handle survives cancellation until getter returns.
        block->release(); idle(worker);
        const auto result = worker.take();
        assert(result && result->revision == fresh && result->value.proof == 2);
        assert(worker.state().discarded == 1 && worker.state().completed == 2);
        assert(worker.state().diagnostic.calls == 2 && worker.state().pair.calls == 2);
        assert(destroyed == 1);
    }
    {
        Worker worker;
        auto block = std::make_shared<Block>();
        assert(worker.request(worker.state().revision, [block] { block->wait(); return Sample{9}; }));
        block->waitEntered();
        const auto began = Clock::now();
        worker.requestStop();
        assert(worker.state().active && worker.state().stopping);
        assert(!worker.request(worker.state().revision, [] { return Sample{10}; }));
        assert(Clock::now() - began < std::chrono::milliseconds(100));
        // Models application HTTP stop dispatch before close's mandatory join.
        bool httpStopDispatched = true;
        std::atomic<bool> joined{false};
        std::thread close([&] { assert(httpStopDispatched); worker.stop(); joined = true; });
        assert(!joined); block->release(); close.join();
        assert(joined && !worker.take() && worker.state().discarded == 1);
        Worker replacement;
        assert(replacement.request(replacement.state().revision, [] { return Sample{11}; }));
        idle(replacement);
        const auto result = replacement.take();
        assert(result && result->value.proof == 11); // No old tenant proof transferred.
    }
    std::cout << "Async ICE mailbox: blocked read nonblocking publication, bounded coalescing, stale rejection, strong transport lifetime, stop-before-join and fresh owner passed\n";
}
