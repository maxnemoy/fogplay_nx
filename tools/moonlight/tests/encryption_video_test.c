// Exact prepared receive loop with real pinned crypto; only I/O/queue/clock are fakes.
#include "Limelight-internal.h"
#include "FogMoonlightDiagnosticsInternal.h"
#include <assert.h>
#include <stdio.h>
static int scenario, iterations, rtpCalls;
static uint64_t clockUs = 100000;
static unsigned char storage[256], encryptedStorage[256];
STREAM_CONFIGURATION StreamConfig;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
static PPLT_CRYPTO_CONTEXT decryptionCtx;
static void quiet(const char* format, ...) { (void)format; }
static void terminate(int error) { (void)error; assert(false); }
static void FogIdrRecoveryPollDecoder(void) {}
uint64_t PltGetMicroseconds(void) { return scenario == 2 ? --clockUs : ++clockUs; }
uint64_t PltGetMillis(void) { return 1; }
#define PltIsThreadInterrupted(thread) (iterations++ >= 1)
#define FIRST_FRAME_TIMEOUT_SEC 1
#define RtpvGetCurrentFrameNumber(queue) 1
static void* allocate(size_t size) { assert(size <= sizeof(storage)); return storage; }
#define malloc allocate
static int receivePacket(void) {
    const unsigned char cipher[16]={0x03,0x88,0xda,0xce,0x60,0xb6,0xa3,0x92,0xf3,0x28,0xc2,0xb9,0x71,0xb2,0xfe,0x78};
    const unsigned char tag[16]={0xab,0x6e,0x47,0xd4,0x2c,0xec,0x13,0xbd,0xf5,0x3a,0x67,0xb2,0x12,0x57,0xbd,0xdf};
    PENC_VIDEO_HEADER header=(PENC_VIDEO_HEADER)encryptedStorage;
    memset(header,0,sizeof(*header)); memcpy(header->tag,tag,16);
    memcpy(header+1,cipher,16);
    if (scenario == 1) header->tag[0] ^= 1;
    return sizeof(*header)+16;
}
#define recvUdpSocket(...) receivePacket()
static int addRtp(void) { ++rtpCalls; return RTPF_RET_QUEUED; }
#define RtpvAddPacket(...) addRtp()
static void receiveOne(void) {
    int err, queueStatus;
    const int bufferSize=sizeof(storage), minSize=sizeof(ENC_VIDEO_HEADER)+sizeof(RTP_PACKET), decryptedSize=128;
    char* buffer=NULL; char* encryptedBuffer=(char*)encryptedStorage;
    bool receivedDataFromPeer=false, receivedFullFrame=false, encrypted=true;
    int waitingForVideoMs=0; uint64_t firstDataTimeMs=0;
RECEIVE_LOOP
    (void)decryptedSize; (void)buffer; (void)firstDataTimeMs;
}
int main(void) {
    ListenerCallbacks.logMessage=quiet; ListenerCallbacks.connectionTerminated=terminate;
    for (scenario=0;scenario<3;++scenario) {
        FogDiagReset(); iterations=rtpCalls=0;
        decryptionCtx=PltCreateCryptoContext(); assert(decryptionCtx);
        receiveOne(); PltDestroyCryptoContext(decryptionCtx);
        FogMoonlightDiagnostics snapshot; FogMoonlightGetDiagnostics(&snapshot);
        assert(snapshot.videoDecryptSuccesses == (scenario == 1 ? 0 : 1));
        assert(snapshot.videoDecryptFailures == (scenario == 1 ? 1 : 0));
        assert(rtpCalls == (scenario == 1 ? 0 : 1));
        if (scenario == 2) assert(snapshot.videoReceiveTimingInvalidTimes > 0 && !snapshot.videoReceiveDecryptCount);
    }
    puts("Prepared video receive: real AES-GCM success/tag rejection, no RTP on failure, success survives invalid timing passed");
}