// Actual prepared audio decoder boundary, with real CBC and a sample sink.
#include "Limelight-internal.h"
#include "FogMoonlightDiagnosticsInternal.h"
#include <assert.h>
#include <stdio.h>
#define CHECK(test) do { if (!(test)) { fprintf(stderr,"Audio check failed at %d\n",__LINE__); abort(); } } while (0)
AUDIO_TYPES
static PPLT_CRYPTO_CONTEXT audioDecryptionCtx;
static uint32_t avRiKeyId;
static unsigned short lastSeq;
STREAM_CONFIGURATION StreamConfig;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
AUDIO_RENDERER_CALLBACKS AudioCallbacks;
bool AudioEncryptionEnabled;
static int calls;
static void quiet(const char* format, ...) { (void)format; }
static void sample(char* bytes, int size) {
    ++calls;
    if (bytes) CHECK(size==16 && !memcmp(bytes,"Synthetic opus!!",16));
    else CHECK(size==0);
}
AUDIO_DECODE
int main(void) {
    ListenerCallbacks.logMessage=quiet; AudioCallbacks.decodeAndPlaySample=sample;
    for (int scenario=0;scenario<4;++scenario) {
        FogDiagReset(); calls=0; lastSeq=0;
        QUEUED_AUDIO_PACKET packet={0}; PRTP_PACKET rtp=(PRTP_PACKET)packet.data;
        rtp->sequenceNumber=1;
        AudioEncryptionEnabled=scenario<2;
        audioDecryptionCtx=PltCreateCryptoContext(); CHECK(audioDecryptionCtx);
        if (AudioEncryptionEnabled) {
            PPLT_CRYPTO_CONTEXT enc=PltCreateCryptoContext(); CHECK(enc);
            unsigned char iv[16]={0}, plain[64]="Synthetic opus!!";
            uint32_t seq=BE32(1); memcpy(iv,&seq,sizeof(seq)); int size=0;
            CHECK(PltEncryptMessage(enc,ALGORITHM_AES_CBC,CIPHER_FLAG_RESET_IV|CIPHER_FLAG_FINISH|CIPHER_FLAG_PAD_TO_BLOCK_SIZE,
                (unsigned char*)StreamConfig.remoteInputAesKey,16,iv,16,NULL,0,plain,16,(unsigned char*)(rtp+1),&size));
            packet.header.size=sizeof(*rtp)+(scenario==1?15:size);
            PltDestroyCryptoContext(enc);
        } else {
            memcpy(rtp+1,"Synthetic opus!!",16);
            packet.header.size=scenario==3?0:(int)sizeof(*rtp)+16;
        }
        decodeInputData(&packet); PltDestroyCryptoContext(audioDecryptionCtx);
        FogMoonlightDiagnostics snapshot; FogMoonlightGetDiagnostics(&snapshot);
        CHECK(snapshot.audioDecryptSuccesses==(scenario==0?1:0));
        CHECK(snapshot.audioDecryptFailures==(scenario==1?1:0));
        CHECK(calls==(scenario==1?0:1));
    }
    puts("Prepared audio boundary: real CBC success/reject and plaintext/PLC counters passed");
}