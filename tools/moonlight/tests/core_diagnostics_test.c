#include "FogMoonlightDiagnosticsInternal.h"
#include "LinkedBlockingQueue.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>

// Exercise the real prepared LinkedBlockingQueue with native pthread primitives.
int PltCreateMutex(PLT_MUTEX* mutex) { return pthread_mutex_init(mutex, NULL); }
void PltDeleteMutex(PLT_MUTEX* mutex) { assert(pthread_mutex_destroy(mutex) == 0); }
void PltLockMutex(PLT_MUTEX* mutex) { assert(pthread_mutex_lock(mutex) == 0); }
void PltUnlockMutex(PLT_MUTEX* mutex) { assert(pthread_mutex_unlock(mutex) == 0); }
int PltCreateConditionVariable(PLT_COND* cond, PLT_MUTEX* mutex) {
    (void)mutex;
    return pthread_cond_init(cond, NULL);
}
void PltDeleteConditionVariable(PLT_COND* cond) { assert(pthread_cond_destroy(cond) == 0); }
void PltSignalConditionVariable(PLT_COND* cond) { assert(pthread_cond_signal(cond) == 0); }
void PltWaitForConditionVariable(PLT_COND* cond, PLT_MUTEX* mutex) {
    assert(pthread_cond_wait(cond, mutex) == 0);
}

static atomic_int running;

static void* writer(void* ignored) {
    (void)ignored;
    for (int i = 0; i < 100000; i++) {
        FogDiagAdd(FOG_DIAG_idrRequests, 1);
        if (i % 1000 == 0) {
            FogDiagAdd(FOG_DIAG_videoPacketsRejectedSequence, 1);
            FogDiagFecBlockAbandoned(2, 4, 1, 1, 1);
        }
    }
    atomic_fetch_sub(&running, 1);
    return NULL;
}

int main(void) {
    FogMoonlightDiagnostics snapshot;
    FogDiagReset();
    FogMoonlightGetDiagnostics(NULL);
    FogDiagVideoPacket(1200, 1000);
    FogDiagVideoPacket(1300, 1100);
    FogDiagVideoPacket(1400, 1800);
    FogDiagSetWaitingForIdr(1, 5, 1800);
    FogMoonlightGetDiagnostics(&snapshot);
    assert(snapshot.videoPacketsReceived == 3);
    assert(snapshot.videoBytesReceived == 3900);
    assert(snapshot.lastVideoPacketUs == 1800);
    assert(snapshot.maxVideoPacketGapUs == 700);
    assert(snapshot.waitingForIdr == 1);
    FogDiagSetWaitingForIdr(1, 2, 2000);
    FogDiagSetWaitingForIdr(0, 0, 2800);
    FogMoonlightGetDiagnostics(&snapshot);
    assert(snapshot.idrWaitEntries == 1 && snapshot.idrWaitReason == 5);
    assert(snapshot.idrWaitStartedUs == 0 && snapshot.idrWaitReleases == 1);
    assert(snapshot.idrWaitLastUs == 1000 && snapshot.idrWaitMaxUs == 1000);
    FogDiagVideoPacketProcessing(100, 115);
    FogDiagVideoPacketProcessing(100, 150);
    FogDiagVideoPacketProcessing(100, 105);
    FogDiagVideoPacketProcessing(100, 90);
    FogMoonlightGetDiagnostics(&snapshot);
    assert(snapshot.videoPacketProcessingCount == 3);
    assert(snapshot.videoPacketProcessingTotalUs == 70 && snapshot.videoPacketProcessingMaxUs == 50);
    assert(snapshot.videoPacketProcessingInvalidTimes == 1);
    FogDiagVideoReceiveCycle(100, 110, 170, 200);
    FogDiagVideoReceiveCycle(200, 205, 215, 220);
    FogDiagVideoReceiveCycle(220, 220, 220, 225); // allocation failure before recv
    FogDiagVideoReceiveCycle(100, 90, 170, 200);
    FogDiagVideoReceiveCycle(100, 110, 105, 200);
    FogDiagVideoReceiveCycle(100, 110, 170, 150);
    FogDiagVideoReceiveLoopGap(0, 100); // first cycle has no previous gap
    FogDiagVideoReceiveLoopGap(200, 210);
    FogDiagVideoReceiveLoopGap(220, 250);
    FogDiagVideoReceiveLoopGap(250, 240);
    FogDiagVideoReceiveAllocation(100, 107);
    FogDiagVideoReceiveAllocation(100, 104);
    FogDiagVideoReceiveAllocation(100, 99);
    FogDiagVideoReceiveDecrypt(100, 115);
    FogDiagVideoReceiveDecrypt(100, 125);
    FogDiagVideoReceiveDecrypt(100, 99);
    FogMoonlightGetDiagnostics(&snapshot);
    assert(snapshot.videoReceiveCycleCount == 3);
    assert(snapshot.videoReceiveCycleTotalUs == 125 && snapshot.videoReceiveCycleMaxUs == 100);
    assert(snapshot.videoReceiveWaitTotalUs == 70 && snapshot.videoReceiveWaitMaxUs == 60);
    assert(snapshot.videoReceiveWorkTotalUs == 55 && snapshot.videoReceiveWorkMaxUs == 40);
    assert(snapshot.videoReceiveLoopGapCount == 2);
    assert(snapshot.videoReceiveLoopGapTotalUs == 40 && snapshot.videoReceiveLoopGapMaxUs == 30);
    assert(snapshot.videoReceiveAllocationCount == 2);
    assert(snapshot.videoReceiveAllocationTotalUs == 11 && snapshot.videoReceiveAllocationMaxUs == 7);
    assert(snapshot.videoReceiveDecryptCount == 2);
    assert(snapshot.videoReceiveDecryptTotalUs == 40 && snapshot.videoReceiveDecryptMaxUs == 25);
    assert(snapshot.videoReceiveTimingInvalidTimes == 6);
    FogDiagVideoReceiveBufferRequested(500000);
    FogDiagVideoReceiveBufferSetResult(500000, 12);
    FogDiagVideoReceiveBufferSetResult(400000, 55);
    FogDiagVideoReceiveBufferSetResult(300000, 0);
    FogDiagVideoReceiveBufferActual(600000, 0);
    FogMoonlightGetDiagnostics(&snapshot);
    assert(snapshot.videoReceiveBufferRequestedBytes == 500000);
    assert(snapshot.videoReceiveBufferSelectedBytes == 300000);
    assert(snapshot.videoReceiveBufferActualBytes == 600000);
    assert(snapshot.videoReceiveBufferSetFailures == 2 && snapshot.videoReceiveBufferSetLastError == 55);
    assert(snapshot.videoReceiveBufferQuerySucceeded == 1 && snapshot.videoReceiveBufferQueryError == 0);
    FogDiagVideoReceiveBufferActual(600000, 22);
    FogMoonlightGetDiagnostics(&snapshot);
    assert(snapshot.videoReceiveBufferActualBytes == 0 && snapshot.videoReceiveBufferQuerySucceeded == 0);
    assert(snapshot.videoReceiveBufferQueryError == 22);
    // A completed prefix followed by absent tail blocks is a known failure,
    // even when the pending list is empty at the next frame transition.
    FogDiagFecFrameAbandoned(0, 3, 1, 2);
    FogMoonlightGetDiagnostics(&snapshot);
    assert(snapshot.fecBlocksFailed == 2);
    // A partial current block is counted separately by the core patch.
    FogDiagFecFrameAbandoned(2, 3, 1, 2);
    FogDiagAdd(FOG_DIAG_fecBlocksFailed, 1);
    FogMoonlightGetDiagnostics(&snapshot);
    assert(snapshot.fecBlocksFailed == 4);
    // No buffered state means an unknown whole missing frame, not a known
    // block count. A successfully completed frame is also excluded.
    FogDiagFecFrameAbandoned(0, 0, 0, 0);
    FogDiagFecFrameAbandoned(0, 0, 0, 3);
    FogDiagFecFrameAbandoned(0, 2, 2, 1);
    FogMoonlightGetDiagnostics(&snapshot);
    assert(snapshot.fecBlocksFailed == 4);

    FogDiagFecBlockAbandoned(0, 4, 0, 2, 0); // entirely missing blocks are excluded
    FogDiagFecBlockAbandoned(3, 4, 2, 2, 1);
    FogDiagFecBlockAbandoned(1, 2, 3, 2, 3); // suspicious metadata cannot underflow totals
    FogDiagFecOosMode(1);
    FogMoonlightGetDiagnostics(&snapshot);
    assert(snapshot.fecAbandonedCurrentBlocks == 2 && snapshot.fecAbandonedPacketsQueued == 4);
    assert(snapshot.fecAbandonedDataExpected == 6 && snapshot.fecAbandonedDataReceived == 5);
    assert(snapshot.fecAbandonedDataMissing == 2 && snapshot.fecAbandonedMissingDataMax == 2);
    assert(snapshot.fecAbandonedParityExpected == 4 && snapshot.fecAbandonedParityReceived == 4);
    assert(snapshot.fecAbandonedParityMissing == 1 && snapshot.fecAbandonedMissingParityMax == 1);
    assert(snapshot.fecOosMode == 1);
    FogDiagReset();
    pthread_t threads[4];
    atomic_store(&running, 4);
    for (int i = 0; i < 4; i++) assert(pthread_create(&threads[i], NULL, writer, NULL) == 0);
    uint64_t previous = 0;
    while (atomic_load(&running)) {
        FogMoonlightGetDiagnostics(&snapshot);
        assert(snapshot.idrRequests >= previous && snapshot.idrRequests <= 400000);
        previous = snapshot.idrRequests;
    }
    for (int i = 0; i < 4; i++) assert(pthread_join(threads[i], NULL) == 0);
    FogMoonlightGetDiagnostics(&snapshot);
    assert(snapshot.idrRequests == 400000);
    assert(snapshot.videoPacketsRejectedSequence == 400 && snapshot.fecAbandonedCurrentBlocks == 400);
    assert(snapshot.fecAbandonedDataMissing == 1200 && snapshot.fecAbandonedMissingDataMax == 3);

    LINKED_BLOCKING_QUEUE tracked, unrelated;
    LINKED_BLOCKING_QUEUE_ENTRY entries[4];
    assert(LbqInitializeLinkedBlockingQueue(&tracked, 2) == 0);
    assert(LbqInitializeLinkedBlockingQueue(&unrelated, 2) == 0);
    FogDiagRegisterDecodeQueue(&tracked);
    assert(LbqOfferQueueItem(&unrelated, &entries[3], &entries[3]) == LBQ_SUCCESS);
    FogMoonlightGetDiagnostics(&snapshot);
    assert(snapshot.decodeQueuePending == 0 && snapshot.decodeQueueHigh == 0);
    assert(LbqOfferQueueItem(&tracked, &entries[0], &entries[0]) == LBQ_SUCCESS);
    assert(LbqOfferQueueItem(&tracked, &entries[1], &entries[1]) == LBQ_SUCCESS);
    assert(LbqOfferQueueItem(&tracked, &entries[2], &entries[2]) == LBQ_BOUND_EXCEEDED);
    FogMoonlightGetDiagnostics(&snapshot);
    assert(snapshot.decodeQueuePending == 2 && snapshot.decodeQueueHigh == 2);
    void* item;
    assert(LbqPollQueueElement(&tracked, &item) == LBQ_SUCCESS && item == &entries[0]);
    FogMoonlightGetDiagnostics(&snapshot);
    assert(snapshot.decodeQueuePending == 1);
    assert(LbqWaitForQueueElement(&tracked, &item) == LBQ_SUCCESS && item == &entries[1]);
    FogMoonlightGetDiagnostics(&snapshot);
    assert(snapshot.decodeQueuePending == 0 && snapshot.decodeQueueHigh == 2);
    assert(LbqOfferQueueItem(&tracked, &entries[0], &entries[0]) == LBQ_SUCCESS);
    assert(LbqOfferQueueItem(&tracked, &entries[1], &entries[1]) == LBQ_SUCCESS);
    assert(LbqFlushQueueItems(&tracked) == &entries[0]);
    FogMoonlightGetDiagnostics(&snapshot);
    assert(snapshot.decodeQueuePending == 0 && snapshot.decodeQueueFlushes == 1);
    assert(snapshot.decodeQueueFlushedFrames == 2);
    assert(LbqFlushQueueItems(&tracked) == NULL);
    assert(LbqFlushQueueItems(&unrelated) == &entries[3]);
    FogMoonlightGetDiagnostics(&snapshot);
    assert(snapshot.decodeQueueFlushes == 2 && snapshot.decodeQueueFlushedFrames == 2);
    // Destroy may return pending items, so ensure its mirror is cleared too.
    assert(LbqOfferQueueItem(&tracked, &entries[0], &entries[0]) == LBQ_SUCCESS);
    LbqSignalQueueShutdown(&tracked);
    assert(LbqDestroyLinkedBlockingQueue(&tracked) == &entries[0]);
    LbqSignalQueueShutdown(&unrelated);
    assert(LbqDestroyLinkedBlockingQueue(&unrelated) == NULL);
    FogMoonlightGetDiagnostics(&snapshot);
    assert(snapshot.decodeQueuePending == 0 && snapshot.decodeQueueHigh == 2);

    FogDiagReset();
    FogMoonlightGetDiagnostics(&snapshot);
#define CHECK_ZERO(name) assert(snapshot.name == 0);
    FOG_DIAG_FIELDS(CHECK_ZERO)
#undef CHECK_ZERO
    puts("Moonlight atomic diagnostics and real queue instrumentation passed");
    return 0;
}
