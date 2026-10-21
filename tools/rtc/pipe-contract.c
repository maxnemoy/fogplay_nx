/* Offline check of the exact UDP-pair adapter under strict BSD poll inputs. */
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
static int strict_poll(struct pollfd *fds, nfds_t count, int timeout) {
    for (nfds_t i = 0; i < count; ++i) assert(fds[i].revents == 0);
    return poll(fds, count, timeout);
}
#define poll strict_poll
#include "pipe-switch.c"
#undef poll
int main(void) {
    int fds[2];
    assert(rtc_switch_pipe(fds) == 0);
    struct pollfd pfd = { .fd = fds[0], .events = POLLIN, .revents = POLLIN | POLLERR };
    for (int cycle = 0; cycle < 100; ++cycle) {
        char value = 0;
        for (int i = 0; i < 4; ++i) assert(send(fds[1], &value, 1, 0) == 1);
        assert(rtc_switch_poll(&pfd, 1, 1000) == 1);
        assert(pfd.revents & POLLIN);
        int count = 0;
        while (recv(fds[0], &value, 1, 0) > 0) ++count;
        assert(count == 4 && (errno == EAGAIN || errno == EWOULDBLOCK));
        assert(rtc_switch_poll(&pfd, 1, 0) == 0);
        assert(pfd.revents == 0);
    }
    close(fds[0]); close(fds[1]);
    assert(rtc_switch_pipe(NULL) == -1 && errno == EINVAL);
    assert(rtc_switch_poll(NULL, 1, 0) == -1 && errno == EFAULT);
    assert(rtc_switch_poll(NULL, 0, 0) == 0);
    puts("PASS UDP wake/drain/re-poll x100, stale revents reset, invalid inputs");
}
