# Typed network telemetry: numeric owner snapshots and deferred records

This checkpoint connects bounded numeric owner caches to both production
adapters and pure deferred network, protocol and discovery records. Application
batch scheduling and final admission remain separate integration gates. RunLog now
also supports bounded multiline records; see `typed-telemetry.md` for its limits.
This producer checkpoint alone does not complete D2.

`domain/network_telemetry.hpp` contains numeric RTP/NACK/expiry, repair payload
configuration, incoming control counters/schema totals, server performance,
transport history and RTCP DTOs. No strings, packet/source identifiers, JSON,
SDK handles or owners enter these values. Existing legacy owner formatters and
protocol/window algorithms remain in place. Snapshot getters require the same
owner serialization as their existing classes and do not consume events,
prune history, advance deadlines or reset counters.

`H264RtpAssembler::numericSnapshot()` copies the existing counters, bounded
queues, request/history sizes, unchanged 10-ms arrival histogram and existing
p50/p95 results, frozen gap expiry and late-arrival evidence. Zero samples and
unknown RTT/last-expiry are explicit. It does not invent exact percentiles or
reinterpret NACK correlation as network RTT. `RtpRepair::numericSnapshot()`
exports the fixed PT range 0..127 plus total/out-of-range configuration counts;
FID and source-known are booleans, without source SSRC identifiers.

IncomingControl and IncomingControlDiagnostics expose numeric counters/pending
state without descriptions or remote strings. ServerPerformance exposes the
same eleven allowlisted fields, known/sample time, ageKnown/stale, the existing
strict `age > 5000ms` boundary, Percent for CPU/GPU and RawUnspecified for the
other reported values. TransportHistory exports the same oldest-first 16-entry
ring, first terminal event and explicit age-known flags (known zero differs
from unknown).

`SnapshotSlot<T>` is a separate short value-copy mutex: <=16 KiB trivially
copyable payload, epoch/generation/revision/source time, NotPublished/Live/
FinalRetained/CaptureFailed/ClosedWithoutFinal. Reset invalidates previous
values; old generations, out-of-order publications and updates after final are
rejected. Source time zero is a valid known time. Failed capture can retain the
previous value but marks it stale. Reads never invoke a producer/SDK callback.

`AssemblyTelemetryCache` publishes real H264/repair copies from the assembly
owner, at most once per second plus forced final. It retains no owner
reference. Its paired DTO is bounded to 8 KiB. UI reads use only the cache lock.
The actual assembly worker now publishes at startup, maintenance and exit.
After join and callback sealing, the application owner makes the final retained
copy. Numeric control/history copies use their existing owner serialization.
UI capture wakes the existing assembly worker and immediately returns the
previous publication; it does not acquire the assembly lock or call the SDK.

RtcpFeedbackWorker now aggregates the SDK numeric feedback only on its existing
worker, without holding the publication/state mutex during SDK calls. Cache
publication runs at most every 250ms and once on worker exit; no new thread is
added. `numericSnapshot(nowUs)` and legacy `snapshot()` perform only cached value
copies, with explicit source time/age/stale/final/early state. NumericRead also
copies current worker-start/running/stop/activity flags separately; worker
activity includes query/bookkeeping and is not proof of an actual send.
Feedback inFlight is the observed value at the cache timestamp, not a current
send acknowledgement. Optional legacy metadata keys make that cache boundary
visible while preserving all existing field names and units.

One deliberate lifecycle boundary remains: `quiescent()` retains its existing
SDK inFlight safety probe. `requestStop()` retains existing immediate receiver
sealing/discard calls. Replacing the safety probe with cached telemetry could
permit a retry while a send is blocked, or forbid a safe retry due only to
worker bookkeeping. Moving that lifecycle probe requires separate protocol
coordination; this checkpoint does not claim all off-worker SDK access was
removed. Sent means local callback return, never remote/UDP acknowledgement.

Run focused checks without setup or dependency rebuild:

```sh
bash tools/test/telemetry-network.sh
# After the normal reviewed RTC host dependency setup:
bash tools/test/telemetry-network.sh --rtc
```

The script runs ASan/UBSan numeric/cache tests, blocked SDK/flush RTCP worker
checks through a dedicated fake SDK boundary, six unchanged owner suites,
existing B1 readers and architecture checks. The --rtc option also runs the unchanged actual RTC feedback suite
against already prepared host archives and fails with setup instructions if
absent. Switch syntax remains a separate integration check against reviewed
prepared artifacts. This script never downloads, rebuilds or publishes dependencies.

## Adapter capture and writer handoff

`domain/network_snapshot.hpp` defines `StreamNetworkTelemetrySource`, inherited
by the existing telemetry aggregate. `captureNetworkTelemetry() const noexcept`
returns a trivially copyable `NetworkSnapshot` with per-group Observation:
provider, stream/transport epoch, video generation, source revision, capture
steady microseconds, known age, stale/live/final/unpublished state. Capture time
and source time differ; cached values retain original timestamps. MTS quality
values are copied from the application owner with per-field known/age flags.
Its existing auto-quality numeric getter does not change policy algorithms.

MTS captures assembly/RTP/repair/queues, incoming control/schema totals, server
performance, history, RTCP and the existing numeric DTLS cache. DTLS uses only
the value already published by the ICE worker: no SDK access on UI capture.
It retains the SDK's original transport-lifetime counters, source ICE revision
and timestamp. Available=false cannot distinguish unsupported from not sampled;
the DTO reports unpublished/closed-without-final, rather than invented zeros.

FH captures the existing atomic adapter counters and numeric core diagnostics.
Core clock and adapter steady clock are independent; known flags validate core
age ordering. Live sampling requires the running core; final data comes from
the retained stop result, and failed restart clears previous core samples.
No encryption negotiation, crypto primitives or pinned core sources changed.
The outer FH event context is not asserted to bind historical SDK events to a
current transport: authoritative core connection identity is retained separately.

`application/network_diagnostics_formatter.hpp` is pure from owned values:
`Line`, `networkText`, `NetworkRecord(elapsedMs, Line, const NetworkSnapshot&)`
and `EventRecord(elapsedMs, NetworkEvent, final)`. Records retain no source,
SDK/UI pointer, callback or clock. Prefixes, legacy field keys and units remain;
new availability/journal metadata is additive. Per-group text clamps remain
2048/4096 bytes. DTLS previously allowed 8192, but its complete maximum-width
numeric representation is proved below 4096 by the parity test; no valid DTLS
text is lost. The record header uses the caller's frozen B1 elapsed timestamp.

`application/network_telemetry_logging.hpp` exposes:

- `enqueueNetwork(log, frozenElapsedMs, Line, const NetworkSnapshot&) noexcept`
  returns healthy local admission, not durable delivery. False can mean rejection
  or admitted-with-unhealthy-sink; never retry it as if definitely rejected.
- `drainNetworkEvents(source, log, frozenElapsedMs, budget=4, final=false)` returns
  accepted/uncertain/sourceOverwritten. The caller can pass budget=1 per frame.
- `discardNetworkEvents(source, log, budget=32)` explicitly accounts final drops.

A capacity-32 numeric journal preserves order, cursor and cumulative missed /
uncertain counts. Peek does not consume; only actual append admission determines
retirement accounting. Failed/uncertain admission retires once and stops, so it
cannot produce duplicates on retry. Concurrent overwrite is explicit. MTS RTP
producers no longer allocate/format event strings. FH requires
`enableNetworkEvents(true)` before start: this switches the real core journal
consumer to numeric records; default legacy mode remains compatible and never
emits both versions. Fixed MTS route/quality/transport/settings/scale events are
also numeric. `pollUnmigratedNetworkEvent` now returns false; it has no remaining
MTS transition string producer. `networkEventText(event, final)` dispatches owned
RTP/core/route/quality values with the existing B1 prefixes and field names.
The old `pollVideoDiagnostic` remains a pure-format compatibility aggregate and must not be
used in parallel with typed drain.

The journal also supports a single locked bounded prefix copy through
`peekNetworkEvents(out, capacity)`. The app can enqueue that immutable prefix as
one multiline record, then retire each sequence with the actual admission
outcome. An intervening producer overflow must not retire a newer event outside
that prefix. Final records enter the FIFO before any new stream marker, rather
than being carried into a later frame of another stream. Global thread-history
records are distinct from per-stream finals. Both record and total byte budgets
must be respected by each producer cycle; writer backlog still has explicit
nonblocking drop accounting.

## Bounds and validation

NetworkSnapshot <=8192 bytes, NetworkRecord <=8192, Event <=504 and EventRecord
<=1024 have compile-time checks. The current host measurements are NetworkSnapshot
7872B, ProtocolSnapshot2496B, NetworkEvent488B and ControlDiscoveryEvent2168B.
Thirty-two network events occupy15616B before the small batch-record header;
four control discovery events occupy8672B before their header. App records must
also satisfy the shared16KiB raw-record cap. Exact sizeof values change when numeric groups
are added and are checked by the target/host fixtures. Actual RunLog charge
includes raw + declared worst output + newline. Existing shared limits remain 4 resident
deferred records and 256KiB, including in-flight records. The tests block the
real formatter and assert actual resident accounting, full rejection, FIFO
flush and owner-independent lifetime. Persistent allocation failure is tested
with real failing operator new; existing allocation-safe drop accounting is used.

Run `bash tools/test/network-telemetry.sh` against prepared RTC archives only.
It runs real MTS and FH producer/cache/final/reconnect tests, ASan/UBSan formatter
and journal tests, allocation-failure tests, exact numeric/legacy parity, actual
typed-record bytes through unchanged B1 readers and architecture checks. No
implicit download/build/publication occurs. `telemetry-network.sh --rtc`,
`telemetry-providers.sh` and `stream-ports.sh` remain complementary regression
gates. Target syntax is separate; host success is not physical device acceptance.

## Protocol, mutex and input snapshot

`domain/protocol_telemetry.hpp` defines `ProtocolSnapshot` (<=4096B) and
`StreamProtocolTelemetrySource`; the existing network telemetry source inherits
this focused base. `captureProtocolTelemetry() const noexcept` returns primitive
copies only. `application/protocol_diagnostics_formatter.hpp` exposes pure
`protocolText`, `mutexText`, `inputText` and `controlDeliveryText`. Unpublished
and failed capture emit explicit unavailable text; live/final output carries
state and original source-clock metadata. No owner, callback, SDK/UI pointer or
clock query enters a deferred record.

MTS exports route candidate/protocol/proof enums, selection/ICE-worker and
signaling counters, control sender timings, peer/receive ages, bounded event
queue/history/incoming counters, control buffer/reliability/SCTP RTT observations,
and every PS touch/native/input counter. Reliability is copied numerically in
the existing data-channel callback, never parsed from old diagnostics text.
Ordinary capture follows the existing event -> telemetry lock order. Assembly
publishes completed video TimedMutex scopes while already holding its Guard;
UI capture only copies that separate cache. Final teardown, after assembly join
and callback sealing, retains the last completed scopes under the established
video lock before Shared destruction. The current Guard is excluded, preserving
completed-scope counter semantics. Instrumentation-disabled and unpublished
video mutex values remain explicit through enabled/known fields.

FH exports current/first-failure command status, operation/attempt/curl/http/XML/
ack/elapsed fields, adapter negotiation/request/input/termination fields, queue
lock timings and all encryption negotiation/observation counters. The existing
session worker refreshes its numeric encryption publication at the existing
one-second journal deadline. Capture only copies this publication under
State::mutex. Failed start can legitimately retain negotiation failure evidence
while media core diagnostics remain unavailable; new start clears prior values
and clocks. Atomic adapter fields are approximate independent observations.

Detailed ICE uses the reviewed numeric SDK API only on the existing ICE worker;
no legacy string parsing or SDK call occurs on its capture/formatter. Backend,
availability and provenance are distinct, and signed errors remain signed.
Failed reads retain the prior successful clock. An SDK FinalRetained result
preserves a known live source clock; a first retained-only result has explicit
unknown age. Dedicated ICE/final groups preserve numeric key names without
repeating the detailed counters in the protocol line.

Auto-quality policy events now use a fixed32-entry numeric ring and pure
compatibility formatter. Policy decisions, windows, counters and resets are
unchanged. Route capacity16/drop accounting and quality capacity32 buckets stay
bounded; application-owner peeks move their immutable numeric events into the
existing journal. Each transition union member is selected with an explicit
placement-new setter before trivial copy. RTP/route/fixed events retain the video
generation captured by their producer. Deferred auto-quality metrics have explicit
`videoBindingKnown=false` and generation0 rather than claiming the generation
current when a later peek publishes them. Transport epoch binding remains known
within the same adapter generation. Quality metrics originate from
uint64 deltas and millisecond windows; a conservative finite <=1e24 bound and
explicit invalid-metrics marker prevent corrupt DTO values from silently
truncating terminal fields. Original field formatting remains for valid values.

## Control discovery schema2

`domain/control_discovery.hpp` describes fixed enum keys/ids and bounded numeric
schema/sample tokens, never payload bodies or arbitrary remote strings.
`IncomingControlDiagnostics` builds these directly from the original Json under
the existing depth/object-budget/array and scalar whitelist rules. Known rumble
and FEC schemas retain golden field/value parity. Unknown ids/key names become
`unknown` plus `unknownNames`; values under unknown keys remain type-only.
This deliberately tightens privacy and may merge formerly distinct arbitrary
schema names. It is explicitly revision2, not hidden byte parity. No dynamic
per-server dictionary or hash of a remote identifier crosses into a record.
Source token clipping and writer640-character clipping are separately exposed
as `tokensTruncated` and `formatterTruncated`.

MTS implements `supportsControlDiscoveryTelemetry()`,
`peekControlDiscoveryEvents(out, capacity, final)` and
`retireControlDiscoveryEvent(sequence, admitted)`. A peek freezes count/sample/
first/last/time/sequence. Retirement advances reported only through that frozen
count; newer arrivals remain pending. False admission is retired once with
explicit uncertainty/discard count. The first-three/5s rate rule, coalescing,
32-schema overflow summary and force-final behavior remain. Before destruction,
the adapter retains a numeric bucket copy. Compatibility `pollControlDiagnostic`
uses the same peek/pure-format/retire path and must not run beside typed drain.

The app uses bounded immutable discovery batches; actual append admission is
authoritative. It schedules periodic discovery within the existing four-record
frame budget and explicitly attempts/discards final pending prefixes before a
new stream marker. Those app scheduling/profile guarantees have their own tests.
Run `bash tools/test/protocol-telemetry.sh` for numeric protocol boundaries,
exact mutex/buffer parity with instrumentation0/1, source discovery ASan/UBSan
privacy/admission/final tests, and the unchanged auto-quality policy suite.
The actual MTS adapter regression also sends real discovery records through
RunLog and checks persisted canaries.

## Remaining acceptance

SDK builds/manifests, final combined application tests and Switch syntax/link
are separate gates. Additional network stage distributions, media/UI acceptance
and diagnostic profile integration must be assessed by their owning slices.
Existing p50/p95/p99/window semantics are unchanged; averages/maxima are never
relabeled as percentiles. No physical Switch performance, blocked-SD budget,
TSan, successful on-device encryption or streaming stability acceptance follows
from host snapshot/formatter tests.
