// Harness substitutes I/O and clocks around the exact prepared receive loop.
#include "FogMoonlightDiagnosticsInternal.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define Limelog(...) ((void)0)
#define UDP_RECV_POLL_TIMEOUT_MS 1
#define FIRST_FRAME_TIMEOUT_SEC 1
#define ML_ERROR_NO_VIDEO_TRAFFIC 1
#define ML_ERROR_NO_VIDEO_FRAME 2
#define RTPF_RET_QUEUED 1
#define LE32(value) (value)
#define BE16(value) (value)
#define BE32(value) (value)
typedef struct { uint16_t sequenceNumber; uint32_t timestamp, ssrc; } RTP_PACKET, *PRTP_PACKET;
typedef struct { uint32_t frameNumber; } ENC_VIDEO_HEADER, *PENC_VIDEO_HEADER;
typedef void* PRTPV_QUEUE_ENTRY;
static int scenario, iterations, maximumIterations, terminated, rtpCalls;
static uint64_t nowUs;
static unsigned char storage[256], encryptedStorage[256];
static uint64_t PltGetMicroseconds(void) { nowUs += 10; return nowUs; }
static uint64_t PltGetMillis(void) { return 2000; }
static void terminate(int error) { (void)error; terminated++; }
static const struct { void (*connectionTerminated)(int); } ListenerCallbacks = {terminate};
#define PltIsThreadInterrupted(thread) (iterations++ >= maximumIterations)
#define LastSocketError() 5
#define LastSocketFail() 5
#define SetLastSocketError(value) ((void)(value))
static void* allocate(size_t bytes) { (void)bytes; nowUs += 70; return scenario == 0 ? NULL : storage; }
#define malloc allocate
static int receivePacket(void) {
    nowUs += 500;
    ((PENC_VIDEO_HEADER)encryptedStorage)->frameNumber = scenario == 6 ? 1 : 10;
    if (scenario == 1) return -1;
    if (scenario == 2 || scenario == 3) return 0;
    return scenario == 5 ? 1 : 64;
}
#define recvUdpSocket(...) receivePacket()
#define RtpvGetCurrentFrameNumber(queue) 5
static bool decryptPacket(int* bytes) { nowUs += 30; *bytes = 64; return scenario != 7; }
#define PltDecryptMessage(...) decryptPacket(&err)
static int addRtp(void) { nowUs += 40; rtpCalls++; return RTPF_RET_QUEUED; }
#define RtpvAddPacket(...) addRtp()

static void run(void) {
    int err, queueStatus;
    const int bufferSize = sizeof(storage), minSize = 16, decryptedSize = 100;
    char* buffer = NULL;
    char* encryptedBuffer = (char*)encryptedStorage;
    bool receivedDataFromPeer = scenario != 2, receivedFullFrame = scenario != 4;
    bool encrypted = scenario >= 6 && scenario <= 8;
    int waitingForVideoMs = scenario == 2 ? 999 : 0;
    uint64_t firstDataTimeMs = 0;
RECEIVE_LOOP
    (void)bufferSize; (void)minSize; (void)decryptedSize;
    (void)buffer; (void)firstDataTimeMs;
}

int main(void) {
    for (scenario = 0; scenario <= 9; scenario++) {
        FogDiagReset();
        nowUs = 1000; iterations = 0; terminated = 0; rtpCalls = 0;
        maximumIterations = 2;
        run();
        FogMoonlightDiagnostics snapshot;
        FogMoonlightGetDiagnostics(&snapshot);
        const bool breaks = scenario <= 2 || scenario == 4;
        const uint64_t expectedCycles = breaks ? 1 : 2;
        assert(snapshot.videoReceiveCycleCount == expectedCycles);
        assert(snapshot.videoReceiveAllocationCount == (scenario >= 8 ? 2 : 1));
        assert(snapshot.videoReceiveLoopGapCount == expectedCycles - 1);
        assert(snapshot.videoReceiveCycleTotalUs == snapshot.videoReceiveWorkTotalUs + snapshot.videoReceiveWaitTotalUs);
        assert(snapshot.videoReceiveTimingInvalidTimes == 0);
        if (scenario == 0) assert(snapshot.videoReceiveWaitTotalUs == 0);
        else assert(snapshot.videoReceiveWaitTotalUs >= 500 * expectedCycles);
        assert(snapshot.videoReceiveWorkTotalUs >= 70);
        assert(terminated == (breaks ? 1 : 0));
        assert(rtpCalls == (scenario == 8 || scenario == 9 ? 2 : 0));
        assert(snapshot.videoReceiveDecryptCount == (scenario == 7 || scenario == 8 ? 2 : 0));
        assert(snapshot.videoDecryptFailures == (scenario == 7 ? 2 : 0));
        assert(snapshot.videoRuntPackets == (scenario == 5 ? 2 : 0));
        assert(snapshot.videoReceiveErrors == (scenario == 1 ? 1 : 0));
    }
    puts("Prepared receive timing covers normal, allocation/error/timeout/runt/stale/decryption exits");
    return 0;
}
