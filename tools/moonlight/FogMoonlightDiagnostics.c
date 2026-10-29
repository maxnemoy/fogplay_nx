#include "FogMoonlightDiagnosticsInternal.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>

// 64-bit atomics are lock-free on both supported AArch64 Switch and host test
// targets. Fail compilation rather than introduce a library lock in receive.
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "Diagnostics require lock-free 64-bit atomics");
_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2, "Diagnostics require lock-free pointer atomics");
static _Atomic unsigned long long counters[FOG_DIAG_COUNT];
static _Atomic(const void*) decodeQueue;
enum { EVENT_CAPACITY = 256 };
static FogMoonlightEvent events[EVENT_CAPACITY];
static atomic_bool eventBusy;
static _Atomic unsigned long long eventContention, connectionId;
static uint64_t eventSequence; // Only accessed under eventBusy.
// One CAS publishes both state and unique generation: odd = active wait,
// even = closed. Counts live in keyed records under eventBusy, so a delayed
// observation/exit cannot increment or reset another wait's totals.
static _Atomic unsigned long long waitToken;
typedef struct WaitTotals {
    uint64_t id, requests, intactFrames, fecFailures, recognizedFrame;
} WaitTotals;
enum { WAIT_CAPACITY = 64 };
static WaitTotals waits[WAIT_CAPACITY];
static uint64_t recognizedFrame, recognizedWait; // eventBusy protects these.
static FogMoonlightEvent idleIdrHint; // At most one pre-wait hint; never logged while idle.
static bool hasIdleIdrHint;
static _Thread_local uint64_t dispatchWait;

static int enterJournal(void) {
    bool expected = false;
    return atomic_compare_exchange_strong_explicit(&eventBusy, &expected, true,
                                                    memory_order_acquire, memory_order_relaxed);
}
static void appendLocked(FogMoonlightEvent event) {
    event.sequence = ++eventSequence;
    event.connectionId = atomic_load_explicit(&connectionId, memory_order_relaxed);
    events[(event.sequence - 1) % EVENT_CAPACITY] = event;
}
static void publishLocked(FogMoonlightEvent event) {
    appendLocked(event);
    atomic_store_explicit(&eventBusy, false, memory_order_release);
}
static uint64_t currentWait(void) {
    const uint64_t token = atomic_load_explicit(&waitToken, memory_order_relaxed);
    return token & 1 ? (token + 1) / 2 : 0;
}
static void recordEvent(uint64_t kind, uint64_t frame, uint64_t value, uint64_t nowUs, uint64_t wait) {
    if (!enterJournal()) {
        atomic_fetch_add_explicit(&eventContention, 1, memory_order_relaxed);
        return;
    }
    if (wait == UINT64_MAX) wait = currentWait();
    // Recheck under the gate: an active wait can close after the cheap caller
    // check. Idle FEC loss still updates independent cumulative core counters.
    if (!wait && (kind == FOG_EVENT_FEC_FAILURE || kind == FOG_EVENT_INTACT_FRAME)) {
        atomic_store_explicit(&eventBusy, false, memory_order_release);
        return;
    }
#ifdef FOG_DIAG_TEST_HOOK
    extern void FogDiagTestEventSelected(uint64_t kind, uint64_t wait);
    FogDiagTestEventSelected(kind, wait);
#endif
    if (kind == FOG_EVENT_IDR_RECOGNIZED) {
        recognizedFrame = frame; recognizedWait = wait;
        if (hasIdleIdrHint && idleIdrHint.frameNumber == frame) hasIdleIdrHint = false;
    }
    if (kind == FOG_EVENT_IDR_ASSEMBLED && hasIdleIdrHint && frame >= idleIdrHint.frameNumber)
        hasIdleIdrHint = false;
    if (kind == FOG_EVENT_IDR_ASSEMBLED && recognizedFrame == frame) wait = recognizedWait;
    FogMoonlightEvent event = {0};
    event.kind = kind; event.frameNumber = frame; event.value = value;
    event.timeUs = nowUs; event.waitId = wait;
    if (!wait && kind == FOG_EVENT_IDR_HEADER) {
        idleIdrHint = event; hasIdleIdrHint = true;
        atomic_store_explicit(&eventBusy, false, memory_order_release);
        return;
    }
    if (!wait && (kind == FOG_EVENT_IDR_RECOGNIZED || kind == FOG_EVENT_IDR_ASSEMBLED)) {
        atomic_store_explicit(&eventBusy, false, memory_order_release);
        return;
    }
    if (wait) {
        WaitTotals* totals = &waits[(wait - 1) % WAIT_CAPACITY];
        if (totals->id < wait) {
            memset(totals, 0, sizeof(*totals)); totals->id = wait;
        }
        // A transition can be preempted across more than 64 whole waits.
        // Report unavailable totals rather than reuse another wait's record.
        if (totals->id == wait) {
            if (kind == FOG_EVENT_IDR_REQUEST) ++totals->requests;
            if (kind == FOG_EVENT_INTACT_FRAME) ++totals->intactFrames;
            if (kind == FOG_EVENT_FEC_FAILURE) totals->fecFailures += value;
            if (kind == FOG_EVENT_IDR_RECOGNIZED) totals->recognizedFrame = frame;
            if (kind == FOG_EVENT_WAIT_EXIT) event.frameNumber = totals->recognizedFrame;
            event.requests = totals->requests;
            event.intactFrames = totals->intactFrames;
            event.fecFailures = totals->fecFailures;
            event.totalsValid = 1;
        }
    }
    // A raw prefix can precede the loss which opens this wait. Keep only the
    // most recent raw hint, with its original timestamp and wait=0. Retention
    // is an observation, not proof that the frame remained incomplete.
    if (kind == FOG_EVENT_WAIT_ENTER && hasIdleIdrHint) {
        appendLocked(idleIdrHint); hasIdleIdrHint = false;
    }
    publishLocked(event);
}
static uint64_t transitionWait(uint64_t waiting) {
    unsigned long long token = atomic_load_explicit(&waitToken, memory_order_relaxed);
    do {
        if ((token & 1) == (waiting != 0)) return 0;
    } while (!atomic_compare_exchange_weak_explicit(&waitToken, &token, token + 1,
                                                   memory_order_relaxed, memory_order_relaxed));
    return token + 1;
}
int FogMoonlightReadEvent(uint64_t* cursor, FogMoonlightEvent* event, uint64_t* missed) {
    if (!cursor || !event || !missed || !enterJournal()) return 0;
    *missed += atomic_exchange_explicit(&eventContention, 0, memory_order_relaxed);
    if (eventSequence > *cursor && eventSequence - *cursor > EVENT_CAPACITY) {
        *missed += eventSequence - *cursor - EVENT_CAPACITY;
        *cursor = eventSequence - EVENT_CAPACITY;
    }
    const int available = *cursor < eventSequence;
    if (available) {
        *event = events[*cursor % EVENT_CAPACITY];
        *cursor = event->sequence;
    }
    atomic_store_explicit(&eventBusy, false, memory_order_release);
    return available;
}

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
    atomic_fetch_add_explicit(&connectionId, 1, memory_order_relaxed);
    atomic_store_explicit(&waitToken, 0, memory_order_relaxed);
    memset(waits, 0, sizeof(waits));
    recognizedFrame = recognizedWait = 0;
    hasIdleIdrHint = false;
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
    const uint64_t transition = transitionWait(waiting);
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
    if (transition) recordEvent(waiting ? FOG_EVENT_WAIT_ENTER : FOG_EVENT_WAIT_EXIT,
                                0, reason, nowUs, (transition + 1) / 2);
}
void FogDiagIdrRequested(uint64_t nowUs) {
    FogDiagAdd(FOG_DIAG_idrRequests, 1);
    atomic_store_explicit(&counters[FOG_DIAG_lastIdrRequestUs], nowUs, memory_order_relaxed);
    recordEvent(FOG_EVENT_IDR_REQUEST, 0, 0, nowUs, UINT64_MAX);
}
void FogDiagIdrDispatched(uint64_t nowUs) {
    FogDiagAdd(FOG_DIAG_idrDispatches, 1);
    atomic_store_explicit(&counters[FOG_DIAG_lastIdrDispatchUs], nowUs, memory_order_relaxed);
    dispatchWait = currentWait();
    recordEvent(FOG_EVENT_IDR_DISPATCH, 0, 0, nowUs, dispatchWait);
}
void FogDiagIdrDispatchResult(uint64_t success, uint64_t nowUs) {
    recordEvent(FOG_EVENT_IDR_DISPATCH_RESULT, 0, success, nowUs, dispatchWait);
}
void FogDiagFecFailure(uint64_t frame, uint64_t blocks, uint64_t nowUs) {
    if (blocks && currentWait()) recordEvent(FOG_EVENT_FEC_FAILURE, frame, blocks, nowUs, UINT64_MAX);
}
void FogDiagIntactFrame(uint64_t frame, uint64_t blocks, uint64_t nowUs) {
    if (!currentWait()) return;
    recordEvent(FOG_EVENT_INTACT_FRAME, frame, blocks, nowUs, UINT64_MAX);
}
void FogDiagIdrHeader(uint64_t frame, uint64_t blocks, uint64_t nowUs) {
    recordEvent(FOG_EVENT_IDR_HEADER, frame, blocks, nowUs, UINT64_MAX);
}
void FogDiagIdrRecognized(uint64_t frame, uint64_t nowUs) {
    recordEvent(FOG_EVENT_IDR_RECOGNIZED, frame, 0, nowUs, UINT64_MAX);
}
void FogDiagIdrAssembled(uint64_t frame, uint64_t nowUs) {
    recordEvent(FOG_EVENT_IDR_ASSEMBLED, frame, 0, nowUs, UINT64_MAX);
}
void FogDiagThreadStarted(const char* role, int core, int ideal, uint64_t mask,
                          int priority, uint64_t queryErrors, uint64_t nowUs) {
    FogMoonlightEvent event = {0};
    event.kind = FOG_EVENT_THREAD_START; event.timeUs = nowUs;
    event.threadCore = core; event.threadIdealCore = ideal; event.threadPriority = priority;
    event.threadAffinityMask = mask; event.threadQueryErrors = (uint32_t)queryErrors;
    if (role) strncpy(event.role, role, sizeof(event.role) - 1);
    if (enterJournal()) publishLocked(event);
    else atomic_fetch_add_explicit(&eventContention, 1, memory_order_relaxed);
}
void FogDiagFecFrameAbandonedWithId(uint64_t frame, uint64_t nowUs, unsigned int pending,
                                   unsigned int completed, unsigned int currentBlock, unsigned int lastBlock) {
    if ((pending || completed) && lastBlock >= currentBlock)
        FogDiagFecFailure(frame, lastBlock - currentBlock + (pending == 0), nowUs);
    FogDiagFecFrameAbandoned(pending, completed, currentBlock, lastBlock);
}
int FogDiagPayloadStartsIdr(const unsigned char* payload, int length, unsigned int header, int hevc) {
    if (!payload || length <= 0 || header >= (unsigned int)length) return 0;
    // Only inspect the first NAL and optional AUD/SEI prefix NALs. Do not scan
    // arbitrary picture data for byte patterns. The first packet can be lost,
    // truncated or use an unsupported header: no hint is not proof of P-frame.
    unsigned int offset = header;
    while (offset + 4 < (unsigned int)length) {
        if (payload[offset] || payload[offset + 1]) return 0;
        unsigned int nal = offset + 2;
        if (payload[nal] == 0) ++nal;
        if (payload[nal] != 1 || nal + 1 >= (unsigned int)length) return 0;
        ++nal;
        const unsigned int type = hevc ? (payload[nal] >> 1) & 63 : payload[nal] & 31;
        if (hevc ? type == 32 || type == 19 || type == 20 : type == 7 || type == 5) return 1;
        if (!(hevc ? type == 35 || type == 39 : type == 9 || type == 6)) return 0;
        offset = nal + 1;
        while (offset + 3 < (unsigned int)length &&
               (payload[offset] || payload[offset + 1] ||
                !(payload[offset + 2] == 1 || (!payload[offset + 2] && payload[offset + 3] == 1)))) ++offset;
    }
    return 0;
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
