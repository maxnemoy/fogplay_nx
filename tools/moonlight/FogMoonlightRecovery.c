#define _POSIX_C_SOURCE 200809L
#include "FogMoonlightRecovery.h"
#include <stdatomic.h>

#ifdef FOG_IDR_RECOVERY_TEST_CLOCK
// Deterministic clock port for host tests of stale/preempted samples.
extern uint64_t FogIdrRecoveryTestClock(void);
static uint64_t recoveryNow(void) { return FogIdrRecoveryTestClock(); }
static uint64_t retryInterval(void) { return 500000; }
#elif defined(__SWITCH__)
#include <switch.h>
// Use the hardware monotonic counter directly: no wall-clock adjustments and
// no overflowing nanosecond conversion after a long system uptime.
static uint64_t recoveryNow(void) { return armGetSystemTick(); }
static uint64_t retryInterval(void) { return armGetSystemTickFreq() / 2; }
#else
#include <time.h>
static uint64_t recoveryNow(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
}
static uint64_t retryInterval(void) { return 500000; }
#endif

_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "Recovery requires lock-free atomics");
static _Atomic unsigned long long lastRequestTick;

void FogIdrRecoveryReset(void) {
    atomic_store_explicit(&lastRequestTick, 0, memory_order_relaxed);
}
void FogIdrRecoveryRequested(void) {
    const uint64_t now = recoveryNow();
    unsigned long long previous = atomic_load_explicit(&lastRequestTick, memory_order_relaxed);
    // A thread can be preempted after sampling. Never let its stale sample
    // move a newer API request or retry reservation backwards.
    while (now > previous &&
           !atomic_compare_exchange_weak_explicit(&lastRequestTick, &previous, now,
                                                 memory_order_relaxed, memory_order_relaxed)) {}
}
bool FogIdrRecoveryTryRetry(void) {
    const uint64_t now = recoveryNow();
    unsigned long long previous = atomic_load_explicit(&lastRequestTick, memory_order_relaxed);
    // Also reject failed/zero clock reads and stale preempted samples. A
    // monotonic clock needs no rollback/rebase path that could undo a request.
    if (!now || now <= previous || (previous && now - previous < retryInterval())) return false;
    return atomic_compare_exchange_strong_explicit(&lastRequestTick, &previous, now,
                                                  memory_order_relaxed, memory_order_relaxed);
}
