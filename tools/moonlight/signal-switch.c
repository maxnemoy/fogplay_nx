#include <errno.h>
#include <signal.h>

// Horizon does not deliver POSIX SIGPIPE. Only satisfy Moonlight's explicit
// request to ignore it; report unsupported requests instead of pretending to
// install arbitrary handlers (the upstream Switch wrapper returns 0 for all).
int sigaction(int signal_number, const struct sigaction* action,
              struct sigaction* old_action) {
    if (signal_number == SIGPIPE && action != 0 &&
        action->sa_handler == SIG_IGN && old_action == 0) {
        return 0;
    }
    errno = ENOSYS;
    return -1;
}
