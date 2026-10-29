#include "FogMoonlightDiagnosticsInternal.h"
#include "LinkedBlockingQueue.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sched.h>

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
#ifdef FOG_DIAG_TEST_HOOK
static atomic_int eventPause;
void FogDiagTestEventSelected(uint64_t kind, uint64_t wait) {
    (void)wait;
    int expected = 1;
    if (kind == FOG_EVENT_IDR_REQUEST && atomic_compare_exchange_strong(&eventPause, &expected, 2)) {
        while (atomic_load(&eventPause) != 3) sched_yield();
    }
}
static void* pausedRequest(void* unused) {
    (void)unused;
    FogDiagIdrRequested(1200);
    return NULL;
}
static void transitionAssociationTests(void) {
    uint64_t cursor = 0, missed = 0;
    FogMoonlightEvent event;
    while (FogMoonlightReadEvent(&cursor, &event, &missed)) {}
    missed = 0;
    FogDiagReset();
    FogDiagSetWaitingForIdr(1, 1, 1000);
    atomic_store(&eventPause, 1);
    pthread_t request;
    assert(pthread_create(&request, NULL, pausedRequest, NULL) == 0);
    while (atomic_load(&eventPause) != 2) sched_yield();
    // Deterministically preempt after identity selection. Both transitions
    // publish their unique token while the journal gate is held by wait 1.
    FogDiagSetWaitingForIdr(0, 0, 1300);
    FogDiagSetWaitingForIdr(1, 8, 1400);
    FogDiagFecFailure(99, 4, 1500); // admission loss is explicitly counted
    atomic_store(&eventPause, 3);
    assert(pthread_join(request, NULL) == 0);
    FogDiagIdrRequested(1600);
    FogDiagFecFailure(100, 2, 1700);
    FogDiagIdrDispatched(1800); // bound to wait 2 for the whole transaction
    FogDiagSetWaitingForIdr(0, 0, 1900);
    FogDiagSetWaitingForIdr(1, 5, 2000);
    FogDiagIdrDispatchResult(1, 2100);
    FogDiagIdrRequested(2200);
    unsigned requests = 0, results = 0;
    while (FogMoonlightReadEvent(&cursor, &event, &missed)) {
        if (event.kind == FOG_EVENT_IDR_REQUEST) {
            ++requests;
            assert(event.waitId == requests && event.totalsValid);
            assert(event.requests == 1 && event.intactFrames == 0);
            assert(event.fecFailures == 0);
        }
        if (event.kind == FOG_EVENT_IDR_DISPATCH_RESULT) {
            ++results; assert(event.waitId == 2 && event.requests == 1 && event.fecFailures == 2);
        }
        if (event.kind == FOG_EVENT_WAIT_EXIT) {
            assert(event.waitId == 2 && event.requests == 1 && event.fecFailures == 2);
        }
    }
    assert(requests == 3 && results == 1 && missed == 3);
    atomic_store(&eventPause, 0);
}
#endif

static void idleAndTypedEventTests(void) {
    uint64_t cursor = 0, missed = 0;
    FogMoonlightEvent event;
    while (FogMoonlightReadEvent(&cursor, &event, &missed)) {}
    missed = 0;
    FogDiagReset();
    for (unsigned i = 1; i <= 1000; ++i) {
        FogDiagFecFailure(i, 3, i * 10);
        FogDiagIdrHeader(i, 2, i * 10 + 1);
        FogDiagIdrRecognized(i, i * 10 + 2);
        FogDiagIdrAssembled(i, i * 10 + 3);
    }
    // Completed idle IDRs and idle loss do not enter the detailed journal.
    assert(!FogMoonlightReadEvent(&cursor, &event, &missed) && missed == 0);
    FogDiagIdrHeader(2000, 3, 20000);
    FogDiagIdrHeader(2001, 4, 20010); // bounded retention replaces prior hint
    assert(!FogMoonlightReadEvent(&cursor, &event, &missed));
    FogDiagSetWaitingForIdr(1, 8, 20020);
    assert(FogMoonlightReadEvent(&cursor, &event, &missed));
    assert(event.kind == FOG_EVENT_IDR_HEADER && event.waitId == 0);
    assert(event.frameNumber == 2001 && event.value == 4 && event.timeUs == 20010);
    assert(FogMoonlightReadEvent(&cursor, &event, &missed));
    assert(event.kind == FOG_EVENT_WAIT_ENTER && event.waitId == 1);
    assert(!FogMoonlightReadEvent(&cursor, &event, &missed) && !missed);

    FogDiagThreadStarted("VideoRecv", 2, -2, 5, 0x3b, 0, 30000);
    assert(FogMoonlightReadEvent(&cursor, &event, &missed));
    assert(event.kind == FOG_EVENT_THREAD_START && event.timeUs == 30000);
    assert(event.threadCore == 2 && event.threadIdealCore == -2 && event.threadPriority == 0x3b);
    assert(event.threadAffinityMask == 5 && !event.threadQueryErrors);
    assert(!strcmp(event.role, "VideoRecv"));
    assert(!event.frameNumber && !event.value && !event.waitId && !event.requests && !event.intactFrames && !event.fecFailures);
    FogDiagThreadStarted("FailedQuery", -1, -1, 0, -1, 3, 30001);
    assert(FogMoonlightReadEvent(&cursor, &event, &missed));
    assert(event.threadCore == -1 && event.threadIdealCore == -1 && event.threadPriority == -1);
    assert(event.threadAffinityMask == 0 && event.threadQueryErrors == 3);
}

static void idleHintCompletionTests(void) {
    uint64_t cursor = 0, missed = 0;
    FogMoonlightEvent event;
    const struct {
        uint64_t hintFrame, assembledFrame;
        int retainHint;
    } cases[] = {
        {100, 100, 0}, // same-frame completion without recognition hook
        {100, 101, 0}, // later completed IDR supersedes the saved prefix
        {101, 100, 1}, // older completion cannot erase a newer prefix
        {100, 0, 1},   // no completion: retain the original observation
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        while (FogMoonlightReadEvent(&cursor, &event, &missed)) {}
        missed = 0;
        FogDiagReset();
        const uint64_t hintUs = 1000;
        const uint64_t waitUs = hintUs + 90000000; // later recovery, 90 seconds away
        FogDiagIdrHeader(cases[i].hintFrame, 3, hintUs);
        // Do not call recognition: assembly must independently retire stale
        // hints, including paths where cached codec config hides recognition.
        if (cases[i].assembledFrame) FogDiagIdrAssembled(cases[i].assembledFrame, hintUs + 1000);
        assert(!FogMoonlightReadEvent(&cursor, &event, &missed) && !missed);
        FogDiagSetWaitingForIdr(1, 8, waitUs);
        if (cases[i].retainHint) {
            assert(FogMoonlightReadEvent(&cursor, &event, &missed));
            assert(event.kind == FOG_EVENT_IDR_HEADER && event.waitId == 0);
            assert(event.frameNumber == cases[i].hintFrame && event.timeUs == hintUs && event.value == 3);
        }
        assert(FogMoonlightReadEvent(&cursor, &event, &missed));
        assert(event.kind == FOG_EVENT_WAIT_ENTER && event.waitId == 1 && event.timeUs == waitUs);
        assert(!FogMoonlightReadEvent(&cursor, &event, &missed) && !missed);
    }
}

static void journalTests(void) {
    uint64_t cursor = 0, missed = 0;
    FogMoonlightEvent event;
    while (FogMoonlightReadEvent(&cursor, &event, &missed)) {}
    missed = 0;
    FogDiagReset();
    FogDiagSetWaitingForIdr(1, 8, 1000);
    FogDiagIdrRequested(1100);
    FogDiagSetWaitingForIdr(1, 2, 1200); // repeated drop does not restart a wait
    FogDiagIdrDispatched(1300);
    FogDiagIdrDispatchResult(1, 1400);
    FogDiagIntactFrame(76, 2, 1500);
    FogDiagFecFrameAbandonedWithId(77, 1600, 2, 1, 1, 3);
    FogDiagFecFailure(77, 1, 1601); // partial current block, counted separately
    FogDiagIdrRequested(1700);
    FogDiagIdrHeader(78, 4, 1800);
    FogDiagIntactFrame(78, 4, 1900);
    FogDiagIdrRecognized(78, 1950);
    FogDiagSetWaitingForIdr(0, 0, 2000);
    FogDiagIdrAssembled(78, 2100); // core releases before complete assembly
    const uint64_t expected[] = {FOG_EVENT_WAIT_ENTER, FOG_EVENT_IDR_REQUEST, FOG_EVENT_IDR_DISPATCH,
        FOG_EVENT_IDR_DISPATCH_RESULT, FOG_EVENT_INTACT_FRAME, FOG_EVENT_FEC_FAILURE, FOG_EVENT_FEC_FAILURE,
        FOG_EVENT_IDR_REQUEST, FOG_EVENT_IDR_HEADER, FOG_EVENT_INTACT_FRAME,
        FOG_EVENT_IDR_RECOGNIZED, FOG_EVENT_WAIT_EXIT, FOG_EVENT_IDR_ASSEMBLED};
    uint64_t connection = 0;
    for (unsigned i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        assert(FogMoonlightReadEvent(&cursor, &event, &missed));
        assert(event.kind == expected[i] && event.waitId == 1);
        if (!connection) connection = event.connectionId;
        assert(event.connectionId == connection);
        if (event.kind == FOG_EVENT_IDR_HEADER) assert(event.frameNumber == 78 && event.value == 4);
        if (event.kind == FOG_EVENT_WAIT_EXIT) {
            assert(event.requests == 2 && event.intactFrames == 2 && event.fecFailures == 3);
            assert(event.timeUs == 2000 && event.frameNumber == 78);
        }
        if (event.kind == FOG_EVENT_IDR_ASSEMBLED) assert(event.timeUs == 2100 && event.frameNumber == 78);
    }
    assert(!FogMoonlightReadEvent(&cursor, &event, &missed) && missed == 0);
    FogDiagSetWaitingForIdr(1, 5, 2200);
    assert(FogMoonlightReadEvent(&cursor, &event, &missed));
    assert(event.waitId == 2 && !event.requests && !event.intactFrames && !event.fecFailures);
    FogDiagReset();
    FogDiagSetWaitingForIdr(1, 1, 10); // new connection; clock can have a new epoch
    assert(FogMoonlightReadEvent(&cursor, &event, &missed));
    assert(event.connectionId == connection + 1 && event.waitId == 1 && event.timeUs == 10);
    for (unsigned i = 0; i < 257; ++i) FogDiagIdrRequested(100 + i);
    unsigned count = 0;
    while (FogMoonlightReadEvent(&cursor, &event, &missed)) ++count;
    assert(count == 256 && missed == 1);

    const unsigned char h264[] = {0,0,0,0, 0,0,0,1,0x67,1};
    const unsigned char hevc[] = {0,0,1,0x46,1, 0,0,1,0x40,1}; // AUD then VPS
    const unsigned char pframe[] = {0,0,1,0x41,0,0,1,0x67,1}; // never scan picture data
    assert(FogDiagPayloadStartsIdr(h264, sizeof(h264), 4, 0));
    assert(FogDiagPayloadStartsIdr(hevc, sizeof(hevc), 0, 1));
    assert(!FogDiagPayloadStartsIdr(pframe, sizeof(pframe), 0, 0));
    assert(!FogDiagPayloadStartsIdr(NULL, 1, 0, 0));
    for (unsigned i = 0; i < sizeof(h264); ++i)
        (void)FogDiagPayloadStartsIdr(h264, i, 4, 0);
}

static void* writer(void* ignored) {
    (void)ignored;
    for (int i = 0; i < 100000; i++) {
        FogDiagIdrRequested((uint64_t)i);
        if (i % 1000 == 0) {
            FogDiagAdd(FOG_DIAG_videoPacketsRejectedSequence, 1);
            FogDiagFecBlockAbandoned(2, 4, 1, 1, 1);
        }
    }
    atomic_fetch_sub(&running, 1);
    return NULL;
}

int main(void) {
#ifdef FOG_DIAG_TEST_HOOK
    transitionAssociationTests();
#endif
    idleAndTypedEventTests();
    idleHintCompletionTests();
    journalTests();
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
    uint64_t cursor = 0, missed = 0, consumed = 0;
    FogMoonlightEvent event;
    while (FogMoonlightReadEvent(&cursor, &event, &missed)) {}
    missed = 0;
    atomic_store(&running, 4);
    for (int i = 0; i < 4; i++) assert(pthread_create(&threads[i], NULL, writer, NULL) == 0);
    uint64_t previous = 0;
    while (atomic_load(&running)) {
        FogMoonlightGetDiagnostics(&snapshot);
        assert(snapshot.idrRequests >= previous && snapshot.idrRequests <= 400000);
        previous = snapshot.idrRequests;
        if (FogMoonlightReadEvent(&cursor, &event, &missed)) ++consumed;
    }
    for (int i = 0; i < 4; i++) assert(pthread_join(threads[i], NULL) == 0);
    while (FogMoonlightReadEvent(&cursor, &event, &missed)) ++consumed;
    assert(consumed + missed == 400000);
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
