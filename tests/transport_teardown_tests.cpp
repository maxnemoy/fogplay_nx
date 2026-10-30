#include "features/streaming/application/transport_teardown.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>

namespace {
using namespace std::chrono_literals;
int assertions = 0;
void require(bool value, const char* message) {
    ++assertions;
    if (!value) throw std::runtime_error(message);
}
struct Calls {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, released = false, allowed = true, throws = false;
    bool requireRentalStop = false, rentalStopDispatched = false, closeAfterRentalStop = false;
    int retries = 0, closes = 0;
    std::atomic<int> destroyed{0};
    std::thread::id thread;
    void waitForEntry() {
        std::unique_lock<std::mutex> lock(mutex);
        if (!changed.wait_for(lock, 2s, [this] { return entered; }))
            throw std::runtime_error("teardown worker did not enter mock transport");
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        changed.notify_all();
    }
};
struct Client {
    explicit Client(std::shared_ptr<Calls> calls) : calls(std::move(calls)) {}
    ~Client() { std::lock_guard<std::mutex> lock(calls->mutex); ++calls->destroyed; }
    bool disconnectForRetry() { return invoke(true); }
    void close() { invoke(false); }
    bool invoke(bool retry) {
        std::unique_lock<std::mutex> lock(calls->mutex);
        if (retry) ++calls->retries; else ++calls->closes;
        if (!retry) calls->closeAfterRentalStop = !calls->requireRentalStop || calls->rentalStopDispatched;
        calls->thread = std::this_thread::get_id();
        calls->entered = true;
        calls->changed.notify_all();
        calls->changed.wait(lock, [this] { return calls->released; });
        if (calls->throws) throw std::runtime_error("synthetic transport exception");
        return calls->allowed;
    }
    std::shared_ptr<Calls> calls;
};
using Teardown = fog::stream::TransportTeardown<Client>;
// Ensures failed assertions also release blocked mocks before the reaper joins.
struct ReleaseOnExit {
    std::shared_ptr<Calls> calls;
    ~ReleaseOnExit() { calls->release(); }
};
Teardown::Result completed(Teardown& teardown) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        auto result = teardown.take();
        if (result.client) return result;
        std::this_thread::yield();
    }
    throw std::runtime_error("teardown did not return joined client");
}
void blockedClose() {
    auto calls = std::make_shared<Calls>();
    Teardown teardown;
    ReleaseOnExit release{calls};
    auto client = std::make_unique<Client>(calls);
    auto* identity = client.get();
    std::thread::id callerThread;
    auto begin = std::async(std::launch::async, [&] {
        callerThread = std::this_thread::get_id();
        return teardown.begin(client, false);
    });
    const bool beginResponsive = begin.wait_for(500ms) == std::future_status::ready;
    if (!beginResponsive) calls->release();
    require(begin.get() && beginResponsive && !client,
        "begin transfers unique ownership without waiting for blocked close");
    calls->waitForEntry();
    require(calls->destroyed == 0, "blocked close retains unique client ownership");
    auto ui = std::async(std::launch::async, [&] {
        auto another = std::make_unique<Client>(std::make_shared<Calls>());
        return teardown.pending() && !teardown.take().client &&
            !teardown.begin(another, true) && bool(another);
    });
    const bool responsive = ui.wait_for(500ms) == std::future_status::ready;
    // Release before asserting so even a mutex regression cannot hang the suite.
    calls->release();
    require(responsive && ui.get(), "pending, take and rejected begin never wait for blocked close");
    auto result = completed(teardown);
    require(result.client.get() == identity && !result.retry && result.allowed,
        "final completion returns the original client after close");
    require(!teardown.pending() && !teardown.take().client, "completion is consumed exactly once");
    require(calls->thread != callerThread && calls->closes == 1 && calls->destroyed == 0,
        "close executes on dedicated worker and joined client remains owned");
    result.client.reset();
    require(calls->destroyed == 1, "caller controls destruction after completion");
}
void refusedRetry(bool throws) {
    auto calls = std::make_shared<Calls>();
    calls->allowed = false; calls->throws = throws;
    Teardown teardown;
    ReleaseOnExit release{calls};
    auto client = std::make_unique<Client>(calls);
    auto* identity = client.get();
    require(teardown.begin(client, true), "retry teardown accepted");
    calls->waitForEntry();
    require(teardown.pending() && calls->destroyed == 0 && !teardown.take().client,
        "retry retains client while callback join is blocked");
    calls->release();
    auto result = completed(teardown);
    require(result.client.get() == identity && result.retry && !result.allowed && calls->destroyed == 0,
        "terminal refusal or exception returns ownership without permission to resume");
    // Application must send rental stop before starting final close. The reaper
    // preserves ownership so the caller can enforce that order after refusal.
    calls->requireRentalStop = true;
    calls->rentalStopDispatched = true;
    calls->throws = false;
    require(teardown.begin(result.client, false),
        "same rejected client can be finally closed after rental stop dispatch");
    auto final = completed(teardown);
    require(final.client.get() == identity && !final.retry && final.allowed &&
        calls->retries == 1 && calls->closes == 1 && calls->destroyed == 0 && calls->closeAfterRentalStop,
        "final close never repeats retry or destroys pending ownership");
}
void repeatedAndShutdown() {
    Teardown teardown;
    for (int cycle = 0; cycle < 20; ++cycle) {
        auto calls = std::make_shared<Calls>(); calls->released = true;
        auto client = std::make_unique<Client>(calls);
        require(teardown.begin(client, cycle % 2 == 0), "worker accepts another completed cycle");
        auto result = completed(teardown);
        require(result.client && result.allowed && result.retry == (cycle % 2 == 0),
            "reused mailbox does not leak prior completion state");
        result.client.reset();
        require(calls->destroyed == 1, "each repeated cycle destroys exactly one client");
    }
    auto calls = std::make_shared<Calls>();
    ReleaseOnExit release{calls};
    auto client = std::make_unique<Client>(calls);
    require(teardown.begin(client, false), "shutdown test close accepted");
    calls->waitForEntry();
    auto shutdown = std::async(std::launch::async, [&] { teardown.finish(); });
    const bool joinedTooEarly = shutdown.wait_for(50ms) == std::future_status::ready;
    const bool retained = calls->destroyed == 0;
    calls->release();
    require(shutdown.wait_for(2s) == std::future_status::ready, "shutdown joins released worker");
    shutdown.get();
    require(!joinedTooEarly && retained && calls->destroyed == 1,
        "shutdown waits for blocked callback join before client destruction");
    teardown.finish();
    auto another = std::make_unique<Client>(std::make_shared<Calls>());
    require(!teardown.begin(another, false) && another && !teardown.pending(),
        "finish is idempotent and closed mailbox retains rejected caller ownership");
}
}
int main() {
    try {
        blockedClose();
        refusedRetry(false);
        refusedRetry(true);
        repeatedAndShutdown();
        std::cout << "Transport teardown: " << assertions << " assertions passed\n";
    } catch (const std::exception& error) {
        std::cerr << "Transport teardown failed: " << error.what() << '\n';
        return 1;
    }
}
