# Typed telemetry and bounded writer records

T1 moves the real Stream transport, Input latency and Playback timing logging
paths to immutable numeric copies formatted on the existing RunLog writer.
The T1 contract below is also the foundation for network, Media and UI records.
Later slices use the same writer and accounting rules; their integration and
verification are tracked separately. See `typed-network-telemetry.md` for the
network boundary. Completing a producer does not establish app/device acceptance.

`stream::StreamTelemetrySource::captureTelemetry(Capture)` is an application
owner call. Both production MTS and FogHub adapters implement it. The default
implementation exists for source-compatible older fakes and explicitly reports
NotCollected/NotMigrated, never an available zero snapshot. Data consists only of
fixed numeric/enumeration values. Scope distinguishes transport lifetime, MTS
transport rolling five seconds and FH client rolling five seconds. Capture and
completion timestamps use steady microseconds; durations retain their field
units. A process-local stream epoch and transport-start epoch are independent of
video generation. They contain no rental, account or endpoint identifiers.

MTS copies owner counters, the existing short control sender snapshot and the
existing ICE worker timing state; it does not call ICE/RTCP SDK queries. A
control sender that has not been created is not yet published. Its final
snapshot is retained numerically; closed input without a retained histogram is
explicitly unavailable. FH copies atomic timing counters, command values under
State::mutex and the atomics-only core diagnostics for its own protocol lease.
Before successful core start the core group is unavailable; after completed stop
it uses the final value already retained by the adapter. Early failed starts
cannot expose earlier core receive counters. Atomic field groups are approximate
cross-field observations, not transactions. FH input retains its original
client rolling window across reconnect.

The central pure formatter receives only copies. Compatibility getters use the
same formatter synchronously; application logging uses deferred records and
never calls those getters. Available legacy prefixes, fields, order and units
are preserved, including special final/closed prefixes. Previously unavailable
FH core data is emitted as `core_unavailable=...`, without invented measured
zeros. Existing B1 event strings and the schema=1 steady clock anchor are
unchanged. A record freezes its startup-relative prefix before enqueue; writer
delay does not rewrite event time. Periodic window resets still happen after
capture, at the existing frequency. No duplicate dual emission is added.

RunLog admits prepared lines and immutable deferred records through one FIFO.
The limits are 512 resident declared lines (including writer in-flight entries),
8192 bytes per line and a shared 256 KiB
admission budget. Deferred raw records are limited to 16 KiB, with at most four
outstanding records, including the writer's in-flight records. Their charge is
raw object bytes plus declared worst rendered bytes and newline, with at least
64 rendered bytes reserved for a fixed failure notice. Prepared-line bytes also
remain charged during the writer batch. The budget describes admitted payload
storage; standard container/allocation overhead and temporary batch storage are
not a separate performance/memory measurement.

Concrete T1 records contain only DTO values and a numeric prefix time. Domain
snapshots and records have <=4 KiB static assertions on host and target. The
bounded sink rejects oversized output and embedded CR/LF before append. Formatter
exceptions, including bad_alloc, produce a fixed safe failure notice and drop
counters; they do not expose exception text or stop subsequent records. General
writer/I/O allocation failures retain the existing sticky writer failure path.
Factory/capture allocation failure is accounted as a rejected deferred record;
the existing writer emits its drop notice. The noexcept producer catch does not
allocate an additional telemetry-capture notice. If batch newline growth fails,
the complete deferred payload is rolled back before the fixed failure notice,
preserving earlier complete lines.
Flush, requestFlush and close include deferred records and associated notices.
Stats expose accepted/dropped deferred counts, format failures, resident charge,
outstanding count, maximum residence and formatting elapsed microseconds.

Playback owners can construct `stream::telemetry::Playback` directly or use
`capturePlayback(...)` in `application/playback_telemetry.hpp` with eight existing
calls/totalUs/maxUs timing values, a copied RunLog Stats value and the ten owner
counters. `fogplay::diagnostics::diagnosticPlayback(Playback)` enqueues the owned
value; `playbackText` renders compatibility text. No UI/provider pointer crosses
this boundary.

Run focused checks without downloading or rebuilding dependencies:

```sh
bash tools/test/telemetry.sh
bash tools/test/telemetry-providers.sh
```

The first runs ASan/UBSan bounded/deferred/golden/application bridge tests,
existing RunLog and app diagnostics tests, B1 readers and architecture checks.
The second compiles actual MTS/FH adapters against existing local test fixtures,
checks live/final/reconnect/early failure and blocked worker capture, and runs
the unchanged full FH production-session fixture in full/control/encryption
variants. It links prepared RTC host archives directly and fails with setup
instructions if absent; normal integration must separately verify those archive
manifests. It does not refresh recipes, dependencies or installed headers.

Host/target syntax success does not establish Switch CPU/frame stability, SD
latency, or encryption acceptance. Full link/build and device comparison remain
separate integration/hardware gates. Remaining D2 integration requires its own
whole-tree validation.

## Multiline records and diagnostic profiles

A record defaults to one line. Explicit batches declare 1..128 lines and at
most 65536 rendered bytes including inter-line separators; the per-line 8192
limit and raw 16 KiB limit remain unchanged. Only `nextLine()` emits a separator.
The sink is non-copyable and non-movable, so a formatter cannot replace its
frozen limits or remove its payload. `finish()` requires the exact declared
line count. Formatting/validation failure rolls back the complete record, not
only its last line; prior FIFO records remain intact.

Accepted/written/dropped line counters count declared payload lines, while
accepted/dropped deferred and formatFailures count records. Internal notices
are not written payload lines. False admission can also mean accepted with an
unhealthy sink: never retry an immutable event merely because append returned
false. A producer must check its whole cycle's raw+rendered byte reservations,
not just the four-record limit. Final stream records must be enqueued before
the next stream marker; do not defer them into a later session's frame.

`DIAGNOSTICS_PROFILE=essential|standard|deep` selects a separate validated
Makefile option; default is standard. It does not change streaming policy or
the STAGE1 feature mask. Essential retains errors, lifecycle, required events
and finals. Standard retains periodic detail. Deep enables additional exact
owner-local stage distributions (five-second window, capacity512, nearest rank).
Disabled distributions have no sample rings and explicitly report NotCollected;
they are not measured zeros. Existing bucketed loop and NACK percentiles retain
their original algorithms and units. The startup profile marker lets readers
reject inadequate comparison runs without inferring a profile for older logs.

Additional focused suites are `run-log-batches.sh`, `ui-telemetry.sh` and
`media-telemetry.sh` under tools/test; the common all.sh includes them.
