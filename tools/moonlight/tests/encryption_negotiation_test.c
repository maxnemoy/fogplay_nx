// Full prepared RTSP handshake + pinned SDP generator; only TCP transport is a fixture.
#include "Limelight-internal.h"
#include "FogMoonlightDiagnosticsInternal.h"
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
struct _RTSP_MESSAGE;
static bool fixtureTransaction(struct _RTSP_MESSAGE* request, struct _RTSP_MESSAGE* response, int* error);
static char* fixtureSdpPayload(int version, int* length);
static void fixtureFreeMessage(struct _RTSP_MESSAGE* message);
PREPARED_RTSP

char* RemoteAddrString = "127.0.0.1";
struct sockaddr_storage RemoteAddr, LocalAddr;
SOCKADDR_LEN AddrLen = sizeof(struct sockaddr_in);
int AppVersionQuad[4];
STREAM_CONFIGURATION StreamConfig;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
DECODER_RENDERER_CALLBACKS VideoCallbacks;
AUDIO_RENDERER_CALLBACKS AudioCallbacks;
volatile bool ConnectionInterrupted;
int NegotiatedVideoFormat, AudioPacketDuration;
bool HighQualitySurroundSupported, HighQualitySurroundEnabled, AudioEncryptionEnabled, ReferenceFrameInvalidationSupported;
OPUS_MULTISTREAM_CONFIGURATION NormalQualityOpusConfig, HighQualityOpusConfig;
uint16_t RtspPortNumber = 48010, AudioPortNumber, VideoPortNumber, ControlPortNumber;
SS_PING AudioPingPayload, VideoPingPayload;
uint32_t ControlConnectData, SunshineFeatureFlags, EncryptionFeaturesSupported, EncryptionFeaturesRequested, EncryptionFeaturesEnabled;
static uint32_t fixtureSupported, fixtureRequired;
static int announces, plays, setups;
static bool refuseAnnounce, fixtureEncryptedRtsp;
static int fixtureVersionPatch=450;
static char announced[10000];
static char* allocatedPayload;
static unsigned payloadAllocations, payloadFrees;
static char* fixtureSdpPayload(int version, int* length) {
    char* result=getSdpPayloadForStreamConfig(version,length);
    assert(!allocatedPayload);
    if (result) { allocatedPayload=result; ++payloadAllocations; }
    return result;
}
static void fixtureFreeMessage(PRTSP_MESSAGE message) {
    if (allocatedPayload && message->payload==allocatedPayload && (message->flags&FLAG_ALLOCATED_PAYLOAD)) {
        ++payloadFrees; allocatedPayload=NULL;
    }
    freeMessage(message);
}
static void logMessage(const char* format, ...) { (void)format; }
void addrToUrlSafeString(struct sockaddr_storage* addr, char* text, size_t size) {
    (void)addr; snprintf(text, size, "127.0.0.1");
}
bool isReferenceFrameInvalidationSupportedByDecoder(void) { return false; }
bool PltSafeStrcpy(char* dest, size_t size, const char* src) {
    if (strlen(src) >= size) return false;
    memcpy(dest, src, strlen(src)+1); return true;
}
int notifyAudioPortNegotiationComplete(void) { return 0; }
// ENet is never used by these modern host versions. Unexpected routing aborts.
int enet_address_set_address(ENetAddress* a, struct sockaddr* b, socklen_t c) { abort(); }
int enet_address_set_port(ENetAddress* a, enet_uint16 b) { abort(); }
ENetHost* enet_host_create(int a, const ENetAddress* b, size_t c, size_t d, enet_uint32 e, enet_uint32 f) { abort(); }
ENetPeer* enet_host_connect(ENetHost* a, const ENetAddress* b, size_t c, enet_uint32 d) { abort(); }
void enet_host_destroy(ENetHost* a) { abort(); }
void enet_host_flush(ENetHost* a) { abort(); }
void enet_peer_reset(ENetPeer* a) { abort(); }
void enet_peer_disconnect_now(ENetPeer* a, enet_uint32 b) { abort(); }
int serviceEnetHost(ENetHost* a, ENetEvent* b, enet_uint32 c) { abort(); }
void PltSleepMs(int ms) { (void)ms; }
uint64_t PltGetMillis(void) { return 1; }
static bool fixtureTransaction(PRTSP_MESSAGE request, PRTSP_MESSAGE response, int* error) {
    *error = 0;
    const char* method = request->message.request.command;
    char payload[512] = "";
    int status = 200;
    if (!strcmp(method, "DESCRIBE")) {
        snprintf(payload, sizeof(payload), "v=0\r\na=x-ss-general.encryptionSupported:%u\r\na=x-ss-general.encryptionRequested:%u\r\n", fixtureSupported, fixtureRequired);
    } else if (!strcmp(method, "SETUP")) ++setups;
    else if (!strcmp(method, "ANNOUNCE")) {
        ++announces;
        assert(request->payloadLength < (int)sizeof(announced));
        memcpy(announced, request->payload, request->payloadLength);
        announced[request->payloadLength] = 0;
        status = refuseAnnounce ? 403 : 200;
    } else if (!strcmp(method, "PLAY")) ++plays;
    createRtspResponse(response, NULL, 0, "RTSP/1.0", status, "Fixture", 1, NULL, NULL, 0);
    if (payload[0]) {
        response->payload = strdup(payload);
        response->payloadLength = strlen(payload);
        response->flags |= FLAG_ALLOCATED_PAYLOAD;
    }
    if (!strcmp(method, "SETUP")) {
        assert(addOption(response, "Session", "fixture-session"));
        assert(addOption(response, "Transport", "server_port=48001"));
    }
    return true;
}
static void check(int flags, uint32_t supported, uint32_t required, int versionTail,
                  uint32_t selected, bool audio, int expected) {
    FogDiagReset();
    FogDiagSet(FOG_DIAG_encryptionClientFlags, (uint32_t)flags);
    memset(&StreamConfig, 0, sizeof(StreamConfig));
    StreamConfig.width = 1280; StreamConfig.height = 720; StreamConfig.fps = 30;
    StreamConfig.bitrate = 20000; StreamConfig.packetSize = 1024;
    StreamConfig.streamingRemotely = STREAM_CFG_REMOTE;
    StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    StreamConfig.supportedVideoFormats = VIDEO_FORMAT_H264;
    StreamConfig.encryptionFlags = flags;
    AppVersionQuad[0] = 7; AppVersionQuad[1] = 1; AppVersionQuad[2] = fixtureVersionPatch; AppVersionQuad[3] = versionTail;
    RemoteAddr.ss_family = AF_INET;
    AudioPortNumber = VideoPortNumber = ControlPortNumber = 0;
    fixtureSupported = supported; fixtureRequired = required;
    announces = plays = setups = 0; announced[0] = 0;
    assert(!allocatedPayload); payloadAllocations=payloadFrees=0;
    SERVER_INFORMATION server = {0};
    server.rtspSessionUrl = fixtureEncryptedRtsp ? "rtspenc://127.0.0.1:48010" : "rtsp://127.0.0.1:48010";
    assert(performRtspHandshake(&server) == expected);
    FogMoonlightDiagnostics snapshot;
    FogMoonlightGetDiagnostics(&snapshot);
    assert(snapshot.encryptionClientFlags == (uint32_t)flags);
    assert(snapshot.encryptionHostSupported == supported && snapshot.encryptionHostRequired == required);
    assert(snapshot.encryptionRtsp == fixtureEncryptedRtsp);
    assert(!allocatedPayload && payloadAllocations==payloadFrees);
    assert(snapshot.encryptionSelected == selected && snapshot.encryptionAudio == audio);
    if (expected == FOG_MOONLIGHT_ENCRYPTION_CONFLICT) {
        assert(snapshot.encryptionState == FOG_ENCRYPTION_FAILED);
        assert(snapshot.encryptionFailure == FOG_ENCRYPTION_FAILURE_CONFLICT);
        assert(!announces && !plays);
        const bool earlyConflict=(required & ~supported & 7)!=0;
        assert(payloadAllocations==(earlyConflict?0:1));
        assert(earlyConflict ? setups==0 : setups>0);
    } else {
        assert(snapshot.encryptionSelected == selected && snapshot.encryptionAudio == audio);
        assert(announces == 1 && plays == (expected == 0 ? (fixtureVersionPatch >= 431 ? 1 : 2) : 0));
        assert(snapshot.encryptionState == (expected == 0 ? FOG_ENCRYPTION_ACCEPTED : FOG_ENCRYPTION_FAILED));
        assert(snapshot.encryptionFailure == (expected == 0 ? 0 : FOG_ENCRYPTION_FAILURE_HANDSHAKE));
        if (versionTail < 0) {
            char field[80]; snprintf(field, sizeof(field), "a=x-ss-general.encryptionEnabled:%u", selected);
            assert(strstr(announced, field));
        }
    }
}
static void checkRtspCrypto(void) {
    const unsigned char reply[]="RTSP/1.0 200 OK\r\nCSeq: 1\r\n\r\n";
    struct { ENC_RTSP_HEADER header; unsigned char data[128]; } packet;
    unsigned char iv[12]={1,0,0,0,0,0,0,0,0,0,'H','R'};
    memset(StreamConfig.remoteInputAesKey,0,sizeof(StreamConfig.remoteInputAesKey));
    encryptedRtspEnabled=true;
    for (int fault=0;fault<3;++fault) {
        memset(&packet,0,sizeof(packet));
        packet.header.typeAndLength=BE32(ENCRYPTED_RTSP_BIT|(sizeof(reply)-1));
        packet.header.sequenceNumber=BE32(1);
        PPLT_CRYPTO_CONTEXT host=PltCreateCryptoContext(); decryptionCtx=PltCreateCryptoContext();
        assert(host && decryptionCtx); int length=0;
        assert(PltEncryptMessage(host,ALGORITHM_AES_GCM,0,(unsigned char*)StreamConfig.remoteInputAesKey,16,iv,12,
            packet.header.tag,16,(unsigned char*)reply,sizeof(reply)-1,packet.data,&length));
        if (fault==1) packet.header.tag[0]^=1;
        RTSP_MESSAGE response;
        bool ok=fault==2 ? unsealRtspMessage((char*)reply,sizeof(reply)-1,&response) :
            unsealRtspMessage((char*)&packet,sizeof(packet.header)+length,&response);
        assert(ok == (fault==0));
        if (ok) { assert(response.message.response.statusCode==200); freeMessage(&response); }
        PltDestroyCryptoContext(host); PltDestroyCryptoContext(decryptionCtx); decryptionCtx=NULL;
    }
    encryptionCtx=PltCreateCryptoContext(); assert(encryptionCtx); encryptionSequenceNumber=0;
    RTSP_MESSAGE request; createRtspRequest(&request,NULL,0,"OPTIONS","rtspenc://fixture","RTSP/1.0",1,NULL,NULL,0);
    int length=0; char* sealed=sealRtspMessage(&request,&length); assert(sealed);
    assert(length>(int)sizeof(ENC_RTSP_HEADER));
    assert(BE32(((PENC_RTSP_HEADER)sealed)->typeAndLength)&ENCRYPTED_RTSP_BIT);
    free(sealed); freeMessage(&request); PltDestroyCryptoContext(encryptionCtx); encryptionCtx=NULL;
    encryptedRtspEnabled=false;
}
int main(void) {
    ListenerCallbacks.logMessage = logMessage;
    check(ENCFLG_ALL, 7, 0, -1, 7, true, 0);
    check(ENCFLG_NONE, 7, 0, -1, 1, false, 0);
    check(ENCFLG_ALL, 0, 0, -1, 0, true, 0); // NV audio path remains independent.
    check(ENCFLG_ALL, 3, 0, -1, 3, true, 0);
    check(ENCFLG_ALL, 5, 0, -1, 5, true, 0);
    check(ENCFLG_NONE, 7, 6, -1, 7, true, 0);
    check(ENCFLG_ALL, 0, 2, -1, 0, false, FOG_MOONLIGHT_ENCRYPTION_CONFLICT);
    check(ENCFLG_NONE, 0, 4, -1, 0, false, FOG_MOONLIGHT_ENCRYPTION_CONFLICT);
    check(ENCFLG_ALL, 0, 1, -1, 0, false, FOG_MOONLIGHT_ENCRYPTION_CONFLICT);
    check(ENCFLG_ALL, 7 | 0x80, 0x80, -1, 7, true, 0); // Unknown extension bits are recorded, not invented.
    check(ENCFLG_ALL, 0, 0, 0, 0, true, 0); // GFE cannot gain video encryption from ALL.
    // Advertised support cannot substitute for actual host/version selection.
    check(ENCFLG_NONE,7,2,0,0,false,FOG_MOONLIGHT_ENCRYPTION_CONFLICT);
    check(ENCFLG_ALL,7,2,0,0,true,FOG_MOONLIGHT_ENCRYPTION_CONFLICT);
    check(ENCFLG_NONE,7,1,0,0,false,FOG_MOONLIGHT_ENCRYPTION_CONFLICT);
    check(ENCFLG_NONE,7,0,0,0,false,0); // Optional GFE fallback remains compatible.
    fixtureVersionPatch=430;
    check(ENCFLG_NONE,4,4,-1,4,false,FOG_MOONLIGHT_ENCRYPTION_CONFLICT);
    check(ENCFLG_ALL,1,1,-1,1,false,FOG_MOONLIGHT_ENCRYPTION_CONFLICT);
    check(ENCFLG_ALL,4,0,-1,4,false,0); // Optional SS audio bit does not imply CBC.
    check(ENCFLG_ALL,0,0,0,0,false,0);
    fixtureVersionPatch=450; fixtureEncryptedRtsp=true;
    check(ENCFLG_ALL,7,0,-1,7,true,0);
    fixtureEncryptedRtsp=false;
    checkRtspCrypto();
    refuseAnnounce = true;
    check(ENCFLG_ALL, 7, 0, -1, 7, true, 403);
    puts("Prepared RTSP handshake/SDP encryption selection, explicit fallback and pre-ANNOUNCE conflict rejection passed");
}