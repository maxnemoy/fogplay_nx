// SDK/network doubles are shared with lifecycle tests. The production session,
// core event journal, application diagnostics bridge and RunLog remain real.
#define FOG_MOONLIGHT_LOG_INTEGRATION_TEST
#include "session_test.cpp"
#include "composition/moonlight_diagnostics.hpp"
extern "C" {
#include "FogMoonlightDiagnosticsInternal.h"
}
#include <fstream>
#include <iterator>

int main() {
    const auto previous = std::filesystem::current_path();
    const auto directory = std::filesystem::temp_directory_path() /
        ("fogplay-moonlight-journal-test-" + std::to_string(::getpid()));
    std::filesystem::create_directories(directory);
    std::filesystem::current_path(directory);
    {
        fogplay::diagnostics::LogSession log;
        log.start();
        {
            // Same bridge used by FogHubServices, installed before host work.
            fogplay::MoonlightDiagnosticsBridge bridge;
            assert(moonlight_diagnostics::sink.load());
            moonlight::MoonlightSession session((directory / "identity").string());
            pair(session);
            auto client = session.create(); assert(client);
            FogDiagReset();
            FogDiagSetWaitingForIdr(1, 5, 1000);
            // Fill the actual bounded journal before its sole session reader
            // starts. Overflow must become an explicit production log record.
            for (unsigned i = 0; i < 400; ++i) FogDiagIdrRequested(1100 + i);
            FogDiagSetWaitingForIdr(0, 0, 2000);
            FogDiagSetWaitingForIdr(1, 5, 3000);
            FogDiagIdrRequested(3100);
            FogDiagIdrDispatched(3200);
            FogDiagIdrDispatchResult(1, 3300);
            FogDiagIdrHeader(77, 3, 3400);
            FogDiagIntactFrame(77, 3, 3500);
            FogDiagIdrRecognized(77, 3600);
            FogDiagSetWaitingForIdr(0, 0, 3700);
            FogDiagIdrAssembled(77, 3800);
            FogDiagThreadStarted("VideoRecv", 2, -2, 7, 59, 0, 3900);
            assert(client->start({rental()}));
            eventually([&] { return client->ready(); });
            client->close(); // joins producer and performs the final real drain
        }
        assert(fogplay::diagnostics::runLog.flush());
        std::ifstream file(fogplay::diagnostics::runLog.archivePath());
        const std::string text{std::istreambuf_iterator<char>(file), {}};
        for (const auto* event : {"request", "dispatch", "dispatch-result", "idr-receive-hint",
                "intact-frame", "idr-recognized", "wait-exit", "idr-assembled"}) {
            assert(text.find(std::string("Moonlight IDR event=") + event + " ") != std::string::npos);
        }
        assert(text.find("connection=1 wait=2") != std::string::npos);
        assert(text.find("frame=77") != std::string::npos);
        assert(text.find("Moonlight IDR journal missed=") != std::string::npos);
        assert(text.find("Moonlight thread role=VideoRecv connection=1 core=2") != std::string::npos);
        assert(fogplay::diagnostics::runLog.stats().droppedLines == 0);
    }
    std::filesystem::current_path(previous);
    std::filesystem::remove_all(directory);
    std::cout << "Production Moonlight journal -> session drain -> app diagnostics -> RunLog passed\n";
}
