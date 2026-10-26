#pragma once

#include <cstdio>

namespace brls {
// The upstream format strings use fmt syntax. Keep only the message text here:
// no URL parameters, PINs, certificates, or pairing challenges enter the log.
class Logger {
public:
    template <typename... Args>
    static void info(const char* message, const Args&...) {
        std::fprintf(stderr, "[gamestream] %s\n", message);
    }
    template <typename... Args>
    static void error(const char* message, const Args&...) {
        std::fprintf(stderr, "[gamestream] %s\n", message);
    }
};
}
