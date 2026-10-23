"""Numeric RTP/RTCP callback and SRTP reverse-send timings (pinned recipe)."""
from pathlib import Path

FIELDS = '''
    // Completed callbacks split by successfully decrypted datagram type.
    // Their sum equals recvMediaCallback*; totals include waits/preemption.
    uint64_t recvRtpCallbackCount = 0, recvRtpCallbackTotalUs = 0, recvRtpCallbackMaxUs = 0;
    uint64_t recvRtcpCallbackCount = 0, recvRtcpCallbackTotalUs = 0, recvRtcpCallbackMaxUs = 0;
    // sendMedia's distinct SRTP serialization lock, not mSslMutex. All calls,
    // including pre-key/null rejections; hold includes protect and lower send.
    uint64_t sendSrtpLockCount = 0, sendSrtpLockWaitTotalUs = 0, sendSrtpLockWaitMaxUs = 0;
    uint64_t sendSrtpLockHoldTotalUs = 0, sendSrtpLockHoldMaxUs = 0;
    uint64_t sendSrtpRejected = 0, sendSrtpFailed = 0;
    // Valid post-key send attempts, including crypto/transport failures.
    uint64_t sendRtpCount = 0, sendRtcpCount = 0;
    uint64_t sendRtpProtectCount = 0, sendRtpProtectTotalUs = 0, sendRtpProtectMaxUs = 0;
    uint64_t sendRtcpProtectCount = 0, sendRtcpProtectTotalUs = 0, sendRtcpProtectMaxUs = 0;
    // Lower send includes ICE serialization, DSCP/socket work and UDP. Does
    // not split them or include SRTP protect/serialization-lock wait.
    uint64_t sendRtpLowerCount = 0, sendRtpLowerTotalUs = 0, sendRtpLowerMaxUs = 0;
    uint64_t sendRtcpLowerCount = 0, sendRtcpLowerTotalUs = 0, sendRtcpLowerMaxUs = 0;
'''
NAMES = [part.strip().split(' =')[0] for line in FIELDS.splitlines()
         if line.strip().startswith('uint64_t ') for part in line.strip()[9:].rstrip(';').split(',')]

DECLARATIONS = '''
	// Aggregate only after releasing the SRTP send mutex. A snapshot never
	// acquires media/TLS/ICE locks and is safe from an inline send callback.
	struct MediaCallbackDiagnosticScope {
		DtlsSrtpTransport &t;
		bool rtcp;
		std::chrono::steady_clock::time_point at = std::chrono::steady_clock::now();
		~MediaCallbackDiagnosticScope();
	};
	struct MediaSendDiagnosticScope {
		DtlsSrtpTransport &t;
		std::chrono::steady_clock::time_point waitingAt = std::chrono::steady_clock::now();
		std::unique_lock<std::mutex> lock;
		std::chrono::steady_clock::time_point acquiredAt;
		bool valid = false, rtcp = false, sent = false;
		bool protectedPacket = false, lowerAttempted = false;
		uint64_t protectUs = 0, lowerUs = 0;
		explicit MediaSendDiagnosticScope(DtlsSrtpTransport &transport);
		~MediaSendDiagnosticScope();
	};
'''

IMPLEMENTATION = '''
DtlsSrtpTransport::MediaCallbackDiagnosticScope::~MediaCallbackDiagnosticScope() {
	const auto us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
	    std::chrono::steady_clock::now() - at).count());
	std::lock_guard lock(t.mDiagnosticsMutex);
	auto &d = t.mRecvDiagnostics;
	++d.recvMediaCallbackCount;
	d.recvMediaCallbackTotalUs += us;
	d.recvMediaCallbackMaxUs = std::max(d.recvMediaCallbackMaxUs, us);
	if (rtcp) {
		++d.recvRtcpCallbackCount;
		d.recvRtcpCallbackTotalUs += us;
		d.recvRtcpCallbackMaxUs = std::max(d.recvRtcpCallbackMaxUs, us);
	} else {
		++d.recvRtpCallbackCount;
		d.recvRtpCallbackTotalUs += us;
		d.recvRtpCallbackMaxUs = std::max(d.recvRtpCallbackMaxUs, us);
	}
}

DtlsSrtpTransport::MediaSendDiagnosticScope::MediaSendDiagnosticScope(DtlsSrtpTransport &transport)
    : t(transport), lock(t.sendMutex), acquiredAt(std::chrono::steady_clock::now()) {}

DtlsSrtpTransport::MediaSendDiagnosticScope::~MediaSendDiagnosticScope() {
	const auto releasedAt = std::chrono::steady_clock::now();
	lock.unlock();
	const auto waitUs = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(acquiredAt - waitingAt).count());
	const auto holdUs = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(releasedAt - acquiredAt).count());
	std::lock_guard diagnosticLock(t.mDiagnosticsMutex);
	auto &d = t.mRecvDiagnostics;
	++d.sendSrtpLockCount;
	d.sendSrtpLockWaitTotalUs += waitUs;
	d.sendSrtpLockWaitMaxUs = std::max(d.sendSrtpLockWaitMaxUs, waitUs);
	d.sendSrtpLockHoldTotalUs += holdUs;
	d.sendSrtpLockHoldMaxUs = std::max(d.sendSrtpLockHoldMaxUs, holdUs);
	if (!valid) { ++d.sendSrtpRejected; return; }
	if (!sent) ++d.sendSrtpFailed;
	if (rtcp) {
		++d.sendRtcpCount;
		if (protectedPacket) {
			++d.sendRtcpProtectCount; d.sendRtcpProtectTotalUs += protectUs;
			d.sendRtcpProtectMaxUs = std::max(d.sendRtcpProtectMaxUs, protectUs);
		}
		if (lowerAttempted) {
			++d.sendRtcpLowerCount; d.sendRtcpLowerTotalUs += lowerUs;
			d.sendRtcpLowerMaxUs = std::max(d.sendRtcpLowerMaxUs, lowerUs);
		}
	} else {
		++d.sendRtpCount;
		if (protectedPacket) {
			++d.sendRtpProtectCount; d.sendRtpProtectTotalUs += protectUs;
			d.sendRtpProtectMaxUs = std::max(d.sendRtpProtectMaxUs, protectUs);
		}
		if (lowerAttempted) {
			++d.sendRtpLowerCount; d.sendRtpLowerTotalUs += lowerUs;
			d.sendRtpLowerMaxUs = std::max(d.sendRtpLowerMaxUs, lowerUs);
		}
	}
}

'''


def apply(root):
    rtc = root / 'third_party/rtc/libdatachannel'

    def replace(name, old, new, count=1):
        path = rtc / name
        source = path.read_text().replace(new, old)
        if source.count(old) != count:
            raise SystemExit('Unrecognized pinned media-send diagnostic site: ' + name + ': ' + old[:70])
        path.write_text(source.replace(old, new))

    replace('include/rtc/dtlsdiagnostics.hpp', '\n};', FIELDS + '\n};')
    prefix = '''DtlsDiagnostics DtlsTransport::getDtlsDiagnostics() const {
	auto result = mIncomingQueue.diagnostics();
	std::lock_guard lock(mDiagnosticsMutex);
'''
    replace('src/impl/dtlstransport.cpp', prefix,
            prefix + ''.join('\tresult.' + n + ' = mRecvDiagnostics.' + n + ';\n' for n in NAMES))
    replace('src/impl/dtlssrtptransport.hpp', '\tstd::mutex sendMutex;', DECLARATIONS + '\tstd::mutex sendMutex;')
    replace('src/impl/dtlssrtptransport.cpp', 'bool DtlsSrtpTransport::sendMedia(message_ptr message) {',
            IMPLEMENTATION + 'bool DtlsSrtpTransport::sendMedia(message_ptr message) {')
    replace('src/impl/dtlssrtptransport.cpp', '\tstd::lock_guard lock(sendMutex);', '\tMediaSendDiagnosticScope diagnostic{*this};')
    replace('src/impl/dtlssrtptransport.cpp', '\tint size = int(message->size());\n\tPLOG_VERBOSE << "Send size=" << size;',
            '\tdiagnostic.valid = true;\n\tdiagnostic.rtcp = IsRtcp(*message);\n\tint size = int(message->size());\n\tPLOG_VERBOSE << "Send size=" << size;')
    for function in ('srtp_protect_rtcp', 'srtp_protect'):
        old = '\t\tif (srtp_err_status_t err = ' + function + '(mSrtpOut, message->data(), &size)) {'
        new = '''		srtp_err_status_t err;
		const auto protectAt = std::chrono::steady_clock::now();
		err = ''' + function + '''(mSrtpOut, message->data(), &size);
		diagnostic.protectedPacket = true;
		diagnostic.protectUs = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - protectAt).count());
		if (err) {'''
        replace('src/impl/dtlssrtptransport.cpp', old, new)
    replace('src/impl/dtlssrtptransport.cpp', '\treturn Transport::outgoing(message); // bypass DTLS DSCP marking', '''	const auto lowerAt = std::chrono::steady_clock::now();
	try {
		diagnostic.sent = Transport::outgoing(message); // bypass DTLS DSCP marking
	} catch (...) {
		diagnostic.lowerAttempted = true;
		diagnostic.lowerUs = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - lowerAt).count());
		throw;
	}
	diagnostic.lowerAttempted = true;
	diagnostic.lowerUs = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - lowerAt).count());
	return diagnostic.sent;''')
    # The earlier ingress recipe reintroduces its generic callback scope on a
    # full reapplication. Keep one scope, updating generic and split totals in
    # a single diagnostics critical section.
    path = rtc / 'src/impl/dtlssrtptransport.cpp'
    source = path.read_text()
    generic = '\tPacketDiagnosticScope callbackScope{*this, PacketStage::MediaCallback};'
    split = '\tMediaCallbackDiagnosticScope callbackScope{*this, message->type == Message::Control};'
    if generic in source and split in source:
        path.write_text(source.replace(split + '\n', ''))
    replace('src/impl/dtlssrtptransport.cpp', generic, split)


if __name__ == '__main__':
    apply(Path(__file__).resolve().parents[2])