#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace moonlight_diagnostics {
using Sink = void (*)(const std::string&);
inline std::atomic<Sink> sink{nullptr};
enum class CommandOperation { None, Launch, Resume };
struct CommandSnapshot {
    CommandOperation operation = CommandOperation::None;
    int curl = -1, xmlStatus = 0, xmlResult = -1, gameSession = -1, result = -1;
    long http = 0;
    std::uint64_t httpElapsedMs = 0, elapsedMs = 0;
    bool dispatched = false, cancelled = false;
};
// GameStream HTTP/XML work is serialized on its owner worker. Only the adapter
// copies this numeric snapshot into its separately locked presentation state.
inline thread_local CommandSnapshot command;
inline thread_local bool commandActive = false;
inline thread_local bool httpDispatched = false;
inline thread_local std::chrono::steady_clock::time_point commandStarted;
inline void beginCommand(CommandOperation operation) {
    command = {}; command.operation = operation; commandActive = true;
    commandStarted = std::chrono::steady_clock::now();
}
inline void commandHttp(int curl,long http,std::uint64_t elapsed,bool dispatched,bool cancelled) {
    if (!commandActive) return;
    command.curl = curl; command.http = http; command.httpElapsedMs = elapsed;
    command.dispatched = dispatched; command.cancelled = cancelled;
}
inline void commandXmlStatus(int status) { if (commandActive) command.xmlStatus = status; }
inline void commandXmlStatusText(const char* value) {
    if (!commandActive) return;
    command.xmlStatus = 0;
    if (!value) return;
    int status = 0;
    for (unsigned i = 0; i < 3; ++i) {
        if (value[i] < '0' || value[i] > '9') return;
        status = status * 10 + value[i] - '0';
    }
    if (value[3] == '\0') command.xmlStatus = status;
}
inline void commandXmlResult(int result) { if (commandActive) command.xmlResult = result; }
inline void commandGameSession(int result) { if (commandActive) command.gameSession = result; }
inline void finishCommand(int result,bool cancelled) {
    command.result = result; command.cancelled = command.cancelled || cancelled;
    command.elapsedMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - commandStarted).count());
    commandActive = false;
}
inline CommandSnapshot commandSnapshot() { return command; }

inline void emit(const std::string& message) noexcept {
    try { if (auto target = sink.load()) target(message); } catch (...) {}
}

// Only fixed operation names may enter the log; query values contain secrets.
inline const char* operation(const std::string& url) noexcept {
    if (url.find("/pair?") != std::string::npos) {
        if (url.find("phrase=getservercert") != std::string::npos) return "pair.wait_pin";
        if (url.find("phrase=pairchallenge") != std::string::npos) return "pair.https_confirm";
        if (url.find("clientchallenge=") != std::string::npos) return "pair.client_challenge";
        if (url.find("serverchallengeresp=") != std::string::npos) return "pair.server_response";
        if (url.find("clientpairingsecret=") != std::string::npos) return "pair.client_secret";
        return "pair.other";
    }
    if (url.find("/serverinfo?") != std::string::npos) return "server_info";
    if (url.find("/unpair?") != std::string::npos) return "unpair";
    if (url.find("/applist?") != std::string::npos) return "app_list";
    if (url.find("/launch?") != std::string::npos) return "launch";
    if (url.find("/resume?") != std::string::npos) return "resume";
    if (url.find("/cancel?") != std::string::npos) return "cancel";
    return "other";
}
}
