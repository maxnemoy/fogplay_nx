# Stage 0: streaming thread and queue ownership

This describes the current implementation, checked against the working tree on
2026-10-29. It is a source-level ownership map, not console/runtime acceptance.
Stage 0 preserves the existing MTS synchronous media path and FH media workers.

## Threads and lifetime

The application/UI thread owns `ApplicationController`, transport lifecycle,
`StreamingClient::pump()`, input policy, and SDL texture creation/upload/drawing.
`startTransport()` in `playback_controller.cpp` starts `Media` workers only for
FogHub. MTS decoding and audio feeding remain in `pumpStream()` on the UI thread.
`closeTransport()` stops and joins media workers before closing or replacing the
transport. A worker's reference to `StreamingClient` cannot outlive that join.

MTS additionally has libdatachannel callbacks, an `AsyncControlSender` worker,
ICE snapshot work, and an RTCP feedback worker. FH has a Moonlight session worker,
Moonlight core receive/control/audio threads, and the two `Media` workers.
These are distinct from the provider HTTP/session workers that may call
`diagnostics::diagnostic()` through the callback in `composition/mts_services.cpp`.

## MTS compressed `MediaQueue`

`MediaQueue` in `stream_queue.hpp` has **external synchronization**. Its owner is
one transport generation's `Client::Impl::Shared`; every live queue operation is
under `Shared::mutex` (`TimedMutex`). The container is a mixed audio/video FIFO,
with separate counts, byte bounds, and audio selection through `popAudio()`.

| Operation | Caller/thread | Synchronization and effect |
| --- | --- | --- |
| Video `push` | `Shared::videoRtp()` on an RTC callback; also `Client::pump()` on UI through `video.tick()` and `Shared::drainVideo()` | `Shared::mutex`; assembler output becomes queued AUs. There are currently **two video producer contexts**. |
| Audio `push` | RTC track callback through `Shared::packet()` | `Shared::mutex`; oldest audio may be evicted to preserve bounds. |
| Mixed `pop` | UI `pumpStream()` through `Client::poll()` | Route/running gate first, then `Shared::mutex`, expiration, and `media.pop()`. The result can be video **or audio**. |
| Audio `popAudio` | UI `drainStreamAudio()` through `Client::pollAudio()` | Same gate/lock and expiration; removes the earliest audio regardless of queued video. |
| Expiration | RTC video/audio callbacks, UI assembler drain, and UI `poll`/`pollAudio` through `Shared::expireMedia()`; `push()` also calls `expireAudio()` | All under `Shared::mutex`; expired audio is discarded, aged video requests chain resynchronization. |
| Video clear/resync | `Shared::reconcileFault()` / `resyncQueue()` from callback or UI work above | `Shared::mutex`; `clearVideo()` drops queued video, generation changes invalidate the decoder prediction chain. |
| Generation initialization | UI `Client::start()` creates fresh `Shared`; SDP handling replaces the assembler | Fresh queue before publication; assembler replacement under `Shared::mutex`. |
| Teardown/discard | UI transport shutdown | Under `Shared::mutex`, set `stopped`, swap queued packets/events out; release packet/event owners outside the lock. Callback-held `Shared` survives but rejects new work. |
| Queue diagnostics/count/age | UI-facing client methods | `Shared::mutex`; format/read shared queue state there. |

`Shared::drainVideo()` is a helper requiring the caller's lock, not a dedicated
producer thread. Replacing this queue with SPSC would require removing the
second producer context and accounting for expiration/reset writers first.

The queue lock alone does not make MTS `poll()` safe for media workers: before
locking it reads UI-owned `Impl` running/route state. In addition, `Media::runVideo`
expects a video packet from `poll()`, while MTS `poll()` returns the mixed FIFO.
MTS therefore must retain its current UI consumers until route publication and
separate video/audio selection are implemented. Stage 0 does not fix S7/P1/P7.

## FH compressed queues and polling

FH does not use `MediaQueue`. `moonlight_session.cpp` owns separate bounded
`State::video` and `State::audio` deques under `State::mutex`.

The Moonlight session worker is the sole video producer: `drainVideo()` pulls
complete units with `LiPollNextVideoFrame()`, copies bytes, enqueues, and completes
each core frame. It leaves frames in the core queue when its video FIFO is full.
Its assembly failure path can clear the video FIFO and advance its generation
under the same mutex. The core audio callback `audioSample()` produces audio and
evicts the oldest packet at its bound. Start clears both queues under the mutex
before the new session begins.

`Media::runVideo()` calls FH `poll()`; `Media::runAudio()` calls FH `pollAudio()`.
Both use `Client::take()` under `State::mutex`, selecting only their own deque.
The UI skips synchronous draining while media workers run. FH's `pump()` does
not consume these queues. Shutdown joins media consumers before the transport;
the Moonlight session worker stops the core before its own join completes.

## Decoded frame queue and presentation

`MediaFrameQueue` has external synchronization through `Media::Impl::readyMutex`.
Only converted, independently owned CPU pixels cross the boundary.

| Operation | Owner/thread |
| --- | --- |
| Decode, convert and `readyFrames.push()` | Video media worker. Push checks the captured `epoch`, pending reset, and stopping flags while holding `readyMutex`. |
| `readyFrames.take()` and stale/coalesced-frame removal | UI `Media::preparePresentation()`, under `readyMutex`; texture upload occurs after releasing the lock. |
| `readyFrames.reset()` / `discardReady()` | UI at start/stop and reset request; video worker on reset, codec change, or transport generation change. All use `readyMutex`. |
| Queue depth/stats snapshot | UI `Media::diagnostics()`, under `readyMutex`; format the copied stats after unlock. |

The queue is bounded to three frames. Its VSYNC/display-paced policy is enabled
only for the qualifying Switch GLES 60 fps path. MTS's synchronous path uses its
pending decoder frame and presentation cadence, not this worker frame queue.

## Command delivery and feedback

* **MTS signaling/events:** RTC callbacks publish bounded `Shared::events` and
  terminal/error evidence under `Shared::mutex`; UI `pump()` swaps/drains events
  and changes UI-owned `Impl` state. Callbacks do not mutate that state directly.
* **MTS input/control:** UI `sendGamepad` / `sendInput` and quality/control paths
  enqueue into `AsyncControlSender`. Incoming ping callbacks can enqueue pongs
  directly. Its own mutex protects the bounded queue and coalescing; its worker
  serializes DataChannel sends outside the queue lock. UI gates admission/route
  and observes snapshots. Stop seals/discards old-generation work before join;
  an already-running synchronous RTC send may still delay join.
* **MTS regular RTCP / ICE:** dedicated workers publish protected snapshots;
  UI consumes results. RTP assembly remains under `Shared::mutex`; these workers
  do not become the sole producer of `MediaQueue` video.
* **FH input:** UI calls `LiSendMultiControllerEvent` / input APIs under
  `State::apiMutex`; Moonlight core owns subsequent network delivery. There is no
  additional application input-sender worker. Keyframe requests use the same API
  mutex, while decoder-waiting publication is atomic.
* **Media reset/codec commands:** UI publishes `requestedCodec`, `codecRequested`,
  `resetRequested`, and advances atomic `epoch`; `wake.notify_all()` wakes the
  video worker. The worker handles reset/codec change before more polling.
  Stopping is atomic, followed by notification and joins. The epoch check rejects
  output decoded across a reset request. These are flags, not an ordered command
  FIFO; repeated requests can coalesce.
* **Media errors/snapshots:** workers enqueue errors under `errorMutex` for UI
  `takeWorkerError()`, and publish diagnostic strings under `snapshotMutex`.
  UI still owns presentation diagnostics; calling UI-only `Media` or MTS client
  diagnostic methods from a new background formatter is not supported.
* **Global diagnostic accounting (S6):** all timing records, snapshots and resets,
  plus the one-time logger failure latch, use `diagnosticMutex` in
  `app_diagnostics.cpp`. UI formats a `diagnosticTimingSnapshot()` copy. RunLog's
  own mutex independently protects its bounded writer queue. This does not change
  the thread/lifetime restrictions of the objects producing diagnostic text.

Queue capacity, recovery, input gating, blocking teardown, and worker scheduling
policies remain as implemented. Future ownership changes need separate tests and
console validation across both providers, codecs, frame rates, and stop/recovery.
