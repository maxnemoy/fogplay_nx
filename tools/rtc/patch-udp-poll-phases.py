"""Sampled wall-time phases and sparse useful detail over the approved UDP recipe.
No scheduling, receive budget/order, socket setup or callback policy changes.
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
            raise RuntimeError('Unrecognized UDP phase site: ' + str(path))
        path.write_text(text.replace(before, after, 1))

    edit('include/juice/juice.h', '\tunsigned int datagrams, flags, exit_reason;\n} juice_udp_poll_event_t;',
         '\tunsigned int datagrams, flags, exit_reason;\n\tuint64_t phase_recv_us, phase_callback_us;\n} juice_udp_poll_event_t;')
    edit('include/juice/juice.h', '\tuint64_t events_total, events_overwritten;', '''\t// Legacy >=5ms ORlarge/budget eligible count; detail now>=50ms ORlarge/budget/error.
\tuint64_t legacy_eligible_batches;
\tunsigned int phase_sample_every;
\tuint64_t phase_sampled_batches, phase_timed_batches, phase_invalid_batches;
\tuint64_t phase_sampled_datagrams, phase_timed_datagrams;
\tuint64_t phase_sampled_receive_calls, phase_timed_receive_calls;
\tuint64_t phase_recv_total_us, phase_recv_batch_max_us;
\tuint64_t phase_callback_total_us, phase_callback_batch_max_us;
\tuint64_t events_total, events_overwritten;''')
    edit('src/udp_poll_clock.h', '#include <stdint.h>', '#include <stdint.h>\n#include <errno.h>\n#ifdef _WIN32\n#include <winsock2.h>\n#endif')
    edit('src/udp_poll_clock.h', '\n#endif\n#endif\n', '''
#endif
// Diagnostics must not modify the caller's receive/socket error, including
// failed test clocks or platforms where clock_gettime can set errno.
static inline bool juice_udp_poll_clock_preserving_errno(uint64_t *out) {
#ifdef _WIN32
\tconst int saved = WSAGetLastError();
#else
\tconst int saved = errno;
#endif
\tconst bool valid = juice_udp_poll_clock(out);
#ifdef _WIN32
\tWSASetLastError(saved);
#else
\terrno = saved;
#endif
\treturn valid;
}
#endif
''')
    # Existing boundaries and the cold query get the same non-invasive helper.
    for name in ('src/conn_poll.c', 'src/juice.c'):
        path = juice / name
        path.write_text(path.read_text().replace('juice_udp_poll_clock(', 'juice_udp_poll_clock_preserving_errno('))
    edit('src/conn_poll.c', 'static void conn_poll_record_udp(', '''typedef struct udp_poll_phase {
\tbool sampled, valid;
\tuint64_t last, recv_us, callback_us, receive_calls;
} udp_poll_phase_t;

// Only selected complete batches use packet clocks: 3 per positive receive,
// 2 for a terminal receive. At most3000 extra reads for the1000-receive budget.
static uint64_t conn_poll_phase_clock(udp_poll_phase_t *phase) {
\tuint64_t now = 0;
\tif (!juice_udp_poll_clock_preserving_errno(&now) || now < phase->last)
\t\tphase->valid = false;
\tphase->last = now;
\treturn now;
}

static void conn_poll_record_udp(''')
    edit('src/conn_poll.c', '    unsigned int exit_reason) {\n\tjuice_udp_poll_diagnostics_t *d',
         '    unsigned int exit_reason, udp_poll_phase_t *phase) {\n\tjuice_udp_poll_diagnostics_t *d')
    edit('src/conn_poll.c', '\td->pending_yield = exit_reason == 1 && valid;', '''\td->phase_sample_every = 16;
\tif (phase->sampled) {
\t\t++d->phase_sampled_batches;
\t\td->phase_sampled_datagrams += received;
\t\td->phase_sampled_receive_calls += phase->receive_calls;
\t\t// A phase failure does not invalidate overall timing. Valid phases require
\t\t// valid whole-batch boundaries and monotonic intermediate clocks.
\t\tphase->valid = phase->valid && valid && phase->last <= end &&
\t\t    phase->recv_us <= elapsed && phase->callback_us <= elapsed - phase->recv_us;
\t\tif (phase->valid) {
\t\t\t++d->phase_timed_batches;
\t\t\td->phase_timed_datagrams += received;
\t\t\td->phase_timed_receive_calls += phase->receive_calls;
\t\t\td->phase_recv_total_us += phase->recv_us;
\t\t\td->phase_callback_total_us += phase->callback_us;
\t\t\tif (phase->recv_us > d->phase_recv_batch_max_us)
\t\t\t\td->phase_recv_batch_max_us = phase->recv_us;
\t\t\tif (phase->callback_us > d->phase_callback_batch_max_us)
\t\t\t\td->phase_callback_batch_max_us = phase->callback_us;
\t\t} else ++d->phase_invalid_batches;
\t}
\td->pending_yield = exit_reason == 1 && valid;''')
    edit('src/conn_poll.c', '\tif (flags) {\n\t\tif (valid) flags |= 8;', '''\tif (flags) ++d->legacy_eligible_batches;
\tif (received >= 256 || (valid && elapsed >= 50000) ||
\t    exit_reason == 1 || exit_reason == 3 || exit_reason == 4) {
\t\tif (phase->sampled) flags |= 64;
\t\tif (phase->sampled && phase->valid) flags |= 128;
\t\tif (valid) flags |= 8;''')
    edit('src/conn_poll.c', '\t\t    received, flags, exit_reason};',
         '\t\t    received, flags, exit_reason,\n\t\t    phase->sampled && phase->valid ? phase->recv_us : 0,\n\t\t    phase->sampled && phase->valid ? phase->callback_us : 0};')
    edit('src/conn_poll.c', '\t\tunsigned int exit_reason = 0;', '''\t\tunsigned int exit_reason = 0;
\t\tudp_poll_phase_t phase = {.sampled = poll_diag->batches % 16 == 0,
\t\t    .valid = start_valid, .last = start};''')
    edit('src/conn_poll.c', '\t\t\tret = conn_poll_recv_udp(conn_impl->udp_sock, buffer, BUFFER_SIZE, &src);', '''\t\t\tuint64_t phase_before = 0, phase_received = 0;
\t\t\tif (phase.sampled) {
\t\t\t\t++phase.receive_calls;
\t\t\t\tphase_before = conn_poll_phase_clock(&phase);
\t\t\t}
\t\t\tret = conn_poll_recv_udp(conn_impl->udp_sock, buffer, BUFFER_SIZE, &src);
\t\t\tif (phase.sampled) {
\t\t\t\tphase_received = conn_poll_phase_clock(&phase);
\t\t\t\tif (phase.valid) phase.recv_us += phase_received - phase_before;
\t\t\t}''')
    edit('src/conn_poll.c', '\t\t\tif (agent_conn_recv(agent, buffer, (size_t)ret, &src) != 0) {', '''\t\t\tconst int callback_result = agent_conn_recv(agent, buffer, (size_t)ret, &src);
\t\t\tif (phase.sampled) {
\t\t\t\tconst uint64_t phase_done = conn_poll_phase_clock(&phase);
\t\t\t\tif (phase.valid) phase.callback_us += phase_done - phase_received;
\t\t\t}
\t\t\tif (callback_result != 0) {''')
    edit('src/conn_poll.c', '\t\t    ready_delay, ready_valid, received, bytes, exit_reason);',
         '\t\t    ready_delay, ready_valid, received, bytes, exit_reason, &phase);')


if __name__ == '__main__':
    apply(Path(__file__).resolve().parents[2])
