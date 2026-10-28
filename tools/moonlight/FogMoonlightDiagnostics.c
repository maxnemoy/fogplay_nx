#include "FogMoonlightDiagnosticsInternal.h"
#include <stddef.h>
#include <stdatomic.h>

// 64-bit atomics are lock-free on both supported AArch64 Switch and host test
// targets. Fail compilation rather than introduce a library lock in receive.
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "Diagnostics require lock-free 64-bit atomics");
_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2, "Diagnostics require lock-free pointer atomics");
static _Atomic unsigned long long counters[FOG_DIAG_COUNT];
static _Atomic(const void*) decodeQueue;

static void setMaximum(FogDiagCounter counter, uint64_t value) {
    unsigned long long previous = atomic_load_explicit(&counters[counter], memory_order_relaxed);
    while (previous < value &&
           !atomic_compare_exchange_weak_explicit(&counters[counter], &previous, value,
                                                 memory_order_relaxed, memory_order_relaxed)) {
    }
}

void FogDiagReset(void) {
    // Core permits one connection; the previous connection's threads have
    // joined and this runs before the next connection creates any threads.
    for (int i = 0; i < FOG_DIAG_COUNT; i++) {
        atomic_store_explicit(&counters[i], 0, memory_order_relaxed);
    }
    atomic_store_explicit(&decodeQueue, NULL, memory_order_relaxed);
}

void FogDiagAdd(FogDiagCounter counter, uint64_t amount) {
    atomic_fetch_add_explicit(&counters[counter], amount, memory_order_relaxed);
}

void FogDiagVideoPacket(uint64_t bytes, uint64_t nowUs) {
    // Single video receive thread, so last-arrival ordering is unambiguous.
    const uint64_t previous = atomic_exchange_explicit(&counters[FOG_DIAG_lastVideoPacketUs],
                                                       nowUs, memory_order_relaxed);
    if (previous && nowUs >= previous) {
        setMaximum(FOG_DIAG_maxVideoPacketGapUs, nowUs - previous);
    }
    FogDiagAdd(FOG_DIAG_videoPacketsReceived, 1);
    FogDiagAdd(FOG_DIAG_videoBytesReceived, bytes);
}

void FogDiagSetWaitingForIdr(uint64_t waiting, uint64_t reason, uint64_t nowUs) {
    const uint64_t previous = atomic_exchange_explicit(&counters[FOG_DIAG_waitingForIdr], waiting, memory_order_relaxed);
    if (waiting && !previous) {
        FogDiagAdd(FOG_DIAG_idrWaitEntries, 1);
        atomic_store_explicit(&counters[FOG_DIAG_idrWaitReason], reason, memory_order_relaxed);
        atomic_store_explicit(&counters[FOG_DIAG_idrWaitStartedUs], nowUs, memory_order_relaxed);
    }
    else if (!waiting && previous) {
        const uint64_t started = atomic_exchange_explicit(&counters[FOG_DIAG_idrWaitStartedUs], 0, memory_order_relaxed);
        FogDiagAdd(FOG_DIAG_idrWaitReleases, 1);
        if (started && nowUs >= started) {
            atomic_store_explicit(&counters[FOG_DIAG_idrWaitLastUs], nowUs - started, memory_order_relaxed);
            setMaximum(FOG_DIAG_idrWaitMaxUs, nowUs - started);
        }
    }
}
void FogDiagIdrRequested(uint64_t nowUs) {
    FogDiagAdd(FOG_DIAG_idrRequests, 1);
    atomic_store_explicit(&counters[FOG_DIAG_lastIdrRequestUs], nowUs, memory_order_relaxed);
}
void FogDiagIdrDispatched(uint64_t nowUs) {
    FogDiagAdd(FOG_DIAG_idrDispatches, 1);
    atomic_store_explicit(&counters[FOG_DIAG_lastIdrDispatchUs], nowUs, memory_order_relaxed);
}

void FogDiagVideoPacketProcessing(uint64_t startUs, uint64_t endUs) {
    if (endUs < startUs) {
        FogDiagAdd(FOG_DIAG_videoPacketProcessingInvalidTimes, 1);
        return;
    }
    const uint64_t elapsedUs = endUs - startUs;
    FogDiagAdd(FOG_DIAG_videoPacketProcessingCount, 1);
    FogDiagAdd(FOG_DIAG_videoPacketProcessingTotalUs, elapsedUs);
    setMaximum(FOG_DIAG_videoPacketProcessingMaxUs, elapsedUs);
}

static void addDuration(FogDiagCounter count, FogDiagCounter total, FogDiagCounter maximum,
                        uint64_t elapsedUs) {
    FogDiagAdd(count, 1);
    FogDiagAdd(total, elapsedUs);
    setMaximum(maximum, elapsedUs);
}

void FogDiagVideoReceiveCycle(uint64_t startUs, uint64_t waitStartUs,
                              uint64_t waitEndUs, uint64_t endUs) {
    if (waitStartUs < startUs || waitEndUs < waitStartUs || endUs < waitEndUs) {
        FogDiagAdd(FOG_DIAG_videoReceiveTimingInvalidTimes, 1);
        return;
    }
    const uint64_t cycleUs = endUs - startUs;
    const uint64_t waitUs = waitEndUs - waitStartUs;
    addDuration(FOG_DIAG_videoReceiveCycleCount, FOG_DIAG_videoReceiveCycleTotalUs,
                FOG_DIAG_videoReceiveCycleMaxUs, cycleUs);
    FogDiagAdd(FOG_DIAG_videoReceiveWaitTotalUs, waitUs);
    setMaximum(FOG_DIAG_videoReceiveWaitMaxUs, waitUs);
    FogDiagAdd(FOG_DIAG_videoReceiveWorkTotalUs, cycleUs - waitUs);
    setMaximum(FOG_DIAG_videoReceiveWorkMaxUs, cycleUs - waitUs);
}

static void recordDuration(FogDiagCounter count, FogDiagCounter total, FogDiagCounter maximum,
                           uint64_t startUs, uint64_t endUs) {
    if (endUs < startUs) {
        FogDiagAdd(FOG_DIAG_videoReceiveTimingInvalidTimes, 1);
        return;
    }
    addDuration(count, total, maximum, endUs - startUs);
}

void FogDiagVideoReceiveLoopGap(uint64_t previousEndUs, uint64_t startUs) {
    if (previousEndUs == 0) return;
    recordDuration(FOG_DIAG_videoReceiveLoopGapCount, FOG_DIAG_videoReceiveLoopGapTotalUs,
                    FOG_DIAG_videoReceiveLoopGapMaxUs, previousEndUs, startUs);
}

void FogDiagVideoReceiveAllocation(uint64_t startUs, uint64_t endUs) {
    recordDuration(FOG_DIAG_videoReceiveAllocationCount, FOG_DIAG_videoReceiveAllocationTotalUs,
                    FOG_DIAG_videoReceiveAllocationMaxUs, startUs, endUs);
}

void FogDiagVideoReceiveDecrypt(uint64_t startUs, uint64_t endUs) {
    recordDuration(FOG_DIAG_videoReceiveDecryptCount, FOG_DIAG_videoReceiveDecryptTotalUs,
                    FOG_DIAG_videoReceiveDecryptMaxUs, startUs, endUs);
}

void FogDiagVideoReceiveBufferRequested(uint64_t bytes) {
    atomic_store_explicit(&counters[FOG_DIAG_videoReceiveBufferRequestedBytes], bytes, memory_order_relaxed);
}

void FogDiagVideoReceiveBufferSetResult(uint64_t bytes, unsigned int error) {
    if (error == 0) {
        atomic_store_explicit(&counters[FOG_DIAG_videoReceiveBufferSelectedBytes], bytes, memory_order_relaxed);
    }
    else {
        FogDiagAdd(FOG_DIAG_videoReceiveBufferSetFailures, 1);
        atomic_store_explicit(&counters[FOG_DIAG_videoReceiveBufferSetLastError], error, memory_order_relaxed);
    }
}

void FogDiagVideoReceiveBufferActual(uint64_t bytes, unsigned int error) {
    atomic_store_explicit(&counters[FOG_DIAG_videoReceiveBufferQueryError], error, memory_order_relaxed);
    atomic_store_explicit(&counters[FOG_DIAG_videoReceiveBufferQuerySucceeded], error == 0, memory_order_relaxed);
    atomic_store_explicit(&counters[FOG_DIAG_videoReceiveBufferActualBytes], error == 0 ? bytes : 0, memory_order_relaxed);
}

void FogDiagFecFrameAbandoned(unsigned int pending, unsigned int completed,
                             unsigned int currentBlock, unsigned int lastBlock) {
    if ((pending || completed) && lastBlock >= currentBlock) {
        // Without any pending packets the current block was entirely absent.
        FogDiagAdd(FOG_DIAG_fecBlocksFailed, lastBlock - currentBlock + (pending == 0));
    }
}

void FogDiagFecBlockAbandoned(unsigned int pending, unsigned int expectedData,
                              unsigned int receivedData, unsigned int expectedParity,
                              unsigned int receivedParity) {
    if (pending == 0) return;
    const uint64_t missingData = expectedData > receivedData ? expectedData - receivedData : 0;
    const uint64_t missingParity = expectedParity > receivedParity ? expectedParity - receivedParity : 0;
    FogDiagAdd(FOG_DIAG_fecAbandonedCurrentBlocks, 1);
    FogDiagAdd(FOG_DIAG_fecAbandonedPacketsQueued, pending);
    FogDiagAdd(FOG_DIAG_fecAbandonedDataExpected, expectedData);
    FogDiagAdd(FOG_DIAG_fecAbandonedDataReceived, receivedData);
    FogDiagAdd(FOG_DIAG_fecAbandonedDataMissing, missingData);
    FogDiagAdd(FOG_DIAG_fecAbandonedParityExpected, expectedParity);
    FogDiagAdd(FOG_DIAG_fecAbandonedParityReceived, receivedParity);
    FogDiagAdd(FOG_DIAG_fecAbandonedParityMissing, missingParity);
    setMaximum(FOG_DIAG_fecAbandonedMissingDataMax, missingData);
    setMaximum(FOG_DIAG_fecAbandonedMissingParityMax, missingParity);
}

void FogDiagFecOosMode(uint64_t enabled) {
    atomic_store_explicit(&counters[FOG_DIAG_fecOosMode], enabled, memory_order_relaxed);
}

void FogDiagRegisterDecodeQueue(const void* queue) {
    atomic_store_explicit(&decodeQueue, queue, memory_order_relaxed);
}

void FogDiagQueueDepth(const void* queue, uint64_t depth) {
    if (queue != atomic_load_explicit(&decodeQueue, memory_order_relaxed)) return;
    atomic_store_explicit(&counters[FOG_DIAG_decodeQueuePending], depth, memory_order_relaxed);
    setMaximum(FOG_DIAG_decodeQueueHigh, depth);
}

void FogDiagQueueFlushed(const void* queue, uint64_t frames) {
    if (queue != atomic_load_explicit(&decodeQueue, memory_order_relaxed)) return;
    FogDiagAdd(FOG_DIAG_decodeQueueFlushes, 1);
    FogDiagAdd(FOG_DIAG_decodeQueueFlushedFrames, frames);
}

void FogMoonlightGetDiagnostics(FogMoonlightDiagnostics* snapshot) {
    if (snapshot == NULL) return;
#define FOG_DIAG_LOAD(name) snapshot->name = atomic_load_explicit(&counters[FOG_DIAG_##name], memory_order_relaxed);
    FOG_DIAG_FIELDS(FOG_DIAG_LOAD)
#undef FOG_DIAG_LOAD
}
