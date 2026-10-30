"""Cancellable pinned RTSP waits and unambiguous termination provenance."""
from pathlib import Path


def apply(dest: Path):
    def change(file, old, new, count=1):
        path = dest / "core" / file
        text = path.read_text()
        if text.count(old) != count:
            raise RuntimeError(f"Pinned transport source mismatch: {file}: {old[:70]!r}")
        path.write_text(text.replace(old, new))

    for file in ("Connection.c", "ControlStream.c"):
        change(file, '#include "Limelight-internal.h"',
               '#include "Limelight-internal.h"\n#include "FogMoonlightTermination.h"')
    change("Connection.c", """    if (ConnectionInterrupted ||
        atomic_exchange_explicit(&alreadyTerminated, true, memory_order_relaxed)) {
        return;
    }
    originalTerminationCallback(errorCode);""", """    // An explicit host stop remains authoritative if it races socket failure
    // or teardown. All delivery is synchronous and the owner joins this thread.
    if (FogMoonlightGetTerminationOrigin() == FOG_TERMINATION_HOST) {
        atomic_store_explicit(&alreadyTerminated, true, memory_order_relaxed);
    }
    else if (ConnectionInterrupted ||
             atomic_exchange_explicit(&alreadyTerminated, true, memory_order_relaxed)) {
        return;
    }
    originalTerminationCallback(errorCode);""")
    change("ControlStream.c", "ListenerCallbacks.connectionTerminated((int)terminationErrorCode);",
           "FogMoonlightNotifyHostTermination(ListenerCallbacks.connectionTerminated, (int)terminationErrorCode);")

    # Keep the ordinary connectivity probe independent of ConnectionInterrupted:
    # the preceding stream may have left that flag set. Only RTSP opts in.
    change("PlatformSockets.c", "SOCKET connectTcpSocket(struct sockaddr_storage* dstaddr, SOCKADDR_LEN addrlen, unsigned short port, int timeoutSec) {", """int FogMoonlightPollSockets(struct pollfd* fds, int count, int timeoutMs) {
    const uint64_t started = PltGetMillis();
    for (;;) {
        if (ConnectionInterrupted) {
            SetLastSocketError(EINTR);
            return -1;
        }
        const uint64_t elapsed = PltGetMillis() - started;
        const int remaining = elapsed < (uint64_t)timeoutMs ? timeoutMs - (int)elapsed : 0;
        const int result = pollSockets(fds, count, remaining < 100 ? remaining : 100);
        if (ConnectionInterrupted) {
            SetLastSocketError(EINTR);
            return -1;
        }
        if (result != 0 || PltGetMillis() - started >= (uint64_t)timeoutMs) return result;
    }
}

static SOCKET connectTcpSocketInternal(struct sockaddr_storage* dstaddr, SOCKADDR_LEN addrlen, unsigned short port, int timeoutSec, bool interruptible) {""")
    change("PlatformSockets.c", "err = pollSockets(&pfd, 1, timeoutSec * 1000);", """err = interruptible ? FogMoonlightPollSockets(&pfd, 1, timeoutSec * 1000) :
                          pollSockets(&pfd, 1, timeoutSec * 1000);""")
    change("PlatformSockets.c", """    return s;
}

int getLocalAddressByUdpConnect""", """    return s;
}

SOCKET connectTcpSocket(struct sockaddr_storage* dstaddr, SOCKADDR_LEN addrlen, unsigned short port, int timeoutSec) {
    return connectTcpSocketInternal(dstaddr, addrlen, port, timeoutSec, false);
}

SOCKET FogMoonlightConnectTcpSocket(struct sockaddr_storage* dstaddr, SOCKADDR_LEN addrlen, unsigned short port, int timeoutSec) {
    if (ConnectionInterrupted) { SetLastSocketError(EINTR); return INVALID_SOCKET; }
    return connectTcpSocketInternal(dstaddr, addrlen, port, timeoutSec, true);
}

int getLocalAddressByUdpConnect""")
    change("RtspConnection.c", '#include "Limelight-internal.h"', """#include "Limelight-internal.h"
extern int FogMoonlightPollSockets(struct pollfd*, int, int);
extern SOCKET FogMoonlightConnectTcpSocket(struct sockaddr_storage*, SOCKADDR_LEN, unsigned short, int);""")
    change("RtspConnection.c", "sock = connectTcpSocket(&RemoteAddr, AddrLen, RtspPortNumber, RTSP_CONNECT_TIMEOUT_SEC);",
           "sock = FogMoonlightConnectTcpSocket(&RemoteAddr, AddrLen, RtspPortNumber, RTSP_CONNECT_TIMEOUT_SEC);")
    change("RtspConnection.c", "err = pollSockets(&pfd, 1, RTSP_RECEIVE_TIMEOUT_SEC * 1000);",
           "err = FogMoonlightPollSockets(&pfd, 1, RTSP_RECEIVE_TIMEOUT_SEC * 1000);")
    change("RtspConnection.c", "PltSleepMs(RTSP_RETRY_DELAY_MS);", """for (int slept = 0; slept < RTSP_RETRY_DELAY_MS && !ConnectionInterrupted; slept += 100) {
                    PltSleepMs(RTSP_RETRY_DELAY_MS - slept < 100 ? RTSP_RETRY_DELAY_MS - slept : 100);
                }""")
