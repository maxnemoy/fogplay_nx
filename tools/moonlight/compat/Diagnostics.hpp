#pragma once

#include <atomic>
#include <string>

namespace moonlight_diagnostics {
using Sink = void (*)(const std::string&);
inline std::atomic<Sink> sink{nullptr};

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
    if (url.find("/cancel?") != std::string::npos) return "cancel";
    return "other";
}
}
