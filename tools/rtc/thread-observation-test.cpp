#include <atomic>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>

namespace {
std::mutex observationMutex;
std::set<std::string> observedRoles;
std::atomic<bool> observerFailed{false};
}
extern "C" void fogObserveThread(const char* role) noexcept {
    try {
        std::lock_guard<std::mutex> lock(observationMutex);
        observedRoles.emplace(role); // Copy temporary RTC std::string storage.
    } catch (...) { observerFailed = true; }
}

#define main rtcLoopbackMain
#include "host-loopback.cpp"
#undef main

int main() {
    const int result = rtcLoopbackMain();
    if (result || observerFailed) return 1;
    std::lock_guard<std::mutex> lock(observationMutex);
    for (const char* role : {"RTC worker", "RTC cleanup", "RTC poll", "RTC server",
                             "juice poll", "SCTP timer", "SCTP iterator"}) {
        if (!observedRoles.count(role)) {
            std::fprintf(stderr, "Missing RTC thread-start observation: %s\n", role);
            return 2;
        }
    }
    std::printf("PASS RTC thread-start hook: pool/DTLS, poll, cleanup, WebSocket server, UDP receive, SCTP timer/iterator; roles copied; normal loopback unchanged\n");
    return 0;
}
