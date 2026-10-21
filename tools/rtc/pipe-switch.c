#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
/* Used only by RTC poll wakeups, not as a general POSIX stream pipe. */
int rtc_switch_pipe(int fds[2]) {
    int a = -1, b = -1;
    struct sockaddr_in addr = {0};
    socklen_t len = sizeof(addr);
#ifdef __SWITCH__
    addr.sin_len = sizeof(addr);
#endif
    if (!fds) { errno = EINVAL; return -1; }
    fds[0] = fds[1] = -1;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    b = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (a < 0 || b < 0) goto fail;
    if (bind(a, (struct sockaddr *)&addr, sizeof(addr)) < 0) goto fail;
    if (bind(b, (struct sockaddr *)&addr, sizeof(addr)) < 0) goto fail;
    if (getsockname(a, (struct sockaddr *)&addr, &len) < 0) goto fail;
    if (connect(b, (struct sockaddr *)&addr, len) < 0) goto fail;
    len = sizeof(addr);
    if (getsockname(b, (struct sockaddr *)&addr, &len) < 0) goto fail;
    if (connect(a, (struct sockaddr *)&addr, len) < 0) goto fail;
    if (fcntl(a, F_SETFL, O_NONBLOCK) < 0 || fcntl(b, F_SETFL, O_NONBLOCK) < 0) goto fail;
    fds[0] = a;
    fds[1] = b;
    return 0;
fail: {
    int saved = errno;
    if (a >= 0) close(a);
    if (b >= 0) close(b);
    errno = saved;
    return -1;
}
}

/* BSD poll expects revents to be clear on entry; reused RTC arrays retain
 * the previous result. Clear only output fields, preserving fd/events. */
#ifdef __SWITCH__
#undef poll
#endif
int rtc_switch_poll(struct pollfd *fds, nfds_t count, int timeout) {
    if (!fds && count) { errno = EFAULT; return -1; }
    for (nfds_t i = 0; i < count; ++i) fds[i].revents = 0;
    return poll(fds, count, timeout);
}
