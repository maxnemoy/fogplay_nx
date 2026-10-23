"""Bounded non-waiting DTLS ingress and per-stage diagnostics, pinned recipe."""
from pathlib import Path
import shutil

FIELDS = '''
    // DTLS ingress only: capacity 1024, at most 960 RTP datagrams; FIFO among
    // survivors. Critical = DTLS/SRTCP. New RTP is shed; critical admission at
    // capacity evicts oldest RTP. All-critical overflow asynchronously fails.
    uint64_t admissionMediaAccepted = 0, admissionControlAccepted = 0;
    uint64_t admissionMediaDropped = 0, admissionMediaEvicted = 0;
    uint64_t admissionInvalid = 0, admissionStopped = 0, admissionCriticalOverflow = 0;
    // Completed per-packet stages; total/max in microseconds, including waits
    // and preemption. Demux includes unprotect and media callback; nested
    // stages must not be added together. Unprotect includes failed attempts.
    uint64_t recvDemuxCount = 0, recvDemuxTotalUs = 0, recvDemuxMaxUs = 0;
    uint64_t recvUnprotectCount = 0, recvUnprotectTotalUs = 0, recvUnprotectMaxUs = 0;
    uint64_t recvMediaCallbackCount = 0, recvMediaCallbackTotalUs = 0, recvMediaCallbackMaxUs = 0;
    uint64_t recvDataCallbackCount = 0, recvDataCallbackTotalUs = 0, recvDataCallbackMaxUs = 0;
    uint64_t recvBatchTimeYields = 0, recvBatchWorkYields = 0, recvBatchEmpty = 0;
'''
NAMES = [part.strip().split(' =')[0] for line in FIELDS.splitlines()
         if line.strip().startswith('uint64_t ') for part in line.strip()[9:].rstrip(';').split(',')]


def apply(root):
    rtc = root / 'third_party/rtc/libdatachannel'
    shutil.copyfile(root / 'tools/rtc/dtls-ingress-queue.h', rtc / 'src/impl/dtlsingressqueue.hpp')

    def replace(name, old, new, count=1):
        path = rtc / name
        source = path.read_text()
        # Earlier recipes regenerate only the mbedTLS receive body. Normalize
        # each already-patched occurrence, then patch every backend again.
        # A global `new in source` test would retain an unpatched mbedTLS site
        # when the GnuTLS/OpenSSL instance survives the preceding recipe.
        source = source.replace(new, old)
        if source.count(old) != count:
            raise SystemExit('Unrecognized pinned ingress site: ' + name + ': ' + old[:70])
        path.write_text(source.replace(old, new))

    replace('include/rtc/dtlsdiagnostics.hpp', '\n};', FIELDS + '\n};')
    replace('src/impl/dtlstransport.hpp', '#include "queue.hpp"', '#include "queue.hpp"\n#include "dtlsingressqueue.hpp"')
    replace('src/impl/dtlstransport.hpp', 'Queue<message_ptr> mIncomingQueue;', 'DtlsIngressQueue mIncomingQueue;')
    replace('src/impl/dtlstransport.hpp', '\tvoid enqueueRecv();', '''\tvoid enqueueRecv();
\tbool failIngressOverflow();
\tenum class PacketStage { Demux, Unprotect, MediaCallback, DataCallback };
\tstruct PacketDiagnosticScope {
\t\tDtlsTransport &t;
\t\tPacketStage stage;
\t\tstd::chrono::steady_clock::time_point at = std::chrono::steady_clock::now();
\t\t~PacketDiagnosticScope();
\t};''')
    replace('src/impl/dtlstransport.cpp', '\tresult.recvRuns = mRecvDiagnostics.recvRuns;',
            ''.join('\tresult.' + name + ' = mRecvDiagnostics.' + name + ';\n'
                    for name in NAMES if name.startswith('recv')) + '\tresult.recvRuns = mRecvDiagnostics.recvRuns;')
    replace('src/impl/dtlstransport.cpp', 'void DtlsTransport::enqueueRecv() {', '''DtlsTransport::PacketDiagnosticScope::~PacketDiagnosticScope() {
\tconst auto us = uint64_t(duration_cast<microseconds>(steady_clock::now() - at).count());
\t// Clock and work execute without the diagnostics lock, including crypto.
\tstd::lock_guard lock(t.mDiagnosticsMutex);
\tuint64_t *count = nullptr, *total = nullptr, *maximum = nullptr;
\tswitch (stage) {
''' + ''.join('\tcase PacketStage::' + stage + ': count = &t.mRecvDiagnostics.recv' + field + 'Count; total = &t.mRecvDiagnostics.recv' + field + 'TotalUs; maximum = &t.mRecvDiagnostics.recv' + field + 'MaxUs; break;\n'
            for stage, field in [('Demux', 'Demux'), ('Unprotect', 'Unprotect'), ('MediaCallback', 'MediaCallback'), ('DataCallback', 'DataCallback')]) + '''\t}
\t++*count; *total += us; *maximum = std::max(*maximum, us);
}

bool DtlsTransport::failIngressOverflow() {
\tif (!mIncomingQueue.overflowed()) return false;
\tmIncomingQueue.discard();
\tif (state() == State::Connecting || state() == State::Connected) {
\t\tconst bool connected = state() == State::Connected;
\t\tPLOG_ERROR << "DTLS critical ingress capacity exhausted";
\t\tchangeState(State::Failed);
\t\tif (connected) recv(nullptr);
\t}
\treturn true;
}

void DtlsTransport::enqueueRecv() {''')
    replace('src/impl/dtlstransport.cpp', '\tRecvDiagnosticScope diagnosticScope{*this, startedAt};',
            '\tRecvDiagnosticScope diagnosticScope{*this, startedAt};\n\tif (failIngressOverflow()) return;', count=3)
    # Receiver loop, not incoming(): no state or application callback runs on
    # the ICE producer. Also abort after an already-running callback returns.
    replace('src/impl/dtlstransport.cpp', '\t\twhile (true) {\n\t\t\tif (work >= maxWork',
            '\t\twhile (true) {\n\t\t\tif (failIngressOverflow()) return;\n\t\t\tif (work >= maxWork')
    replace('src/impl/dtlstransport.cpp', '\t\t\t\t\t++mRecvDiagnostics.recvBatchYields;', '''\t\t\t\t\t++mRecvDiagnostics.recvBatchYields;
\t\t\t\t\tif (work >= maxWork) ++mRecvDiagnostics.recvBatchWorkYields;
\t\t\t\t\telse ++mRecvDiagnostics.recvBatchTimeYields;''')
    replace('src/impl/dtlstransport.cpp', '\t\t\tauto next = mIncomingQueue.pop();\n\t\t\tif (!next) {', '''\t\t\tauto next = mIncomingQueue.pop();
\t\t\tif (!next) {
\t\t\t\tif (failIngressOverflow()) return;
\t\t\t\t{ std::lock_guard lock(mDiagnosticsMutex); ++mRecvDiagnostics.recvBatchEmpty; }''', count=2)
    replace('src/impl/dtlstransport.cpp', '\t\t\tif (demuxMessage(message))\n\t\t\t\tcontinue; // application/SRTP work is outside mSslMutex', '''\t\t\tbool demuxed;
\t\t\t{
\t\t\t\tPacketDiagnosticScope packetScope{*this, PacketStage::Demux};
\t\t\t\tdemuxed = demuxMessage(message);
\t\t\t}
\t\t\tif (demuxed)
\t\t\t\tcontinue; // application/SRTP work is outside mSslMutex''')
    replace('src/impl/dtlstransport.cpp', '\t\t\t\t\trecv(make_message(b, b + ret));', '''\t\t\t\t\tPacketDiagnosticScope callbackScope{*this, PacketStage::DataCallback};
\t\t\t\t\trecv(make_message(b, b + ret));''', count=2)
    for function in ('srtp_unprotect_rtcp', 'srtp_unprotect'):
        old = '\t\tif (srtp_err_status_t err = ' + function + '(mSrtpIn, message->data(), &size)) {'
        new = '''\t\tsrtp_err_status_t err;
\t\t{
\t\t\tPacketDiagnosticScope cryptoScope{*this, PacketStage::Unprotect};
\t\t\terr = ''' + function + '''(mSrtpIn, message->data(), &size);
\t\t}
\t\tif (err) {'''
        replace('src/impl/dtlssrtptransport.cpp', old, new)
    replace('src/impl/dtlssrtptransport.cpp', '\tmSrtpRecvCallback(message);',
            '\tPacketDiagnosticScope callbackScope{*this, PacketStage::MediaCallback};\n\tmSrtpRecvCallback(message);')


if __name__ == '__main__':
    apply(Path(__file__).resolve().parents[2])
