#include "Limelight-internal.h"
#include "FogMoonlightTermination.h"
#include <assert.h>
#include <stdatomic.h>
#include <pthread.h>

#undef Limelog
#define Limelog(...) ((void)0)
static atomic_bool alreadyTerminated;
volatile bool ConnectionInterrupted;
static void (*originalTerminationCallback)(int);
TERMINATION_SHIM
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
static int events, lastCode, lastOrigin;
static void capture(int code) {
    events++;
    lastCode = code;
    lastOrigin = FogMoonlightGetTerminationOrigin();
}
static void hostMessage(const uint8_t* bytes, int packetLength) {
    const struct { uint16_t type; }* ctlHdr = (const void*)bytes;
    // The pinned header is a two-byte message type.
    struct { uint16_t type; } header;
    (void)header;
    const unsigned lastSeenFrame = 1;
    BYTE_BUFFER bb;
    HOST_PARSER
}
static pthread_barrier_t originBarrier;
static _Thread_local int expectedOrigin;
static void parallelCapture(int unused) {
    (void)unused;
    pthread_barrier_wait(&originBarrier);
    assert(FogMoonlightGetTerminationOrigin() == expectedOrigin);
    pthread_barrier_wait(&originBarrier);
}
static void* parallelNotification(void* host) {
    expectedOrigin = host ? FOG_TERMINATION_HOST : FOG_TERMINATION_TRANSPORT;
    if (host) FogMoonlightNotifyHostTermination(parallelCapture, ECONNRESET);
    else parallelCapture(ECONNRESET);
    assert(FogMoonlightGetTerminationOrigin() == FOG_TERMINATION_TRANSPORT);
    return NULL;
}
int main(void) {
    originalTerminationCallback = capture;
    ListenerCallbacks.connectionTerminated = ClInternalConnectionTerminated;
    // This positive host reason deliberately collides with a retryable errno.
    const uint8_t shortReason[] = {0, 0, ECONNRESET, 0};
    ClInternalConnectionTerminated(ECONNRESET);
    assert(events == 1 && lastOrigin == FOG_TERMINATION_TRANSPORT && lastCode == ECONNRESET);
    hostMessage(shortReason, sizeof(shortReason));
    assert(events == 2 && lastOrigin == FOG_TERMINATION_HOST && lastCode == ECONNRESET);
    assert(FogMoonlightGetTerminationOrigin() == FOG_TERMINATION_TRANSPORT);
    // Even during stop an already received explicit message remains terminal.
    ConnectionInterrupted = true;
    const uint8_t unknown[] = {0, 0, 0x34, 0x12};
    hostMessage(unknown, sizeof(unknown));
    assert(events == 3 && lastCode == 0x1234 && lastOrigin == FOG_TERMINATION_HOST);
    ClInternalConnectionTerminated(ETIMEDOUT);
    assert(events == 3);
    const uint8_t extended[] = {0, 0, 0x80, 0x03, 0, 0x23};
    hostMessage(extended, sizeof(extended));
    assert(events == 4 && lastCode == ML_ERROR_GRACEFUL_TERMINATION && lastOrigin == FOG_TERMINATION_HOST);
    assert(pthread_barrier_init(&originBarrier, NULL, 2) == 0);
    pthread_t socketThread, hostThread;
    assert(pthread_create(&socketThread, NULL, parallelNotification, NULL) == 0);
    assert(pthread_create(&hostThread, NULL, parallelNotification, &originBarrier) == 0);
    pthread_join(socketThread, NULL); pthread_join(hostThread, NULL);
    pthread_barrier_destroy(&originBarrier);
    puts("Prepared host short/extended parsing and termination shim provenance passed");
    return 0;
}
