"""Pinned mbedTLS receive isolation, fair continuation and SSL lock diagnostics."""
from pathlib import Path


def apply(root):
    rtc = root / 'third_party/rtc/libdatachannel'
    header = rtc / 'src/impl/dtlstransport.hpp'
    source = rtc / 'src/impl/dtlstransport.cpp'
    s = source.read_text()
    h = header.read_text()
    names = ('sendSslLockCount', 'sendSslLockWaitTotalUs', 'sendSslLockWaitMaxUs',
             'sendSslLockHoldTotalUs', 'sendSslLockHoldMaxUs', 'recvSslLockCount',
             'recvSslLockWaitTotalUs', 'recvSslLockWaitMaxUs', 'recvSslLockHoldTotalUs',
             'recvSslLockHoldMaxUs', 'recvBatchPacketsTotal', 'recvBatchPacketsMax', 'recvBatchYields')
    if 'result.sendSslLockCount' not in s:
        s = s.replace('\tresult.recvProcessingMaxUs = mRecvDiagnostics.recvProcessingMaxUs;',
                      '\tresult.recvProcessingMaxUs = mRecvDiagnostics.recvProcessingMaxUs;\n' +
                      ''.join('\tresult.' + n + ' = mRecvDiagnostics.' + n + ';\n' for n in names))
    if 'struct SslDiagnosticScope' not in h:
        h = h.replace('\tstd::recursive_mutex mSslMutex;', '''\tstd::recursive_mutex mSslMutex;
\t// Accessed only under mSslMutex. BIO never demuxes or calls application code.
\tmessage_ptr mReadMessage;
\tstruct SslDiagnosticScope {
\t\tDtlsTransport &transport;
\t\tbool sending;
\t\tstd::chrono::steady_clock::time_point waitingAt;
\t\tstd::unique_lock<std::recursive_mutex> lock;
\t\tstd::chrono::steady_clock::time_point acquiredAt;
\t\tSslDiagnosticScope(DtlsTransport &transport, bool sending);
\t\t~SslDiagnosticScope();
\t};''')
        header.write_text(h)
    # Coalesce atomically. Timer calls do not own a queued-receive token.
    s = s.replace('''\tif (mPendingRecvCount > 0)
\t\treturn;

\t++mPendingRecvCount;''', '''\tint expected = 0;
\tif (!mPendingRecvCount.compare_exchange_strong(expected, 1))
\t\treturn;''')
    s = s.replace('\t--mPendingRecvCount;', '''\tif (queuedAt != steady_clock::time_point{})
\t\tmPendingRecvCount = 0;''')
    # A lower-transport close must wake an idle receiver, just like stop().
    s = s.replace('''\tif (!message) {
\t\tmIncomingQueue.stop();
\t\treturn;
\t}''', '''\tif (!message) {
\t\tmIncomingQueue.stop();
\t\tenqueueRecv();
\t\treturn;
\t}''')
    a = s.index('#elif USE_MBEDTLS')
    b = s.index('#else // OpenSSL', a) if '#else // OpenSSL' in s[a:] else s.index('#else // OPENSSL', a)
    part = s[a:b]
    if 'DtlsTransport::SslDiagnosticScope::SslDiagnosticScope' not in part:
        part = part.replace('void DtlsTransport::start() {', '''DtlsTransport::SslDiagnosticScope::SslDiagnosticScope(DtlsTransport &t, bool send)
    : transport(t), sending(send), waitingAt(steady_clock::now()), lock(t.mSslMutex),
      acquiredAt(steady_clock::now()) {}

DtlsTransport::SslDiagnosticScope::~SslDiagnosticScope() {
\tconst auto releasedAt = steady_clock::now();
\tlock.unlock();
\tconst auto waitUs = uint64_t(duration_cast<microseconds>(acquiredAt - waitingAt).count());
\tconst auto holdUs = uint64_t(duration_cast<microseconds>(releasedAt - acquiredAt).count());
\tstd::lock_guard diagnosticLock(transport.mDiagnosticsMutex);
\tauto &d = transport.mRecvDiagnostics;
\tif (sending) {
\t\t++d.sendSslLockCount;
\t\td.sendSslLockWaitTotalUs += waitUs;
\t\td.sendSslLockWaitMaxUs = std::max(d.sendSslLockWaitMaxUs, waitUs);
\t\td.sendSslLockHoldTotalUs += holdUs;
\t\td.sendSslLockHoldMaxUs = std::max(d.sendSslLockHoldMaxUs, holdUs);
\t} else {
\t\t++d.recvSslLockCount;
\t\td.recvSslLockWaitTotalUs += waitUs;
\t\td.recvSslLockWaitMaxUs = std::max(d.recvSslLockWaitMaxUs, waitUs);
\t\td.recvSslLockHoldTotalUs += holdUs;
\t\td.recvSslLockHoldMaxUs = std::max(d.recvSslLockHoldMaxUs, holdUs);
\t}
}

void DtlsTransport::start() {''')
        # Only established application writes are counted as send SSL locks.
        begin = part.index('bool DtlsTransport::send(')
        end = part.index('\nvoid DtlsTransport::incoming', begin)
        send = part[begin:end].replace('std::lock_guard lock(mSslMutex);', 'SslDiagnosticScope sslLock(*this, true);')
        part = part[:begin] + send + part[end:]
    start = part.index('void DtlsTransport::doRecv(')
    end = part.index('\nint DtlsTransport::CertificateCallback', start)
    # Preserve the existing common diagnostic prefix, used by the earlier patch.
    prefix = part[start:part.index('\n', part.index('\tRecvDiagnosticScope diagnosticScope', start)) + 1]
    prefix = prefix.replace('\tif (queuedAt != steady_clock::time_point{})\n\t\tmPendingRecvCount = 0;\n', '')
    body = '''
\t// The scheduling token covers both queued and running work: new arrivals
\t// never park another pool worker on mRecvMutex while a callback is active.
\t// Release then recheck to close the empty-queue/incoming() wakeup race.
\tbool continueRecv = false;
\tstruct ScheduleScope {
\t\tDtlsTransport &t;
\t\tbool queued;
\t\tbool &again;
\t\t~ScheduleScope() {
\t\t\tif (queued) t.mPendingRecvCount = 0;
\t\t\tif ((t.state() == State::Connecting || t.state() == State::Connected) &&
\t\t\t    (again || !t.mIncomingQueue.empty() || !t.mIncomingQueue.running()))
\t\t\t\tt.enqueueRecv();
\t\t}
\t} scheduleScope{*this, queuedAt != steady_clock::time_point{}, continueRecv};

\tif (state() != State::Connecting && state() != State::Connected)
\t\treturn;


\t// One datagram or TLS call is a work unit. A slow application callback may
\t// overrun the time budget, but never owns mSslMutex. mRecvMutex preserves
\t// SRTP replay-context serialization and packet/callback ordering.
\tconstexpr size_t maxWork = 64;
\tconstexpr auto maxTime = milliseconds(2);
\tsize_t work = 0, packets = 0;
\tstruct BatchScope {
\t\tDtlsTransport &t;
\t\tsize_t &packets;
\t\t~BatchScope() {
\t\t\tstd::lock_guard lock(t.mDiagnosticsMutex);
\t\t\tt.mRecvDiagnostics.recvBatchPacketsTotal += packets;
\t\t\tt.mRecvDiagnostics.recvBatchPacketsMax = std::max(
\t\t\t    t.mRecvDiagnostics.recvBatchPacketsMax, uint64_t(packets));
\t\t}
\t} batchScope{*this, packets};
\tbool readTls = true;

\ttry {
\t\tunsigned char buffer[4096];
\t\twhile (true) {
\t\t\tif (work >= maxWork || (work > 0 && steady_clock::now() - startedAt >= maxTime)) {
\t\t\t\t{
\t\t\t\t\tstd::lock_guard diagnosticLock(mDiagnosticsMutex);
\t\t\t\t\t++mRecvDiagnostics.recvBatchYields;
\t\t\t\t}
\t\t\t\t// Also continue with no queued datagrams: mbedTLS may still own
\t\t\t\t// plaintext records from the last datagram. Never strand those.
\t\t\t\tcontinueRecv = true;
\t\t\t\treturn;
\t\t\t}

\t\t\tif (readTls) {
\t\t\t\t++work;
\t\t\t\tint ret;
\t\t\t\tconst bool connecting = state() == State::Connecting;
\t\t\t\t{
\t\t\t\t\tSslDiagnosticScope sslLock(*this, false);

\t\t\t\t\tret = connecting ? mbedtls_ssl_handshake(&mSsl)
\t\t\t\t\t                 : mbedtls_ssl_read(&mSsl, buffer, sizeof(buffer));
\t\t\t\t\tif (connecting && ret == 0) {
\t\t\t\t\t\tmbedtls_ssl_set_mtu(&mSsl, static_cast<unsigned int>(sizeof(buffer) + 1));
\t\t\t\t\t\t// SRTP profile inspection uses mSsl. Publish Connected only
\t\t\t\t\t\t// after keys are ready, before handling following media.
\t\t\t\t\t\tpostHandshake();
\t\t\t\t\t}
\t\t\t\t}
\t\t\t\tif (connecting && ret == 0) {
\t\t\t\t\tPLOG_INFO << "DTLS handshake finished";
\t\t\t\t\tchangeState(State::Connected);
\t\t\t\t\tcontinue;
\t\t\t\t}
\t\t\t\tif (ret == MBEDTLS_ERR_SSL_WANT_READ) {
\t\t\t\t\treadTls = false;
\t\t\t\t} else if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || (!connecting && ret == 0)) {
\t\t\t\t\tbreak;
\t\t\t\t} else if (mbedtls::check(ret, connecting ? "Handshake failed" : "DTLS read failed")) {
\t\t\t\t\tauto *b = reinterpret_cast<byte *>(buffer);
\t\t\t\t\trecv(make_message(b, b + ret));
\t\t\t\t\tcontinue; // drain buffered TLS records before the next datagram
\t\t\t\t} else {
\t\t\t\t\tcontinue;
\t\t\t\t}
\t\t\t}

\t\t\tauto next = mIncomingQueue.pop();
\t\t\tif (!next) {
\t\t\t\tif (!mIncomingQueue.running())
\t\t\t\t\tbreak;
\t\t\t\tif (state() == State::Connecting && mFinMs != 0) {
\t\t\t\t\tThreadPool::Instance().schedule(mTimerSetAt + milliseconds(mFinMs),
\t\t\t\t\t    [weak_this = weak_from_this()] {
\t\t\t\t\t\t    if (auto locked = weak_this.lock()) locked->enqueueRecv();
\t\t\t\t\t    });
\t\t\t\t}
\t\t\t\treturn;
\t\t\t}
\t\t\t++work;
\t\t\t++packets;
\t\t\tauto message = std::move(*next);
\t\t\tif (demuxMessage(message))
\t\t\t\tcontinue; // application/SRTP work is outside mSslMutex
\t\t\t{
\t\t\t\tSslDiagnosticScope sslLock(*this, false);
\t\t\t\tmReadMessage = std::move(message);
\t\t\t}
\t\t\treadTls = true;
\t\t}
\t} catch (const std::exception &e) {
\t\tPLOG_ERROR << "DTLS recv: " << e.what();
\t}

\tif (state() == State::Connected) {
\t\tPLOG_INFO << "DTLS closed";
\t\tchangeState(State::Disconnected);
\t\trecv(nullptr);
\t} else {
\t\tPLOG_ERROR << "DTLS handshake failed";
\t\tchangeState(State::Failed);
\t}
}
'''
    part = part[:start] + prefix + body + part[end:]
    start = part.index('int DtlsTransport::ReadCallback(')
    end = part.index('\nvoid DtlsTransport::SetTimerCallback', start)
    part = part[:start] + '''int DtlsTransport::ReadCallback(void *ctx, unsigned char *buf, size_t len) {
\tauto *t = static_cast<DtlsTransport *>(ctx);
\tif (!t->mReadMessage)
\t\treturn MBEDTLS_ERR_SSL_WANT_READ;
\tauto message = std::move(t->mReadMessage);
\tif (message->size() > len)
\t\treturn MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL;
\tstd::memcpy(buf, message->data(), message->size());
\treturn int(message->size());
}
''' + part[end:]
    source.write_text(s[:a] + part + s[b:])


if __name__ == '__main__':
    apply(Path(__file__).resolve().parents[2])