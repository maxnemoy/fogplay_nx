#!/usr/bin/env python3
"""Offline Stage 0 evidence: isolated log observations, AV stimulus, signed skew."""
import argparse
from collections import Counter, deque
import csv
import hashlib
import json
import math
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import wave


BASELINE_VERSION = "0.6.18"
BASELINE_SHA256 = "85621608ea1a1d0997d1aee37f8df77e2bcc684fe17323261663465b7b1aca04"
INSTRUMENTED_VERSIONS = ("0.6.19", "0.6.20", "0.6.21")
COUNTERS = ("decoded_frames", "uploaded_frames", "presented_frames", "superseded_frames")
KV = re.compile(r"([A-Za-z_][A-Za-z_0-9-]*)=([^\s{};]+)")
HEADER = re.compile(r"FogPlay NX (\d+\.\d+\.\d+) startup\.")
STAMP = re.compile(r"^\[(\d+) ms\]\s*(.*)$")
IDR_KINDS = {"wait-enter", "request", "dispatch", "dispatch-result", "fec-failure", "intact-frame",
             "idr-receive-hint", "idr-recognized", "wait-exit", "idr-assembled"}


class SchedulingJournal:
    """Bounded observed scheduling records, never an inferred live inventory."""
    RowLimit, SnapshotLimit, EpochLimit = 4096, 256, 256

    def __init__(self):
        self.rows = deque(maxlen=self.RowLimit)
        self.snapshots = deque(maxlen=self.SnapshotLimit)
        self.epochs = {}
        self.rows_seen = self.snapshots_seen = self.unknown_rows = self.error_rows = 0
        self.malformed = self.omitted_epochs = 0

    @staticmethod
    def integer(value):
        if isinstance(value, int):
            return value
        try:
            return int(value, 16 if str(value).lower().startswith("0x") else 10)
        except (TypeError, ValueError):
            return None

    def observe(self, text, at_ms, line_number, stream_hint):
        values = fields(text)
        source = "moonlight" if "Moonlight thread " in text else "application-registry"
        base = {"at_ms": at_ms, "line": line_number, "source": source, "ui_stream_hint": stream_hint}
        if source == "application-registry" and "role=" not in text:
            if any(not isinstance(values.get(key), int) or values[key] < 0 for key in ("count", "overflow")):
                self.malformed += 1
                return
            epoch = values.get("epoch")
            if not isinstance(epoch, int) or epoch < 0:
                epoch = None
            snapshot = {**base, "epoch": epoch, **{key: values[key] for key in
                ("scope", "inventory", "count", "overflow", "persistentOverflowLifetime") if key in values}}
            self.snapshots_seen += 1
            self.snapshots.append(snapshot)
            if epoch not in self.epochs and len(self.epochs) < self.EpochLimit:
                self.epochs[epoch] = {"epoch": epoch, "count_max_observed": 0, "overflow_max_observed": 0,
                                      "persistent_overflow_lifetime_max_observed": None}
            target = self.epochs.get(epoch)
            if target is None:
                self.omitted_epochs += 1
            else:
                target["count_max_observed"] = max(target["count_max_observed"], values["count"])
                target["overflow_max_observed"] = max(target["overflow_max_observed"], values["overflow"])
                persistent = self.integer(values.get("persistentOverflowLifetime"))
                if persistent is not None and persistent >= 0:
                    target["persistent_overflow_lifetime_max_observed"] = max(
                        target["persistent_overflow_lifetime_max_observed"] or 0, persistent)
            return
        # Native roles can contain spaces (e.g. 'RTC worker', 'SCTP timer').
        match = re.search(r"\brole=(.*?)(?=\s+[A-Za-z_][A-Za-z_0-9-]*=|$)", text.strip())
        if not match or not match[1].strip():
            self.malformed += 1
            return
        names = ("core", "ideal", "mask", "priority", "available", "queryErrors", "affinityResult",
                 "priorityResult", "epoch", "observedEpoch", "carried", "slot", "connection", "seq", "core_us")
        raw = {key: self.integer(values[key]) for key in names if key in values}
        invalid = [key for key in raw if raw[key] is None]
        errors = raw.get("queryErrors")
        if errors is not None and errors < 0:
            invalid.append("queryErrors")
            errors = None
        available = raw.get("available")
        if available not in (0, 1, None):
            invalid.append("available")
            available = None
        # A partially failed native query may still have a valid priority or
        # affinity. An unavailable host observation has neither query result.
        affinity_known = raw.get("affinityResult") == 0 or available == 1 or (errors is not None and errors > 0 and not errors & 1)
        priority_known = raw.get("priorityResult") == 0 or available == 1 or (errors is not None and errors > 0 and not errors & 2)
        if errors is not None and errors & 1:
            affinity_known = False
        if errors is not None and errors & 2:
            priority_known = False
        valid = lambda key: raw.get(key) if isinstance(raw.get(key), int) and raw[key] >= 0 else None
        normalized = {"core": valid("core"), "ideal_core": valid("ideal") if affinity_known else None,
                      "affinity_mask": valid("mask") if affinity_known else None,
                      "priority": valid("priority") if priority_known else None}
        unknown = [key for key, value in normalized.items() if value is None]
        availability = ("query-error" if errors else "available" if available == 1 else
                        "unavailable" if available == 0 else "unknown")
        self.rows_seen += 1
        self.unknown_rows += bool(unknown)
        self.error_rows += bool(errors)
        self.rows.append({**base, "role": match[1].strip()[:128], "availability": availability,
            **normalized, "unknown_fields": unknown, "invalid_fields": invalid, "raw": raw,
            "epoch": valid("epoch"), "observed_epoch": valid("observedEpoch"),
            "carried": bool(raw["carried"]) if raw.get("carried") in (0, 1) else None,
            "connection": valid("connection")})

    def summary(self, moonlight_journal_missed):
        return {"observations": list(self.rows), "registry_snapshots": list(self.snapshots),
                "epochs": list(self.epochs.values()), "observed_rows": self.rows_seen,
                "rows_with_unknown_fields": self.unknown_rows, "query_error_rows": self.error_rows,
                "malformed_lines": self.malformed,
                "moonlight_journal_missed_unknown_event_types": moonlight_journal_missed,
                "retention": {"row_limit": self.RowLimit, "snapshot_limit": self.SnapshotLimit,
                    "epoch_limit": self.EpochLimit, "omitted_rows": max(0, self.rows_seen - self.RowLimit),
                    "omitted_snapshots": max(0, self.snapshots_seen - self.SnapshotLimit),
                    "omitted_epoch_snapshots": self.omitted_epochs},
                "note": "Thread-start scheduling snapshots only, not live threads or sustained execution cores. Carried rows retain old observations; UI stream hints reflect log position only. Registry overflow is a per-epoch maximum, never summed; Moonlight journal loss has unknown thread/IDR attribution."}


class IdrJournal:
    """Bounded evidence summaries; late events retain their immutable wait key."""
    EventLimit, WaitLimit, FrameLimit, LossLimit = 4096, 256, 1024, 256

    def __init__(self):
        self.events = deque(maxlen=self.EventLimit)
        self.waits, self.frames = {}, {}
        self.losses = deque(maxlen=self.LossLimit)
        self.sequences = set()
        self.sequence_order = deque()
        self.counts = Counter()
        self.missed = self.loss_records = self.duplicates = self.malformed = 0
        self.omitted_events = self.omitted_wait_events = self.omitted_frame_events = 0

    def observe(self, values, at_ms, line_number):
        if "missed" in values:
            if not isinstance(values["missed"], int) or values["missed"] < 0:
                self.malformed += 1
                return
            self.missed += values["missed"]
            self.loss_records += 1
            self.losses.append({"at_ms": at_ms, "line": line_number,
                                "missed": values["missed"], "through_seq": values.get("through_seq")})
            return
        kind = values.get("event")
        if kind not in IDR_KINDS or not all(isinstance(values.get(key), int) and values[key] >= 0
                for key in ("seq", "connection", "wait", "core_us", "frame", "value")):
            self.malformed += 1
            return
        if values["seq"] in self.sequences:
            self.duplicates += 1
            return
        # The bounded source journal is emitted in sequence order. Keep a wider
        # duplicate window than that journal; never sort by the fallible clock.
        self.sequences.add(values["seq"])
        self.sequence_order.append(values["seq"])
        if len(self.sequence_order) > self.EventLimit:
            self.sequences.remove(self.sequence_order.popleft())
        event = {"at_ms": at_ms, "line": line_number, **{key: values[key] for key in
                 ("event", "seq", "connection", "wait", "core_us", "frame", "value")}}
        event.update({key: values[key] for key in ("totalsValid", "requests", "intact", "fecFailedBlocks") if key in values})
        if len(self.events) == self.EventLimit:
            self.omitted_events += 1
        self.events.append(event)
        self.counts[kind] += 1
        if values["wait"]:
            key = (values["connection"], values["wait"])
            if key not in self.waits and len(self.waits) < self.WaitLimit:
                self.waits[key] = {"connection": key[0], "wait": key[1], "observed_event_counts": Counter(),
                    "admitted_totals_max": None, "totals_unavailable_events": 0,
                    "entry": None, "exit": None, "first_seq": values["seq"], "last_seq": values["seq"]}
            wait = self.waits.get(key)
            if wait is None:
                self.omitted_wait_events += 1
            else:
                wait["observed_event_counts"][kind] += 1
                wait["last_seq"] = values["seq"]
                if kind == "wait-enter" and wait["entry"] is None:
                    wait["entry"] = event
                if kind == "wait-exit" and wait["exit"] is None:
                    wait["exit"] = event
                if values.get("totalsValid") == 1 and all(isinstance(values.get(name), int) and values[name] >= 0
                        for name in ("requests", "intact", "fecFailedBlocks")):
                    totals = wait["admitted_totals_max"]
                    if totals is None:
                        totals = dict.fromkeys(("requests", "intact", "fecFailedBlocks"), 0)
                        wait["admitted_totals_max"] = totals
                    for name in totals:
                        totals[name] = max(totals[name], values[name])
                else:
                    wait["totals_unavailable_events"] += 1
        if kind in ("idr-receive-hint", "fec-failure", "intact-frame", "idr-recognized", "idr-assembled"):
            key = (values["connection"], values["frame"])
            if key not in self.frames and len(self.frames) < self.FrameLimit:
                self.frames[key] = {"connection": key[0], "frame": key[1], "waits": set(),
                    "observed_event_counts": Counter(), "hint_advertised_blocks": set(),
                    "omitted_wait_links": 0, "omitted_hint_block_values": 0,
                    "observed_failed_blocks": 0, "first_seq": values["seq"], "last_seq": values["seq"]}
            frame = self.frames.get(key)
            if frame is None:
                self.omitted_frame_events += 1
            else:
                if len(frame["waits"]) < self.WaitLimit + 1 or values["wait"] in frame["waits"]:
                    frame["waits"].add(values["wait"])
                else:
                    frame["omitted_wait_links"] += 1
                frame["observed_event_counts"][kind] += 1
                frame["last_seq"] = values["seq"]
                if kind == "idr-receive-hint":
                    if len(frame["hint_advertised_blocks"]) < 16 or values["value"] in frame["hint_advertised_blocks"]:
                        frame["hint_advertised_blocks"].add(values["value"])
                    else:
                        frame["omitted_hint_block_values"] += 1
                if kind == "fec-failure":
                    frame["observed_failed_blocks"] += values["value"]

    def summary(self):
        waits = []
        for wait in self.waits.values():
            entry, exit_event = wait["entry"], wait["exit"]
            duration = exit_event["core_us"] - entry["core_us"] if entry and exit_event else None
            reasons = []
            if self.missed:
                reasons.append("journal-loss-unknown-wait-attribution")
            if self.malformed:
                reasons.append("malformed-journal-lines")
            if not entry or not exit_event:
                reasons.append("entry-or-exit-not-observed")
            if wait["totals_unavailable_events"]:
                reasons.append("some-per-wait-totals-unavailable")
            if duration is not None and duration < 0:
                reasons.append("backward-core-clock-duration-rejected")
            waits.append({**wait, "observed_event_counts": dict(wait["observed_event_counts"]),
                "wait_duration_us": duration if duration is not None and duration >= 0 else None,
                "late_after_exit_observed": bool(exit_event and wait["last_seq"] > exit_event["seq"]),
                "incompleteness_reasons": reasons,
                "totals_scope": "maximum observed admitted per-wait snapshots; not final totals"})
        frames = [{**frame, "waits": sorted(frame["waits"]),
                   "hint_advertised_blocks": sorted(frame["hint_advertised_blocks"]),
                   "observed_event_counts": dict(frame["observed_event_counts"]),
                   "hint_and_fec_failure_observed": bool(frame["observed_event_counts"]["idr-receive-hint"]
                                                         and frame["observed_event_counts"]["fec-failure"])}
                  for frame in self.frames.values()]
        return {"observed_event_counts": dict(self.counts), "events_in_log_order": list(self.events),
                "waits": waits, "frames": frames, "journal_missed": self.missed,
                "loss_attribution": "unknown" if self.missed else "no-loss-notice-observed",
                "loss_notices": list(self.losses), "omitted_loss_notices": max(0, self.loss_records - self.LossLimit),
                "duplicate_sequences_in_retained_window": self.duplicates, "malformed_lines": self.malformed,
                "report_retention": {"event_limit": self.EventLimit, "wait_limit": self.WaitLimit,
                    "frame_limit": self.FrameLimit, "omitted_ordered_events": self.omitted_events,
                    "omitted_wait_events": self.omitted_wait_events, "omitted_frame_events": self.omitted_frame_events},
                "note": "Admitted/logged evidence only. Missing hints do not prove no IDR; dispatch acceptance does not prove host receipt; wait-exit does not prove assembly/decode/presentation."}


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def log_lines(path):
    with path.open(encoding="utf-8", errors="replace") as source:
        yield from source


def fields(text):
    result = {}
    for key, value in KV.findall(text):
        if re.fullmatch(r"-?\d+", value):
            result[key] = int(value)
        else:
            result[key] = value
    return result


def codec(value):
    return {"h264": "H.264", "h.264": "H.264", "hevc": "H.265", "h265": "H.265",
            "h.265": "H.265"}.get(str(value).lower())


def identity(version, capture):
    artifact = capture.get("artifact_sha256") if capture else None
    declared_version = capture.get("artifact_version") if capture else None
    verified = bool(capture and capture.get("readback_verified") is True)
    version_matches = version is not None and declared_version == version
    if capture and not version_matches:
        eligibility = "capture-version-conflict"
    elif version in ("0.6.15", "0.6.16"):
        eligibility = "historical-separate-from-baseline"
    elif version == BASELINE_VERSION:
        eligibility = ("baseline-capture-declared" if artifact == BASELINE_SHA256 and verified
                       else "baseline-version-only-unverified")
    elif version in INSTRUMENTED_VERSIONS:
        eligibility = "instrumented-separate-from-baseline"
    else:
        eligibility = "unknown-or-other-build"
    return {"version": version, "artifact_sha256": artifact,
            "artifact_version": declared_version, "readback_verified_declared": verified,
            "identity_source": "external-capture-manifest" if capture else "version-header-only",
            "eligibility": eligibility,
            "comparison_group": f"version:{version}" if version is not None and eligibility != "capture-version-conflict" else None,
            "note": "Comparison groups separate version headers, not binary identity; matching groups do not verify identical artifacts. External manifest is operator evidence; no binary identity is embedded in the log."}


def new_stream(number, at_ms, inferred=False):
    return {"stream": number, "start_ms": at_ms, "provider": None,
            "boundary_inferred": inferred, "configurations": [], "counter_samples": [],
            "latency_windows": [], "display_paced_observations": [], "final_decoder_seen": False}


def observe_configuration(stream, values, at_ms):
    selected = codec(values.get("codec"))
    fps = values.get("fps")
    actual = {"codec": selected, "fps": fps if isinstance(fps, int) and fps > 0 else None,
              "presentation_fps": values.get("presentation_fps"),
              "requested_codec": codec(values.get("requestedCodec")),
              "requested_fps": values.get("requestedFps")}
    if not any(value is not None for value in actual.values()):
        return
    previous = stream["configurations"][-1] if stream["configurations"] else {}
    if any(actual[key] != previous.get(key) for key in actual):
        stream["configurations"].append({"at_ms": at_ms, **actual})


def counter_summary(samples):
    totals = dict.fromkeys(COUNTERS, 0)
    intervals, resets = [], 0
    for previous, current in zip(samples, samples[1:]):
        if any(current[key] < previous[key] for key in COUNTERS):
            resets += 1
            continue  # First sample after a reset is an anchor, never an assumed zero.
        delta = {key: current[key] - previous[key] for key in COUNTERS}
        for key in COUNTERS:
            totals[key] += delta[key]
        intervals.append({"from_ms": previous["at_ms"], "to_ms": current["at_ms"], **delta})
    decoded = totals["decoded_frames"]
    ratio = totals["superseded_frames"] / decoded if decoded else None
    return {"sample_count": len(samples), "counter_reset_boundaries": resets,
            "observed_deltas": totals, "intervals": intervals,
            "superseded_per_decoded": ratio,
            "note": "Consecutive monotonic snapshots only. First samples anchor each stream/reset; cumulative snapshots are never summed."}


def parse_log(path, captures=None):
    path = Path(path)
    file_hash = sha256(path)
    capture = (captures or {}).get(file_hash)
    runs, run, stream = [], None, None
    for line_number, line in enumerate(log_lines(path), 1):
        stamp = STAMP.match(line)
        at_ms, text = (int(stamp[1]), stamp[2]) if stamp else (None, line)
        header = HEADER.search(text)
        if header:
            run = {"run": len(runs) + 1, "header_line": line_number,
                   "identity": identity(header[1], capture), "streams": [], "_idr": IdrJournal(), "_threads": SchedulingJournal()}
            runs.append(run)
            stream = None
            continue
        relevant = any(marker in text for marker in
                       ("Controller profile:", "Stream protocol:", "Video decoder", "Client latency:", "Input latency:", "Moonlight IDR ",
                        "Thread scheduling ", "Moonlight thread "))
        if not relevant:
            continue
        if run is None:
            run = {"run": 1, "header_line": None, "identity": identity(None, capture), "streams": [], "_idr": IdrJournal(), "_threads": SchedulingJournal()}
            runs.append(run)
        if "Moonlight IDR " in text:
            run["_idr"].observe(fields(text), at_ms, line_number)
            continue  # Stable connection/wait IDs, never the current UI attempt.
        if "Thread scheduling " in text or "Moonlight thread " in text:
            run["_threads"].observe(text, at_ms, line_number, stream["stream"] if stream else None)
            continue
        if "Controller profile:" in text or stream is None:
            stream = new_stream(len(run["streams"]) + 1, at_ms, "Controller profile:" not in text)
            run["streams"].append(stream)
        values = fields(text)
        if "Controller profile:" in text:
            stream["provider"] = {"mts": "MTS", "moonlight": "FH", "FH": "FH"}.get(values.get("provider"))
        if "Stream protocol:" in text:
            if "Moonlight" in text:
                stream["provider"] = "FH"
            observe_configuration(stream, values, at_ms)
        if "Video decoder" in text:
            observe_configuration(stream, values, at_ms)
            if all(isinstance(values.get(key), int) and values[key] >= 0 for key in COUNTERS):
                stream["counter_samples"].append({"at_ms": at_ms, **{key: values[key] for key in COUNTERS}})
            if "displayPaced" in values:
                stream["display_paced_observations"].append({"at_ms": at_ms, **{
                    key: values[key] for key in ("displayPaced", "queueFps", "readyEmpty", "pushCapacityDrop",
                                               "takeStaleDrop", "timedLateCoalesce", "taken", "resets") if key in values}})
            if "Video decoder final:" in text:
                stream["final_decoder_seen"] = True
        if "Client latency:" in text:
            windows = {name: fields(body) for name, body in
                       re.findall(r"(frameAge|assembledQueueWait|presentGap)\{([^{}]*)\}", text)}
            stream["latency_windows"].append({"at_ms": at_ms, "kind": "video", "histograms": windows,
                **{key: values[key] for key in ("windowMs", "lateThresholdUs", "lateCount", "latePpm",
                    "frameAgeUnavailableLifetime", "queueWaitUnavailableLifetime", "originMissesLifetime") if key in values}})
        if "Input latency:" in text:
            if values.get("provider") in ("FH", "MTS"):
                stream["provider"] = values["provider"]
            stream["latency_windows"].append({"at_ms": at_ms, "kind": "input", **{
                key: values[key] for key in ("scope", "boundary", "accepted-only", "samples", "p50Us", "p95Us",
                                           "p99Us", "overflowLifetime", "unavailable") if key in values}})
    for item in runs:
        item["moonlight_idr"] = item.pop("_idr").summary()
        item["thread_scheduling"] = item.pop("_threads").summary(item["moonlight_idr"]["journal_missed"])
        for entry in item["streams"]:
            entry["counters"] = counter_summary(entry["counter_samples"])
            entry["latency_note"] = "Rolling windows may overlap. Their percentiles and sample counts are observations, not mergeable distributions."
    return {"log": str(path), "log_sha256": file_hash, "capture_manifest_matched": capture is not None,
            "runs": runs, "av_skew": "unavailable: no common mapped audio/video presentation clock"}


def load_captures(path):
    if path is None:
        return {}
    document = json.loads(Path(path).read_text(encoding="utf-8"))
    result = {}
    for capture in document["captures"]:
        for key in ("log_sha256", "artifact_sha256"):
            if not re.fullmatch(r"[0-9a-f]{64}", capture.get(key, "")):
                raise ValueError(f"capture {key} must be a lowercase SHA-256")
        if capture["log_sha256"] in result:
            raise ValueError("duplicate capture log_sha256")
        if "readback_verified" in capture and not isinstance(capture["readback_verified"], bool):
            raise ValueError("readback_verified must be a JSON boolean")
        result[capture["log_sha256"]] = capture
    return result


def positive(value):
    value = int(value)
    if value < 1:
        raise argparse.ArgumentTypeError("must be positive")
    return value


def stimulus(directory, events=8, fps=60, interval_frames=120, lead_frames=120,
             flash_frames=3, tail_frames=120, width=320, height=180, render=False):
    rate, click_ms = 48000, 20
    if any(value < 1 for value in (events, fps, interval_frames, lead_frames, flash_frames, tail_frames, width, height)):
        raise ValueError("stimulus parameters must be positive")
    if rate % fps or flash_frames >= interval_frames or width % 2 or height % 2:
        raise ValueError("sample rate must divide into whole video frames; flashes must be separated; dimensions must be even")
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    click_samples = rate * click_ms // 1000
    if click_samples > interval_frames * rate // fps:
        raise ValueError("audio clicks must not overlap")
    click_frames = (click_samples * fps + rate - 1) // rate
    total_frames = lead_frames + (events - 1) * interval_frames + max(flash_frames, click_frames) + tail_frames
    total_samples = total_frames * rate // fps
    timeline = [{"event_id": n + 1, "video_frame": lead_frames + n * interval_frames,
                 "audio_sample": (lead_frames + n * interval_frames) * rate // fps,
                 "expected_skew_ms": 0} for n in range(events)]
    # A deterministic 1 kHz burst, with a short tail taper and a nonzero first
    # sample, makes the source onset sample identifiable in an audio editor.
    click = [int(24000 * math.cos(2 * math.pi * 1000 * n / rate) * min(1, (click_samples - n) / 96))
             for n in range(click_samples)]
    wav_path = directory / "flash-click.wav"
    with wave.open(str(wav_path), "wb") as target:
        target.setparams((1, 2, rate, total_samples, "NONE", "not compressed"))
        cursor = 0
        for event in timeline:
            target.writeframesraw(b"\x00\x00" * (event["audio_sample"] - cursor))
            target.writeframesraw(struct.pack("<" + "h" * len(click), *click))
            cursor = event["audio_sample"] + click_samples
        target.writeframesraw(b"\x00\x00" * (total_samples - cursor))
    document = {"schema": 1, "fps": fps, "sample_rate": rate, "width": width, "height": height,
                "total_frames": total_frames, "total_samples": total_samples, "flash_frames": flash_frames,
                "click_samples": click_samples, "events": timeline,
                "boundary": "Source video-frame start and source PCM onset; player/display/speaker timing is not calibrated.",
                "wav_sha256": sha256(wav_path), "video_rendered": False}
    if render:
        executable = shutil.which("ffmpeg")
        if not executable:
            raise ValueError("ffmpeg unavailable; WAV/timeline can be generated without --render")
        movie = directory / "flash-click.mkv"
        command = [executable, "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo",
                   "-pixel_format", "gray", "-video_size", f"{width}x{height}", "-framerate", str(fps),
                   "-i", "pipe:0", "-i", str(wav_path), "-c:v", "libx264", "-preset", "veryfast",
                   "-crf", "18", "-pix_fmt", "yuv420p", "-c:a", "pcm_s16le", str(movie)]
        onsets = {event["video_frame"] for event in timeline}
        black, white = bytes(width * height), b"\xff" * (width * height)
        with (directory / "ffmpeg.log").open("wb") as log:
            process = subprocess.Popen(command, stdin=subprocess.PIPE, stderr=log)
            try:
                for frame in range(total_frames):
                    flashing = any(frame - age in onsets for age in range(flash_frames))
                    process.stdin.write(white if flashing else black)
                process.stdin.close()
            except BaseException:
                process.kill()
                process.wait()
                raise
            if process.wait() != 0:
                raise ValueError(f"ffmpeg failed; inspect {directory / 'ffmpeg.log'}")
        document.update(video_rendered=True, movie_sha256=sha256(movie), render_command=command)
    (directory / "timeline.json").write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    return document


def quantiles(values):
    ordered = sorted(values)
    if not ordered:
        return {"samples": 0, "p50_ms": None, "p95_ms": None, "p99_ms": None, "min_ms": None, "max_ms": None}
    return {"samples": len(ordered), **{f"p{p}_ms": ordered[math.ceil(len(ordered) * p / 100) - 1]
            for p in (50, 95, 99)}, "min_ms": ordered[0], "max_ms": ordered[-1]}


def skew(path, timeline_path, calibration_ms=0):
    if not math.isfinite(calibration_ms):
        raise ValueError("calibration must be finite")
    timeline = json.loads(Path(timeline_path).read_text(encoding="utf-8"))
    expected = {str(event["event_id"]): event for event in timeline["events"]}
    observations, seen = [], set()
    with Path(path).open(newline="", encoding="utf-8-sig") as source:
        reader = csv.DictReader(source)
        if not {"event_id", "video_ms", "audio_ms"}.issubset(reader.fieldnames or []):
            raise ValueError("CSV needs event_id,video_ms,audio_ms columns")
        for row in reader:
            event_id = row["event_id"].strip()
            if event_id not in expected or event_id in seen:
                raise ValueError(f"unknown or duplicate event_id: {event_id}")
            video, audio = float(row["video_ms"]), float(row["audio_ms"])
            if any(not math.isfinite(value) or value < 0 for value in (video, audio)):
                raise ValueError("recording timestamps must be finite and nonnegative")
            raw = audio - video
            corrected = raw - expected[event_id]["expected_skew_ms"] - calibration_ms
            observations.append({"event_id": event_id, "video_ms": video, "audio_ms": audio,
                                 "raw_skew_ms": raw, "corrected_skew_ms": corrected})
            seen.add(event_id)
    if not observations:
        raise ValueError("no annotated events")
    corrected = [event["corrected_skew_ms"] for event in observations]
    return {"sign": "audio_ms - video_ms; positive means audio late, negative means audio early",
            "boundary": "Annotated visible flash and recorded click in the SAME recording timeline; not inferred from queue depth or RTP.",
            "calibration_ms": calibration_ms, "raw_signed": quantiles([event["raw_skew_ms"] for event in observations]),
            "corrected_signed": quantiles(corrected), "corrected_absolute": quantiles([abs(value) for value in corrected]),
            "expected_events": len(expected), "missing_event_ids": sorted(set(expected) - seen, key=int),
            "observations": observations}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    logs = commands.add_parser("logs", help="Separate build identities and retain per-stream observations")
    logs.add_argument("paths", nargs="+", type=Path)
    logs.add_argument("--manifest", type=Path)
    source = commands.add_parser("stimulus", help="Generate exact source flash/click timing and PCM WAV")
    source.add_argument("directory", type=Path)
    source.add_argument("--render", action="store_true", help="Also render MKV using installed ffmpeg")
    for name, default in (("events", 8), ("fps", 60), ("interval-frames", 120), ("lead-frames", 120),
                          ("flash-frames", 3), ("tail-frames", 120), ("width", 320), ("height", 180)):
        source.add_argument("--" + name, type=positive, default=default)
    offset = commands.add_parser("skew", help="Signed skew from manually annotated recording timestamps")
    offset.add_argument("annotations", type=Path)
    offset.add_argument("--timeline", type=Path, required=True)
    offset.add_argument("--calibration-ms", type=float, default=0)
    args = parser.parse_args(argv)
    try:
        if args.command == "logs":
            captures = load_captures(args.manifest)
            result = {"schema": 1, "baseline_version": BASELINE_VERSION, "baseline_sha256": BASELINE_SHA256,
                      "logs": [parse_log(path, captures) for path in args.paths]}
            if captures and any(not entry["capture_manifest_matched"] for entry in result["logs"]):
                raise ValueError("capture manifest has no matching log_sha256 for an input log")
        elif args.command == "stimulus":
            result = stimulus(args.directory, args.events, args.fps, args.interval_frames, args.lead_frames,
                              args.flash_frames, args.tail_frames, args.width, args.height, args.render)
        else:
            result = skew(args.annotations, args.timeline, args.calibration_ms)
    except (OSError, ValueError, KeyError, csv.Error) as error:
        parser.error(str(error))
    json.dump(result, sys.stdout, indent=2, allow_nan=False)
    print()


if __name__ == "__main__":
    main()
