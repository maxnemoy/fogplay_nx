# Stage 0 measurement procedure

This is a reproducible capture and analysis procedure. It does not establish
Switch runtime acceptance or a measured improvement. The current authorized
scope is code, build, and packaging; the device steps below are for a later run.

## Keep build groups separate

* Logs from 0.6.15 and 0.6.16 are historical incident evidence. They cannot serve
  as measurements of 0.6.18 or of the instrumented build.
* The preserved **0.6.18 baseline artifact** is
  `D:/dev/experements/mts/_review/stage0-baseline-20261029/fogplay_nx.nro`.
  Its SHA-256 is
  `85621608ea1a1d0997d1aee37f8df77e2bcc684fe17323261663465b7b1aca04`.
  The same directory contains the source status, tracked patch, and untracked
  archive. Prior installation readback evidence supports that earlier install;
  it does not establish a new streaming capture or a fresh installation.
* The revised instrumented build is **0.6.21**. Record its own artifact hash after
  building and preserve its source snapshot. Instrumentation itself may affect
  timing; do not pool its samples with 0.6.18 or call it an unchanged baseline.

The log startup header identifies the application version, not the executable's
SHA-256. Version text alone cannot prove which dirty-tree artifact produced a log.
Keep artifact/readback evidence and exact log bytes together for each future run.

## Future console capture

1. Save the source diff/status/untracked list, exact NRO, its SHA-256, and package
   metadata. Use the stable install location `switch/fogplay_nx`; preserve login,
   settings, and existing logs. Record successful readback verification against
   the same NRO before associating a future log with that artifact.
2. Capture separate runs for MTS and FH, H.264 and H.265, and 30 and 60 fps. Record
   requested and negotiated codec/fps separately: FH may initially report zero
   negotiated fps or a default codec before connecting. Keep scene, host, display,
   network, input pattern, and warmup/measurement duration consistent for A/B.
3. Record UI menu/focus/stop and recovery transitions separately from steady
   playback. Preserve a clean stop and the final diagnostic lines when possible.
   Reconnects and later transports may reuse cumulative `Media` counters.
4. Copy original log files before another run overwrites `last-run.log`. Hash the
   exact copied log. Store a capture manifest with paths to the readback/source
   evidence; never alter a log to insert an inferred build identity.

Example external manifest (replace all placeholders with the actual capture):

```json
{
  "captures": [
    {
      "log_sha256": "<64 lowercase hex digits of exact copied log>",
      "artifact_sha256": "<64 lowercase hex digits of installed NRO>",
      "artifact_version": "0.6.21",
      "readback_verified": true,
      "evidence": "<path to this capture's readback and source-snapshot report>"
    }
  ]
}
```

`readback_verified` is an operator declaration backed by the referenced evidence;
the analyzer cannot independently verify a past installation. For copied logs
without that evidence, omit the manifest and retain the version-only identity.
An explicit manifest must match every input file's hash; mismatches fail analysis.

## Offline log analysis

Run from the repository root in WSL:

```bash
python3 tools/diagnostics/stage0-report.py logs /path/to/copied-run.log > work/stage0-log-report.json
python3 tools/diagnostics/stage0-report.py logs /path/to/copied-run.log --manifest /path/to/captures.json > work/stage0-log-report-with-identity.json
python3 -m unittest discover -s tools/diagnostics -p 'test_*.py' -v
```

The JSON keeps startup runs and transport attempts separate. A
`Controller profile:` line starts a transport attempt; truncated logs can have an
inferred boundary and unknown provider. Configuration entries are observations,
including provisional values, rather than a claim that connection succeeded.
`fps` comes from transport telemetry; `presentation_fps` is kept separately and
cannot establish negotiated fps.

`Video decoder` snapshots contain cumulative counters. The report subtracts
consecutive monotonic snapshots **within one attempt**, taking the first as an
anchor. It does not assume counters start at zero. A decrease creates a reset
boundary; the first value after it is another anchor. Duplicate final snapshots
contribute zero. `superseded_per_decoded` uses those observed deltas only. Its
window excludes activity before the first sample, and it is not a network loss
rate. Check `displayPaced=1`, actual 60 fps, and the observed delta interval when
assessing FH pacing. Missing `displayPaced` telemetry proves neither mode.

`Client latency:` contributes the logged `frameAge`, `assembledQueueWait`, and
`presentGap` distributions. `Input latency:` contributes accepted
pad-to-send-return samples. Keep their sample counts, unavailable-origin counts,
and histogram overflow alongside p50/p95/p99. Their 5-second windows may overlap;
the analyzer retains them without averaging percentiles or summing window samples.
These are local client boundaries. They do not measure remote game response,
network input acknowledgment, visible panel photons, or speaker output.

Neither queued audio milliseconds nor independent audio/video RTP timestamps
establish AV skew. There is currently no common mapped audio/video presentation
clock in these logs. The report explicitly marks AV skew unavailable.

## FH IDR-wait evidence (S15)

Each run's `moonlight_idr` section groups `Moonlight IDR` events by the immutable
`connection` and `wait` identifiers, independently of the current UI transport
attempt. Entry records may be logged after another event for the same wait.
Dispatch results and other old-wait events can follow `wait-exit` or the start
of a later wait; they retain their original key. The ordered event list preserves
log/sequence order rather than sorting by the core clock. A backward core-clock
entry/exit duration is rejected explicitly.

`observed_event_counts` counts admitted event records actually present in the
log, including repeated receive hints with distinct sequences. Exact duplicate
sequences within the retained window are counted separately as duplicates and
do not inflate events. `admitted_totals_max` takes the maximum valid per-wait
`requests`, `intact`, and `fecFailedBlocks` snapshots across **all** observed
events, including events after exit. These snapshots are never summed, and the
exit snapshot is not a final total. `totalsValid=0` means totals are unavailable
for that record; the event itself is still retained and counted. Observations
before/after entry or exit are not reassigned by their timestamp.

Frame correlation uses `connection + frame`, and records all associated waits.
This joins an `idr-receive-hint` with `wait=0` to a FEC failure after that frame
opened an IDR wait. The hint's `value` is the advertised FEC block count;
`fec-failure.value` contributes failed blocks, not a network packet-loss count.
Duplicate first packets can produce multiple hints. A matched hint and FEC
failure is explicit correlated evidence, not a causal proof that a particular
request produced that frame. A missing first packet or an unsupported prefix
can prevent a hint, so absence of a hint does not establish absence of an IDR.

`journal missed=N` can result from producer contention or overwritten records.
The notice has no wait identity: its loss count remains run-level and its
attribution is **unknown**. Every per-wait chain in that run is conservatively
marked potentially incomplete. Maxima can include admitted observations whose
individual records were overwritten, while contention can prevent admission
entirely. Neither the event counts nor the snapshot maxima become complete
network/recovery totals. No loss notice observed is also not proof of a complete
capture, especially for a truncated log.

The S15 report retains the latest 4096 ordered events, the first 256 wait
summaries, the first 1024 frame correlations, and the latest 256 loss notices per
run. Omitted detail is counted under `report_retention` and `omitted_loss_notices`;
global observed event counts continue advancing. Each frame additionally bounds
wait links and distinct advertised block values, with explicit omitted counters.
Keep captures short enough for the desired detail, or analyze separate original
run logs. These report limits are independent of the core's own bounded journal.

Control dispatch success means local transport acceptance, not host receipt.
`wait-exit` means parser recognition released the wait; it does not establish
complete assembly, successful decoding, or visible presentation. Preserve
`idr-recognized`, `idr-assembled`, and the subsequent media telemetry as distinct
observations. The analyzer does not infer damaged IDRs or absence of requests
from incomplete chains.

## External flash/click stimulus

Generate a deterministic source artifact on the streaming host or another PC
with Python 3 and FFmpeg installed:

```bash
python3 tools/diagnostics/stage0-report.py stimulus work/stage0-av-stimulus --render > work/stage0-av-stimulus.json
```

The default artifact has eight full-frame flashes at 60 fps, spaced 120 frames
apart, with a 120-frame lead-in. Each flash lasts three frames. A 20 ms, 1 kHz PCM
burst begins at the exact source sample corresponding to each flash's first
frame. `flash-click.wav` is mono 48 kHz/16-bit PCM; `flash-click.mkv` combines it
with H.264 video and lossless PCM audio. `timeline.json` records event IDs,
frame/sample onsets, expected skew (zero), total frame/sample counts, and hashes.
Without `--render`, the tool generates the WAV and source timeline only, so there
is no visual stimulus until a movie has been rendered. Play the MKV with a player
that supports PCM audio in Matroska; disable any user-selected AV offset.

The source artifact is synchronized by construction. Host playback, browser or
video-player scheduling, streaming capture, console decoding, the display, and
audio hardware can each add skew. For a client-specific comparison, first record
the host playback reference with the same recording setup, then the streamed
Switch output. Preserve the host reference and use its measured offset as an
explicit calibration; source timestamps alone do not calibrate the player.

Record visible flashes and audible clicks on **one common recording timeline**.
For optical measurement, use a high-frame-rate camera that also records audio;
otherwise record video and audio through an acquisition setup with a calibrated
shared clock. A video-only 240 fps recording plus unrelated phone audio is not a
common timeline. Fix recorder position and speaker distance, disable recorder
enhancements where possible, and document camera/audio synchronization and
measurement uncertainty. Headphones require a suitable audio capture path.

Do not assume the recording is calibrated: acoustic propagation, camera AV
offset, frame exposure/rolling shutter, and audio onset annotation add uncertainty.
Frame quantization alone is one frame interval; report the recording frame rate
and the timestamp convention used for the first bright frame. Repeated events
improve the distribution estimate but cannot remove an unknown constant offset.

## Signed skew from annotated recordings

In a video/audio editor, identify matching event IDs from `timeline.json`.
Annotate the first visibly bright frame and the corresponding click onset in
milliseconds on that same recording timeline. A skipped/unidentifiable event is
omitted and remains explicitly missing. Do not shift event IDs to hide a miss.

```csv
event_id,video_ms,audio_ms
1,2012.5,2023.0
2,4016.7,4020.0
3,6012.5,6008.0
```

```bash
python3 tools/diagnostics/stage0-report.py skew /path/to/annotations.csv --timeline work/stage0-av-stimulus/timeline.json > work/stage0-av-skew.json
python3 tools/diagnostics/stage0-report.py skew /path/to/annotations.csv --timeline work/stage0-av-stimulus/timeline.json --calibration-ms 4.0 > work/stage0-av-skew-calibrated.json
```

Signed skew is **audio timestamp minus video timestamp**: positive means late
audio; negative means early audio. The tool reports raw signed values, calibrated
signed values, and absolute calibrated magnitudes. Calibration subtracts the
declared reference offset; zero is the default. p50/p95/p99 use the nearest-rank
definition over individual annotated events. Signed p95/p99 describe the late
audio tail; absolute p95/p99 describe mismatch magnitude in either direction.
The report includes sample count and missing event IDs. With eight events, p95
and p99 both select the maximum; collect more repeated runs before treating
tails as stable. Keep runs/builds/providers/configurations separate.

The tests verify PCM onset/count, rendered flash frame/count, lossless muxed audio,
signed offsets/calibration, version separation, and counter-delta behavior.
Synthetic artifacts and host tests establish tool behavior; only a later
calibrated device recording can establish actual AV skew.

## Diagnostic overhead and comparable A/B runs

Before attributing a performance change to instrumentation, perform one control
comparison of the preserved 0.6.18 artifact against 0.6.21 on the same scene,
host, provider, codec, fps and network. This control has not been run on Switch.
For Stage 1 and later, both A and B must carry the same diagnostic revision,
logging gates and sampling settings. A change to diagnostic code starts a new
comparison group; matching application versions alone is insufficient.

Do not merge 0.6.19 and 0.6.20 latency windows: 0.6.19's MTS receipt boundary
excluded Shared mutex contention, while 0.6.20 captures at batch handler entry
before RTX normalization and RTCP/assembly locking. Both are client software
boundaries after RTC delivery, not NIC receive timestamps; FH still uses the
core's receiveTimeUs. Cross-provider absolute latency therefore needs that
boundary distinction, even though MTS no longer hides its Shared mutex waits.
0.6.20 keeps processing clocks for repair deadlines separate from this metadata.

Detailed FEC loss events are now limited to active IDR waits. Cumulative loss
counters remain unchanged. At most one pre-wait IDR receive hint is retained
without logging and emitted when a wait opens. In 0.6.21, assembly of that IDR
or a newer IDR clears the retained hint even outside a wait. A retained hint
is an observation of receipt, not proof of an incomplete or damaged frame. No extra high-rate idle FEC
journal traffic is introduced for comparison runs.

Thread scheduling is grouped by transport epoch. Carried persistent-worker
start observations include observedEpoch/carried and may describe an exited
thread; they are not fresh affinity queries or a live inventory. The analyzer
retains core/ideal/priority/affinity, availability/query errors, epochs and
explicit registry/journal overflow. Moonlight's scheduling payload uses explicit
fields rather than repurposing IDR counters.

0.6.21 also starts a separate comparison group from 0.6.19 and 0.6.20:
it fixes stale idle IDR hints after assembly. Capture the new baseline with
0.6.21, and retain the exact artifact SHA-256 and diagnostic settings.

The analyzer exposes `identity.comparison_group` as `version:<version>` to
separate these revisions while retaining the existing eligibility category.
A missing version or capture-version conflict has no group. Matching version
groups still require matching artifact hashes and diagnostic settings for A/B.
