#include "Limelight-internal.h"
#include <assert.h>
#include <stdio.h>
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
static void quiet(const char* format, ...) { (void)format; }
static void checkGcm(void) {
    const unsigned char ciphertext[16] = {0x03,0x88,0xda,0xce,0x60,0xb6,0xa3,0x92,0xf3,0x28,0xc2,0xb9,0x71,0xb2,0xfe,0x78};
    const unsigned char tag[16] = {0xab,0x6e,0x47,0xd4,0x2c,0xec,0x13,0xbd,0xf5,0x3a,0x67,0xb2,0x12,0x57,0xbd,0xdf};
    unsigned char key[16] = {0}, iv[12] = {0}, packet[32], output[64];
    for (int fault=0; fault<3; ++fault) {
        memcpy(packet,tag,16); memcpy(packet+16,ciphertext,16);
        if (fault == 1) packet[0] ^= 1;
        if (fault == 2) packet[20] ^= 1;
        int size=0;
        PPLT_CRYPTO_CONTEXT ctx=PltCreateCryptoContext(); assert(ctx);
        bool result=PltDecryptMessage(ctx,ALGORITHM_AES_GCM,0,key,sizeof(key),iv,sizeof(iv),packet,16,packet+16,16,output,&size);
        assert(result == (fault == 0));
        if (!fault) { assert(size == 16); for (int i=0;i<size;++i) assert(output[i] == 0); }
        PltDestroyCryptoContext(ctx);
    }
}
static void checkCbc(void) {
    // RFC 3602 test case 1; verify its first CBC block plus PKCS7 round trip.
    unsigned char key[16]={0x06,0xa9,0x21,0x40,0x36,0xb8,0xa1,0x5b,0x51,0x2e,0x03,0xd5,0x34,0x12,0x00,0x06};
    unsigned char iv[16]={0x3d,0xaf,0xba,0x42,0x9d,0x9e,0xb4,0x30,0xb4,0x22,0xda,0x80,0x2c,0x9f,0xac,0x41};
    const unsigned char block[16]={0xe3,0x53,0x77,0x9c,0x10,0x79,0xae,0xb8,0x27,0x08,0x94,0x2d,0xbe,0x77,0x18,0x1a};
    unsigned char plain[64]="Single block msg", cipher[80], output[80];
    PPLT_CRYPTO_CONTEXT enc=PltCreateCryptoContext(), dec=PltCreateCryptoContext();
    assert(enc && dec); int size=0, outSize=0;
    assert(PltEncryptMessage(enc,ALGORITHM_AES_CBC,CIPHER_FLAG_RESET_IV|CIPHER_FLAG_FINISH|CIPHER_FLAG_PAD_TO_BLOCK_SIZE,
        key,16,iv,16,NULL,0,plain,16,cipher,&size));
    assert(size == 32 && !memcmp(cipher,block,16));
    assert(PltDecryptMessage(dec,ALGORITHM_AES_CBC,CIPHER_FLAG_RESET_IV|CIPHER_FLAG_FINISH,
        key,16,iv,16,NULL,0,cipher,size,output,&outSize));
    assert(outSize == 16 && !memcmp(output,"Single block msg",16));
    PltDestroyCryptoContext(enc); PltDestroyCryptoContext(dec);
}
int main(void) {
    ListenerCallbacks.logMessage=quiet;
    checkGcm(); checkCbc();
    puts("Pinned PlatformCrypto with real mbedTLS: NIST GCM vector/tag/cipher tampering and RFC3602 CBC passed");
}