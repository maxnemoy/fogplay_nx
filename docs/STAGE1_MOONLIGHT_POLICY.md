# Stage 1: Moonlight connection and video recovery policy

## Late successful launch/resume

The HTTP command budget controls metadata reads, dispatch and command retries.
A `GS_OK` response is an acknowledged host side effect: retain the selected app
identity before evaluating cancellation. Do not replay `/launch` because the
reply arrived at or just after that budget boundary.

An acknowledged response enters a distinct transport stage with a 15-second
deadline. Replacing the deadline does not clear manual cancellation. The
application's `pump()` interrupts a core handshake when that deadline expires;
ordinary close/shutdown continues to interrupt and join the worker. A manual
cancel, invalid authenticated generation, or stop intent wins over a late
success and prevents core handoff. Failed or ambiguous command responses keep
the original fail-closed policy and never gain a fresh budget.

After either an initial launch or resume acknowledgment, only explicitly
classified socket/transport errors from `LiStartConnection` authorize the
existing application recovery flow. Generic `-1`, setup/configuration failures,
unknown errors, authentication failures and ambiguous HTTP outcomes remain
terminal. Recovery retains the exact rental, endpoint and app, performs the
existing provider preflight, and uses resume-only mode; it never pairs or
launches another app.

## Video and input

FogHub reports an input suspension while local assembly or the decoder needs
an IDR, the core reports an IDR wait, or no complete frame has entered the client
queue for 150 milliseconds. Startup is suspended until a frame exists. This
feeds the application's existing neutralization and input gate.
It is a separate `inputSuspension()` signal; core waits and the 150 ms check do
not change `waitingForVideoKeyframe()`, the recovery banner or presentation-gap
eligibility. The application records suspension counts and durations by reason.

RTP assembly, Moonlight ingestion and the decoder share an 8 MiB encoded video
access-unit limit. An oversized Moonlight frame is completed with `DR_NEED_IDR`
and enters recovery instead of reaching a mismatched fatal decoder guard.

A keyframe request following a local decoder error posts an atomic command to
the core receive owner and discards queued client frames from the old video
generation. The core receive loop enters its existing decoder refresh lifecycle,
flushes dependent frames, and uses its existing 500 ms IDR retry reservation.
The receive loop polls on socket timeouts as well, so even complete RTP silence
permits the initial request and retries. Those requests coalesce with the
existing frame-driven core requests. Timeout retries apply only to the scope
opened by this local decoder command. IDR recognition and connection reset close
the scope. Startup and network-induced IDR waits retain their previous
frame-driven retries and receive no extra timer requests. There is no second
client retry timer. A sustained outage still enters the existing no-video
transport recovery. The `localIdrRetry` experiment bit can restore the previous
single `LiRequestIdrFrame` call for comparison.

## Production event journal

`FogHubServices` installs `MoonlightDiagnosticsBridge` before starting host work.
Its stateless callback forwards the SDK and core event messages into the
thread-safe application `diagnostic()` path. The sink captures no service or
session object, and service destruction joins Moonlight workers before the
process logger closes. The session remains the sole journal reader and retains
its cursor across connections; overflow/contention is logged as `journal missed`.

## Verification

`bash tools/moonlight/test.sh` exercises the production session against host
SDK/core doubles and the prepared core depacketizer/recovery code under
AddressSanitizer and UndefinedBehaviorSanitizer. It covers initial transport
failure classification, late success versus manual cancel, IDR input gating,
the 150 ms boundary, 5/8 MiB accepted AUs and an over-limit AU, and local decoder
refresh/retry/release/reset. The separate production log integration binary links
the real core journal, session drain, composition bridge, app diagnostics and
RunLog. It verifies IDR stages, thread observations and an explicit overflow
notice in the archive. Default and control builds exercise enabled and disabled
local decoder retry. These tests do not establish console latency or network
performance; the hardware IDR-chain check and A/B matrix remain required.
