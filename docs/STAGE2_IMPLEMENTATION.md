# Stage 2 — transport recovery and teardown

Scope: S1, S4, S3 (items 16–18 in REVIEW-REPORT.md). The pre-change 0.6.25
source patch, changed/untracked files and hashes are archived at
`D:/dev/experements/mts/_review/stage2-baseline-20261030-211649`.
No branches or commits are created. Stage 1 settings and the FH bonus-price
fix are retained; frame scheduling, codecs and audio policy are unchanged.

## S1: MTS backpressure, outstanding calls and input

`AsyncControlSender` distinguishes bounded queue backpressure from a transport
operation that exceeds its own 15-second deadline. The UI owner inspects health
on every pump; diagnostic snapshots do not publish failures or mutate state.
Backpressure seals new ordinary work. A short already-started availability
probe/send may return; an idle sealed sender is retryable. A non-cancellable
operation still active at teardown, or an operation past its deadline, remains
terminal. `disconnectForRetry` seals operations before its final checks.

`beginFinish` continues to dispatch the independent HTTP rental stop before
closing/joining an unsafe MTS transport. Terminal evidence observed while
closing prevents a replacement connection. The original rental, recovery
budget and ownership journal remain authoritative.

On saturation the sender drops queued input history and retains bounded release
obligations, a neutral gamepad and the current gamepad sample for local queue
reconciliation. Backpressure starts recovery; old presses, taps, motion and stick
samples are not replayed on the replacement connection. A saturated tap may be
lost; preserving historical presses indefinitely is not promised.

The application retains the same MTS Client object across an accepted retry.
Its rental-bound release ledger survives replacement of the RTC implementation.
The new channel sends initialization first, then outstanding key/mouse/touch
releases and gamepad neutral, before fresh input. Held physical controls can
become active again only from a new sample. Changing the rental or finally
closing the session discards this retry context.

Diagnostics add controlBackpressure, controlOperationStalled,
controlOperationMs and controlQueueResets. Local send acceptance does not mean
the remote host acknowledged delivery.

## S4: FH termination provenance

The prepared core marks host termination messages through a callback-scoped,
thread-local origin. Transport callbacks cannot inherit another thread's host
origin. An explicit host stop remains terminal even if its numeric value is the
same as a retryable errno, if it is unknown, or if it races transport failure
and teardown. Fatal evidence is sticky until the next authenticated session.

Only transport-origin failures use the verified transport whitelist, including
ENOTCONN, EHOSTDOWN, ENOBUFS and EBADF. Startup failures retain the Stage 1
acknowledged-launch/resume and cancellation policy. Diagnostics include
terminationOrigin. Existing captured console logs contained no nonzero
termination code; actual sleep/Wi-Fi codes still need device validation.

## S3: asynchronous FH teardown

The application owns a dedicated `TransportTeardown` worker, created before any
paid session. It moves the unique client out of the UI after media workers and
local input have stopped. The reaper calls the existing blocking close or
disconnectForRetry contract. Its mailbox mutex never covers an SDK call.
Completion returns both the old client and the post-join retry decision.

No replacement client or FH preflight starts while teardown is outstanding.
Late terminal callbacks win before any resume. A refused retry returns ownership
for final teardown after rental-stop dispatch. Manual stop/focus loss cancels
pending resume, without destroying an active client on the UI thread. The
recovery deadline continues to apply while closing; quality reconnect has a
15-second close budget before requesting rental stop. Further settings edits
while quality teardown is pending coalesce into the next same-rental start.

The UI keeps drawing and processing the streaming menu, with a stopping notice.
New rental actions are gated until the old client is reaped. Application
shutdown cancels provider work and joins the reaper before destroying provider
services or networking; application destruction is intentionally allowed to
wait for owned threads.

Moonlight clears running under apiMutex, then performs neutralization and
LiStopConnection outside that mutex. Input observes running=false promptly.
The authenticated State and exclusive protocol lease remain occupied through
the actual client-worker join, not merely until the worker's last callback.

Pinned RTSP connect, receive poll and retry sleep check interruption in slices
of at most 100 ms, preserving their overall timeout budgets. The ordinary host
connectivity probe does not inherit a previous connection's cancellation flag.
This bounds cancellation checks, not an absolute platform-wide shutdown time.

## Validation and acceptance

Targeted native tests cover idle saturation, blocked availability/send,
observational snapshots, release-before-fresh-input across a real RTC retry,
terminal races, blocked reaper ownership, deferred settings changes and manual
cancellation. Moonlight tests execute the prepared RTSP implementation with
local sockets, the real host termination parser/shim, concurrent origin checks,
and the production session under ASan/UBSan.

Final suite/build results are recorded with the release artifacts. No actual
paid rental is created by tests. Source/static/native checks and a successful
Switch build do not establish physical-console acceptance.

Final validation on 2026-10-30, all exit 0:

- `bash tools/test/all.sh` (serial), including real RTC integration and the
  late-completion timeout regression, all provider/rental, input and media tests.
- `bash tools/moonlight/test.sh`: production session and prepared-core tests
  with ASan/UBSan, actual RTSP local sockets and host-origin collision tests.
- Dedicated teardown tests: 81 assertions, also passed under ASan/UBSan.
- Paired settings tests: 71 assertions; transport recovery: 154 assertions.
- `tools/moonlight/build.sh`, manifest check and standalone/dual-curl links.
- `JOBS=6 bash build.sh STAGE1_PROFILE=full BOUNDED_SYNC_AUDIO=0` and
  `python3 package.py`: Switch 0.6.26, verified NRO/NACP/assets and SD ZIP.
- Architecture check and `git diff --check`.

Independent review found and resolved two races: same-frame stop versus a ready
quality-resume completion, and a late RTC exception overriding an already
expired operation. Resume now follows sampled overlay intent; operation
deadlines remain terminal even if the callback subsequently returns.

On Switch still verify: ordinary FH start/stop and quality reconnect, cancel
during handshake/teardown, host-initiated stop without auto-resume, Wi-Fi loss
and recovery, MTS held controls through retry, repeated sessions, and terminal
rental confirmation. Compare performance against the archived 0.6.25; Stage 2
does not claim to fix the previously observed low frame rate.
