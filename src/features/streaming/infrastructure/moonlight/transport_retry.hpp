#pragma once
#include <Limelight.h>
#include <cerrno>

namespace moonlight::detail {
// The core reports socket errno values directly; negative Moonlight errors have
// their own meanings. Unknown errors and host-requested termination fail closed.
inline bool retryableTransportError(int code) {
    return code == -1 || code == ML_ERROR_NO_VIDEO_TRAFFIC || code == ML_ERROR_NO_VIDEO_FRAME ||
        code == ECONNRESET || code == ECONNABORTED || code == ECONNREFUSED ||
        code == ETIMEDOUT || code == ENETDOWN || code == ENETUNREACH ||
        code == EHOSTUNREACH || code == EPIPE;
}

// Host metadata reads have no launch/resume side effects. Only classified
// network failures or explicit service-unavailable responses may be repeated.
inline bool retryableHostRead(int curl, long http, bool cancelled) {
    if (cancelled) return false;
    if (http == 503 && (curl == 0 || curl == 22)) return true;
    if (http != 0) return false;
    return curl == 6 || curl == 7 || curl == 28 || curl == 52 || curl == 55 || curl == 56;
}
}
