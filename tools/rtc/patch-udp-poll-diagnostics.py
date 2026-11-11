"""Add bounded cold-path UDP poll evidence to the pinned libjuice fork.

Applied after the original ICE recipe (or its frozen approved series). No
receive budget, callback ordering, scheduling or socket configuration changes.
"""
from pathlib import Path


def apply(root):
    juice = root / 'third_party/rtc/libdatachannel/deps/libjuice'

    def edit(name, before, after):
        path = juice / name
        text = path.read_text()
        if after in text:
            return
        if text.count(before) != 1:
            raise RuntimeError('Unrecognized UDP diagnostics site: ' + str(path))
        path.write_text(text.replace(before, after, 1))

    edit('include/juice/juice.h', 'typedef struct juice_diagnostics {', '''typedef struct juice_udp_poll_event {
\tuint64_t sequence, start_mono_us, elapsed_us, bytes, service_gap_us, ready_delay_us;
\tunsigned int datagrams, flags, exit_reason;
} juice_udp_poll_event_t;

typedef struct juice_udp_poll_diagnostics {
\tbool clock_available, sample_clock_available, last_clock_available, pending_yield;
\tbool rcvbuf_checked, rcvbuf_available;
\tint rcvbuf_bytes, rcvbuf_errno;
\tuint64_t sample_mono_us, clock_invalid;
\tuint64_t batches, bytes, timed_batches, elapsed_total_us, elapsed_max_us;
\tuint64_t last_start_mono_us, last_end_mono_us;
\tuint64_t service_gaps, service_gap_max_us;
\tuint64_t ready_delays, ready_delay_max_us;
\tuint64_t events_total, events_overwritten;
\tunsigned int event_count;
\tjuice_udp_poll_event_t events[16]; // circular storage; getter sorts oldest first
} juice_udp_poll_diagnostics_t;

typedef struct juice_diagnostics {''')
    edit('include/juice/juice.h', 'typedef void (*juice_cb_state_changed_t)', '''// One connection-lock acquisition, coherent summary and cold poll history.
JUICE_EXPORT int juice_get_udp_poll_diagnostics(juice_agent_t *agent,
    juice_diagnostics_t *summary, juice_udp_poll_diagnostics_t *out);

typedef void (*juice_cb_state_changed_t)''')
    edit('src/agent.h', '\tjuice_diagnostics_t diagnostics; // guarded by conn_lock',
         '\tjuice_diagnostics_t diagnostics; // guarded by conn_lock\n\tjuice_udp_poll_diagnostics_t udp_poll_diagnostics; // same lock')
    edit('src/juice.c', 'JUICE_EXPORT int juice_get_diagnostics(', '''JUICE_EXPORT int juice_get_udp_poll_diagnostics(juice_agent_t *agent,
    juice_diagnostics_t *summary, juice_udp_poll_diagnostics_t *out) {
\tif (!agent || !summary || !out)
\t\treturn JUICE_ERR_INVALID;
\tconn_lock(agent);
\t*summary = agent->diagnostics;
\tsummary->state = agent->state;
\t*out = agent->udp_poll_diagnostics;
\t// Normalize only on this cold query; writes remain O(1).
\tconst unsigned int count = out->event_count;
\tconst unsigned int first = (unsigned int)((out->events_total - count) % 16);
\tfor (unsigned int i = 0; i < count; ++i)
\t\tout->events[i] = agent->udp_poll_diagnostics.events[(first + i) % 16];
\t// Filled by the clock helper shared with conn_poll.c.
\tout->sample_clock_available = juice_udp_poll_clock(&out->sample_mono_us);
\tconn_unlock(agent);
\treturn JUICE_ERR_SUCCESS;
}

JUICE_EXPORT int juice_get_diagnostics(''')
    # A standalone helper header makes the exact production boundary clock
    # replaceable in deterministic direct-path tests, without packet clocks.
    clock = juice / 'src/udp_poll_clock.h'
    clock.write_text('''#ifndef JUICE_UDP_POLL_CLOCK_H
#define JUICE_UDP_POLL_CLOCK_H
#include <stdbool.h>
#include <stdint.h>
#if (defined(__linux__) || defined(__SWITCH__)) && !defined(JUICE_UDP_POLL_TEST_CLOCK)
#include <time.h>
#endif
#ifdef JUICE_UDP_POLL_TEST_CLOCK
bool juice_udp_poll_clock(uint64_t *out);
#else
static inline bool juice_udp_poll_clock(uint64_t *out) {
\t*out = 0;
#if defined(__linux__) || defined(__SWITCH__)
\tstruct timespec ts;
\tif (clock_gettime(CLOCK_MONOTONIC, &ts) || ts.tv_sec < 0 ||
\t    ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000)
\t\treturn false;
\t*out = (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
\treturn true;
#else
\treturn false; // no wall-clock fallback or unsupported CPU-time claim
#endif
}
#endif
#endif
''')
    edit('src/juice.c', '#include "agent.h"', '#include "agent.h"\n#include "udp_poll_clock.h"')
    edit('src/conn_poll.c', '#include <assert.h>', '#include "udp_poll_clock.h"\n\n#include <assert.h>')
    edit('src/conn_poll.c', '\ttimestamp_t next_timestamp;',
         '\tuint64_t poll_ready_mono_us;\n\tbool poll_ready_valid;\n\ttimestamp_t next_timestamp;')
    edit('src/conn_poll.c', 'int conn_poll_process(conn_registry_t *registry, pfds_record_t *pfds) {', '''int conn_poll_process(conn_registry_t *registry, pfds_record_t *pfds) {
\tuint64_t poll_ready_mono_us = 0;
\tconst bool poll_ready_valid = juice_udp_poll_clock(&poll_ready_mono_us);''')
    edit('src/conn_poll.c', '\t\tconn_poll_process_udp(agent, udp_pfd);', '''\t\tconn_impl->poll_ready_mono_us = poll_ready_mono_us;
\t\tconn_impl->poll_ready_valid = poll_ready_valid;
\t\tconn_poll_process_udp(agent, udp_pfd);''')
    edit('src/conn_poll.c', 'void conn_poll_process_udp(juice_agent_t *agent, struct pollfd *pfd) {', '''// Only connection initialization and poll processing write these fields.
// Initialization precedes publication; poll processing holds registry->mutex.
static void conn_poll_sample_rcvbuf(juice_agent_t *agent, socket_t sock) {
\tjuice_udp_poll_diagnostics_t *d = &agent->udp_poll_diagnostics;
\tif (d->rcvbuf_checked)
\t\treturn;
\td->rcvbuf_checked = true;
\tsockopt_t size = 0;
\tsocklen_t len = sizeof(size);
\tconst int saved_errno = sockerrno;
\td->rcvbuf_available = getsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char *)&size, &len) == 0;
\td->rcvbuf_errno = d->rcvbuf_available ? 0 : sockerrno;
\td->rcvbuf_bytes = d->rcvbuf_available ? (int)size : 0;
#ifdef _WIN32
\tWSASetLastError(saved_errno);
#else
\terrno = saved_errno;
#endif
}

static void conn_poll_record_udp(juice_agent_t *agent, uint64_t start, bool start_valid,
    uint64_t gap, bool gap_valid, uint64_t ready_delay, bool ready_valid,
    unsigned int received, uint64_t bytes,
    unsigned int exit_reason) {
\tjuice_udp_poll_diagnostics_t *d = &agent->udp_poll_diagnostics;
\tuint64_t end = 0;
\tconst bool valid = juice_udp_poll_clock(&end) && start_valid && end >= start;
\tconst uint64_t elapsed = valid ? end - start : 0;
\t++d->batches;
\td->bytes += bytes;
\td->clock_available = valid;
\td->last_clock_available = valid;
\td->last_start_mono_us = start_valid ? start : 0;
\td->last_end_mono_us = valid ? end : 0;
\tif (valid) {
\t\t++d->timed_batches;
\t\td->elapsed_total_us += elapsed;
\t\tif (elapsed > d->elapsed_max_us) d->elapsed_max_us = elapsed;
\t} else {
\t\t++d->clock_invalid;
\t}
\tif (gap_valid) {
\t\t++d->service_gaps;
\t\tif (gap > d->service_gap_max_us) d->service_gap_max_us = gap;
\t}
\tif (ready_valid) {
\t\t++d->ready_delays;
\t\tif (ready_delay > d->ready_delay_max_us) d->ready_delay_max_us = ready_delay;
\t}
\td->pending_yield = exit_reason == 1 && valid;
\tunsigned int flags = (received >= 256 ? 1u : 0u) | (valid && elapsed >= 5000 ? 2u : 0u) |
\t    (exit_reason == 1 ? 4u : 0u);
\tif (flags) {
\t\tif (valid) flags |= 8;
\t\tif (gap_valid) flags |= 16;
\t\tif (ready_valid) flags |= 32;
\t\tjuice_udp_poll_event_t *event = &d->events[d->events_total % 16];
\t\t*event = (juice_udp_poll_event_t){++d->events_total, start_valid ? start : 0,
\t\t    elapsed, bytes, gap_valid ? gap : 0, ready_valid ? ready_delay : 0,
\t\t    received, flags, exit_reason};
\t\tif (d->event_count < 16) ++d->event_count;
\t\telse ++d->events_overwritten;
\t}
}

void conn_poll_process_udp(juice_agent_t *agent, struct pollfd *pfd) {''')
    edit('src/conn_poll.c', '\t\tunsigned int received = 0;', '''\t\tunsigned int received = 0;
\t\tuint64_t bytes = 0, start = 0, gap = 0;
\t\tjuice_udp_poll_diagnostics_t *poll_diag = &agent->udp_poll_diagnostics;
\t\tconst bool start_valid = juice_udp_poll_clock(&start);
\t\tconst bool ready_valid = start_valid && conn_impl->poll_ready_valid &&
\t\t    start >= conn_impl->poll_ready_mono_us;
\t\tconst uint64_t ready_delay = ready_valid ? start - conn_impl->poll_ready_mono_us : 0;
\t\tconn_impl->poll_ready_valid = false;
\t\tconst bool gap_valid = poll_diag->pending_yield && poll_diag->last_clock_available &&
\t\t    start_valid && start >= poll_diag->last_end_mono_us;
\t\tif (gap_valid) gap = start - poll_diag->last_end_mono_us;
\t\tunsigned int exit_reason = 0;''')
    edit('src/conn_poll.c', '\t\t\t++received;', '\t\t\t++received;\n\t\t\tbytes += (unsigned int)ret;')
    edit('src/conn_poll.c', '\t\t\tif (agent_conn_recv(agent, buffer, (size_t)ret, &src) != 0) {',
         '\t\t\tif (agent_conn_recv(agent, buffer, (size_t)ret, &src) != 0) {\n\t\t\t\texit_reason = 4;')
    edit('src/conn_poll.c', '\t\tdiag->udp_datagrams += received;', '''\t\tif (exit_reason != 4)
\t\t\texit_reason = ret > 0 ? 1 : ret == -SEINTR ? 2 :
\t\t\t    (ret == -SEAGAIN || ret == -SEWOULDBLOCK) ? 0 : 3;
\t\tconn_poll_record_udp(agent, start, start_valid, gap, gap_valid,
\t\t    ready_delay, ready_valid, received, bytes, exit_reason);
\t\tdiag->udp_datagrams += received;''')
    edit('src/conn_poll.c', '\tmutex_init(&conn_impl->send_mutex, 0);',
         '\tconn_poll_sample_rcvbuf(agent, conn_impl->udp_sock);\n\tmutex_init(&conn_impl->send_mutex, 0);')
    edit('src/conn_poll.c', '\tif (pfd->revents & POLLIN) {\n\t\tchar buffer[BUFFER_SIZE];',
         '\tif (!(pfd->revents & POLLIN)) agent->udp_poll_diagnostics.pending_yield = false;\n\tif (pfd->revents & POLLIN) {\n\t\tchar buffer[BUFFER_SIZE];')


if __name__ == '__main__':
    apply(Path(__file__).resolve().parents[2])
