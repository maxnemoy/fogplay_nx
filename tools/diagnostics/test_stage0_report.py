#!/usr/bin/env python3
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import wave


SPEC = importlib.util.spec_from_file_location("stage0_report", Path(__file__).with_name("stage0-report.py"))
report = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(report)


class Stage0ReportTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="fogplay-stage0-")
        self.directory = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def log(self, text):
        path = self.directory / "capture.log"
        path.write_text(text, encoding="utf-8")
        return path

    @staticmethod
    def idr(kind, seq, connection=1, wait=1, core_us=None, frame=0, value=0, totals=(0, 0, 0), valid=1):
        requests, intact, failed = totals
        return (f"[{seq} ms] Moonlight IDR event={kind} seq={seq} connection={connection} wait={wait} "
                f"core_us={core_us if core_us is not None else seq * 100} frame={frame} value={value} "
                f"totalsValid={valid} requests={requests} intact={intact} fecFailedBlocks={failed}")

    def parse_idr(self, lines):
        path = self.log("[0 ms] FogPlay NX 0.6.19 startup.\n" + "\n".join(lines) + "\n")
        return report.parse_log(path)["runs"][0]["moonlight_idr"]

    def test_idr_late_old_wait_keeps_identity_and_totals_are_maxima(self):
        result = self.parse_idr([
            self.idr("request", 1, wait=7, core_us=200, totals=(1, 0, 0)),
            self.idr("wait-enter", 2, wait=7, core_us=100),  # Entry publishes after request.
            self.idr("idr-recognized", 3, wait=7, core_us=250, frame=55, totals=(1, 2, 3)),
            self.idr("wait-exit", 4, wait=7, core_us=260, frame=55, totals=(1, 2, 3)),
            self.idr("wait-enter", 5, wait=8, core_us=270),
            self.idr("dispatch-result", 6, wait=7, core_us=280, value=1, totals=(2, 4, 5)),
            self.idr("request", 7, wait=8, core_us=290, totals=(1, 0, 0)),
            self.idr("request", 8, connection=2, wait=7, totals=(1, 0, 0))])
        first, second, other_connection = result["waits"]
        self.assertEqual([event["seq"] for event in result["events_in_log_order"]], list(range(1, 9)))
        self.assertEqual(first["admitted_totals_max"], {"requests": 2, "intact": 4, "fecFailedBlocks": 5})
        self.assertEqual(first["exit"]["requests"], 1)
        self.assertEqual(first["observed_event_counts"]["request"], 1)
        self.assertTrue(first["late_after_exit_observed"])
        self.assertEqual(first["wait_duration_us"], 160)
        self.assertEqual(second["admitted_totals_max"]["requests"], 1)
        self.assertEqual((other_connection["connection"], other_connection["wait"]), (2, 7))

    def test_idr_frame_hint_correlates_across_wait_zero_and_fec_wait(self):
        result = self.parse_idr([
            self.idr("idr-receive-hint", 1, wait=0, frame=70, value=3, valid=0),
            self.idr("idr-receive-hint", 2, wait=0, frame=70, value=3, valid=0),
            self.idr("fec-failure", 3, wait=12, frame=70, value=1, totals=(0, 0, 1)),
            self.idr("intact-frame", 4, wait=12, frame=70, totals=(0, 1, 1)),
            self.idr("idr-recognized", 5, wait=12, frame=70, totals=(0, 1, 1)),
            self.idr("idr-assembled", 6, wait=12, frame=70, totals=(0, 1, 1)),
            self.idr("fec-failure", 7, connection=2, wait=13, frame=70, value=2, totals=(0, 0, 2))])
        first, other_connection = result["frames"]
        self.assertEqual(first["waits"], [0, 12])
        self.assertEqual(first["hint_advertised_blocks"], [3])
        self.assertEqual(first["observed_event_counts"]["idr-receive-hint"], 2)
        self.assertEqual(first["observed_failed_blocks"], 1)
        self.assertTrue(first["hint_and_fec_failure_observed"])
        self.assertFalse(other_connection["hint_and_fec_failure_observed"])
        self.assertNotIn(0, [wait["wait"] for wait in result["waits"]])

    def test_idr_journal_loss_has_unknown_wait_attribution(self):
        result = self.parse_idr([
            self.idr("wait-enter", 1, wait=9),
            self.idr("wait-exit", 2, wait=9, totals=(3, 5, 2)),
            "[3 ms] Moonlight IDR journal missed=5 through_seq=10",
            self.idr("wait-enter", 11, wait=10),
            self.idr("wait-exit", 12, wait=10, totals=(1, 2, 0)),
            "[13 ms] Moonlight IDR journal missed=2 through_seq=12"])
        self.assertEqual(result["journal_missed"], 7)
        self.assertEqual(result["loss_attribution"], "unknown")
        for wait in result["waits"]:
            self.assertIn("journal-loss-unknown-wait-attribution", wait["incompleteness_reasons"])
        self.assertEqual(result["waits"][0]["admitted_totals_max"]["requests"], 3)

    def test_idr_unavailable_totals_clock_rollback_and_duplicate(self):
        entry = self.idr("wait-enter", 1, core_us=1000)
        result = self.parse_idr([entry, entry,
            self.idr("request", 2, totals=(2, 3, 4)),
            self.idr("wait-exit", 3, core_us=900, totals=(999, 999, 999), valid=0)])
        wait = result["waits"][0]
        self.assertEqual(result["duplicate_sequences_in_retained_window"], 1)
        self.assertEqual(wait["observed_event_counts"]["wait-enter"], 1)
        self.assertEqual(wait["admitted_totals_max"], {"requests": 2, "intact": 3, "fecFailedBlocks": 4})
        self.assertEqual(wait["totals_unavailable_events"], 1)
        self.assertIsNone(wait["wait_duration_us"])
        self.assertIn("backward-core-clock-duration-rejected", wait["incompleteness_reasons"])

    def test_idr_report_retention_is_explicit_and_counts_keep_advancing(self):
        with patch.multiple(report.IdrJournal, EventLimit=3, WaitLimit=1, FrameLimit=1, LossLimit=1):
            result = self.parse_idr([self.idr("idr-receive-hint", seq, wait=seq, frame=seq, value=2)
                                     for seq in range(1, 7)])
        self.assertEqual([event["seq"] for event in result["events_in_log_order"]], [4, 5, 6])
        self.assertEqual(result["observed_event_counts"]["idr-receive-hint"], 6)
        self.assertEqual(len(result["waits"]), 1)
        self.assertEqual(len(result["frames"]), 1)
        self.assertEqual(result["report_retention"]["omitted_ordered_events"], 3)
        self.assertEqual(result["report_retention"]["omitted_wait_events"], 5)
        self.assertEqual(result["report_retention"]["omitted_frame_events"], 5)

    def test_versions_and_external_identity_do_not_mix(self):
        path = self.log("\n".join(f"[0 ms] FogPlay NX {version} startup." for version in
                                  ("0.6.15", "0.6.16", "0.6.18", "0.6.19", "0.6.20")))
        parsed = report.parse_log(path)
        self.assertEqual([run["identity"]["eligibility"] for run in parsed["runs"]], [
            "historical-separate-from-baseline", "historical-separate-from-baseline",
            "baseline-version-only-unverified", "instrumented-separate-from-baseline", "instrumented-separate-from-baseline"])
        capture = {report.sha256(path): {"artifact_sha256": report.BASELINE_SHA256,
                                        "artifact_version": "0.6.18", "readback_verified": True}}
        parsed = report.parse_log(path, capture)
        self.assertEqual(parsed["runs"][2]["identity"]["eligibility"], "baseline-capture-declared")
        self.assertEqual(parsed["runs"][3]["identity"]["eligibility"], "capture-version-conflict")
        capture[report.sha256(path)]["readback_verified"] = False
        self.assertEqual(report.parse_log(path, capture)["runs"][2]["identity"]["eligibility"],
                         "baseline-version-only-unverified")

    def test_instrumented_versions_have_distinct_comparison_groups(self):
        versions = ("0.6.19", "0.6.20", "0.6.21", "0.6.22")
        lines = []
        for number, version in enumerate(versions, 1):
            lines.extend((f"[0 ms] FogPlay NX {version} startup.",
                "[1 ms] Controller profile: xbox; provider=mts",
                f"[2 ms] Video decoder: decoded_frames={number} uploaded_frames={number} presented_frames={number} superseded_frames=0"))
        runs = report.parse_log(self.log("\n".join(lines)))["runs"]
        self.assertEqual([run["identity"]["comparison_group"] for run in runs],
                         [f"version:{version}" for version in versions])
        self.assertEqual(len({run["identity"]["comparison_group"] for run in runs}), 4)
        self.assertTrue(all(run["identity"]["eligibility"] == "instrumented-separate-from-baseline" for run in runs))
        self.assertEqual([run["streams"][0]["counter_samples"][0]["decoded_frames"] for run in runs], [1, 2, 3, 4])
        self.assertIsNone(report.identity(None, None)["comparison_group"])
        self.assertIsNone(report.identity("0.6.21", {"artifact_version": "0.6.20"})["comparison_group"])
        self.assertEqual(report.identity("0.6.18", None)["comparison_group"], "version:0.6.18")

    def test_scheduling_rows_preserve_spaces_epochs_native_ids_and_partial_errors(self):
        path = self.log("""[0 ms] FogPlay NX 0.6.19 startup.
[1 ms] Thread scheduling scope=transport-observed-starts inventory=not-live epoch=3 count=2 overflow=0 persistentOverflowLifetime=0
[2 ms] Thread scheduling role=RTC worker slot=0 epoch=3 observedEpoch=0 carried=1 available=1 core=2 ideal=1 mask=0xf priority=44 queryErrors=0 affinityResult=0 priorityResult=0
[3 ms] Controller profile: xbox; provider=moonlight
[4 ms] Moonlight thread role=Video receive connection=7 core=1 priority=42 mask=15 ideal=-2 queryErrors=0 available=1 seq=10 core_us=100
[5 ms] Moonlight thread role=Audio receive connection=7 core=2 priority=41 mask=0 ideal=-1 queryErrors=1 available=0 seq=11 core_us=110
[6 ms] Moonlight thread role=Control receive connection=7 core=3 priority=-1 mask=7 ideal=2 queryErrors=2 available=0 seq=12 core_us=120
""")
        run = report.parse_log(path)["runs"][0]
        scheduling = run["thread_scheduling"]
        self.assertEqual(len(run["streams"]), 1)
        self.assertEqual(scheduling["observed_rows"], 4)
        worker, video, audio, control = scheduling["observations"]
        self.assertEqual(worker["role"], "RTC worker")
        self.assertEqual((worker["epoch"], worker["observed_epoch"], worker["carried"]), (3, 0, True))
        self.assertIsNone(worker["ui_stream_hint"])
        self.assertEqual((worker["core"], worker["ideal_core"], worker["affinity_mask"], worker["priority"]), (2, 1, 15, 44))
        self.assertEqual((video["source"], video["connection"], video["ui_stream_hint"]), ("moonlight", 7, 1))
        self.assertEqual(video["raw"]["ideal"], -2)
        self.assertIsNone(video["ideal_core"])
        self.assertEqual((audio["priority"], audio["availability"]), (41, "query-error"))
        self.assertIsNone(audio["affinity_mask"])
        self.assertEqual((control["affinity_mask"], control["ideal_core"]), (7, 2))
        self.assertIsNone(control["priority"])
        self.assertEqual(scheduling["query_error_rows"], 2)

    def test_scheduling_overflow_is_max_per_epoch_and_loss_not_assigned_to_threads(self):
        path = self.log("""[0 ms] FogPlay NX 0.6.19 startup.
[1 ms] Thread scheduling scope=transport-observed-starts epoch=1 count=128 overflow=4 persistentOverflowLifetime=2
[2 ms] Thread scheduling scope=transport-observed-starts epoch=1 count=128 overflow=4 persistentOverflowLifetime=2
[3 ms] Thread scheduling scope=transport-observed-starts epoch=2 count=3 overflow=0 persistentOverflowLifetime=2
[4 ms] Thread scheduling role=media-video slot=1 epoch=2 observedEpoch=2 carried=0 available=0 core=-1 ideal=-1 mask=0 priority=-1 queryErrors=0 affinityResult=-1 priorityResult=-1
[5 ms] Moonlight IDR journal missed=6 through_seq=12
""")
        scheduling = report.parse_log(path)["runs"][0]["thread_scheduling"]
        self.assertEqual([epoch["overflow_max_observed"] for epoch in scheduling["epochs"]], [4, 0])
        self.assertEqual(scheduling["epochs"][0]["persistent_overflow_lifetime_max_observed"], 2)
        self.assertEqual(scheduling["moonlight_journal_missed_unknown_event_types"], 6)
        row = scheduling["observations"][0]
        self.assertEqual(row["availability"], "unavailable")
        self.assertEqual(set(row["unknown_fields"]), {"core", "ideal_core", "affinity_mask", "priority"})
        self.assertIsNone(row["affinity_mask"]) # Host's zero placeholder is not a queried mask.

    def test_scheduling_legacy_unknown_invalid_and_run_isolation(self):
        path = self.log("""Thread scheduling scope=observed-starts count=1 overflow=8
Thread scheduling role=RTC worker slot=0 available=1 core=0 ideal=0 mask=3 priority=44
Moonlight thread role=legacy core=bad priority=-1 mask=0 ideal=-1 available=bad queryErrors=-1
Thread scheduling scope=bad count=oops overflow=0
Moonlight thread role= core=0
[0 ms] FogPlay NX 0.6.20 startup.
Thread scheduling scope=transport-observed-starts epoch=1 count=0 overflow=0
""")
        old, new = report.parse_log(path)["runs"]
        journal = old["thread_scheduling"]
        self.assertEqual(len(old["streams"]), 0)
        self.assertEqual(journal["observations"][0]["role"], "RTC worker")
        self.assertIsNone(journal["observations"][0]["epoch"])
        self.assertEqual(journal["observations"][1]["availability"], "unknown")
        self.assertEqual(set(journal["observations"][1]["invalid_fields"]), {"core", "available", "queryErrors"})
        self.assertEqual(journal["malformed_lines"], 2)
        self.assertEqual(new["thread_scheduling"]["observed_rows"], 0)
        self.assertEqual(new["thread_scheduling"]["epochs"][0]["overflow_max_observed"], 0)

    def test_scheduling_retention_reports_omissions(self):
        with patch.multiple(report.SchedulingJournal, RowLimit=2, SnapshotLimit=1, EpochLimit=1):
            path = self.log("\n".join(
                [f"Thread scheduling epoch={n} count=1 overflow={n}" for n in range(3)] +
                [f"Moonlight thread role=worker-{n} core=1 ideal=1 mask=2 priority=42 available=1 queryErrors=0 connection=1"
                 for n in range(4)]))
            scheduling = report.parse_log(path)["runs"][0]["thread_scheduling"]
        self.assertEqual([row["role"] for row in scheduling["observations"]], ["worker-2", "worker-3"])
        self.assertEqual(scheduling["retention"]["omitted_rows"], 2)
        self.assertEqual(scheduling["retention"]["omitted_snapshots"], 2)
        self.assertEqual(scheduling["retention"]["omitted_epoch_snapshots"], 2)

    def test_stream_deltas_do_not_sum_lifetime_counters(self):
        path = self.log("""[0 ms] FogPlay NX 0.6.19 startup.
[100 ms] Controller profile: xbox; provider=moonlight
[200 ms] Stream protocol: Moonlight codec=H.265 requestedCodec=H.265 fps=60 requestedFps=60
[1000 ms] Video decoder: codec=hevc decoded_frames=100 uploaded_frames=90 presented_frames=90 superseded_frames=10 displayPaced=1 queueFps=60
[2000 ms] Video decoder: codec=hevc decoded_frames=200 uploaded_frames=185 presented_frames=185 superseded_frames=15 displayPaced=1 queueFps=60
[2001 ms] Video decoder final: codec=hevc decoded_frames=200 uploaded_frames=185 presented_frames=185 superseded_frames=15
[3000 ms] Controller profile: xbox; provider=moonlight
[4000 ms] Video decoder: codec=hevc decoded_frames=250 uploaded_frames=230 presented_frames=230 superseded_frames=20
[5000 ms] Video decoder: codec=hevc decoded_frames=300 uploaded_frames=275 presented_frames=275 superseded_frames=25
""")
        first, second = report.parse_log(path)["runs"][0]["streams"]
        self.assertEqual(first["provider"], "FH")
        self.assertEqual(first["configurations"][0]["fps"], 60)
        self.assertEqual(first["configurations"][0]["codec"], "H.265")
        self.assertTrue(first["final_decoder_seen"])
        self.assertEqual(first["display_paced_observations"][0]["displayPaced"], 1)
        self.assertEqual(first["counters"]["observed_deltas"]["decoded_frames"], 100)
        self.assertEqual(first["counters"]["superseded_per_decoded"], 0.05)
        self.assertEqual(second["counters"]["observed_deltas"]["decoded_frames"], 50)

    def test_reset_and_unknown_build_are_conservative(self):
        path = self.log("""[100 ms] Video decoder: presentation_fps=60 decoded_frames=200 uploaded_frames=190 presented_frames=190 superseded_frames=10
[200 ms] Video decoder: decoded_frames=5 uploaded_frames=4 presented_frames=4 superseded_frames=1
[300 ms] Video decoder: decoded_frames=15 uploaded_frames=13 presented_frames=13 superseded_frames=2
""")
        run = report.parse_log(path)["runs"][0]
        self.assertEqual(run["identity"]["eligibility"], "unknown-or-other-build")
        stream = run["streams"][0]
        self.assertTrue(stream["boundary_inferred"])
        self.assertIsNone(stream["provider"])
        self.assertIsNone(stream["configurations"][0]["fps"])
        self.assertEqual(stream["configurations"][0]["presentation_fps"], 60)
        self.assertEqual(stream["counters"]["counter_reset_boundaries"], 1)
        self.assertEqual(stream["counters"]["observed_deltas"]["decoded_frames"], 10)

    def test_rolling_quantiles_are_retained_not_averaged(self):
        path = self.log("""[0 ms] FogPlay NX 0.6.19 startup.
[1 ms] Controller profile: xbox; provider=mts
[2000 ms] Client latency: videoLatency windowMs=5000 frameAge{samples=60 p50Us=11000 p95Us=23000 p99Us=31000 overflowLifetime=0} assembledQueueWait{samples=60 p50Us=1000 p95Us=2000 p99Us=3000 overflowLifetime=0} presentGap{samples=59 p50Us=16667 p95Us=33334 p99Us=50000 overflowLifetime=0} lateCount=2 latePpm=33898
[2000 ms] Input latency: provider=MTS scope=rolling-5s boundary=pad-to-send-return accepted-only=1 samples=40 p50Us=50 p95Us=150 p99Us=800 overflowLifetime=0
[4000 ms] Input latency: provider=MTS scope=rolling-5s boundary=pad-to-send-return accepted-only=1 samples=80 p50Us=70 p95Us=250 p99Us=900 overflowLifetime=0
""")
        stream = report.parse_log(path)["runs"][0]["streams"][0]
        self.assertEqual(stream["provider"], "MTS")
        windows = stream["latency_windows"]
        self.assertEqual(windows[0]["histograms"]["frameAge"]["p95Us"], 23000)
        self.assertEqual([window["p95Us"] for window in windows[1:]], [150, 250])
        self.assertEqual([window["samples"] for window in windows[1:]], [40, 80])

    def test_capture_manifest_requires_matching_log_bytes(self):
        path = self.log("[0 ms] FogPlay NX 0.6.18 startup.\n")
        manifest = self.directory / "captures.json"
        manifest.write_text(json.dumps({"captures": [{"log_sha256": "a" * 64,
            "artifact_sha256": report.BASELINE_SHA256, "artifact_version": "0.6.18",
            "readback_verified": True}]}), encoding="utf-8")
        with self.assertRaises(SystemExit) as failure:
            report.main(["logs", str(path), "--manifest", str(manifest)])
        self.assertEqual(failure.exception.code, 2)

    def test_stimulus_pcm_count_and_onsets(self):
        timeline = report.stimulus(self.directory, events=3, fps=60, interval_frames=12,
                                   lead_frames=6, flash_frames=2, tail_frames=6, width=64, height=36)
        self.assertEqual(len(timeline["events"]), 3)
        self.assertEqual(timeline["total_frames"], 38)
        with wave.open(str(self.directory / "flash-click.wav"), "rb") as source:
            self.assertEqual(source.getframerate(), 48000)
            self.assertEqual(source.getnframes(), timeline["total_samples"])
            pcm = source.readframes(source.getnframes())
        samples = struct.unpack("<" + "h" * (len(pcm) // 2), pcm)
        for event in timeline["events"]:
            onset = event["audio_sample"]
            self.assertEqual(onset, event["video_frame"] * 800)
            self.assertEqual(samples[onset - 1], 0)
            self.assertEqual(samples[onset], 24000)
            self.assertEqual(samples[onset + timeline["click_samples"]], 0)
        self.assertFalse(timeline["video_rendered"])

    @unittest.skipUnless(shutil.which("ffmpeg") and shutil.which("ffprobe"), "ffmpeg/ffprobe unavailable")
    def test_rendered_artifact_has_exact_flash_and_audio_event_count(self):
        timeline = report.stimulus(self.directory, events=3, fps=60, interval_frames=12,
                                   lead_frames=6, flash_frames=2, tail_frames=6, width=64, height=36, render=True)
        movie = self.directory / "flash-click.mkv"
        pixels = subprocess.check_output(["ffmpeg", "-v", "error", "-i", str(movie), "-map", "0:v:0",
                                          "-f", "rawvideo", "-pix_fmt", "gray", "pipe:1"])
        size = 64 * 36
        self.assertEqual(len(pixels), size * timeline["total_frames"])
        onsets, active = [], False
        bright_count = 0
        for frame in range(timeline["total_frames"]):
            bright = sum(pixels[frame * size:(frame + 1) * size]) > size * 128
            bright_count += bright
            if bright and not active:
                onsets.append(frame)
            active = bright
        self.assertEqual(onsets, [event["video_frame"] for event in timeline["events"]])
        self.assertEqual(bright_count, 3 * timeline["flash_frames"])
        pcm = subprocess.check_output(["ffmpeg", "-v", "error", "-i", str(movie), "-map", "0:a:0",
                                       "-f", "s16le", "pipe:1"])
        with wave.open(str(self.directory / "flash-click.wav"), "rb") as source:
            self.assertEqual(pcm, source.readframes(source.getnframes()))

    def test_signed_skew_percentiles_and_calibration(self):
        report.stimulus(self.directory, events=3)
        annotations = self.directory / "observations.csv"
        annotations.write_text("event_id,video_ms,audio_ms\n1,1000,980\n2,2000,2010\n3,3000,3030\n", encoding="utf-8")
        result = report.skew(annotations, self.directory / "timeline.json", calibration_ms=5)
        self.assertEqual(result["raw_signed"]["p50_ms"], 10)
        self.assertEqual(result["corrected_signed"]["p50_ms"], 5)
        self.assertEqual(result["corrected_signed"]["p95_ms"], 25)
        self.assertEqual(result["corrected_signed"]["p99_ms"], 25)
        self.assertEqual(result["corrected_signed"]["min_ms"], -25)
        self.assertEqual(result["corrected_absolute"]["p50_ms"], 25)
        self.assertEqual(result["missing_event_ids"], [])

    def test_annotation_validation_and_missing_events(self):
        report.stimulus(self.directory, events=3)
        annotations = self.directory / "observations.csv"
        for content in ("event_id,video_ms,audio_ms\n1,100,nan\n",
                        "event_id,video_ms,audio_ms\n1,100,110\n1,200,210\n",
                        "event_id,video_ms,audio_ms\n8,100,110\n"):
            annotations.write_text(content, encoding="utf-8")
            with self.assertRaises(ValueError):
                report.skew(annotations, self.directory / "timeline.json")
        annotations.write_text("event_id,video_ms,audio_ms\n2,100,90\n", encoding="utf-8")
        result = report.skew(annotations, self.directory / "timeline.json")
        self.assertEqual(result["missing_event_ids"], ["1", "3"])
        self.assertEqual(result["raw_signed"]["p99_ms"], -10)

    def test_overlapping_clicks_rejected(self):
        with self.assertRaises(ValueError):
            report.stimulus(self.directory, fps=120, interval_frames=2, flash_frames=1)


if __name__ == "__main__":
    unittest.main()
