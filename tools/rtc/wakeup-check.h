#pragma once
#include <cerrno>
#include <cstdint>
#include <poll.h>
struct RtcWakeupOps {
    void *context;
    int (*sendByte)(void *, int, char);
    int (*recvByte)(void *, int, char *);
    int (*pollOne)(void *, struct pollfd *, int);
    int64_t (*nowMillis)(void *);
};
struct RtcWakeupResult {
    int code = 0, cycle = 0, count = 0, sendResult = 0, recvResult = 0;
    int sendError = 0, recvError = 0, pollResult = 0, pollError = 0;
    short flags = 0;
    const char *stage = "start";
    unsigned polls = 0, extraWaits = 0, completed = 0, totalReceived = 0;
    int64_t elapsedMillis = 0;
};
/* Nonblocking reads may reach EAGAIN before the rest of a UDP burst arrives.
 * All readiness waits and drains share one monotonic deadline per cycle. */
static int rtcCheckWakeup(int readFd, int writeFd, int cycles, const RtcWakeupOps &ops, RtcWakeupResult &out) {
    struct pollfd pfd = {};
    pfd.fd = readFd; pfd.events = POLLIN; pfd.revents = POLLIN | POLLERR;
    const int64_t started = ops.nowMillis(ops.context);
    for (int cycle = 0; cycle < cycles; ++cycle) {
        out.cycle = cycle + 1; out.count = 0; out.stage = "send";
        const int64_t deadline = ops.nowMillis(ops.context) + 1000;
        for (int i = 0; i < 4; ++i) {
            out.sendResult = ops.sendByte(ops.context, writeFd, 0);
            out.sendError = out.sendResult < 0 ? errno : 0;
            if (out.sendResult != 1) { out.code = 2; break; }
        }
        if (out.code) break;
        bool drained = false;
        unsigned waitsThisCycle = 0;
        while (!drained && !out.code) {
            out.stage = "wait";
            int64_t remaining = deadline - ops.nowMillis(ops.context);
            if (remaining <= 0) { out.code = 6; break; }
            out.pollResult = ops.pollOne(ops.context, &pfd, int(remaining));
            out.pollError = out.pollResult < 0 ? errno : 0;
            out.flags = pfd.revents;
            ++out.polls;
            if (waitsThisCycle++) ++out.extraWaits;
            if (out.pollResult < 0) {
                if (out.pollError == EINTR || out.pollError == EAGAIN) continue;
                out.code = 3; break;
            }
            if (out.pollResult == 0) continue;
            if (out.pollResult != 1 || !(out.flags & POLLIN) || (out.flags & (POLLERR | POLLHUP | POLLNVAL))) {
                out.code = 3; break;
            }
            out.stage = "drain";
            while (!out.code) {
                if (ops.nowMillis(ops.context) >= deadline) { out.code = 6; break; }
                char dummy = 0;
                out.recvResult = ops.recvByte(ops.context, readFd, &dummy);
                out.recvError = out.recvResult < 0 ? errno : 0;
                if (out.recvResult == 1) {
                    ++out.count; ++out.totalReceived;
                    if (out.count > 4) out.code = 4;
                    continue;
                }
                if (out.recvResult < 0 && out.recvError == EINTR) continue;
                if (out.recvResult < 0 && (out.recvError == EAGAIN || out.recvError == EWOULDBLOCK)) {
                    drained = out.count == 4;
                    break;
                }
                // A zero-length datagram or unexpected errno is a hard failure.
                out.code = 4; break;
            }
        }
        if (out.code) break;
        out.stage = "empty-repoll";
        out.pollResult = ops.pollOne(ops.context, &pfd, 0);
        out.pollError = out.pollResult < 0 ? errno : 0;
        out.flags = pfd.revents;
        if (out.pollResult != 0 || out.flags) { out.code = 5; break; }
        ++out.completed;
    }
    out.elapsedMillis = ops.nowMillis(ops.context) - started;
    if (!out.code) out.stage = "complete";
    return out.code;
}
