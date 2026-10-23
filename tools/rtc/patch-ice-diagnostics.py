"""Pinned, idempotent libjuice UDP budget fix and per-agent diagnostics."""
from pathlib import Path

def apply(root):
    rtc = root / 'third_party/rtc/libdatachannel'
    juice = rtc / 'deps/libjuice'

    def replace(path, old, new, previous=None):
        source = path.read_text()
        if new in source:
            return
        if previous is not None and previous in source:
            path.write_text(source.replace(previous, new, 1))
            return
        if ((old == 'namespace rtc::impl {' and 'string IceTransport::iceDiagnostics() const' in source) or
                (old.startswith('bool PeerConnection::getSelectedCandidatePair(') and
                 'string PeerConnection::iceDiagnostics() const' in source)):
            raise SystemExit('Unexpected existing ICE diagnostic method: ' + str(path))
        if source.count(old) != 1:
            raise SystemExit('Unrecognized pinned ICE diagnostic site: ' + str(path))
        path.write_text(source.replace(old, new, 1))

    replace(juice / 'include/juice/juice.h',
            'typedef void (*juice_cb_state_changed_t)', '''// Fork extension: numeric counters and fixed enums only, no endpoint or credential data.
typedef enum juice_failure_reason {
	JUICE_FAILURE_NONE = 0,
	JUICE_FAILURE_UDP_RECV,
	JUICE_FAILURE_UDP_POLL,
	JUICE_FAILURE_CONNECTION,
	JUICE_FAILURE_CONSENT_EXPIRED,
	JUICE_FAILURE_STUN_TIMEOUT,
	JUICE_FAILURE_STUN_SEND,
	JUICE_FAILURE_STUN_RESPONSE,
	JUICE_FAILURE_CONNECTIVITY_TIMEOUT
} juice_failure_reason_t;

typedef struct juice_diagnostics {
	juice_state_t state;
	juice_failure_reason_t failure_reason;
	uint64_t udp_budget_yields;
	uint64_t udp_datagrams;
	unsigned int udp_max_batch;
	int recv_errno;
	unsigned int poll_flags;
	int socket_error;
	bool socket_error_available;
	uint64_t consent_expired;
	uint64_t stun_timeouts;
	uint64_t stun_send_failures;
	uint64_t stun_error_responses;
	unsigned int stun_error_code;
	int stun_send_errno;
	uint64_t connectivity_timeouts;
} juice_diagnostics_t;

// Coherent per-agent snapshot under the connection lock; retained until destruction.
JUICE_EXPORT int juice_get_diagnostics(juice_agent_t *agent, juice_diagnostics_t *out);

typedef void (*juice_cb_state_changed_t)''')
    replace(juice / 'src/agent.h', '\ttimestamp_t next_transmission;',
            '\tjuice_failure_reason_t failure_reason;\n\ttimestamp_t next_transmission;')
    replace(juice / 'src/agent.h', '\tjuice_state_t state;',
            '\tjuice_state_t state;\n\tjuice_diagnostics_t diagnostics; // guarded by conn_lock')
    replace(juice / 'src/juice.c',
            'JUICE_EXPORT juice_state_t juice_get_state(juice_agent_t *agent) { return agent_get_state(agent); }',
            '''JUICE_EXPORT juice_state_t juice_get_state(juice_agent_t *agent) { return agent_get_state(agent); }

JUICE_EXPORT int juice_get_diagnostics(juice_agent_t *agent, juice_diagnostics_t *out) {
	if (!agent || !out)
		return JUICE_ERR_INVALID;
	conn_lock(agent);
	*out = agent->diagnostics;
	out->state = agent->state;
	conn_unlock(agent);
	return JUICE_ERR_SUCCESS;
}''')

    poll = juice / 'src/conn_poll.c'
    source = poll.read_text()
    begin = source.index('void conn_poll_process_udp(juice_agent_t *agent, struct pollfd *pfd) {')
    end = source.index('\nint conn_poll_recv_udp(', begin)
    current = source[begin:end]
    replacement = '''void conn_poll_process_udp(juice_agent_t *agent, struct pollfd *pfd) {
	conn_impl_t *conn_impl = agent->conn_impl;
	juice_diagnostics_t *diag = &agent->diagnostics;

	if (pfd->revents & (POLLERR | POLLNVAL | POLLHUP)) {
		// Snapshot SO_ERROR before recv or callbacks can consume/change the error.
		diag->poll_flags = (unsigned int)(unsigned short)pfd->revents;
		int error = 0;
		socklen_t error_size = sizeof(error);
		diag->socket_error_available =
		    getsockopt(conn_impl->udp_sock, SOL_SOCKET, SO_ERROR, (char *)&error, &error_size) == 0;
		diag->socket_error = diag->socket_error_available ? error : 0;
		diag->failure_reason = JUICE_FAILURE_UDP_POLL;
		JLOG_WARN("UDP socket poll error, flags=%u, socket_error=%d, available=%d",
		          diag->poll_flags, diag->socket_error, diag->socket_error_available);
		agent_conn_fail(agent);
		conn_impl->state = CONN_STATE_FINISHED;
		return;
	}

	if (pfd->revents & POLLIN) {
		char buffer[BUFFER_SIZE];
		addr_record_t src;
		int ret = 0;
		unsigned int received = 0;
		// Remaining datagrams stay readable for the next level-triggered poll.
		for (unsigned int attempts = 0; attempts < 1000; ++attempts) {
			ret = conn_poll_recv_udp(conn_impl->udp_sock, buffer, BUFFER_SIZE, &src);
			if (ret <= 0)
				break;
			++received;
			if (agent_conn_recv(agent, buffer, (size_t)ret, &src) != 0) {
				JLOG_WARN("Agent receive failed");
				conn_impl->state = CONN_STATE_FINISHED;
				break;
			}
		}
		diag->udp_datagrams += received;
		if (received > diag->udp_max_batch)
			diag->udp_max_batch = received;

		if (conn_impl->state == CONN_STATE_FINISHED)
			return;

		if (ret > 0) {
			++diag->udp_budget_yields;
		} else if (ret == -SEAGAIN || ret == -SEWOULDBLOCK || ret == -SEINTR) {
			// EINTR is transient: yield to poll rather than spin or fail ICE.
			JLOG_VERBOSE("No more datagrams to receive or receive interrupted");
		} else {
			diag->recv_errno = ret < 0 ? -ret : 0;
			diag->failure_reason = JUICE_FAILURE_UDP_RECV;
			agent_conn_fail(agent);
			conn_impl->state = CONN_STATE_FINISHED;
			return;
		}
	}

	if ((pfd->revents & POLLIN) || conn_impl->next_timestamp <= current_timestamp()) {
		if (agent_conn_update(agent, &conn_impl->next_timestamp) != 0) {
			JLOG_WARN("Agent update failed");
			conn_impl->state = CONN_STATE_FINISHED;
		}
	}
}
'''
    if current != replacement:
        if 'int left = 1000; // limit for fairness between sockets' not in current:
            raise SystemExit('Unrecognized pinned conn_poll_process_udp')
        poll.write_text(source[:begin] + replacement + source[end:])

    agent = juice / 'src/agent.c'
    replace(agent, 'int agent_conn_fail(juice_agent_t *agent) {\n\tagent_change_state',
            '''int agent_conn_fail(juice_agent_t *agent) {
	if (agent->diagnostics.failure_reason == JUICE_FAILURE_NONE)
		agent->diagnostics.failure_reason = JUICE_FAILURE_CONNECTION;
	agent_change_state''')
    replace(agent, '\t\t\t// Failure sending or end of retransmissions\n',
            '''			// Failure sending or end of retransmissions
			if (entry->retransmissions < 0) {
				++agent->diagnostics.stun_timeouts;
				entry->failure_reason = JUICE_FAILURE_STUN_TIMEOUT;
			} else {
				++agent->diagnostics.stun_send_failures;
				entry->failure_reason = JUICE_FAILURE_STUN_SEND;
			}
''')
    replace(agent, '\t\t\t\tJLOG_INFO("STUN entry %d: Consent expired for candidate pair", i);',
            '''				JLOG_INFO("STUN entry %d: Consent expired for candidate pair", i);
				++agent->diagnostics.consent_expired;
				entry->failure_reason = JUICE_FAILURE_CONSENT_EXPIRED;''')
    replace(agent, '\t\tJLOG_WARN("Lost connectivity");',
            '''		JLOG_WARN("Lost connectivity");
		agent->diagnostics.failure_reason = JUICE_FAILURE_CONNECTION;
		for (int i = 0; i < agent->entries_count; ++i) {
			if (agent->entries[i].pair == nominated_pair &&
			    agent->entries[i].failure_reason != JUICE_FAILURE_NONE) {
				agent->diagnostics.failure_reason = agent->entries[i].failure_reason;
				break;
			}
		}''')
    replace(agent, '\t\t\tJLOG_INFO("Connectivity timer expired");',
            '''			JLOG_INFO("Connectivity timer expired");
			++agent->diagnostics.connectivity_timeouts;
			agent->diagnostics.failure_reason = JUICE_FAILURE_CONNECTIVITY_TIMEOUT;''')
    replace(agent, '\t\t\t\tJLOG_DEBUG("Chandidate pair check failed (unrecoverable error)");',
            '''				JLOG_DEBUG("Chandidate pair check failed (unrecoverable error)");
				++agent->diagnostics.stun_error_responses;
				agent->diagnostics.stun_error_code = msg->error_code;
				entry->failure_reason = JUICE_FAILURE_STUN_RESPONSE;''')
    replace(agent, '\t\t\tJLOG_INFO("STUN server binding failed (unrecoverable error)");',
            '''			JLOG_INFO("STUN server binding failed (unrecoverable error)");
			++agent->diagnostics.stun_error_responses;
			agent->diagnostics.stun_error_code = msg->error_code;
			entry->failure_reason = JUICE_FAILURE_STUN_RESPONSE;''')
    replace(agent, '\t// Direct send\n\tint ret = agent_direct_send(agent, &entry->record, buffer, size, 0);\n\tif (ret < 0) {',
            '''	// Direct send
	int ret = agent_direct_send(agent, &entry->record, buffer, size, 0);
	if (ret < 0) {
		agent->diagnostics.stun_send_errno = -ret;''')

    replace(rtc / 'include/rtc/peerconnection.hpp',
            '\tbool getSelectedCandidatePair(Candidate *local, Candidate *remote);',
            '''	bool getSelectedCandidatePair(Candidate *local, Candidate *remote);
	// Fork extension: sanitized per-transport counters; empty before ICE exists.
	string iceDiagnostics() const;''')
    replace(rtc / 'src/peerconnection.cpp',
            'bool PeerConnection::getSelectedCandidatePair(Candidate *local, Candidate *remote) {',
            '''string PeerConnection::iceDiagnostics() const {
	return impl()->iceDiagnostics();
}

bool PeerConnection::getSelectedCandidatePair(Candidate *local, Candidate *remote) {''')
    replace(rtc / 'src/impl/icetransport.hpp',
            '\tbool getSelectedCandidatePair(Candidate *local, Candidate *remote);',
            '\tbool getSelectedCandidatePair(Candidate *local, Candidate *remote);\n\tstring iceDiagnostics() const;')
    replace(rtc / 'src/impl/icetransport.cpp', 'namespace rtc::impl {',
            '''namespace rtc::impl {

string IceTransport::iceDiagnostics() const {
#if !USE_NICE
	juice_diagnostics_t d{};
	if (juice_get_diagnostics(mAgent.get(), &d) != JUICE_ERR_SUCCESS)
		return {};
	const char *reason = "unknown";
	switch (d.failure_reason) {
	case JUICE_FAILURE_NONE: reason = "none"; break;
	case JUICE_FAILURE_UDP_RECV: reason = "udp-recv"; break;
	case JUICE_FAILURE_UDP_POLL: reason = "udp-poll"; break;
	case JUICE_FAILURE_CONNECTION: reason = "connection"; break;
	case JUICE_FAILURE_CONSENT_EXPIRED: reason = "consent-expired"; break;
	case JUICE_FAILURE_STUN_TIMEOUT: reason = "stun-timeout"; break;
	case JUICE_FAILURE_STUN_SEND: reason = "stun-send"; break;
	case JUICE_FAILURE_STUN_RESPONSE: reason = "stun-response"; break;
	case JUICE_FAILURE_CONNECTIVITY_TIMEOUT: reason = "connectivity-timeout"; break;
	}
	std::ostringstream out;
	out.imbue(std::locale::classic());
	out << "iceState=" << static_cast<unsigned int>(d.state) << " iceReason=" << reason
	    << " udpBudgetYields=" << d.udp_budget_yields << " udpDatagrams=" << d.udp_datagrams
	    << " udpMaxBatch=" << d.udp_max_batch << " recvErrno=" << d.recv_errno
	    << " pollFlags=" << d.poll_flags << " socketError=" << d.socket_error
	    << " socketErrorAvailable=" << unsigned(d.socket_error_available)
	    << " consentExpired=" << d.consent_expired << " stunTimeouts=" << d.stun_timeouts
	    << " stunSendFailures=" << d.stun_send_failures
	    << " stunErrorResponses=" << d.stun_error_responses << " stunErrorCode=" << d.stun_error_code
	    << " stunSendErrno=" << d.stun_send_errno
	    << " connectivityTimeouts=" << d.connectivity_timeouts;
	return out.str();
#else
	return "iceDiagnostics=unavailable";
#endif
}
''')

    replace(poll, """	if (len < 0) {
		if (sockerrno != SEAGAIN && sockerrno != SEWOULDBLOCK)
			JLOG_ERROR("recvfrom failed, errno=%d", sockerrno);

		return -sockerrno;
	}""", """	if (len < 0) {
		const int error = sockerrno; // logging must not clobber the receive error
		if (error != SEAGAIN && error != SEWOULDBLOCK && error != SEINTR)
			JLOG_ERROR("recvfrom failed, errno=%d", error);

		return -error;
	}""")
    replace(agent, '\t\t\t\tJLOG_WARN("Sending keepalive failed");', """				JLOG_WARN("Sending keepalive failed");
				++agent->diagnostics.stun_send_failures;""")
    replace(agent, 'void agent_arm_transmission(juice_agent_t *agent, agent_stun_entry_t *entry, timediff_t delay) {',
            """void agent_arm_transmission(juice_agent_t *agent, agent_stun_entry_t *entry, timediff_t delay) {
	entry->failure_reason = JUICE_FAILURE_NONE;""")

    peer_header = rtc / 'src/impl/peerconnection.hpp'
    peer = rtc / 'src/impl/peerconnection.cpp'
    replace(peer_header, '\tshared_ptr<IceTransport> getIceTransport() const;',
            '\tshared_ptr<IceTransport> getIceTransport() const;\n\tstring iceDiagnostics() const;')
    replace(peer_header, '\tshared_ptr<IceTransport> mIceTransport;',
            """	shared_ptr<IceTransport> mIceTransport;
	mutable std::mutex mIceDiagnosticsMutex;
	mutable string mFinalIceDiagnostics;""", previous="""	shared_ptr<IceTransport> mIceTransport;
	mutable std::mutex mIceDiagnosticsMutex;
	string mFinalIceDiagnostics;""")
    replace(peer, 'shared_ptr<DtlsTransport> PeerConnection::getDtlsTransport() const {',
            """string PeerConnection::iceDiagnostics() const {
	if (auto ice = getIceTransport()) {
		// The caller chooses where this potentially blocking ICE read runs.
		// Never hold the cache mutex while acquiring the ICE connection lock.
		auto snapshot = ice->iceDiagnostics();
		std::lock_guard lock(mIceDiagnosticsMutex);
		if (!snapshot.empty()) mFinalIceDiagnostics = snapshot;
		return snapshot;
	}
	std::lock_guard lock(mIceDiagnosticsMutex);
	return mFinalIceDiagnostics;
}

shared_ptr<DtlsTransport> PeerConnection::getDtlsTransport() const {""", previous="""string PeerConnection::iceDiagnostics() const {
	if (auto ice = getIceTransport())
		return ice->iceDiagnostics();
	std::lock_guard lock(mIceDiagnosticsMutex);
	return mFinalIceDiagnostics;
}

shared_ptr<DtlsTransport> PeerConnection::getDtlsTransport() const {""")
    replace(peer, '\t// Pass the pointers to a thread, allowing to terminate a transport from its own thread',
            """	// Retain the final per-peer snapshot before the ICE pointer disappears.
	// The last explicit iceDiagnostics() sample is cached; teardown never queries ICE.

	// Pass the pointers to a thread, allowing to terminate a transport from its own thread""", previous="""	// Retain the final per-peer snapshot before the ICE pointer disappears.
	// Snapshot on this processor thread, outside libjuice callbacks and without
	// holding the cache lock while acquiring the libjuice connection lock.
	try {
		if (auto ice = getIceTransport()) {
			auto snapshot = ice->iceDiagnostics();
			std::lock_guard lock(mIceDiagnosticsMutex);
			mFinalIceDiagnostics = std::move(snapshot);
		}
	} catch (...) {
		// Diagnostics must never prevent transport teardown.
		PLOG_WARNING << "ICE diagnostic snapshot unavailable at teardown";
	}

	// Pass the pointers to a thread, allowing to terminate a transport from its own thread""")

if __name__ == '__main__':
    apply(Path(__file__).resolve().parents[2])

