#pragma once
#include <cstdint>

namespace moonlight::detail {
struct ResumeRetry {
    static constexpr unsigned MaxAttempts = 3;
    static constexpr unsigned BudgetMs = 6000;
    static constexpr unsigned backoffMs(unsigned attempt) { return attempt == 1 ? 250 : 750; }
    // Only explicit service-unavailable refusals or failures before HTTP
    // dispatch are repeatable. Lost/partial replies have an unknown outcome.
    static bool retryable(int curl,long http,int xmlStatus,int xmlResult,bool dispatched,bool cancelled) {
        if (cancelled || http == 401 || http == 403 || xmlStatus == 401 || xmlStatus == 403) return false;
        if (http == 503 && (curl == 0 || curl == 22)) return true;
        if (curl == 0 && http == 200 && xmlStatus == 503 && xmlResult == -9) return true;
        if (dispatched || http != 0) return false;
        // libcurl: could-not-resolve-host, could-not-connect, timeout before dispatch.
        return curl == 6 || curl == 7 || curl == 28;
    }
};
}
