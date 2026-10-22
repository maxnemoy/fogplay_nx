// Compile the production poll path directly, with deterministic receive supply.
// Unused registry/TCP sections are discarded; no external sessions are created.
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include "../../third_party/rtc/libdatachannel/deps/libjuice/src/conn_poll.c"
#include "../../third_party/rtc/libdatachannel/deps/libjuice/src/juice.c"

static unsigned int pending, delivered, updates, failures;
static int injected_errno;
static bool real_socket;
static int lock_depth;

int udp_recvfrom(socket_t fd, char *buffer, size_t size, addr_record_t *src) {
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
	return 0;
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

int main(void) {
	juice_set_log_level(JUICE_LOG_LEVEL_NONE);
	batch(999, 0, 1);
	batch(1000, 1, 1);
	batch(1001, 1, 2);
	batch(3501, 3, 4);
	interrupted();
	receive_error();
	poll_error();
	assert(juice_get_diagnostics(NULL, NULL) == JUICE_ERR_INVALID);
	puts("UDP fairness and per-agent diagnostics regression PASS");
	return 0;
}

