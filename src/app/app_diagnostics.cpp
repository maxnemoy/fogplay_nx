#include "app/app_diagnostics.hpp"
#include "platform/switch/switch_runtime.hpp"
#include <ctime>
#include <sys/stat.h>

namespace fogplay::diagnostics {
fog::RunLog runLog;
PlaybackTiming diagnosticTiming;
namespace { bool logFailureReported = false; }

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
    static const auto began = started;
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-began).count();
    const std::string line = "[" + std::to_string(milliseconds) + " ms] " + safeText(text,maximum);
    if (!runLog.append(line) && !logFailureReported) {
        logFailureReported = true;
        runLog.append("[log warning] " + runLog.error());
    }
    diagnosticTiming.record(started);
}

LogSession::LogSession() {
    using platform::dataDir;
    mkdir("sdmc:/switch",0777);
    mkdir(dataDir,0777);
    runLog.start(dataDir,std::time(nullptr),[]{ return R_SUCCEEDED(fsdevCommitDevice("sdmc")); });
    diagnostic("FogPlay NX 0.4.24 startup. Live session candidate; tested LAN AV stack. No credentials logged.");
    diagnostic("Diagnostic archive: " + runLog.archivePath());
    if (!runLog.error().empty()) diagnostic("Log initialization warning: " + runLog.error());
}
LogSession::~LogSession() { runLog.close(); }
}
