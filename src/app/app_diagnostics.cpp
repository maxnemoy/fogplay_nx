#include "core/constants/app_info.hpp"
#include "core/constants/storage_paths.hpp"
#include "app/app_diagnostics.hpp"
#include "platform/switch/switch_runtime.hpp"
#include <ctime>
#include <exception>
#include <mutex>
#include <vector>
#include <sys/stat.h>

namespace fogplay::diagnostics {
fog::RunLog runLog;
namespace {
std::mutex diagnosticMutex;
PlaybackTiming diagnosticTiming;
bool logFailureReported = false, logStarted = false, uiPresented = false;
std::chrono::steady_clock::time_point startupAt = std::chrono::steady_clock::now();
std::mutex startupMutex;
std::vector<std::string> startupLines;
unsigned startupDropped = 0;
}

PlaybackTiming diagnosticTimingSnapshot() {
    std::lock_guard<std::mutex> lock(diagnosticMutex);
    return diagnosticTiming;
}

PlaybackTiming takeDiagnosticTiming() {
    std::lock_guard<std::mutex> lock(diagnosticMutex);
    const auto result = diagnosticTiming;
    diagnosticTiming = {};
    return result;
}

void resetDiagnosticTiming() {
    std::lock_guard<std::mutex> lock(diagnosticMutex);
    diagnosticTiming = {};
}

std::string safeText(std::string text, std::size_t maximum) {
    if (text.size() > maximum) {
        auto cut = maximum;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
        text.resize(cut);
    }
    for (char& c : text) if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
    return text;
}

void diagnostic(const std::string& text, std::size_t maximum) {
    const auto started = std::chrono::steady_clock::now();

    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(started-startupAt).count();
    const std::string line = "[" + std::to_string(milliseconds) + " ms] " + safeText(text,maximum);
    {
        std::lock_guard<std::mutex> lock(startupMutex);
        if (!logStarted) {
            if (startupLines.size() < 64) startupLines.push_back(line);
            else ++startupDropped;
            return;
        }
    }
    const bool accepted = runLog.append(line);
    std::lock_guard<std::mutex> lock(diagnosticMutex);
    if (!accepted && !logFailureReported) {
        logFailureReported = true;
        runLog.append("[log warning] " + runLog.error());
    }
    diagnosticTiming.record(started);
}

StartupStage::StartupStage(const char* name)
    : name_(name), began_(std::chrono::steady_clock::now()), exceptions_(std::uncaught_exceptions()) {
    diagnostic(std::string("Startup stage=")+name_+" begin");
}
StartupStage::~StartupStage() noexcept {
    try {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now()-began_).count();
        diagnostic(std::string("Startup stage=")+name_+" elapsed_ms="+std::to_string(elapsed)+
            (std::uncaught_exceptions() > exceptions_ ? " failed" : " complete"));
    } catch (...) {}
}
void firstUiFramePresented() {
    if (uiPresented) return;
    uiPresented = true;
    diagnostic("Startup first_ui_frame_presented; elapsed measured from main entry");
}
LogSession::LogSession() {
    startupAt = std::chrono::steady_clock::now();
    diagnostic(std::string(fog::constants::common::Title) + " startup. Live session candidate; tested LAN AV stack. No credentials logged.");
    diagnostic("Startup main_entered; time before main (loader/SD executable read) is not measured");
}
void LogSession::start() {
    if (started_) return;
    started_ = true;
    StartupStage stage("log_setup");
    using platform::dataDir;
    mkdir(fog::constants::storage::SdRoot,0777);
    mkdir(dataDir,0777);
    runLog.start(dataDir,std::time(nullptr),[]{ return R_SUCCEEDED(fsdevCommitDevice("sdmc")); });
    {
        std::lock_guard<std::mutex> lock(startupMutex);
        for (const auto& line : startupLines) runLog.append(line);
        startupLines.clear();
        logStarted = true;
    }
    diagnostic("Diagnostic archive: " + runLog.archivePath());
    if (startupDropped) diagnostic("Startup buffered lines dropped="+std::to_string(startupDropped));
    if (!runLog.error().empty()) diagnostic("Log initialization warning: " + runLog.error());
    const auto stats = runLog.stats();
    diagnostic("Startup log_prepare_us="+std::to_string(stats.startupUs)+
        " renamed_bytes="+std::to_string(stats.startupRenamedBytes)+
        " reused_bytes="+std::to_string(stats.startupReusedBytes)+
        " compared_bytes="+std::to_string(stats.startupComparedBytes)+
        " preserved_bytes="+std::to_string(stats.startupPreservedBytes));
}
LogSession::~LogSession() { runLog.close(); }
}
