#include "app/app_diagnostics.hpp"
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void validSnapshot(const fogplay::diagnostics::PlaybackTiming& value) {
    check(value.maxUs <= value.totalUs, "snapshot combines different timing updates");
    check(value.calls != 0 || (value.totalUs == 0 && value.maxUs == 0), "partial timing reset");
}
void validClockAnchor(std::int64_t earliest, std::int64_t latest) {
    using namespace fogplay::diagnostics;
    check(runLog.flush(), "startup diagnostics did not flush");
    std::ifstream input(runLog.archivePath());
    check(input.good(), "startup archive missing");
    const std::string marker = "Diagnostic clock: schema=1 clock=steady startupEpochMs=";
    unsigned anchors = 0;
    std::string line;
    while (std::getline(input, line)) {
        const auto at = line.find(marker);
        if (at == std::string::npos) continue;
        ++anchors;
        const auto origin = std::stoll(line.substr(at + marker.size()));
        check(origin >= earliest && origin <= latest, "clock origin is not session startup steady time");
        check(line.find(" resolutionMs=1") != std::string::npos, "clock precision missing");
    }
    check(anchors == 1, "clock anchor absent or duplicated");
}
void concurrentDiagnostics(bool reset) {
    using namespace fogplay::diagnostics;
    constexpr unsigned producers = 4, callsPerProducer = 10000;
    resetDiagnosticTiming();
    std::atomic<bool> begin{false}, done{false};
    std::atomic<unsigned> failures{0};
    std::uint64_t takenCalls = 0;
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < producers; ++i) workers.emplace_back([&] {
        while (!begin.load()) std::this_thread::yield();
        for (unsigned n = 0; n < callsPerProducer; ++n) diagnostic("concurrent diagnostic");
    });
    std::thread observer([&] {
        while (!begin.load()) std::this_thread::yield();
        while (!done.load()) {
            try {
                const auto snapshot = reset ? takeDiagnosticTiming() : diagnosticTimingSnapshot();
                if (reset) takenCalls += snapshot.calls;
                validSnapshot(snapshot);
                check(!snapshot.text("logEnqueue").empty(), "empty timing text");
            } catch (...) { ++failures; }
        }
    });
    begin = true;
    for (auto& worker : workers) worker.join();
    done = true;
    observer.join();
    check(failures == 0, "invalid concurrent snapshot");
    const auto snapshot = diagnosticTimingSnapshot();
    validSnapshot(snapshot);
    check(takenCalls + snapshot.calls == producers * callsPerProducer, "concurrent take/reset lost or duplicated timing samples");
}
}

int main() {
    const auto originalDirectory = std::filesystem::current_path();
    char pattern[] = "/tmp/fogplay-app-diagnostics-XXXXXX";
    const auto* directory = mkdtemp(pattern);
    check(directory != nullptr, "temporary directory creation failed");
    std::filesystem::current_path(directory);
    try {
        {
            const auto earliest = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            fogplay::diagnostics::LogSession session;
            const auto latest = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            session.start();
            validClockAnchor(earliest, latest);
            concurrentDiagnostics(false);
            concurrentDiagnostics(true);
            // All producers hit the append-failure latch concurrently after close.
            fogplay::diagnostics::runLog.close();
            concurrentDiagnostics(false);
            concurrentDiagnostics(true);
            fogplay::diagnostics::resetDiagnosticTiming();
            check(fogplay::diagnostics::diagnosticTimingSnapshot().calls == 0, "final reset failed");
        }
        std::filesystem::current_path(originalDirectory);
        std::filesystem::remove_all(directory);
    } catch (...) {
        std::filesystem::current_path(originalDirectory);
        std::filesystem::remove_all(directory);
        throw;
    }
    std::cout << "app diagnostics concurrent tests passed\n";
}
