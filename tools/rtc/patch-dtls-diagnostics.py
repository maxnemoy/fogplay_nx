"""Pinned, idempotent DTLS receive pipeline diagnostics (no scheduling changes)."""
from pathlib import Path


def apply(root):
    rtc = root / 'third_party/rtc/libdatachannel'

    def replace(name, old, new, count=1):
        path = rtc / name
        source = path.read_text()
        if new in source:
            return
        if source.count(old) != count:
            raise SystemExit('Unrecognized pinned DTLS diagnostic site: ' + str(path))
        path.write_text(source.replace(old, new))

    # Keep generated additions in the reproducible patch recipe, rather than
    # depending on ignored source-tree files surviving a fresh fetch.
    (rtc / 'include/rtc/dtlsdiagnostics.hpp').write_text('''#ifndef RTC_DTLS_DIAGNOSTICS_H
#define RTC_DTLS_DIAGNOSTICS_H

#include <cstdint>

namespace rtc {

// Fork extension. Numeric aggregates for the lifetime of the DTLS transport;
// not reset by clearStats(). No packet contents, endpoints or credentials.
// All durations are integer microseconds on a steady clock, rounded per sample.
// Queue and receiver groups are each coherent, but sampled independently.
struct DtlsDiagnostics {
    bool available = false; // false before DTLS exists
    uint64_t queueDepth = 0, queueHighWater = 0, queueOldestAgeUs = 0;
    uint64_t enqueued = 0, dequeued = 0;
    // Enqueue acceptance to pop, excluding a producer's full-queue wait.
    // Average = residenceTotalUs / dequeued (zero if dequeued == 0).
    uint64_t residenceTotalUs = 0, residenceMaxUs = 0;
    // Completed push calls that waited on a full queue (including stop wakeups).
    // In-progress waits are not included. Average = total / count.
    uint64_t fullPushWaitCount = 0, fullPushWaitTotalUs = 0, fullPushWaitMaxUs = 0;
    uint64_t recvRuns = 0, recvScheduleCount = 0;
    // Immediate enqueueRecv tasks only: submission to receiver lock acquisition.
    // Includes thread-pool and receiver-lock contention; excludes handshake timers.
    uint64_t recvScheduleWaitTotalUs = 0, recvScheduleWaitMaxUs = 0;
    // Completed doRecv calls: after receiver lock acquisition to return, including
    // synchronous demux/application callbacks. Average = total / recvRuns.
    uint64_t recvProcessingTotalUs = 0, recvProcessingMaxUs = 0;
};

} // namespace rtc
#endif
''')

    replace('include/rtc/peerconnection.hpp', '#include "description.hpp"',
            '#include "description.hpp"\n#include "dtlsdiagnostics.hpp"')
    replace('CMakeLists.txt', '\t${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/peerconnection.hpp',
            '\t${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/peerconnection.hpp\n\t${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/dtlsdiagnostics.hpp')
    replace('include/rtc/peerconnection.hpp', '\tstring iceDiagnostics() const;',
            '''\tstring iceDiagnostics() const;
\t// Live transport snapshot; close retains a last pre-teardown snapshot.
\tDtlsDiagnostics getDtlsDiagnostics() const;''')
    replace('src/peerconnection.cpp', 'string PeerConnection::iceDiagnostics() const {',
            '''DtlsDiagnostics PeerConnection::getDtlsDiagnostics() const {
\treturn impl()->getDtlsDiagnostics();
}

string PeerConnection::iceDiagnostics() const {''')
    replace('src/impl/peerconnection.hpp', '\tstring iceDiagnostics() const;',
            '\tstring iceDiagnostics() const;\n\tDtlsDiagnostics getDtlsDiagnostics() const;')
    replace('src/impl/peerconnection.hpp', '\tshared_ptr<DtlsTransport> mDtlsTransport;',
            '''\tshared_ptr<DtlsTransport> mDtlsTransport;
\tmutable std::mutex mDtlsDiagnosticsMutex;
\tDtlsDiagnostics mFinalDtlsDiagnostics;''')
    replace('src/impl/peerconnection.cpp',
            'string PeerConnection::iceDiagnostics() const {',
            '''DtlsDiagnostics PeerConnection::getDtlsDiagnostics() const {
\tif (auto dtls = getDtlsTransport())
\t\treturn dtls->getDtlsDiagnostics();
\tstd::lock_guard lock(mDtlsDiagnosticsMutex);
\treturn mFinalDtlsDiagnostics;
}

string PeerConnection::iceDiagnostics() const {''')
    replace('src/impl/peerconnection.cpp',
            '\t// Retain the final per-peer snapshot before the ICE pointer disappears.',
            '''\t// Cache before exchanging away the transport; readers retain a shared
\t// pointer or see this snapshot. Do not hold the cache mutex while sampling.
\tif (auto dtls = getDtlsTransport()) {
\t\tauto snapshot = dtls->getDtlsDiagnostics();
\t\tstd::lock_guard lock(mDtlsDiagnosticsMutex);
\t\tmFinalDtlsDiagnostics = snapshot;
\t}

\t// Retain the final per-peer snapshot before the ICE pointer disappears.''')

    # A timestamp is stored beside each element, so push/pop retain their single
    # queue allocation and exception behavior. Other queues never read the clock.
    replace('src/impl/queue.hpp', '#include "common.hpp"',
            '#include "common.hpp"\n#include "rtc/dtlsdiagnostics.hpp"')
    replace('src/impl/queue.hpp', 'amount_function func = nullptr);',
            'amount_function func = nullptr, bool diagnostics = false);')
    replace('src/impl/queue.hpp', '\toptional<T> exchange(T element);',
            '\toptional<T> exchange(T element);\n\tDtlsDiagnostics diagnostics() const;')
    replace('src/impl/queue.hpp', '\tstd::queue<T> mQueue;', '''\tusing clock = std::chrono::steady_clock;
\tstruct Entry { T value; clock::time_point enqueuedAt; };
\tstd::queue<Entry> mQueue;
\tconst bool mDiagnosticsEnabled;
\tDtlsDiagnostics mDiagnostics;''')
    replace('src/impl/queue.hpp',
            'Queue<T>::Queue(size_t limit, amount_function func) : mLimit(limit), mAmount(0) {',
            '''Queue<T>::Queue(size_t limit, amount_function func, bool diagnostics)
    : mLimit(limit), mAmount(0), mDiagnosticsEnabled(diagnostics) {''')
    replace('src/impl/queue.hpp',
            '\tmPushCondition.wait(lock, [this]() { return mLimit == 0 || mQueue.size() < mLimit || mStopping; });',
            '''\tconst bool fullWait = mDiagnosticsEnabled && !mStopping && mLimit > 0 && mQueue.size() >= mLimit;
\tconst auto waitStart = fullWait ? clock::now() : clock::time_point{};
\tmPushCondition.wait(lock, [this]() { return mLimit == 0 || mQueue.size() < mLimit || mStopping; });
\tif (fullWait) {
\t\tconst auto us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - waitStart).count());
\t\t++mDiagnostics.fullPushWaitCount;
\t\tmDiagnostics.fullPushWaitTotalUs += us;
\t\tmDiagnostics.fullPushWaitMaxUs = std::max(mDiagnostics.fullPushWaitMaxUs, us);
\t}''')
    replace('src/impl/queue.hpp', '\tmQueue.emplace(std::move(element));',
            '''\tmQueue.push(Entry{std::move(element), mDiagnosticsEnabled ? clock::now() : clock::time_point{}});
\tif (mDiagnosticsEnabled) {
\t\t++mDiagnostics.enqueued;
\t\tmDiagnostics.queueHighWater = std::max(mDiagnostics.queueHighWater, uint64_t(mQueue.size()));
\t}''')
    replace('src/impl/queue.hpp', '\tmAmount -= mAmountFunction(mQueue.front());\n\toptional<T> element{std::move(mQueue.front())};',
            '''\tmAmount -= mAmountFunction(mQueue.front().value);
\toptional<T> element{std::move(mQueue.front().value)};
\tif (mDiagnosticsEnabled) {
\t\tconst auto us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - mQueue.front().enqueuedAt).count());
\t\t++mDiagnostics.dequeued;
\t\tmDiagnostics.residenceTotalUs += us;
\t\tmDiagnostics.residenceMaxUs = std::max(mDiagnostics.residenceMaxUs, us);
\t}''')
    replace('src/impl/queue.hpp', 'std::make_optional(mQueue.front())',
            'std::make_optional(mQueue.front().value)')
    replace('src/impl/queue.hpp', 'std::swap(mQueue.front(), element);',
            '''std::swap(mQueue.front().value, element);
\tif (mDiagnosticsEnabled)
\t\tmQueue.front().enqueuedAt = clock::now();''')
    replace('src/impl/queue.hpp', '} // namespace rtc::impl', '''template <typename T> DtlsDiagnostics Queue<T>::diagnostics() const {
\tstd::lock_guard lock(mMutex);
\tauto result = mDiagnostics;
\tresult.available = mDiagnosticsEnabled;
\tif (mDiagnosticsEnabled) {
\t\tresult.queueDepth = mQueue.size();
\t\tif (!mQueue.empty())
\t\t\tresult.queueOldestAgeUs = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - mQueue.front().enqueuedAt).count());
\t}
\treturn result;
}

} // namespace rtc::impl''')
    replace('src/impl/queue.hpp', '#include <atomic>', '#include <algorithm>\n#include <atomic>')

    replace('src/impl/dtlstransport.hpp', '\tbool isClient() const { return mIsClient; }',
            '\tbool isClient() const { return mIsClient; }\n\tDtlsDiagnostics getDtlsDiagnostics() const;')
    replace('src/impl/dtlstransport.hpp', '\tvoid doRecv();',
            '''\tvoid doRecv(std::chrono::steady_clock::time_point queuedAt = {});
\tstruct RecvDiagnosticScope {
\t\tDtlsTransport &transport;
\t\tstd::chrono::steady_clock::time_point startedAt;
\t\t~RecvDiagnosticScope();
\t};
\tmutable std::mutex mDiagnosticsMutex;
\tDtlsDiagnostics mRecvDiagnostics;''')
    replace('src/impl/dtlstransport.cpp',
            'mIncomingQueue(RECV_QUEUE_LIMIT, message_size_func)',
            'mIncomingQueue(RECV_QUEUE_LIMIT, message_size_func, true)', count=3)
    replace('src/impl/dtlstransport.cpp',
            '''\tThreadPool::Instance().enqueue([weak_this = weak_from_this()]() {
\t\tif (auto locked = weak_this.lock())
\t\t\tlocked->doRecv();
\t});''',
            '''\tconst auto queuedAt = steady_clock::now();
\tThreadPool::Instance().enqueue([weak_this = weak_from_this(), queuedAt]() {
\t\tif (auto locked = weak_this.lock())
\t\t\tlocked->doRecv(queuedAt);
\t});''')
    replace('src/impl/dtlstransport.cpp',
            '''void DtlsTransport::doRecv() {
\tstd::lock_guard lock(mRecvMutex);''',
            '''void DtlsTransport::doRecv(steady_clock::time_point queuedAt) {
\tstd::lock_guard lock(mRecvMutex);
\tconst auto startedAt = steady_clock::now();
\tif (queuedAt != steady_clock::time_point{}) {
\t\tconst auto us = uint64_t(duration_cast<microseconds>(startedAt - queuedAt).count());
\t\tstd::lock_guard diagnosticLock(mDiagnosticsMutex);
\t\t++mRecvDiagnostics.recvScheduleCount;
\t\tmRecvDiagnostics.recvScheduleWaitTotalUs += us;
\t\tmRecvDiagnostics.recvScheduleWaitMaxUs = std::max(mRecvDiagnostics.recvScheduleWaitMaxUs, us);
\t}
\tRecvDiagnosticScope diagnosticScope{*this, startedAt};''', count=3)
    replace('src/impl/dtlstransport.cpp', 'void DtlsTransport::enqueueRecv() {',
            '''DtlsDiagnostics DtlsTransport::getDtlsDiagnostics() const {
\tauto result = mIncomingQueue.diagnostics();
\tstd::lock_guard lock(mDiagnosticsMutex);
\tresult.recvRuns = mRecvDiagnostics.recvRuns;
\tresult.recvScheduleCount = mRecvDiagnostics.recvScheduleCount;
\tresult.recvScheduleWaitTotalUs = mRecvDiagnostics.recvScheduleWaitTotalUs;
\tresult.recvScheduleWaitMaxUs = mRecvDiagnostics.recvScheduleWaitMaxUs;
\tresult.recvProcessingTotalUs = mRecvDiagnostics.recvProcessingTotalUs;
\tresult.recvProcessingMaxUs = mRecvDiagnostics.recvProcessingMaxUs;
\treturn result;
}

DtlsTransport::RecvDiagnosticScope::~RecvDiagnosticScope() {
\tconst auto us = uint64_t(duration_cast<microseconds>(steady_clock::now() - startedAt).count());
\tstd::lock_guard lock(transport.mDiagnosticsMutex);
\t++transport.mRecvDiagnostics.recvRuns;
\ttransport.mRecvDiagnostics.recvProcessingTotalUs += us;
\ttransport.mRecvDiagnostics.recvProcessingMaxUs = std::max(transport.mRecvDiagnostics.recvProcessingMaxUs, us);
}

void DtlsTransport::enqueueRecv() {''')


if __name__ == '__main__':
    apply(Path(__file__).resolve().parents[2])
