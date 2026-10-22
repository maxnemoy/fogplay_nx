#include "features/streaming/infrastructure/timed_mutex.hpp"
#include <atomic>
#include <cassert>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using stream::TimedMutex;
using Scope = TimedMutex::Scope;
static const TimedMutex::Stats& stats(const TimedMutex::Snapshot& snapshot, Scope scope) {
    return snapshot[static_cast<std::size_t>(scope)];
}
static void contention() {
    TimedMutex mutex;
    std::promise<void> held, release, attempting;
    auto released = release.get_future();
    std::thread holder([&] {
        TimedMutex::Guard lock(mutex, Scope::Client);
        held.set_value();
        released.wait();
    });
    held.get_future().wait();
    std::thread waiter([&] {
        attempting.set_value();
        TimedMutex::Guard lock(mutex, Scope::Callback);
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    });
    attempting.get_future().wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    release.set_value(); holder.join(); waiter.join();
    const auto snapshot = mutex.snapshot();
    assert(stats(snapshot, Scope::Client).count == 1);
    assert(stats(snapshot, Scope::Client).holdMaxNs >= 30000000);
    assert(stats(snapshot, Scope::Client).holdSlow == 1);
    assert(stats(snapshot, Scope::Callback).count == 1);
    assert(stats(snapshot, Scope::Callback).waitMaxNs >= 10000000);
    assert(stats(snapshot, Scope::Callback).waitSlow == 1);
    assert(stats(snapshot, Scope::Callback).holdMaxNs >= 2000000);
    assert(stats(snapshot, Scope::Callback).holdSlow == 1);
    const auto text = TimedMutex::diagnostics(snapshot);
    assert(text.find("sharedCallbackCount=1") != std::string::npos);
    assert(text.find("sharedClientHoldSlow=1") != std::string::npos);
    assert(text.find("sharedDiagnosticsCount=0") != std::string::npos);
    assert(text.find("sharedSlowUs=1000") != std::string::npos);
    assert(text.size() < 2048);
    for (unsigned n = 0; n < 10; ++n)
        assert(TimedMutex::diagnostics(mutex.snapshot()) == text);
    {
        TimedMutex::Guard lock(mutex, Scope::Diagnostics);
        assert(TimedMutex::diagnostics(lock.snapshot()) == text);
    }
    assert(stats(mutex.snapshot(), Scope::Diagnostics).count == 1);
}
static void exceptionRelease() {
    TimedMutex mutex;
    try {
        TimedMutex::Guard lock(mutex, Scope::Callback);
        throw std::runtime_error("fixture");
    } catch (const std::runtime_error&) {}
    {
        TimedMutex::Guard lock(mutex, Scope::Callback);
        assert(stats(lock.snapshot(), Scope::Callback).count == 1);
    }
    assert(stats(mutex.snapshot(), Scope::Callback).count == 2);
}
static void concurrentSnapshots() {
    TimedMutex mutex;
    std::atomic<unsigned> finished{0};
    unsigned protectedValue = 0;
    std::vector<std::thread> workers;
    for (unsigned n = 0; n < 4; ++n) workers.emplace_back([&, n] {
        for (unsigned i = 0; i < 5000; ++i) {
            TimedMutex::Guard lock(mutex, n % 2 ? Scope::Callback : Scope::Client);
            ++protectedValue;
        }
        ++finished;
    });
    std::uint64_t previous = 0;
    while (finished.load() != 4) {
        const auto snapshot = mutex.snapshot();
        std::uint64_t total = 0;
        for (const auto& sample : snapshot) {
            total += sample.count;
            assert(sample.waitNs >= sample.waitMaxNs && sample.holdNs >= sample.holdMaxNs);
            assert(sample.waitSlow <= sample.count && sample.holdSlow <= sample.count);
        }
        assert(total >= previous); previous = total;
        std::this_thread::yield();
    }
    for (auto& worker : workers) worker.join();
    assert(protectedValue == 20000);
    const auto snapshot = mutex.snapshot();
    assert(stats(snapshot, Scope::Callback).count == 10000);
    assert(stats(snapshot, Scope::Client).count == 10000);
    assert(stats(snapshot, Scope::Diagnostics).count == 0);
}
int main() {
    contention(); exceptionRelease(); concurrentSnapshots();
    std::cout << "timed mutex contention, hold, snapshots and concurrency passed\n";
}
