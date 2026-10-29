#pragma once
#include "core/logging/run_log.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>

namespace fogplay::diagnostics {
std::string safeText(std::string text, std::size_t maximum = 512);
struct PlaybackTiming {
    std::uint64_t calls = 0, totalUs = 0, maxUs = 0;
    void record(std::chrono::steady_clock::time_point start) {
        const auto elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count());
        ++calls; totalUs += elapsed; maxUs = std::max(maxUs, elapsed);
    }
    std::string text(const char* name) const {
        return std::string(name) + "Calls=" + std::to_string(calls) + " " + name + "AvgUs=" +
            std::to_string(calls ? totalUs / calls : 0) + " " + name + "MaxUs=" + std::to_string(maxUs);
    }
};

extern fog::RunLog runLog;
// diagnostic() also runs on provider workers. Only copies leave the timing lock.
PlaybackTiming diagnosticTimingSnapshot();
PlaybackTiming takeDiagnosticTiming();
void resetDiagnosticTiming();
void diagnostic(const std::string& text, std::size_t maximum = 512);
void firstUiFramePresented();
class StartupStage {
public:
    explicit StartupStage(const char* name);
    ~StartupStage() noexcept;
    StartupStage(const StartupStage&) = delete;
    StartupStage& operator=(const StartupStage&) = delete;
private:
    const char* name_;
    std::chrono::steady_clock::time_point began_;
    int exceptions_;
};
struct LogSession {
    LogSession();
    void start();
    ~LogSession();
    LogSession(const LogSession&) = delete;
    LogSession& operator=(const LogSession&) = delete;
private:
    bool started_ = false;
};
}
