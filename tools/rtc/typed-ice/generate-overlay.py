#!/usr/bin/env python3
"""Generate the review overlay in a fresh, disposable pinned fixture."""
import difflib
import json
from pathlib import Path
import sys
import importlib.util
from fixture import prepare, sha

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sdk, evidence = prepare(ROOT, Path(sys.argv[1]).resolve())
old = {}

# Replay the poll evidence additively over the immutable approved baseline.
# The baseline itself remains unchanged and continues to gate every old byte.
for name in ['deps/libjuice/include/juice/juice.h', 'deps/libjuice/src/agent.h',
             'deps/libjuice/src/juice.c', 'deps/libjuice/src/conn_poll.c']:
    old[name] = (sdk / name).read_text()
old['deps/libjuice/src/udp_poll_clock.h'] = ''
poll_spec = importlib.util.spec_from_file_location('udp_poll', HERE.parent / 'patch-udp-poll-diagnostics.py')
poll_module = importlib.util.module_from_spec(poll_spec)
poll_spec.loader.exec_module(poll_module)
poll_module.apply(sdk.parents[2])
phase_spec = importlib.util.spec_from_file_location('udp_phase', HERE.parent / 'patch-udp-poll-phases.py')
phase_module = importlib.util.module_from_spec(phase_spec)
phase_spec.loader.exec_module(phase_module)
phase_module.apply(sdk.parents[2])


def edit(name, before, after):
    path = sdk / name
    text = path.read_text()
    old.setdefault(name, text)
    assert text.count(before) == 1, (name, before)
    path.write_text(text.replace(before, after))


header = 'include/rtc/icediagnostics.hpp'
old[header] = ''
(sdk / header).write_bytes((HERE / 'icediagnostics.hpp').read_bytes())
edit('CMakeLists.txt', '${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/dtlsdiagnostics.hpp',
     '${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/dtlsdiagnostics.hpp\n\t${CMAKE_CURRENT_SOURCE_DIR}/include/rtc/icediagnostics.hpp')
edit('include/rtc/peerconnection.hpp', '#include "dtlsdiagnostics.hpp"',
     '#include "dtlsdiagnostics.hpp"\n#include "icediagnostics.hpp"')
edit('include/rtc/peerconnection.hpp', '\tstring iceDiagnostics() const;', '''\tstring iceDiagnostics() const;
\t// Explicit potentially blocking query: call on a background diagnostics owner.
\t// After close, returns only the last successful typed observation (if any),
\t// marked FinalRetained. This is not a fresh observation of the closed transport.
\tIceDiagnostics getIceDiagnostics() const;
\t// Cold query: coherent summary plus bounded UDP batch history, one ICE lock.
\tIceUdpPollDiagnostics getIceUdpPollDiagnostics() const;''')
edit('src/peerconnection.cpp', 'string PeerConnection::iceDiagnostics() const {', '''IceDiagnostics PeerConnection::getIceDiagnostics() const {
\treturn impl()->getIceDiagnostics();
}

IceUdpPollDiagnostics PeerConnection::getIceUdpPollDiagnostics() const {
\treturn impl()->getIceUdpPollDiagnostics();
}

string PeerConnection::iceDiagnostics() const {''')
edit('src/impl/icetransport.hpp', '\tstring iceDiagnostics() const;',
     '\tstring iceDiagnostics() const;\n\tIceDiagnostics getIceDiagnostics() const;\n\tIceUdpPollDiagnostics getIceUdpPollDiagnostics() const;')
edit('src/impl/icetransport.hpp', '#include "global.hpp"',
     '#include "global.hpp"\n#include "rtc/icediagnostics.hpp"')
edit('src/impl/peerconnection.hpp', '\tstring iceDiagnostics() const;',
     '\tstring iceDiagnostics() const;\n\tIceDiagnostics getIceDiagnostics() const;\n\tIceUdpPollDiagnostics getIceUdpPollDiagnostics() const;')
edit('src/impl/peerconnection.hpp', '\tmutable string mFinalIceDiagnostics;', '''\tmutable string mFinalIceDiagnostics;
\t// Only explicit typed queries use these locks; teardown never takes them.
\t// Serialize live sampling, but never hold the cache lock across a juice call.
\tmutable std::mutex mTypedIceQueryMutex;
\tmutable std::mutex mTypedIceCacheMutex;
\tmutable IceDiagnostics mFinalTypedIceDiagnostics;
\tmutable IceUdpPollDiagnostics mFinalUdpPollDiagnostics;''')
edit('src/impl/peerconnection.cpp', 'string PeerConnection::iceDiagnostics() const {', '''IceDiagnostics PeerConnection::getIceDiagnostics() const {
\t// Closed reads bypass a possibly blocked live query. The shared transport
\t// reference below keeps the agent alive if close races with that query.
\tstd::unique_lock queryLock(mTypedIceQueryMutex, std::defer_lock);
\tif (state.load() != State::Closed)
\t\tqueryLock.lock();
\tif (state.load() != State::Closed) {
\t\tif (auto ice = getIceTransport()) {
\t\t\tauto snapshot = ice->getIceDiagnostics();
\t\t\tstd::lock_guard cacheLock(mTypedIceCacheMutex);
\t\t\tif (snapshot.availability == IceDiagnostics::Availability::Available) {
\t\t\t\tmFinalTypedIceDiagnostics = snapshot;
\t\t\t\tif (state.load() == State::Closed)
\t\t\t\t\tsnapshot.provenance = IceDiagnostics::Provenance::FinalRetained;
\t\t\t}
\t\t\treturn snapshot;
\t\t}
\t}
\tstd::lock_guard cacheLock(mTypedIceCacheMutex);
\tauto snapshot = mFinalTypedIceDiagnostics;
\tif (snapshot.availability == IceDiagnostics::Availability::Available) {
\t\tsnapshot.provenance = IceDiagnostics::Provenance::FinalRetained;
\t} else {
#if !USE_NICE
\t\tsnapshot.backend = IceDiagnostics::Backend::LibJuice;
\t\tsnapshot.availability = state.load() == State::Closed
\t\t    ? IceDiagnostics::Availability::ClosedWithoutSample
\t\t    : IceDiagnostics::Availability::NotStarted;
#else
\t\tsnapshot.backend = IceDiagnostics::Backend::LibNice;
\t\tsnapshot.availability = IceDiagnostics::Availability::NotSupported;
#endif
\t}
\treturn snapshot;
}

IceUdpPollDiagnostics PeerConnection::getIceUdpPollDiagnostics() const {
\tstd::unique_lock queryLock(mTypedIceQueryMutex, std::defer_lock);
\tif (state.load() != State::Closed) queryLock.lock();
\tif (state.load() != State::Closed) {
\t\tif (auto ice = getIceTransport()) {
\t\t\tauto snapshot = ice->getIceUdpPollDiagnostics();
\t\t\tstd::lock_guard cacheLock(mTypedIceCacheMutex);
\t\t\tif (snapshot.ice.availability == IceDiagnostics::Availability::Available) {
\t\t\t\tmFinalUdpPollDiagnostics = snapshot;
\t\t\t\tmFinalTypedIceDiagnostics = snapshot.ice;
\t\t\t\tif (state.load() == State::Closed)
\t\t\t\t\tsnapshot.ice.provenance = IceDiagnostics::Provenance::FinalRetained;
\t\t\t}
\t\t\treturn snapshot;
\t\t}
\t}
\tstd::lock_guard cacheLock(mTypedIceCacheMutex);
\tauto snapshot = mFinalUdpPollDiagnostics;
\tif (snapshot.ice.availability == IceDiagnostics::Availability::Available) {
\t\tsnapshot.ice.provenance = IceDiagnostics::Provenance::FinalRetained;
\t} else {
#if !USE_NICE
\t\tsnapshot.ice.backend = IceDiagnostics::Backend::LibJuice;
\t\tsnapshot.ice.availability = state.load() == State::Closed
\t\t    ? IceDiagnostics::Availability::ClosedWithoutSample
\t\t    : IceDiagnostics::Availability::NotStarted;
#else
\t\tsnapshot.ice.backend = IceDiagnostics::Backend::LibNice;
\t\tsnapshot.ice.availability = IceDiagnostics::Availability::NotSupported;
#endif
\t}
\treturn snapshot;
}

string PeerConnection::iceDiagnostics() const {''')

pairs = [('state', 'state'), ('udpBudgetYields', 'udp_budget_yields'),
         ('udpDatagrams', 'udp_datagrams'), ('udpMaxBatch', 'udp_max_batch'),
         ('recvErrno', 'recv_errno'), ('pollFlags', 'poll_flags'),
         ('socketError', 'socket_error'), ('socketErrorAvailable', 'socket_error_available'),
         ('consentExpired', 'consent_expired'), ('stunTimeouts', 'stun_timeouts'),
         ('stunSendFailures', 'stun_send_failures'), ('stunErrorResponses', 'stun_error_responses'),
         ('stunErrorCode', 'stun_error_code'), ('stunSendErrno', 'stun_send_errno'),
         ('connectivityTimeouts', 'connectivity_timeouts')]
reasons = [('NONE', 'None'), ('UDP_RECV', 'UdpRecv'), ('UDP_POLL', 'UdpPoll'),
           ('CONNECTION', 'Connection'), ('CONSENT_EXPIRED', 'ConsentExpired'),
           ('STUN_TIMEOUT', 'StunTimeout'), ('STUN_SEND', 'StunSend'),
           ('STUN_RESPONSE', 'StunResponse'), ('CONNECTIVITY_TIMEOUT', 'ConnectivityTimeout')]
getter = '''IceDiagnostics IceTransport::getIceDiagnostics() const {
\tIceDiagnostics snapshot;
#if !USE_NICE
\tsnapshot.backend = IceDiagnostics::Backend::LibJuice;
\tjuice_diagnostics_t d{};
\tif (juice_get_diagnostics(mAgent.get(), &d) != JUICE_ERR_SUCCESS) {
\t\tsnapshot.availability = IceDiagnostics::Availability::ReadFailed;
\t\treturn snapshot;
\t}
\tsnapshot.availability = IceDiagnostics::Availability::Available;
\tsnapshot.provenance = IceDiagnostics::Provenance::Live;
'''
for public, juice in pairs:
    getter += f'\tsnapshot.{public} = ' + (f'static_cast<unsigned int>(d.{juice})' if public == 'state' else f'd.{juice}') + ';\n'
getter += '\tswitch (d.failure_reason) {\n'
for juice, public in reasons:
    getter += f'\tcase JUICE_FAILURE_{juice}: snapshot.failureReason = IceDiagnostics::FailureReason::{public}; break;\n'
getter += '''\tdefault: snapshot.failureReason = IceDiagnostics::FailureReason::Unknown; break;
\t}
#else
\tsnapshot.backend = IceDiagnostics::Backend::LibNice;
\tsnapshot.availability = IceDiagnostics::Availability::NotSupported;
#endif
\treturn snapshot;
}

'''
edit('src/impl/icetransport.cpp', 'string IceTransport::iceDiagnostics() const {',
     getter + 'string IceTransport::iceDiagnostics() const {')

# The rich getter uses the same summary conversion with a single lock/query.
rich = getter.replace('IceDiagnostics IceTransport::getIceDiagnostics()',
                      'IceUdpPollDiagnostics IceTransport::getIceUdpPollDiagnostics()')
rich = rich.replace('\tIceDiagnostics snapshot;', '\tIceUdpPollDiagnostics result;\n\tauto &snapshot = result.ice;')
rich = rich.replace('\tjuice_diagnostics_t d{};', '\tjuice_diagnostics_t d{};\n\tjuice_udp_poll_diagnostics_t p{};')
rich = rich.replace('juice_get_diagnostics(mAgent.get(), &d)',
                    'juice_get_udp_poll_diagnostics(mAgent.get(), &d, &p)')
rich = rich.replace('\treturn snapshot;', '\treturn result;')
poll_pairs = [('clockAvailable', 'clock_available'), ('sampleClockAvailable', 'sample_clock_available'),
              ('lastClockAvailable', 'last_clock_available'), ('pendingYield', 'pending_yield'),
              ('rcvbufChecked', 'rcvbuf_checked'), ('rcvbufAvailable', 'rcvbuf_available'),
              ('rcvbufBytes', 'rcvbuf_bytes'), ('rcvbufErrno', 'rcvbuf_errno'),
              ('sampleMonoUs', 'sample_mono_us'), ('clockInvalid', 'clock_invalid'),
              ('batches', 'batches'), ('bytes', 'bytes'), ('timedBatches', 'timed_batches'),
              ('elapsedTotalUs', 'elapsed_total_us'), ('elapsedMaxUs', 'elapsed_max_us'),
              ('lastStartMonoUs', 'last_start_mono_us'), ('lastEndMonoUs', 'last_end_mono_us'),
              ('serviceGaps', 'service_gaps'), ('serviceGapMaxUs', 'service_gap_max_us'),
              ('readyDelays', 'ready_delays'), ('readyDelayMaxUs', 'ready_delay_max_us'),
              ('eventsTotal', 'events_total'), ('eventsOverwritten', 'events_overwritten'),
              ('eventCount', 'event_count'),
              ('legacyEligibleBatches', 'legacy_eligible_batches'),
              ('phaseSampleEvery', 'phase_sample_every'),
              ('phaseSampledBatches', 'phase_sampled_batches'),
              ('phaseTimedBatches', 'phase_timed_batches'),
              ('phaseInvalidBatches', 'phase_invalid_batches'),
              ('phaseSampledDatagrams', 'phase_sampled_datagrams'),
              ('phaseTimedDatagrams', 'phase_timed_datagrams'),
              ('phaseSampledReceiveCalls', 'phase_sampled_receive_calls'),
              ('phaseTimedReceiveCalls', 'phase_timed_receive_calls'),
              ('phaseRecvTotalUs', 'phase_recv_total_us'),
              ('phaseRecvBatchMaxUs', 'phase_recv_batch_max_us'),
              ('phaseCallbackTotalUs', 'phase_callback_total_us'),
              ('phaseCallbackBatchMaxUs', 'phase_callback_batch_max_us')]
copy = ''.join(f'\tresult.{public} = p.{backend};\n' for public, backend in poll_pairs)
copy += '''\tfor (unsigned int i = 0; i < result.eventCount && i < result.events.size(); ++i) {
\t\tconst auto &e = p.events[i];
\t\tresult.events[i] = {e.sequence, e.start_mono_us, e.elapsed_us, e.bytes,
\t\t    e.service_gap_us, e.ready_delay_us, e.datagrams, e.flags, e.exit_reason,
\t\t    e.phase_recv_us, e.phase_callback_us};
\t}
'''
rich = rich.replace('\tswitch (d.failure_reason)', copy + '\tswitch (d.failure_reason)')
edit('src/impl/icetransport.cpp', 'string IceTransport::iceDiagnostics() const {',
     rich + 'string IceTransport::iceDiagnostics() const {')

patch = []
inventory = {}
for name, before in sorted(old.items()):
    after = (sdk / name).read_text()
    patch.append(f'diff --git a/{name} b/{name}\n')
    if not before:
        patch.append('new file mode 100644\n')
    patch.extend(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                 fromfile='a/' + name if before else '/dev/null', tofile='b/' + name))
    inventory[name] = {'approved_sha256': evidence['baseline_files'].get(name),
                       'candidate_sha256': sha(sdk / name)}
(HERE / 'typed-ice.patch').write_text(''.join(patch))
(HERE / 'candidate.json').write_text(json.dumps({
    'schema': 1, 'series_sha256': evidence['series_sha256'],
    'overlay_sha256': sha(HERE / 'typed-ice.patch'), 'files': inventory}, indent=2) + '\n')
print('Overlay generated in', HERE, 'using private fixture', sdk)
