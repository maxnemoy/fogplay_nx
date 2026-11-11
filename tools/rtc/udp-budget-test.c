// Compile the production poll path directly, with deterministic receive supply.
// Unused registry/TCP sections are discarded; no external sessions are created.
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#define JUICE_UDP_POLL_TEST_CLOCK 1
#include "../../third_party/rtc/libdatachannel/deps/libjuice/src/conn_poll.c"
#include "../../third_party/rtc/libdatachannel/deps/libjuice/src/juice.c"

static unsigned int pending, delivered, updates, failures;
static int injected_errno;
static bool real_socket;
static int lock_depth;
static uint64_t clock_us;
static unsigned int clock_calls;
static bool clock_fail, clock_backwards, callback_fail;
static unsigned int clock_fail_call, clock_backwards_call;
static uint64_t clock_step;
static uint64_t recv_work_us, callback_work_us;

bool juice_udp_poll_clock(uint64_t *out) {
	++clock_calls;
	*out = clock_us;
	clock_us += clock_step;
	errno = EDOM; // clock success/failure must never clobber socket errno
	if ((clock_backwards && clock_calls == 2) || clock_calls == clock_backwards_call) *out = 1;
	return !clock_fail && clock_calls != clock_fail_call;
}

int udp_recvfrom(socket_t fd, char *buffer, size_t size, addr_record_t *src) {
	clock_us += recv_work_us;
	if (real_socket) {
		src->len = sizeof(src->addr);
		return (int)recvfrom(fd, buffer, size, 0, (struct sockaddr *)&src->addr, &src->len);
	}
	if (injected_errno) {
		errno = injected_errno;
		injected_errno = 0;
		return -1;
	}
	if (!pending) {
		errno = EAGAIN;
		return -1;
	}
	--pending;
	memset(src, 0, sizeof(*src));
	((struct sockaddr *)&src->addr)->sa_family = AF_INET;
	src->len = sizeof(struct sockaddr_in);
	buffer[0] = 'x';
	return 1;
}

int agent_conn_recv(juice_agent_t *agent, char *buf, size_t len, const addr_record_t *src) {
	(void)agent; (void)buf; (void)src;
	assert(len == 1);
	++delivered;
	clock_us += callback_work_us;
	return callback_fail ? -1 : 0;
}
int agent_conn_update(juice_agent_t *agent, timestamp_t *next) {
	(void)agent;
	++updates;
	*next = current_timestamp() + 60000;
	return 0;
}
int agent_conn_fail(juice_agent_t *agent) {
	++failures;
	agent->state = JUICE_STATE_FAILED;
	return 0;
}
void conn_lock(juice_agent_t *agent) { (void)agent; assert(lock_depth++ == 0); }
void conn_unlock(juice_agent_t *agent) { (void)agent; assert(--lock_depth == 0); }

static void initialize(juice_agent_t *agent, conn_impl_t *conn) {
	memset(agent, 0, sizeof(*agent));
	memset(conn, 0, sizeof(*conn));
	conn->state = CONN_STATE_READY;
	conn->udp_sock = INVALID_SOCKET;
	conn->next_timestamp = current_timestamp() + 60000;
	agent->conn_impl = conn;
	agent->state = JUICE_STATE_COMPLETED;
	pending = delivered = updates = failures = 0;
	injected_errno = 0;
	real_socket = false;
	clock_us = 10000;
	clock_calls = 0;
	clock_fail = clock_backwards = callback_fail = false;
	clock_fail_call = clock_backwards_call = 0;
	clock_step = 100;
	recv_work_us = callback_work_us = 0;
}

static void batch(unsigned int total, unsigned int yields, unsigned int batches) {
	juice_agent_t agent;
	conn_impl_t conn;
	initialize(&agent, &conn);
	pending = total;
	struct pollfd pfd = {.fd = INVALID_SOCKET, .revents = POLLIN};
	for (unsigned int i = 0; i < batches; ++i)
		conn_poll_process_udp(&agent, &pfd);
	assert(pending == 0 && delivered == total);
	assert(agent.state == JUICE_STATE_COMPLETED && conn.state == CONN_STATE_READY);
	assert(failures == 0 && updates == batches);
	assert(agent.diagnostics.udp_budget_yields == yields);
	assert(agent.diagnostics.udp_datagrams == total);
	assert(agent.diagnostics.udp_max_batch == (total < 1000 ? total : 1000));
	assert(agent.udp_poll_diagnostics.bytes == total);
	assert(agent.udp_poll_diagnostics.batches == batches);
	assert(agent.udp_poll_diagnostics.timed_batches == batches);
	const unsigned int sampled_received = total < 1000 ? total : 1000;
	const unsigned int sampled_terminal = total < 1000 ? 1 : 0;
	const unsigned int phase_clocks = 3 * sampled_received + 2 * sampled_terminal;
	assert(agent.udp_poll_diagnostics.elapsed_total_us == (batches + phase_clocks) * 100);
	assert(agent.udp_poll_diagnostics.service_gaps == (batches > 1 ? batches - 1 : 0));
	assert(clock_calls == batches * 2 + phase_clocks);
	assert(phase_clocks <= 3000); // exact worst case: 1000 successful receives
	assert(agent.udp_poll_diagnostics.phase_sampled_batches == 1);
	assert(agent.udp_poll_diagnostics.phase_timed_batches == 1);
	assert(agent.udp_poll_diagnostics.phase_sampled_receive_calls == sampled_received + sampled_terminal);
	juice_diagnostics_t snapshot;
	assert(juice_get_diagnostics(&agent, &snapshot) == JUICE_ERR_SUCCESS);
	assert(lock_depth == 0 && snapshot.state == JUICE_STATE_COMPLETED);
	assert(snapshot.failure_reason == JUICE_FAILURE_NONE);
	assert(snapshot.udp_datagrams == total);
	// A separate agent cannot inherit counters or terminal causes.
	juice_agent_t other;
	conn_impl_t other_conn;
	initialize(&other, &other_conn);
	assert(juice_get_diagnostics(&other, &snapshot) == JUICE_ERR_SUCCESS);
	assert(snapshot.udp_datagrams == 0 && snapshot.udp_budget_yields == 0);
	printf("UDP batch %u: yields=%u polls=%u PASS\n", total, yields, batches);
}

static void interrupted(void) {
	juice_agent_t agent;
	conn_impl_t conn;
	initialize(&agent, &conn);
	injected_errno = EINTR;
	struct pollfd pfd = {.fd = INVALID_SOCKET, .revents = POLLIN};
	conn_poll_process_udp(&agent, &pfd);
	assert(failures == 0 && updates == 1 && conn.state == CONN_STATE_READY);
	assert(agent.diagnostics.failure_reason == JUICE_FAILURE_NONE);
	pending = 1;
	conn_poll_process_udp(&agent, &pfd);
	assert(delivered == 1 && failures == 0);
	puts("UDP EINTR yields and resumes PASS");
}

static void receive_error(void) {
	juice_agent_t agent;
	conn_impl_t conn;
	initialize(&agent, &conn);
	real_socket = true;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	assert(fd >= 0);
	assert(close(fd) == 0);
	conn.udp_sock = fd;
	struct pollfd pfd = {.fd = fd, .revents = POLLIN};
	conn_poll_process_udp(&agent, &pfd);
	assert(failures == 1 && updates == 0 && conn.state == CONN_STATE_FINISHED);
	juice_diagnostics_t snapshot;
	assert(juice_get_diagnostics(&agent, &snapshot) == 0);
	assert(snapshot.state == JUICE_STATE_FAILED);
	assert(snapshot.failure_reason == JUICE_FAILURE_UDP_RECV && snapshot.recv_errno == EBADF);
	puts("Real recvfrom EBADF remains fatal and snapshot survives failure PASS");
}

static void poll_error(void) {
	juice_agent_t agent;
	conn_impl_t conn;
	initialize(&agent, &conn);
	int target = socket(AF_INET, SOCK_DGRAM, 0);
	assert(target >= 0);
	struct sockaddr_in local = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
	assert(bind(target, (struct sockaddr *)&local, sizeof(local)) == 0);
	socklen_t len = sizeof(local);
	assert(getsockname(target, (struct sockaddr *)&local, &len) == 0);
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	assert(fd >= 0);
	assert(connect(fd, (struct sockaddr *)&local, sizeof(local)) == 0);
	assert(close(target) == 0);
	assert(send(fd, "x", 1, 0) == 1);
	struct pollfd pfd = {.fd = fd, .events = POLLIN};
	assert(poll(&pfd, 1, 1000) == 1 && (pfd.revents & POLLERR));
	conn.udp_sock = fd;
	// Even POLLIN|POLLERR must return after the fatal error.
	pfd.revents |= POLLIN;
	conn_poll_process_udp(&agent, &pfd);
	assert(failures == 1 && updates == 0 && delivered == 0);
	assert(conn.state == CONN_STATE_FINISHED);
	assert(agent.diagnostics.failure_reason == JUICE_FAILURE_UDP_POLL);
	assert(agent.diagnostics.poll_flags & POLLERR);
	assert(agent.diagnostics.socket_error_available);
	assert(agent.diagnostics.socket_error == ECONNREFUSED);
	assert(close(fd) == 0);
	puts("Real poll POLLERR / SO_ERROR ECONNREFUSED remains fatal PASS");
}

static void poll_evidence(void) {
	juice_agent_t agent;
	conn_impl_t conn;
	initialize(&agent, &conn);
	struct pollfd pfd = {.fd = INVALID_SOCKET, .revents = POLLIN};
	for (unsigned int i = 0; i < 20; ++i) {
		pending = 1000;
		conn.poll_ready_valid = true;
		conn.poll_ready_mono_us = clock_us - 50;
		conn_poll_process_udp(&agent, &pfd);
	}
	juice_diagnostics_t summary;
	juice_udp_poll_diagnostics_t snapshot;
	assert(juice_get_udp_poll_diagnostics(&agent, &summary, &snapshot) == 0);
	assert(lock_depth == 0 && summary.udp_datagrams == 20000);
	assert(snapshot.events_total == 20 && snapshot.events_overwritten == 4);
	assert(snapshot.event_count == 16 && snapshot.ready_delays == 20);
	assert(snapshot.ready_delay_max_us == 50 && snapshot.sample_clock_available);
	assert(snapshot.sample_mono_us == clock_us - 100);
	for (unsigned int i = 0; i < 16; ++i) {
		assert(snapshot.events[i].sequence == i + 5);
		const bool sampled = i + 4 == 16;
		assert(snapshot.events[i].flags == (1|4|8|16|32|(sampled ? 2|64|128 : 0)));
		assert(snapshot.events[i].ready_delay_us == 50);
		assert(snapshot.events[i].bytes == 1000);
		assert(snapshot.events[i].elapsed_us == (sampled ? 300100 : 100));
	}
	// Non-readable service clears the after-yield marker; no idle inference.
	pfd.revents = 0;
	conn_poll_process_udp(&agent, &pfd);
	assert(!agent.udp_poll_diagnostics.pending_yield);
	const uint64_t gaps = agent.udp_poll_diagnostics.service_gaps;
	pfd.revents = POLLIN;
	pending = 1;
	clock_us += 1000000;
	conn_poll_process_udp(&agent, &pfd);
	assert(agent.udp_poll_diagnostics.service_gaps == gaps);
	assert(agent.udp_poll_diagnostics.events_total == 20);
	initialize(&agent, &conn);
	clock_fail = true;
	pending = 1000;
	conn_poll_process_udp(&agent, &pfd);
	assert(agent.udp_poll_diagnostics.clock_invalid == 1);
	assert(agent.udp_poll_diagnostics.timed_batches == 0);
	assert(agent.udp_poll_diagnostics.events[0].flags == (1|4|64));
	assert(!agent.udp_poll_diagnostics.pending_yield);
	initialize(&agent, &conn);
	clock_backwards = true;
	pending = 256;
	conn_poll_process_udp(&agent, &pfd);
	assert(agent.udp_poll_diagnostics.clock_invalid == 0);
	assert(agent.udp_poll_diagnostics.phase_invalid_batches == 1);
	assert(agent.udp_poll_diagnostics.events[0].phase_recv_us == 0);
	initialize(&agent, &conn);
	callback_fail = true;
	pending = 1000;
	conn_poll_process_udp(&agent, &pfd);
	assert(agent.udp_poll_diagnostics.batches == 1 && agent.udp_poll_diagnostics.bytes == 1);
	assert(agent.diagnostics.udp_budget_yields == 0 && conn.state == CONN_STATE_FINISHED);
	puts("UDP clock validity, filtered bounded ring, ready delay, idle and callback failure PASS");
}

static void receive_buffer(void) {
	juice_agent_t agent;
	conn_impl_t conn;
	initialize(&agent, &conn);
	assert(!agent.udp_poll_diagnostics.rcvbuf_checked);
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	assert(fd >= 0);
	errno = EINTR;
	conn_poll_sample_rcvbuf(&agent, fd);
	assert(errno == EINTR);
	assert(agent.udp_poll_diagnostics.rcvbuf_checked);
	assert(agent.udp_poll_diagnostics.rcvbuf_available);
	assert(agent.udp_poll_diagnostics.rcvbuf_bytes > 0);
	int observed = agent.udp_poll_diagnostics.rcvbuf_bytes;
	close(fd);
	conn_poll_sample_rcvbuf(&agent, fd);
	assert(agent.udp_poll_diagnostics.rcvbuf_bytes == observed);
	initialize(&agent, &conn);
	conn_poll_sample_rcvbuf(&agent, INVALID_SOCKET);
	assert(agent.udp_poll_diagnostics.rcvbuf_checked);
	assert(!agent.udp_poll_diagnostics.rcvbuf_available);
	assert(agent.udp_poll_diagnostics.rcvbuf_bytes == 0);
	assert(agent.udp_poll_diagnostics.rcvbuf_errno == EBADF);
	puts("Real SO_RCVBUF success/failure/once and errno preservation PASS");
}


static void phase_evidence(void) {
    juice_agent_t agent;
    conn_impl_t conn;
    struct pollfd pfd = {.fd = INVALID_SOCKET, .revents = POLLIN};
    initialize(&agent, &conn);
    clock_step = 0; recv_work_us = 10; callback_work_us = 20;
    pending = 3; errno = ENOENT;
    conn_poll_process_udp(&agent, &pfd);
    juice_udp_poll_diagnostics_t *d = &agent.udp_poll_diagnostics;
    assert(errno == EAGAIN); // terminal socket errno survives successful clocks
    assert(d->phase_sample_every == 16 && d->phase_sampled_batches == 1);
    assert(d->phase_timed_batches == 1 && d->phase_invalid_batches == 0);
    assert(d->phase_sampled_datagrams == 3 && d->phase_timed_datagrams == 3);
    assert(d->phase_sampled_receive_calls == 4 && d->phase_timed_receive_calls == 4);
    assert(d->phase_recv_total_us == 40 && d->phase_callback_total_us == 60);
    assert(d->phase_recv_batch_max_us == 40 && d->phase_callback_batch_max_us == 60);
    assert(d->elapsed_total_us == 100 && clock_calls == 13);
    // Unsampled batches have only two boundaries and do not contaminate phase sums.
    pending = 3;
    const unsigned int before = clock_calls;
    conn_poll_process_udp(&agent, &pfd);
    assert(clock_calls == before + 2 && d->phase_sampled_batches == 1);
    assert(d->phase_recv_total_us == 40 && d->phase_callback_total_us == 60);
    // Complete sampling period: samples occur atbatch0,16; no reset or biased trigger.
    for(unsigned int i=2;i<17;++i) { pending=1;conn_poll_process_udp(&agent,&pfd); }
    assert(d->batches==17 && d->phase_sampled_batches==2 && d->phase_timed_batches==2);
    assert(d->phase_sampled_datagrams==4 && d->phase_sampled_receive_calls==6);
    assert(d->phase_recv_total_us==60 && d->phase_callback_total_us==80);
    // Failed and backwards packet clocks invalidate phases, independent of valid boundaries.
    for(unsigned int bad=0;bad<2;++bad) {
        initialize(&agent,&conn);clock_step=0;recv_work_us=30000;callback_work_us=20;
        if(bad) clock_backwards_call=3; else clock_fail_call=3;
        pending=1;conn_poll_process_udp(&agent,&pfd);
        assert(errno==EAGAIN && d->timed_batches==1 && d->clock_invalid==0);
        assert(d->phase_sampled_batches==1 && d->phase_invalid_batches==1);
        assert(d->phase_sampled_receive_calls==2 && d->phase_timed_receive_calls==0);
        assert(d->phase_recv_total_us==0 && d->phase_callback_total_us==0);
        assert(d->events[0].flags & 64);assert(!(d->events[0].flags & 128));
        assert(d->events[0].phase_recv_us==0 && d->events[0].phase_callback_us==0);
    }
    // Invalid final boundary invalidates both elapsed and full phase coverage.
    initialize(&agent,&conn);clock_step=0;pending=1;clock_fail_call=7;
    conn_poll_process_udp(&agent,&pfd);
    assert(d->clock_invalid==1 && d->phase_invalid_batches==1);
    // EINTR terminal receive and callback failure preserve policy/coverage/errno.
    initialize(&agent,&conn);clock_step=0;injected_errno=EINTR;
    conn_poll_process_udp(&agent,&pfd);
    assert(errno==EINTR && failures==0 && updates==1);
    assert(d->phase_sampled_receive_calls==1 && d->phase_timed_receive_calls==1);
    initialize(&agent,&conn);clock_step=0;callback_fail=true;pending=1;
    conn_poll_process_udp(&agent,&pfd);
    assert(delivered==1 && d->phase_sampled_receive_calls==1 && d->phase_timed_datagrams==1);
    assert(d->events[0].exit_reason==4 && (d->events[0].flags&128));
    puts("UDP phases: exact coverage, sampling, terminal errors, invalid clocks and errno PASS");
}

static void batch_boundary_clock_evidence(void) {
    juice_agent_t agent;
    conn_impl_t conn;
    struct pollfd pfd = {.fd = INVALID_SOCKET, .revents = POLLIN};
    initialize(&agent, &conn);
    pending = 1;
    conn_poll_process_udp(&agent, &pfd); // Valid sampled batch 0 seeds both aggregates.
    juice_udp_poll_diagnostics_t *d = &agent.udp_poll_diagnostics;
    assert(d->batches == 1 && d->timed_batches == 1 && d->phase_timed_batches == 1);
    const uint64_t elapsed_total = d->elapsed_total_us, elapsed_max = d->elapsed_max_us;
    const uint64_t recv_total = d->phase_recv_total_us, callback_total = d->phase_callback_total_us;
    const uint64_t phase_receive_calls = d->phase_sampled_receive_calls;
    assert(elapsed_total > 0 && elapsed_max > 0);
    const unsigned int before = clock_calls;
    // Batch 1 is unsampled. Its second clock is the whole-batch END, not a phase.
    clock_backwards_call = before + 2;
    pending = 256; // Force a detail event even though elapsed becomes unavailable.
    conn_poll_process_udp(&agent, &pfd);
    assert(clock_calls == before + 2 && d->batches == 2);
    assert(d->clock_invalid == 1 && d->timed_batches == 1);
    assert(!d->clock_available && !d->last_clock_available && !d->pending_yield);
    assert(d->last_start_mono_us > 1 && d->last_end_mono_us == 0);
    assert(d->elapsed_total_us == elapsed_total && d->elapsed_max_us == elapsed_max);
    assert(d->phase_sampled_batches == 1 && d->phase_timed_batches == 1);
    assert(d->phase_invalid_batches == 0 && d->phase_sampled_datagrams == 1);
    assert(d->phase_sampled_receive_calls == phase_receive_calls);
    assert(d->phase_recv_total_us == recv_total && d->phase_callback_total_us == callback_total);
    assert(d->event_count == 1 && d->events[0].flags == 1); // Large, no valid clock/phase flags.
    assert(d->events[0].elapsed_us == 0 && d->events[0].phase_recv_us == 0);
    assert(d->events[0].phase_callback_us == 0);

    // Also reject a backwards FINAL boundary after otherwise-valid sampled phases.
    initialize(&agent, &conn);
    clock_step = 0;
    recv_work_us = 10;
    callback_work_us = 20;
    pending = 256;
    clock_backwards_call = 3 * 256 + 4; // Start + 3/packet + 2 terminal + END.
    conn_poll_process_udp(&agent, &pfd);
    assert(clock_calls == 3 * 256 + 4);
    assert(d->clock_invalid == 1 && d->timed_batches == 0);
    assert(d->elapsed_total_us == 0 && d->elapsed_max_us == 0);
    assert(d->phase_sampled_batches == 1 && d->phase_invalid_batches == 1);
    assert(d->phase_timed_batches == 0 && d->phase_timed_datagrams == 0);
    assert(d->phase_sampled_receive_calls == 257 && d->phase_timed_receive_calls == 0);
    assert(d->phase_recv_total_us == 0 && d->phase_callback_total_us == 0);
    assert(d->events[0].flags == (1 | 64)); // Sample selected, both timing domains invalid.
    assert(d->events[0].elapsed_us == 0 && d->events[0].phase_recv_us == 0);
    assert(d->events[0].phase_callback_us == 0);
    puts("UDP backwards final batch boundary: unsampled aggregate isolation and sampled invalidation PASS");
}

static void detail_filter(void) {
    juice_agent_t agent;
    conn_impl_t conn;
    struct pollfd pfd={.fd=INVALID_SOCKET,.revents=POLLIN};
    initialize(&agent,&conn);clock_step=0;
    recv_work_us=3000;pending=1;
    conn_poll_process_udp(&agent,&pfd); //6ms, old criterion only
    assert(agent.udp_poll_diagnostics.legacy_eligible_batches==1);
    assert(agent.udp_poll_diagnostics.events_total==0);
    recv_work_us=25000;pending=1;
    conn_poll_process_udp(&agent,&pfd); //50ms detail inclusive
    assert(agent.udp_poll_diagnostics.legacy_eligible_batches==2);
    assert(agent.udp_poll_diagnostics.events_total==1);
    assert(agent.udp_poll_diagnostics.events[0].elapsed_us==50000);
    assert(!(agent.udp_poll_diagnostics.events[0].flags&64));
    recv_work_us=0;pending=256;
    conn_poll_process_udp(&agent,&pfd); //large retained even fast
    assert(agent.udp_poll_diagnostics.events_total==2);
    assert(agent.udp_poll_diagnostics.events[1].flags&1);
    initialize(&agent,&conn);clock_step=0;pending=1;injected_errno=EIO;
    conn_poll_process_udp(&agent,&pfd);
    assert(agent.udp_poll_diagnostics.events_total==1);
    assert(agent.udp_poll_diagnostics.legacy_eligible_batches==0);
    assert(agent.udp_poll_diagnostics.events[0].exit_reason==3);
    puts("UDP50ms detail filter, legacyeligible count, fastlarge/error inclusion PASS");
}

int main(void) {
	juice_set_log_level(JUICE_LOG_LEVEL_NONE);
	batch(999, 0, 1);
	batch(1000, 1, 1);
	batch(1001, 1, 2);
	batch(3501, 3, 4);
	interrupted();
	receive_error();
	poll_error();
	poll_evidence();
	receive_buffer();
	phase_evidence();
	batch_boundary_clock_evidence();
	detail_filter();
	assert(juice_get_diagnostics(NULL, NULL) == JUICE_ERR_INVALID);
	assert(juice_get_udp_poll_diagnostics(NULL, NULL, NULL) == JUICE_ERR_INVALID);
	puts("UDP fairness and per-agent diagnostics regression PASS");
	return 0;
}

