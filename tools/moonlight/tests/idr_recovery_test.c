#include "Limelight-internal.h"
#include "FogMoonlightDiagnosticsInternal.h"
#include "FogMoonlightRecovery.h"
#include <assert.h>
#include <stdio.h>

DECODER_RENDERER_CALLBACKS VideoCallbacks;
STREAM_CONFIGURATION StreamConfig;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
int NegotiatedVideoFormat = VIDEO_FORMAT_H264;
int AppVersionQuad[4] = {7, 1, 431, -1};
static uint64_t clockUs = 1000, monotonicUs;
static bool separateClock;
uint64_t FogIdrRecoveryTestClock(void) { return separateClock ? monotonicUs : clockUs; }
static unsigned presented, invalidations, events, terminations;
static bool rfiEnabled, sendOk = true;
uint64_t PltGetMicroseconds(void) { return clockUs; }
bool isReferenceFrameInvalidationEnabled(void) { return rfiEnabled; }
bool LiGetCurrentHostDisplayHdrMode(void) { return false; }
void connectionReceivedCompleteFrame(uint32_t frame, bool ltr) { (void)frame; (void)ltr; }
void connectionDetectedFrameLoss(uint32_t first, uint32_t last) {
    (void)first; (void)last; invalidations++;
}
void notifyKeyFrameReceived(void) {}
static void logMessage(const char* format, ...) { (void)format; }
static void terminated(int error) { (void)error; terminations++; }
int PltCreateMutex(PLT_MUTEX* mutex) { return pthread_mutex_init(mutex, NULL); }
void PltDeleteMutex(PLT_MUTEX* mutex) { assert(pthread_mutex_destroy(mutex) == 0); }
void PltLockMutex(PLT_MUTEX* mutex) { assert(pthread_mutex_lock(mutex) == 0); }
void PltUnlockMutex(PLT_MUTEX* mutex) { assert(pthread_mutex_unlock(mutex) == 0); }
int PltCreateConditionVariable(PLT_COND* cond, PLT_MUTEX* mutex) {
    (void)mutex; return pthread_cond_init(cond, NULL);
}
void PltDeleteConditionVariable(PLT_COND* cond) { assert(pthread_cond_destroy(cond) == 0); }
void PltSignalConditionVariable(PLT_COND* cond) { assert(pthread_cond_signal(cond) == 0); }
void PltWaitForConditionVariable(PLT_COND* cond, PLT_MUTEX* mutex) {
    assert(pthread_cond_wait(cond, mutex) == 0);
}

// Actual prepared LiRequestIdrFrame and requestIdrFrame transaction bodies;
// transport/OS boundary doubles do not repeat their decision branches.
static LINKED_BLOCKING_QUEUE referenceFrameControlQueue;
static PLT_EVENT idrFrameRequiredEvent;
void PltSetEvent(PLT_EVENT* event) { (void)event; events++; }
static void freeBasicLbqList(PLINKED_BLOCKING_QUEUE_ENTRY list) { assert(list == NULL); }
static bool supportsIdrFrameRequest = true;
static int lastSeenFrame = 50;
#define IDX_INVALIDATE_REF_FRAMES 0
#define IDX_REQUEST_IDR_FRAME 1
static short packetTypes[] = {0, 1}, payloadLengths[] = {0, 0};
static const char* preconstructedPayloads[] = {NULL, NULL};
static bool sendMessageAndDiscardReply(short type, short length, const void* payload,
                                      int channel, int flags, bool more) {
    (void)type; (void)length; (void)payload; (void)channel; (void)flags; (void)more;
    return sendOk;
}
CONTROL_API
CONTROL_DISPATCH
#include "VideoDepacketizer.c"

static FogMoonlightDiagnostics snapshot(void) {
    FogMoonlightDiagnostics result; FogMoonlightGetDiagnostics(&result); return result;
}
static int submit(PDECODE_UNIT unit) {
    assert(unit->frameType == FRAME_TYPE_IDR || !waitingForIdrFrame);
    presented++; return DR_OK;
}
static void init(bool rfi) {
    FogDiagReset(); FogIdrRecoveryReset(); rfiEnabled = rfi; separateClock = false;
    events = presented = invalidations = terminations = 0;
    sendOk = true; supportsIdrFrameRequest = true;
    VideoCallbacks.capabilities = CAPABILITY_DIRECT_SUBMIT;
    VideoCallbacks.submitDecodeUnit = submit;
    ListenerCallbacks.logMessage = logMessage;
    ListenerCallbacks.connectionTerminated = terminated;
    assert(LbqInitializeLinkedBlockingQueue(&referenceFrameControlQueue, 15) == 0);
    initializeVideoDepacketizer(1024);
}
static void done(void) {
    stopVideoDepacketizer(); destroyVideoDepacketizer();
    assert(LbqDestroyLinkedBlockingQueue(&referenceFrameControlQueue) == NULL);
}
static void frame(unsigned index, bool idr, bool last) {
    static const unsigned char key[] = {0,0,0,1,0x67,0x42, 0,0,0,1,0x68,0x11, 0,0,0,1,0x65,0x22};
    static const unsigned char predicted[] = {0,0,0,1,0x61,0x44};
    const size_t bytes = idr ? sizeof(key) : sizeof(predicted);
    unsigned char packet[sizeof(NV_VIDEO_PACKET) + 8 + sizeof(key)] = {0};
    PNV_VIDEO_PACKET nv = (PNV_VIDEO_PACKET)packet;
    nv->frameIndex = index; nv->streamPacketIndex = index << 8;
    nv->flags = FLAG_SOF | (last ? FLAG_EOF : 0);
    unsigned char* data = packet + sizeof(*nv);
    data[0] = 1; data[3] = idr ? 2 : 1;
    memcpy(data + 8, idr ? key : predicted, bytes);
    PLENTRY_INTERNAL entry = NULL;
    processRtpPayload(nv, sizeof(*nv) + 8 + bytes, clockUs, 1, 1, &entry);
    assert(entry == NULL);
}
static void timedRetriesAndRelease(void) {
    init(false);
    // No request exists yet: a complete startup P frame requests immediately.
    frame(1, false, true);
    assert(snapshot().idrWaitRetryRequests == 1 && events == 1 && presented == 0);
    assert(snapshot().idrDispatches == 0); // API/event != actual control dispatch
    requestIdrFrame();
    assert(snapshot().idrDispatches == 1 && snapshot().idrDispatchSuccesses == 1);
    clockUs += 499999; frame(2, false, true);
    assert(events == 1);
    clockUs++; frame(3, false, true);
    assert(events == 2 && snapshot().idrWaitRetryRequests == 2);
    // A decoder/API request delays the retry deadline as well.
    clockUs += 400000; LiRequestIdrFrame();
    clockUs += 100000; frame(4, false, true); assert(events == 3);
    clockUs += 400000; frame(5, false, true); assert(events == 4);
    // Dense frames don't flood and the original120-frame fallback coalesces.
    for (unsigned i = 6; i <= 130; i++) frame(i, false, true);
    assert(events == 4 && presented == 0 && snapshot().idrDropLimitRequests == 0);
    clockUs += 10000; frame(131, true, true);
    assert(presented == 1 && !waitingForIdrFrame);
    assert(snapshot().idrWaitReleases == 1 && snapshot().idrWaitLastUs > 500000);
    assert(snapshot().idrFramesCompleted == 1);
    clockUs += 1000000; frame(132, false, true);
    assert(presented == 2 && events == 4); // no retry after release
    done();
}
static void lossGatingAndReferenceSafety(void) {
    clockUs += 1000000; init(false); frame(1, true, true);
    // Speculative FEC loss enters strict wait without requesting mid-frame.
    notifyFrameLost(2, true);
    assert(waitingForIdrFrame && events == 0 && snapshot().idrWaitReason == 7);
    clockUs += 10000; frame(3, false, true);
    assert(events == 1 && presented == 1 && !waitingForNextSuccessfulFrame);
    clockUs += 500000; frame(4, false, true); assert(events == 2);
    done();

    // With RFI enabled, ordinary reference loss stays on upstream's RFI path.
    clockUs += 1000000; init(true); frame(1, true, true);
    notifyFrameLost(2, true);
    assert(!waitingForIdrFrame && waitingForRefInvalFrame && invalidations == 1);
    clockUs += 1000000; frame(3, false, true);
    assert(events == 0 && presented == 1);
    done();

    // Losing an IDR after its first packet must require another full IDR,
    // even though parsing the IDR prefix already cleared the wait.
    clockUs += 1000000; init(true); frame(1, true, true);
    frame(2, true, false); assert(!waitingForIdrFrame && decodingFrame);
    decodingFrame = false; // exact boundary guaranteed by queue's loss callback
    notifyFrameLost(2, false);
    assert(waitingForIdrFrame && !waitingForRefInvalFrame && invalidations == 0);
    frame(3, false, true); assert(events == 1 && presented == 1);
    done();
}
static void repeatedHolesRequestAtRecovery(void) {
    clockUs += 1000000; init(false); frame(1, true, true);
    // A recent API request must not delay recovery from a new frame loss.
    LiRequestIdrFrame();
    clockUs += 10000; frame(3, false, true);
    assert(events == 2 && presented == 1 && waitingForIdrFrame);
    const uint64_t waitEntries = snapshot().idrWaitEntries;
    // New holes in the same wait each regain an immediate request when the
    // first complete frame shows that packets are flowing again.
    for (unsigned i = 5; i <= 45; i += 2) {
        clockUs += 20000; frame(i, false, true);
        assert(events == 2 + (i - 3) / 2 && presented == 1);
        assert(snapshot().idrWaitEntries == waitEntries);
    }
    const unsigned requests = events;
    const uint64_t latestRequestUs = clockUs;
    // Consecutive complete P frames without another hole retain the existing
    // 500 ms fallback; restoring loss recovery does not remove that bound.
    clockUs = latestRequestUs + 499999; frame(46, false, true);
    assert(events == requests);
    clockUs++; frame(47, false, true);
    assert(events == requests + 1 && snapshot().idrWaitRetryRequests == 1);
    clockUs++; frame(48, true, true);
    clockUs++; frame(50, false, true);
    assert(events == requests + 2 && presented == 2);
    assert(snapshot().idrWaitEntries == waitEntries + 1);
    done();
}
static void lostRequestedIdrRetriesOnNextCompleteFrame(void) {
    clockUs += 1000000; init(false); frame(1, true, true);
    notifyFrameLost(2, false);
    clockUs += 10000; frame(3, false, true);
    assert(events == 1 && waitingForIdrFrame && presented == 1);
    const uint64_t waitEntries = snapshot().idrWaitEntries;
    // The requested IDR is lost before depacketizer recognition. The FEC
    // queue reports that loss through its real depacketizer entry point.
    clockUs += 20000; notifyFrameLost(4, false);
    assert(events == 1 && waitingForIdrFrame);
    clockUs += 10000; frame(5, false, true);
    assert(events == 2 && waitingForIdrFrame && presented == 1);
    assert(snapshot().idrWaitEntries == waitEntries);
    assert(snapshot().idrWaitRetryRequests == 0);
    clockUs += 1000; frame(6, true, true);
    assert(!waitingForIdrFrame && presented == 2);
    done();
}
static void* decoderThreadRefresh(void* context) {
    (void)context;
    requestDecoderRefresh();
    return NULL;
}
static void decoderRefreshAndNetworkRecovery(void) {
    clockUs += 1000000; init(false); frame(1, true, true);
    pthread_t decoder;
    assert(pthread_create(&decoder, NULL, decoderThreadRefresh, NULL) == 0);
    assert(pthread_join(decoder, NULL) == 0);
    assert(events == 1 && waitingForIdrFrame);
    // A network hole after a decoder request gets an immediate recovery
    // request. Without a new hole, complete frames share the normal deadline.
    clockUs += 10000; frame(3, false, true);
    assert(events == 2 && presented == 1);
    clockUs += 499999; frame(4, false, true); assert(events == 2);
    clockUs++; frame(5, false, true); assert(events == 3);
    clockUs++; FogIdrRecoveryRequestDecoderRefresh(); FogIdrRecoveryPollDecoder();
    assert(events == 4 && localDecoderRecovery);
    clockUs += 10000; frame(6, false, true); assert(events == 4);
    clockUs += 489999; FogIdrRecoveryPollDecoder(); assert(events == 4);
    clockUs++; FogIdrRecoveryPollDecoder(); assert(events == 5);
    done();
}
static void exceptionalDirectRequestsRemainImmediate(void) {
    clockUs += 1000000; init(false); frame(1, true, true);
    for (unsigned index = 2; index <= 3; index++) {
        unsigned char packet[sizeof(NV_VIDEO_PACKET) + 8] = {0};
        PNV_VIDEO_PACKET nv = (PNV_VIDEO_PACKET)packet;
        nv->frameIndex = index; nv->streamPacketIndex = (index + 3) << 8;
        nv->flags = FLAG_EOF; // corrupt non-SOF sequence, before frame parsing
        PLENTRY_INTERNAL entry = NULL;
        processRtpPayload(nv, sizeof(packet), clockUs, 1, 1, &entry);
        assert(entry == NULL && events == index - 1 && waitingForIdrFrame);
        clockUs++;
    }
    frame(5, false, true); assert(events == 3 && presented == 1);
    done();
#ifdef LC_FUZZING
    // Disable host-input assertions only; all harness assertions stay active.
    clockUs += 1000000; init(false); frame(1, true, true);
    NegotiatedVideoFormat = VIDEO_FORMAT_AV1_MAIN8;
    for (unsigned index = 2; index <= 3; index++) {
        unsigned char packet[sizeof(NV_VIDEO_PACKET) + 9] = {0};
        PNV_VIDEO_PACKET nv = (PNV_VIDEO_PACKET)packet;
        nv->frameIndex = index; nv->streamPacketIndex = index << 8;
        nv->flags = FLAG_SOF | FLAG_EOF;
        unsigned char* data = packet + sizeof(*nv);
        data[0] = 1; data[3] = 1; // encoded payload length stays zero (invalid)
        PLENTRY_INTERNAL entry = NULL;
        processRtpPayload(nv, sizeof(packet), clockUs, 1, 1, &entry);
        assert(entry == NULL && events == index - 1 && waitingForIdrFrame);
        clockUs++;
    }
    NegotiatedVideoFormat = VIDEO_FORMAT_H264;
    frame(5, false, true); assert(events == 3 && presented == 1);
    done();
#endif
}
static void controlOutcomesAndReset(void) {
    clockUs += 1000000; init(false);
    LiRequestIdrFrame(); sendOk = false; requestIdrFrame();
    assert(snapshot().idrRequests == 1 && snapshot().idrDispatchFailures == 1 && terminations == 1);
    supportsIdrFrameRequest = false; sendOk = true; requestIdrFrame();
    assert(snapshot().idrDispatchSuccesses == 1 && snapshot().idrDispatches == 2);
    // The original120-drop fallback still requests when complete frames never
    // reach the retry gate; repeats at the same time are coalesced.
    clockUs += 500000;
    for (unsigned i = 0; i < 120; i++) notifyFrameLost(1, false);
    assert(events == 2 && snapshot().idrDropLimitRequests == 1);
    for (unsigned i = 0; i < 120; i++) notifyFrameLost(1, false);
    assert(events == 2 && snapshot().idrDropLimitRequests == 1);
    // Core wall-clock rollback has no effect on the monotonic retry clock.
    const uint64_t latestRequest = clockUs;
    separateClock = true; monotonicUs = latestRequest + 1;
    clockUs = 100; frame(1, false, true); assert(events == 2);
    monotonicUs = latestRequest + 499999;
    frame(2, false, true); assert(events == 2);
    monotonicUs++; frame(3, false, true); assert(events == 3);
    done();
    // New connection resets policy and telemetry; no inherited retry deadline.
    clockUs = 100; init(false); frame(1, false, true);
    assert(events == 1 && snapshot().idrRequests == 1 && snapshot().idrWaitEntries == 1);
    done();
}
static void staleSamples(void) {
    FogIdrRecoveryReset(); separateClock = true;
    monotonicUs = 2000000; FogIdrRecoveryRequested();
    // Retry sampled before a newer request was recorded: it must not rebase
    // the newer request backwards or permit another retry one microsecond later.
    monotonicUs = 1000000; assert(!FogIdrRecoveryTryRetry());
    // An API call preempted after sampling must likewise preserve newer state.
    FogIdrRecoveryRequested();
    monotonicUs = 2000001; assert(!FogIdrRecoveryTryRetry());
    monotonicUs = 2499999; assert(!FogIdrRecoveryTryRetry());
    monotonicUs = 2500000; assert(FogIdrRecoveryTryRetry());
    // A stale API sample also cannot undo a successful retry reservation.
    monotonicUs = 2400000; FogIdrRecoveryRequested();
    monotonicUs = 2500001; assert(!FogIdrRecoveryTryRetry());
    monotonicUs = 3000000; assert(FogIdrRecoveryTryRetry());
    monotonicUs = 0; assert(!FogIdrRecoveryTryRetry());
    separateClock = false;
}
static void localDecoderRecoveryScenarios(void) {
    clockUs += 1000000; init(false); frame(1, true, true);
    assert(presented == 1 && !waitingForIdrFrame);
    FogIdrRecoveryRequestDecoderRefresh();
    FogIdrRecoveryRequestDecoderRefresh();
    assert(FogIdrRecoveryTakeDecoderRefresh());
    assert(!FogIdrRecoveryTakeDecoderRefresh()); // command bursts coalesce
    FogIdrRecoveryRequestDecoderRefresh();
    FogIdrRecoveryPollDecoder(); // receive loop, with no new RTP at all
    assert(waitingForIdrFrame && events == 1);
    clockUs += 499999; FogIdrRecoveryPollDecoder(); assert(events == 1 && presented == 1);
    clockUs++; FogIdrRecoveryPollDecoder(); assert(events == 2 && presented == 1);
    frame(2, false, true); assert(events == 2); // frame-driven gate coalesces
    clockUs += 500000; FogIdrRecoveryPollDecoder(); assert(events == 3);
    clockUs++; frame(4, true, true); assert(!waitingForIdrFrame && presented == 2);
    assert(!localDecoderRecovery);
    clockUs += 500000; FogIdrRecoveryPollDecoder(); frame(5, false, true);
    assert(events == 3 && presented == 3);
    FogIdrRecoveryRequestDecoderRefresh(); FogIdrRecoveryReset();
    assert(!FogIdrRecoveryTakeDecoderRefresh()); // no inherited decoder command
    done();
}
static void ordinaryWaitDoesNotGainTimeoutRetry(void) {
    clockUs += 1000000; init(false);
    // Startup wait: socket timeout polls must not create IDR requests.
    FogIdrRecoveryPollDecoder(); clockUs += 1000000; FogIdrRecoveryPollDecoder();
    assert(events == 0 && !localDecoderRecovery);
    frame(1, true, true);
    notifyFrameLost(2, true);
    assert(waitingForIdrFrame && !localDecoderRecovery);
    clockUs += 1000000; FogIdrRecoveryPollDecoder(); assert(events == 0);
    frame(3, false, true); assert(events == 1); // prior frame-driven policy
    clockUs += 1000000; FogIdrRecoveryPollDecoder(); assert(events == 1);
    // A local scope closes when the IDR is recognized, including a partial
    // IDR later lost to network damage before the next timeout poll.
    FogIdrRecoveryRequestDecoderRefresh(); FogIdrRecoveryPollDecoder();
    assert(localDecoderRecovery && events == 2);
    frame(4, true, false); assert(!waitingForIdrFrame && !localDecoderRecovery);
    decodingFrame = false; notifyFrameLost(4, false);
    clockUs += 1000000; FogIdrRecoveryPollDecoder(); assert(events == 2);
    FogIdrRecoveryRequestDecoderRefresh(); FogIdrRecoveryPollDecoder();
    assert(localDecoderRecovery); // reset a still-active local scope
    done();
    clockUs += 1000000; init(false); assert(!localDecoderRecovery);
    FogIdrRecoveryPollDecoder(); assert(events == 0); done();
}
static void pullRendererAdmissionAndRefresh(void) {
    init(false);
    VideoCallbacks.capabilities = CAPABILITY_PULL_RENDERER;
    VIDEO_FRAME_HANDLE handle;
    PDECODE_UNIT unit;
    // Actual prepared SDK entry points: a wake before wait is sticky, even
    // when a frame is already queued. The frame is retained for the next pull.
    frame(1, true, true);
    assert(LiGetPendingVideoFrames() == 1);
    LiWakeWaitForVideoFrame();
    assert(!LiWaitForNextVideoFrame(&handle, &unit));
    assert(LiGetPendingVideoFrames() == 1);
    assert(LiWaitForNextVideoFrame(&handle, &unit));
    assert(unit->frameNumber == 1 && unit->frameType == FRAME_TYPE_IDR);
    LiCompleteVideoFrame(handle, DR_OK);
    assert(LiGetPendingVideoFrames() == 0 && idrFrameProcessed);
    frame(2, false, true);
    assert(LiWaitForNextVideoFrame(&handle, &unit) && unit->frameNumber == 2);
    LiCompleteVideoFrame(handle, DR_NEED_IDR);
    assert(waitingForIdrFrame && LiGetPendingVideoFrames() == 0);
    frame(3, true, true);
    assert(LiWaitForNextVideoFrame(&handle, &unit) && unit->frameType == FRAME_TYPE_IDR);
    LiCompleteVideoFrame(handle, DR_OK);
    assert(!waitingForIdrFrame && LiGetPendingVideoFrames() == 0);
    done();
}
int main(void) {
    repeatedHolesRequestAtRecovery(); lostRequestedIdrRetriesOnNextCompleteFrame();
    decoderRefreshAndNetworkRecovery();
    exceptionalDirectRequestsRemainImmediate();
    timedRetriesAndRelease(); lossGatingAndReferenceSafety(); controlOutcomesAndReset(); staleSamples(); localDecoderRecoveryScenarios(); ordinaryWaitDoesNotGainTimeoutRetry();
    pullRendererAdmissionAndRefresh();
    puts("Prepared IDR wait/retry, reference safety, dispatch, reset and actual pull/wake/completion tests passed");
    return 0;
}
