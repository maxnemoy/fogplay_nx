#include "core/diagnostics/latency_histogram.hpp"
#include "core/diagnostics/thread_observation.hpp"
#include "features/streaming/infrastructure/async_control_sender.hpp"
#include <cassert>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

int main() {
    using fog::metrics::LatencyHistogram;
    LatencyHistogram samples;
    assert(samples.snapshot(1).samples == 0);
    for (unsigned i = 1; i <= 100; ++i) samples.record(i, i);
    const auto snapshot = samples.snapshot(100);
    assert(snapshot.samples == 100 && snapshot.p50Us == 50 && snapshot.p95Us == 95 && snapshot.p99Us == 99);
    assert(samples.countAbove(95, 100) == 5);
    assert(samples.snapshot(5000050).samples == 50);
    assert(samples.snapshot(5000100).samples == 0);
    assert(samples.snapshot(0).samples == 0); // Future timestamps excluded.
    samples = {};
    for (unsigned i = 0; i < 600; ++i) samples.record(i, 1000);
    assert(samples.snapshot(1000).samples == 512);
    assert(samples.snapshot(1000).overflow == 88);
    samples.record(1, 6000000);
    assert(samples.snapshot(6000000).overflow == 88); // Expired eviction is not overflow.
    assert(samples.snapshot(6000000).samples == 1);

    // The latest coalesced gamepad keeps its own pad timestamp; metrics are
    // published only after the real sender callback returns, never at enqueue.
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false, release = false;
    stream::AsyncControlSender sender([] { return true; }, [&](const std::string& payload) {
        assert(payload == "latest");
        std::unique_lock<std::mutex> lock(mutex);
        entered = true; condition.notify_all();
        condition.wait(lock, [&] { return release; });
    });
    const auto now = fog::metrics::steadyNowUs();
    assert(sender.enqueueGamepad("old", std::uint32_t{0}, false, now - 1000000));
    assert(sender.enqueueGamepad("latest", std::uint32_t{0}, false, now - 100000));
    assert(sender.inputLatencyDiagnostics().find("samples=0") != std::string::npos);
    sender.allow(true);
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(condition.wait_for(lock, std::chrono::seconds(2), [&] { return entered; }));
        assert(sender.inputLatencyDiagnostics().find("samples=0") != std::string::npos);
        release = true; condition.notify_all();
    }
    sender.stop();
    const auto diagnostic = sender.inputLatencyDiagnostics();
    assert(diagnostic.find("samples=1") != std::string::npos);
    assert(sender.snapshot().gamepadCoalesced == 1);
    const auto value = std::stoull(diagnostic.substr(diagnostic.find("p50Us=") + 6));
    assert(value >= 100000 && value < 1000000);
    std::thread first([] { fog::diagnostics::observeThread("test-worker"); });
    fog::diagnostics::observeThread("test-main");
    first.join();
    assert(fog::diagnostics::threadObservationText().find("role=test-worker") != std::string::npos);
    for (unsigned i = 0; i < 130; ++i) {
        const auto temporaryRole = std::string(60, 'x') + std::to_string(i);
        fog::diagnostics::observeThread((std::to_string(i) + temporaryRole).c_str());
    }
    const auto lines = fog::diagnostics::threadObservationLines();
    assert(lines.size() == 129); // Header plus bounded 128 observations.
    for (const auto& line : lines) assert(line.size() <= 512);
    assert(lines.front().find("overflow=0") == std::string::npos);
    std::cout << "Stage 0 histogram, send completion, coalescing and thread observation tests passed\n";
}
