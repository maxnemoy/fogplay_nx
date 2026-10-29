# Isolated Nintendo Switch Moonlight dependencies

IDR recovery diagnostics use a 256-entry best-effort journal, drained by the
session worker into individual `Moonlight IDR` lines. `connection`, `wait`,
`seq`, `core_us` and `frame` join requests, reliable-control dispatch/results,
received IDR hints, intact FEC frames, depacketizer recognition, wait exit and
complete IDR assembly. Wait-exit `requests`, `intact` and `fecFailedBlocks` are
per-wait counters, including intact frames discarded while waiting. Failed
blocks mean abandonment under the existing FEC policy, not network packet loss;
detailed FEC events are emitted only while a wait is active. The initiating
loss remains visible in the independent cumulative FEC counters.

`idr-receive-hint` observes the first data packet before FEC completion, using
the host's explicit frame type or an H.264 SPS / HEVC VPS/IDR NAL prefix. Its
`value` is the advertised FEC block count. A lost first packet or unsupported
prefix prevents identification; absence of a hint cannot establish that no IDR
was sent. While idle, only the latest raw IDR hint is retained silently;
recognition of that frame, or assembly of the same or a newer IDR, clears it.
A later wait entry publishes the retained hint with its original time and
`wait=0`; retention does not prove that the hinted frame remained incomplete.
Completed idle IDRs do not
produce detailed receive/recognition/assembly lines. Join hints/failures by
connection and frame as well as wait, since a
frame gap can open a wait after the raw first packet arrived. Duplicate first
packets can produce duplicate hints. A control success establishes local
transport acceptance, not host receipt or a causal response to that request.

The pinned core releases its wait at IDR recognition before complete assembly;
`wait-exit` is therefore not decoding or presentation success. Core timestamps
share `LiGetMicroseconds()` and may jump with its clock; use sequence/IDs and
reject backward duration samples. A single atomic transition token publishes
both active state and unique wait identity. Totals belong to that identity in
64 bounded keyed records under the journal's nonblocking gate: a delayed event
cannot reset or increment a later wait's totals. Dispatch results retain the
dispatch's original wait even if it finishes after another wait has started.
`totalsValid=0` means no wait, or the keyed record was already replaced by 64
newer waits. Entry/exit records can appear after observations from another
thread; their stable wait IDs retain the association.

Totals count admitted journal observations. Receive/control threads never wait,
allocate or write logs. `journal missed=N` reports overwritten entries or
producer contention. Rejected admission can undercount a summary; overwriting
an admitted event removes chain detail while its keyed count remains intact.
The associated cumulative recovery counters remain independent and unchanged.
Incomplete event chains with missed records cannot prove
an IDR was damaged. Thread starts are observed after existing scheduling hints
and logged separately as `Moonlight thread`, including query failure bits.
Thread events have dedicated signed core/ideal/priority fields, an affinity
mask and query error bits; they never reuse IDR frame/counter fields.

The raw hint's copied header-size policy is guarded by an exact SHA-256 contract
against the canonical pinned depacketizer block. Preparation fails before edits
if its boundaries or content change; review both implementations before
updating the contract. The test mutates a canonical size branch and verifies
that instrumentation rejects it without modifying the source.

`DECODE_UNIT.receiveTimeUs` becomes the adapter packet's first-receipt timestamp
by subtracting validated core receipt age from the local steady clock. This
preserves age across clock epochs; zero/future/underflow timestamps remain
unavailable. It is a local receive-to-present measurement, not host latency.

Run `tools/moonlight/build.sh` from WSL with devkitPro installed. This fetches
only the required upstream submodules, prepares the GameStream HTTP client,
builds four static libraries, and links a complete Switch smoke executable.
It does not start a connection, build FogPlay, install software, or use rentals.
Build the app's RTC dependencies first: both transports must share its exact
mbedTLS version and `tools/rtc/mbedtls-user-config.h` ABI. The dependency cache
checks archive hashes and this configuration before the main application build.

The output prefix is the ignored `third_party/moonlight/switch/` directory.
Link `gamestream`, `moonlight-common-c`, `enet`, `moonlight-curl`, `expat`, `mbedtls`,
`mbedx509`, `mbedcrypto`, `z`, and `nx`, in that order. Include `include/`,
`include/gamestream/`, and `include/crypto/` from that prefix. The build uses
the installed devkitPro Switch portlibs, except for private curl 8.15.0 built
with the app's bundled mbedTLS 3.6.4. System switch-curl uses libnx TLS, which cannot load the PEM
client identity. No global portlib is replaced. The manifest records source
revisions, tool hashes, compiler version,
and portlib/library SHA256 hashes. It is a reproducible source recipe with
an explicit installed-toolchain boundary; it does not provision portlibs.
Static archives use deterministic member metadata (`ar D` and `ranlib -D`).

The private curl archive exports only `fog_moonlight_`-prefixed symbols.
`namespace-curl.py` derives an exact map from every external definition,
including curl's internal `Curl_*` and `curlx_*` functions and data. It renames
the matching definitions/references with devkitA64 `objcopy` and applies the
same map to the GameStream archive after linking it. References to libc,
mbedTLS and other shared dependencies retain their original names. New
unexpected exports or unmapped curl references fail the build. The map and
its hash are recorded in the output prefix and curl manifest.

Applications can keep their existing portlibs `curl` for libnx TLS and cookies
while linking the isolated Moonlight transport. Do not call private curl via
the unmodified `curl/curl.h` headers; use the GameStream interface. Do not pass
curl handles or call global cleanup across the two implementations. The build
prefix includes private curl headers: compile existing app HTTP sources with
the portlibs include directory before the Moonlight prefix, so they retain
the header version matching their original curl archive. Only GameStream
compilation puts the private curl headers first. The coexistence smoke
explicitly follows this include order for its app-curl translation unit.
The build
also links `moonlight-curl-coexist-smoke.elf` with both archives and checks for
both `curl_easy_init` implementations, the original cookie implementation,
both TLS backends and no undefined symbols. The smoke executables are link
proofs only and are never run by the build.
The main Makefile names all four Moonlight archives by their full paths.
In particular the forked `libenet.a` must never resolve to the portlibs ENet:
the pinned core requires its extended address helpers. The coexistence smoke
uses the main application's library search order while its exact CMake target
references keep the Moonlight archives bound to the private prefix.

## Sources

| Repository | Commit | License |
| --- | --- | --- |
| [XITRIX/Moonlight-Switch](https://github.com/XITRIX/Moonlight-Switch) | `a75fee223914f36ec26b29739d95331307960fa3` | GPL-3.0; libgamestream headers GPL-3.0-or-later |
| [XITRIX/moonlight-common-c](https://github.com/XITRIX/moonlight-common-c) | `206c79c240c342229555494a8cadda6c11cad479` | GPL-3.0 |
| [cgutman/enet](https://github.com/cgutman/enet) | `e261d80db1de7ee92fe4fc45db67887df8b45314` | MIT |
| [sleepybishop/nanors](https://github.com/sleepybishop/nanors) | `b1e3c22ca0cdc0bb83e3cd6ed1a2fc77869ed99a` | MIT |
| [curl](https://curl.se/download/curl-8.15.0.tar.xz) | 8.15.0; SHA256 pinned in `build-curl.sh` | curl license |

Copies of upstream license texts are in `licenses/moonlight/`. These libraries
are linked by the FogPlay application and the separate local probe.
Distributing either binary also requires the corresponding source, local
transformations, and the licenses of its linked portlibs. The top-level
FogPlay license is not a replacement for these component licenses.

## Small application boundary

`Settings.hpp` supplies only an identity directory and cancellation state.
Call `Settings::instance().set_key_dir(path)` once, create that private
directory, and then call `gs_init()`. It contains `client.pem` and `key.pem`.
The stable client ID is derived from the public certificate. All GameStream
operations and settings configuration run on one worker. The main thread may
call `request_cancel()` concurrently. `reset_cancel()` starts a new operation
series; `set_timeout_ms(5000)` sets an absolute deadline shared by subsequent
requests. Reset only after joining an earlier active operation. HTTP requests
check cancellation before connecting, clamp their libcurl timeout to the
remaining deadline, and observe cancellation in the progress callback.

`gs_init`, `gs_pair`, `gs_applist`, `gs_start_app`, and `gs_quit_app` retain the
upstream signatures from `gamestream/client.h`. `SERVER_DATA` must remain at a
stable address: `serverInfo` points into its strings. The caller frees app-list
nodes and names with `free()`, and the launch `rtspSessionUrl` with `delete[]`
after stopping the stream. Upstream's `gs_start_app()` resumes an active app;
the wrapper must check ownership before launch/resume/cancel. It also sets
`currentGame` before validating the launch reply, so a failure requires a
status refresh before cancellation decisions.

## Explicit transformations and limits

`prepare.py` copies the pinned upstream HTTP/crypto sources into an ignored
prepared directory. Every replacement checks its expected occurrence count:

- Replace predictable, repeatedly seeded `rand()` with libnx OS-seeded RNG.
- Implement the upstream TODO for server-secret RSA/SHA256 signature checking
  and compare the authenticated pairing challenge with the received value.
- Preserve actual error codes where upstream assigned boolean expressions.
- Require explicit successful pairing, launch and cancellation acknowledgements.
- Read `gamesession` for launch and `resume` for resume, preserving the original
  command before host state changes; reject missing acknowledgements or session URLs.
- Preserve the host's reported current app ID for ownership checks.
- Derive a unique client ID from the probe certificate.
- Remove verbose libcurl logging and omit formatted logger argument values.
- Require the mbedTLS backend and check initialization; record only fixed
  operation names, numeric HTTP/curl results and timing through `Diagnostics.hpp`.
- Add thread-safe HTTP cancellation/deadlines and close curl handles on errors.
- Preserve the original allocation if HTTP `realloc()` fails.
- Replace the core's detached termination-callback thread with synchronous
  delivery on the originating core thread, using an atomic one-shot guard.
  Callbacks only publish state and never stop reentrantly; the owner joins
  originating stream threads before unloading the NRO.

## Production application adapter

`src/features/streaming/infrastructure/moonlight/moonlight_session.hpp` exposes
one `MoonlightSession` as `PairingClient`, secure `PairingPinGenerator`, and
`StreamingClientFactory`. It owns the authenticated host across checkout and
streaming. After successful checkout/reconnect, call `bindRental(exactRental)`;
created clients reject a different rental. Never retain raw host metadata in
the rental journal. The server rental guard owns paid-host stop; closing a
Moonlight client only tears down Li transport and releases held input.

Every `begin` creates a fresh OS-random identity directory below
`sdmc:/switch/fogplay_nx/moonlight-identities`. Certificates/private keys stay
there, separate from account credentials and settings, and never enter logs.
The crypto cache and HTTP identity paths reload when the directory changes.
An atomic process lease excludes concurrent GameStream contexts, whose upstream
HTTP/crypto/core state is global. Pairing and Li start/stop run on joined workers.

Pairing discovers the actual HTTPS port from HTTP `serverinfo` with an explicit
provider base port; IPv6 URL authorities and raw Li addresses are distinct.
`waitReady` waits at most 15 seconds for curl's outgoing `getservercert` header
event, then settles 500 ms. That event proves local request dispatch only.
Sunshine holds its response until the provider receives the PIN; there is no
remote readiness ACK before that. The complete pairing series has a 60-second
deadline. `submissionReady` rechecks it immediately before the paid POST and
requires at least five seconds remaining. `cancelAndJoin` joins pairing but
preserves a successfully authenticated host for streaming.

Production HTTPS fails closed without a key pin. The PIN challenge and signed
server secret authenticate the server certificate, whose SHA256 SPKI pin is
installed **before** the final HTTPS pairing confirmation. All subsequent
HTTPS requests use the same pin; there is no unsigned HTTP fallback after
authentication. Sunshine's generated self-signed certificate has no useful
hostname identity, so its exact public key authenticates TLS instead of PKI
hostname/CA validation. The private LAN probe retains its separate opt-in
unpinned policy. Debug payloads used for dispatch observation are discarded.

The FogHub production request is 1280x720, 30fps, stereo Opus, with the saved
bitrate applied before connection. The saved H.265 preference advertises both
HEVC Main and H.264 for host fallback; H.264 preference advertises H.264 only.
The video setup callback records the actual negotiated codec and dimensions.
RTC relay, adaptive quality, forced keyframe intervals,
PlayStation emulation, touch and sensor reports are unsupported. Gamepad,
absolute mouse, buttons, wheel, text and the app's Enter/Backspace/Tab/Escape
keys use Li APIs under the input/teardown mutex. Desktop is preferred; hosts
without it choose Playnite for gamepad, Playnite, then Steam. Equal-priority
distinct applications are rejected, as are multiple unknown launchers. No
arbitrary first-app choice and no `/cancel` request are made. An already active
different application is never resumed or stopped.

The worker copies complete video AUs while owning the Li pull-frame handle.
Video is bounded to four AUs of at most 8 MiB each; a full application queue
leaves frames in the core's bounded queue and yields until the consumer has room.
An invalid AU triggers decoder recovery and a fresh IDR. Opus is a separate
20-packet FIFO (at most 200 ms for the accepted 5/10 ms stereo formats); at
capacity it discards only the oldest packet before appending the new sample.
The app's existing Media path performs decoding/presentation.

`tools/moonlight/test.sh` runs deterministic lifecycle/input/app-selection tests
with ASan/UBSan for the worker harness. `tools/moonlight/host-tls-test.sh` builds
the same private curl with the host RTC mbedTLS/configuration and starts only
ephemeral loopback fixture servers. It verifies fail-closed unpinned HTTPS,
mutual TLS, correct-key success, impostor-key rejection and certificate/cache
rotation. Switch full links and host tests do not prove a paid FH session or
on-device performance; those require an explicitly authorized device run.

`signal-switch.c` handles the core's SIGPIPE-ignore request, since Horizon has
no POSIX SIGPIPE delivery. Arbitrary signal requests return `ENOSYS`.

The standalone LAN probe uses `set_production(false)` by default and therefore
retains its explicitly trusted-local-network TLS policy. Production always
enables pin enforcement. The Switch full-link check proves ABI/link completeness
only; actual pairing, hardware decode/audio, controller delivery and cancellation
latency require a device run.
# Core video diagnostics

`FogMoonlightDiagnostics.h` exposes `FogMoonlightGetDiagnostics()` without
enabling upstream logs or accessing queue internals from the app. Counters reset
at `LiStartConnection()` before any new core threads start, and remain available
after stop. Fields are independently loaded with relaxed, lock-free 64-bit
atomics; a snapshot is approximate across fields and across a connection reset.
Use it only for the connection currently owned by the caller. Packet timestamps
use the same clock as `LiGetMicroseconds()` (the pinned Switch core may fall back
to wall time). Validate timestamp ordering. The first packet has no gap;
the ongoing silence duration is computed by the app from `lastVideoPacketUs`.

Receipt counters include UDP bytes before runt/decryption rejection. FEC complete
blocks include blocks requiring no recovery; recovered packets count accepted
reconstructed data shards. Failed blocks count incomplete/missing blocks and
all known trailing blocks abandoned with a dropped frame (including received
but rejected trailing blocks),
excluding whole frames whose block count is unknown. A pending block is counted
only when newer data makes the core abandon it; a silent final partial block
isn't counted as failed at stop.
Reconstruction failures count failed reconstruction attempts after enough shards
arrive, including allocation/corruption failures. IDR requests count API calls,
and IDR wait drops count fully assembled frames discarded while waiting for IDR.
RTP processing duration aggregates measure only `RtpvAddPacket()` work, excluding
UDP wait, allocation and decryption. Backward-clock samples are omitted and
counted separately in `videoPacketProcessingInvalidTimes`.

`videoReceiveCycle*` measures every complete receive-loop iteration, including
timeouts, errors and rejected packets. `videoReceiveWait*` isolates the entire
`recvUdpSocket()` call (poll/recv and internal retries); `videoReceiveWork*`
is cycle time minus that wait. Work includes per-packet allocation, decryption,
RTP/FEC processing and bookkeeping. Allocation and actual decryption calls also
have their own count/total/max aggregates; stale encrypted packets skipped before
decryption do not increment the decrypt count. Initial encrypted staging-buffer
allocation before the loop is excluded. `videoReceiveLoopGap*` measures time
between one iteration's explicit end and the next iteration's start, including
diagnostic publication, the loop condition and scheduling. The first iteration
has no gap sample. All are elapsed clock durations, so a preempted thread can
increase either work or wait; these are not CPU-time or pure scheduler metrics.
Invalid/backward samples are omitted and increment `videoReceiveTimingInvalidTimes`.
The five-second application snapshot uses cumulative counters without receive-path
logging or resets; cumulative maxima may have occurred in an earlier interval.

Video UDP `SO_RCVBUF` diagnostics retain the original requested size, last
successfully selected `setsockopt` argument, failed-attempt count and last numeric
error. An unconditional release-build `getsockopt` records the actual OS-reported
size separately, with explicit query success/error; unknown actual size is zero.
The OS report can include bookkeeping and is not an estimated count of payload
bytes. Audio sockets do not overwrite video diagnostics. No buffer request,
fallback decrement, FEC algorithm or queue policy is changed.

Decode queue pending/high are atomically mirrored while holding its existing
mutex; queue destruction clears pending after users have joined. Flush operations
count even empty flushes and report removed frame totals separately. Diagnostic
reads neither lock nor touch the queue. Instrumentation is applied fail-closed to
prepared copies of seven pinned core translation units; upstream sources stay
unchanged. The build installs the header and verifies it with the dependency
manifest. Both Switch smoke links call the API.

Run `python3 tools/moonlight/tests/core_diagnostics_test.py` (also part of
`tools/moonlight/test.sh`) for repeatable preparation, unchanged pinned source,
patch rejection, concurrent counter snapshots, and the actual prepared queue's
offer/poll/wait/flush/destruction paths under ASan/UBSan.
The same test compiles the actual prepared receive loop with deterministic
I/O/clocks to cover all eight early exits and normal processing, plus the actual
prepared socket-buffer fallback with success, partial/all failure, query failure
and an unrelated socket. These fixtures prove instrumentation paths, not hardware
performance or the OS's real buffer capacity.

### Bounded IDR recovery

The pinned XITRIX core requests once after the first complete frame following
loss, then falls back to another request after 120 dropped frames. At 30 fps
that fallback alone can hold video for about four seconds while UDP reception
continues. `recovery.py` keeps the original first-complete-frame gate, adds a
500 ms retry deadline anchored to the latest API request, and only retries on
complete frames already being discarded during IDR wait. Added retries are
limited to two per second; original loss/decoder-triggered requests retain
their upstream timing. The 120-drop escalation remains for incomplete frames
and RFI escalation, coalesced with a recent request. No dependent frame is
released early. The deadline uses internal monotonic time (Switch hardware ticks), so wall-clock
adjustments have no effect. Atomic updates reject stale samples after thread
preemption; connection start resets the deadline.

The preparation also carries current official common-c's partial-IDR guard:
losing an IDR that has already released the parser's wait must require a fresh
IDR rather than reference invalidation. Pinned source files remain unchanged.

Diagnostics distinguish API requests, actual control transactions and their
local transport outcomes, completed IDR assembly, and parser wait release.
`core_idr_wait_reason` records the entry cause: 1 initial, 2 dropped reference
chain, 3 drop limit, 4 decode queue overflow, 5 decoder refresh, 6 frame gap,
7 speculative FEC loss, 8 confirmed FEC loss, 9 corrupt packet sequence,
10 invalid frame payload. Repeated drops retain the first cause/time.
`core_idr_wait_age_us`, last/max wait duration and request/dispatch ages use the
same validated core clock. Successful dispatch means local reliable transport
acceptance, not proof that the host received the request or produced an IDR.

The native recovery test includes the full prepared depacketizer, actual queue
and exact prepared API/control transaction bodies. It exercises first-frame
recovery, the 500 ms deadline, decoder request deadline reset, dense-frame
coalescing, P-frame exclusion, IDR release, speculative/confirmed loss, ordinary
RFI, partial-IDR loss, both control protocols, send failures, wall-clock rollback, stale out-of-order request/retry samples,
and teardown followed by a new connection. These host tests do not establish
Switch/network/host recovery latency.
