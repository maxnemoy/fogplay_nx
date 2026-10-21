#include "wakeup-check.h"
#include <algorithm>
#include <cstdio>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
extern "C" int rtc_switch_pipe(int fds[2]);
extern "C" int rtc_switch_poll(struct pollfd *, nfds_t, int);
enum class Mode { Delayed, Missing, RecvError, Zero, Excess, PollError, SendError };
struct Fake { Mode mode; int64_t now = 0; int sent = 0; std::vector<int64_t> delivery; };
static int fakeSend(void *context, int fd, char) {
    auto &fake = *static_cast<Fake *>(context);
    if (fd != 12) return -1;
    if (fake.mode == Mode::SendError) { errno = ENOTSOCK; return -1; }
    if (fake.sent++ == 0) fake.delivery.push_back(100);
    else if (fake.mode != Mode::Missing) fake.delivery.push_back(900);
    if (fake.mode == Mode::Excess && fake.sent == 4) fake.delivery.push_back(900);
    return 1;
}
static int fakeRecv(void *context, int fd, char *) {
    auto &fake = *static_cast<Fake *>(context);
    if (fd != 11) return -1;
    if (fake.mode == Mode::RecvError) { errno = ENOTSOCK; return -1; }
    if (!fake.delivery.empty() && fake.delivery.front() <= fake.now) {
        fake.delivery.erase(fake.delivery.begin());
        return fake.mode == Mode::Zero ? 0 : 1;
    }
    errno = EAGAIN; return -1;
}
static int fakePoll(void *context, struct pollfd *pfd, int timeout) {
    auto &fake = *static_cast<Fake *>(context);
    pfd->revents = 0; // same input contract as rtc_switch_poll
    if (fake.mode == Mode::PollError) { errno = ENOTSOCK; return -1; }
    if (!fake.delivery.empty() && fake.delivery.front() <= fake.now + timeout) {
        fake.now = std::max(fake.now, fake.delivery.front()); pfd->revents = POLLIN; return 1;
    }
    fake.now += timeout; return 0;
}
static int64_t fakeNow(void *context) { return static_cast<Fake *>(context)->now; }
static int realSend(void *, int fd, char value) { return send(fd, &value, 1, 0); }
static int realRecv(void *, int fd, char *value) { return recv(fd, value, 1, 0); }
static int realPoll(void *, struct pollfd *fd, int timeout) { return rtc_switch_poll(fd, 1, timeout); }
static int64_t realNow(void *) { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static bool scenario(Mode mode, int expected, int expectedCount) {
    Fake fake{mode,0,0,{}};
    RtcWakeupOps ops{&fake,fakeSend,fakeRecv,fakePoll,fakeNow};
    RtcWakeupResult result;
    int code = rtcCheckWakeup(11,12,1,ops,result);
    if (code != expected || result.count != expectedCount) {
        printf("FAIL mode=%d code=%d count=%d stage=%s errno=%d\n",int(mode),code,result.count,result.stage,result.recvError); return false;
    }
    if (mode == Mode::Delayed && (result.polls != 2 || result.extraWaits != 1 || result.elapsedMillis != 900 || result.recvResult != -1 || result.recvError != EAGAIN)) return false;
    if (mode == Mode::Missing && (result.elapsedMillis != 1000 || result.recvError != EAGAIN)) return false;
    if (mode == Mode::RecvError && (result.recvResult != -1 || result.recvError != ENOTSOCK)) return false;
    if (mode == Mode::Zero && (result.recvResult != 0 || result.recvError != 0)) return false;
    return true;
}
int main() {
    if (!scenario(Mode::Delayed,0,4) || !scenario(Mode::Missing,6,1) ||
        !scenario(Mode::RecvError,4,0) || !scenario(Mode::Zero,4,0) ||
        !scenario(Mode::Excess,4,5) || !scenario(Mode::PollError,3,0) ||
        !scenario(Mode::SendError,2,0)) return 1;
    int fds[2]; if (rtc_switch_pipe(fds)) return 2;
    RtcWakeupOps ops{nullptr,realSend,realRecv,realPoll,realNow}; RtcWakeupResult result;
    int code = rtcCheckWakeup(fds[0],fds[1],100,ops,result); close(fds[0]); close(fds[1]);
    if (code || result.completed != 100 || result.totalReceived != 400) return 3;
    printf("PASS exact shared probe helper: delayed1+3, missing deadline, errno, zero datagram, excess, send/poll error; real UDP100 cycles400 bytes polls=%u extra=%u\n",result.polls,result.extraWaits);
}
