// Uses the exact prepared SO_RCVBUF policy with deterministic OS responses.
#include "FogMoonlightDiagnosticsInternal.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#define RCV_BUFFER_SIZE_MIN 32767
#define RCV_BUFFER_SIZE_STEP 16384
#define SOCK_QOS_TYPE_VIDEO 1
#define SOL_SOCKET 0
#define SO_RCVBUF 0
#define Limelog(...) ((void)0)
typedef unsigned int SOCKADDR_LEN;
static int scenario, attempts, queries, lastError;
static int setBuffer(const char* value) {
    const int bytes = *(const int*)value;
    attempts++;
    if (scenario == 2 || (scenario == 1 && bytes > 65535)) { lastError = 12; return -1; }
    return 0;
}
static int getBuffer(char* value, SOCKADDR_LEN* length) {
    assert(*length == sizeof(int)); queries++;
    if (scenario == 3) { lastError = 22; return -1; }
    *(int*)value = 131070;
    return 0;
}
#define setsockopt(socket, level, option, value, length) setBuffer(value)
#define getsockopt(socket, level, option, value, length) getBuffer(value, length)
#define LastSocketError() lastError
static int bindVideoBuffer(int bufferSize, int socketQosType) {
    int err;
    const int s = 7;
SOCKET_BUFFER
}
int main(void) {
    for (scenario = 0; scenario < 4; scenario++) {
        FogDiagReset(); attempts = queries = lastError = 0;
        assert(bindVideoBuffer(98303, SOCK_QOS_TYPE_VIDEO) == 7);
        FogMoonlightDiagnostics snapshot;
        FogMoonlightGetDiagnostics(&snapshot);
        assert(snapshot.videoReceiveBufferRequestedBytes == 98303);
        assert(snapshot.videoReceiveBufferSelectedBytes == (scenario == 2 ? 0 : scenario == 1 ? 65535 : 98303));
        assert(snapshot.videoReceiveBufferSetFailures == (scenario == 2 ? 5 : scenario == 1 ? 2 : 0));
        assert(snapshot.videoReceiveBufferSetLastError == (scenario == 1 || scenario == 2 ? 12 : 0));
        assert(snapshot.videoReceiveBufferActualBytes == (scenario == 3 ? 0 : 131070));
        assert(snapshot.videoReceiveBufferQuerySucceeded == (scenario != 3));
        assert(snapshot.videoReceiveBufferQueryError == (scenario == 3 ? 22 : 0));
        assert(attempts == (scenario == 2 ? 5 : scenario == 1 ? 3 : 1));
        assert(queries == 1); // query also happens without LC_DEBUG
    }
    FogDiagReset(); attempts = queries = 0;
    assert(bindVideoBuffer(98303, 2) == 7);
    FogMoonlightDiagnostics snapshot;
    FogMoonlightGetDiagnostics(&snapshot);
    assert(snapshot.videoReceiveBufferRequestedBytes == 0 && queries == 0);
    puts("Prepared SO_RCVBUF success, fallback, all-failed, query-failed and unrelated socket cases passed");
    return 0;
}
