#!/usr/bin/env python3
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


SPEC = importlib.util.spec_from_file_location("stage0_report_nack_tests", Path(__file__).with_name("stage0-report.py"))
report = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(report)


class NackReportTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="fogplay-nack-")
        self.directory = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def parse(self, lines):
        path = self.directory / "capture.log"
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return report.parse_log(path)

    @staticmethod
    def start(provider="mts", origin=1000):
        return ["[0 ms] FogPlay NX 0.6.30 startup.",
                f"[0 ms] Diagnostic clock: schema=1 clock=steady startupEpochMs={origin} resolutionMs=1",
                f"[1 ms] Controller profile: xbox; provider={provider}"]

    @staticmethod
    def route(actual="direct", local="host", remote="srflx", event=None, at=2):
        route_event = f" routeEvent={event}" if event else ""
        return (f"[{at} ms] Video RTP event:{route_event} requested=all actual={actual} localType={local} "
                f"remoteType={remote} localProtocol=udp remoteProtocol=udp routeKnown=1")

    @staticmethod
    def snapshot(at=100, final=True, **overrides):
        values = {"nackEnabled": 1, "nackPackets": 0, "repairedPackets": 0,
                  "nackArrivalSamples": 0, "nackArrivalP50Ms": 0, "nackArrivalP95Ms": 0,
                  "nackArrivalBucketMs": 10, "nackArrivalBeforeRtt": 0, "nackArrivalAfterRtt": 0,
                  "nackArrivalRttUnknown": 0, "repairRtxPtCount": 0, "rtxReceived": 0,
                  "missing": 0, "resync": 0, "pli": 0}
        values.update(overrides)
        kind = "Video RTP final:" if final else "Video RTP:"
        return f"[{at} ms] {kind} " + " ".join(f"{key}={value}" for key, value in values.items())

    def test_no_losses_and_direct_route_keep_policy(self):
        result = self.parse(self.start() + [self.route(), self.snapshot()])
        run = result["runs"][0]
        session = run["streams"][0]["nack"]
        self.assertEqual(session["latest_final"]["nackPackets"], 0)
        self.assertEqual(session["arrival_interpretation"]["distribution"],
                         "unavailable-no-observed-arrival-samples")
        self.assertTrue(session["route"]["isolated_route_comparison_eligible"])
        self.assertEqual(run["nack_summary"]["route_comparison"]["direct"]["eligible_sessions"], 1)
        self.assertEqual(run["nack_summary"]["policy_decision"], "keep-policy")
        self.assertEqual(run["nack_summary"]["evidence_sufficiency"], "insufficient-data-for-policy-change")

    def test_diagnostic_profiles_preserve_legacy_and_exclude_essential(self):
        for profile in (None, "essential", "standard", "deep"):
            with self.subTest(profile=profile):
                lines = self.start()
                if profile:
                    lines.append(f"[1 ms] Diagnostic profile: schema=1 profile={profile}")
                run = self.parse(lines + [self.route(), self.snapshot()])["runs"][0]
                self.assertEqual(run["diagnostic_profile"]["profile"], profile)
                self.assertEqual(run["diagnostic_profile"]["status"], "declared" if profile else "legacy-unreported")
                self.assertTrue(run["streams"][0]["nack"]["route"]["isolated_route_comparison_eligible"])
                self.assertEqual(run["nack_summary"]["route_comparison"]["direct"]["eligible_sessions"], int(profile != "essential"))
                self.assertEqual(run["nack_summary"]["diagnostic_profile_sessions_excluded"], int(profile == "essential"))

    def test_invalid_and_conflicting_profiles_are_not_comparison_evidence(self):
        for declarations in (["schema=2 profile=standard"], ["schema=1 profile=secret-canary"],
                             ["schema=1 profile=standard", "schema=1 profile=deep"],
                             ["schema=1 profile=essential", "schema=1 profile=standard"]):
            with self.subTest(declarations=declarations):
                lines = self.start() + ["[1 ms] Diagnostic profile: " + text for text in declarations]
                run = self.parse(lines + [self.route(), self.snapshot()])["runs"][0]
                self.assertFalse(run["diagnostic_profile"]["permits_detailed_comparison"])
                self.assertEqual(run["nack_summary"]["route_comparison"]["direct"]["eligible_sessions"], 0)
                self.assertNotIn("secret-canary", json.dumps(run))

    def test_profile_only_prefix_and_repeated_run_do_not_create_phantom_streams(self):
        run = self.parse(["[0 ms] Diagnostic profile: schema=1 profile=essential"])["runs"][0]
        self.assertEqual(run["streams"], [])
        logs = self.start() + ["[1 ms] Diagnostic profile: schema=1 profile=essential", self.route(), self.snapshot()]
        runs = self.parse(logs + self.start() + [self.route(), self.snapshot()])["runs"]
        self.assertEqual(len(runs), 2)
        self.assertEqual(runs[1]["diagnostic_profile"]["status"], "legacy-unreported")
        self.assertEqual(runs[1]["nack_summary"]["route_comparison"]["direct"]["eligible_sessions"], 1)

    def test_duplicate_profile_or_schema_cannot_restore_comparison_eligibility(self):
        for declaration in ("schema=1 profile=essential profile=standard",
                            "schema=2 schema=1 profile=standard",
                            "schema=1 schema=1 profile=standard",
                            "schema=1 profile=deep profile=deep"):
            run = self.parse(self.start() + ["[1 ms] Diagnostic profile: " + declaration,
                self.route(), self.snapshot()])["runs"][0]
            self.assertEqual(run["diagnostic_profile"]["status"], "malformed")
            self.assertFalse(run["diagnostic_profile"]["permits_detailed_comparison"])
            self.assertEqual(run["nack_summary"]["route_comparison"]["direct"]["eligible_sessions"], 0)

    def test_profile_text_inside_other_messages_is_not_startup_metadata(self):
        for text in ("Stream error: Diagnostic profile: schema=1 profile=deep",
                     "prefix Diagnostic profile: schema=1 profile=standard"):
            self.assertEqual(self.parse(["[1 ms] " + text])["runs"], [])
            run = self.parse(self.start() + ["[1 ms] Diagnostic profile: schema=1 profile=essential",
                "[2 ms] " + text, self.route(), self.snapshot()])["runs"][0]
            self.assertEqual(run["diagnostic_profile"]["profile"], "essential")
            self.assertEqual(run["diagnostic_profile"]["status"], "declared")
            self.assertEqual(run["nack_summary"]["route_comparison"]["direct"]["eligible_sessions"], 0)

    def test_exact_anchor_uses_event_time_for_delayed_out_of_order_log(self):
        lines = self.start() + [self.route(),
            "[95 ms] Video chain resync: worker generation=1",
            "[110 ms] Video RTP event: reason=1 ms=1100 seq=8 missing=1",
            "[120 ms] Video RTP event: reason=1 ms=1090 seq=7 missing=1",
            self.snapshot(at=130, nackPackets=2)]
        session = self.parse(lines)["runs"][0]["streams"][0]["nack"]
        first, second = session["expired_hole_events"]
        self.assertEqual((first["normalized_ms"], second["normalized_ms"]), (100, 90))
        self.assertEqual((first["logging_delay_ms"], second["logging_delay_ms"]), (10, 30))
        self.assertEqual(first["correlation"]["resync"][0]["order"], "after")
        self.assertEqual(second["correlation"]["resync"][0]["order"], "before")
        self.assertEqual((first["line"], second["line"]), (6, 7))

    def test_missing_corrupt_and_conflicting_anchor_are_conservative(self):
        cases = {
            "unavailable": [],
            "malformed": ["[0 ms] Diagnostic clock: schema=1 clock=steady startupEpochMs=bad resolutionMs=1"],
            "conflicting": ["[0 ms] Diagnostic clock: schema=1 clock=steady startupEpochMs=1000 resolutionMs=1",
                            "[0 ms] Diagnostic clock: schema=1 clock=steady startupEpochMs=2000 resolutionMs=1"],
        }
        for status, clock_lines in cases.items():
            with self.subTest(status=status):
                lines = ["[0 ms] FogPlay NX 0.6.30 startup.", "[1 ms] Controller profile: xbox; provider=mts",
                         *clock_lines, "[100 ms] Video RTP event: reason=1 ms=5100 seq=1",
                         "[90 ms] Video chain resync: worker generation=1"]
                run = self.parse(lines)["runs"][0]
                event = run["streams"][0]["nack"]["expired_hole_events"][0]
                self.assertEqual(run["diagnostic_clock"]["status"], status)
                self.assertIsNone(event["normalized_ms"])
                self.assertIsNone(event["normalized_min_ms"])
                self.assertEqual(event["normalized_max_ms"], 102)
                self.assertEqual(event["correlation"]["resync"][0]["order"], "unknown")

    def test_one_ms_truncation_makes_boundary_order_unknown(self):
        lines = self.start() + [
            "[100 ms] Video chain resync: worker generation=1",
            "[100 ms] Video RTP event: reason=1 ms=1101 seq=1",
            "[103 ms] Video RTP event: reason=1 ms=1103 seq=2"]
        events = self.parse(lines)["runs"][0]["streams"][0]["nack"]["expired_hole_events"]
        self.assertEqual(events[0]["normalized_ms"], 101)
        self.assertEqual(events[0]["correlation"]["resync"][0]["order"], "unknown")
        self.assertEqual(events[1]["correlation"]["resync"][0]["order"], "after")

    def test_multiple_runs_providers_reconnect_and_counter_reset(self):
        lines = self.start() + [self.route(), self.snapshot(nackPackets=10, pli=2),
            self.snapshot(at=105, nackPackets=5, pli=0),
            "[110 ms] Transport reconnect attempt=1/3 rollingWindow=120s",
            self.snapshot(at=120, nackPackets=5, pli=0),
            "[200 ms] FogPlay NX 0.6.30 startup.",
            "[200 ms] Diagnostic clock: schema=1 clock=steady startupEpochMs=9000 resolutionMs=1",
            "[201 ms] Controller profile: xbox; provider=moonlight",
            self.route(actual="relay", local="relay", remote="srflx", at=202),
            self.snapshot(at=300, nackPackets=3)]
        runs = self.parse(lines)["runs"]
        first = runs[0]["streams"][0]["nack"]
        self.assertEqual(len(runs), 2)
        self.assertEqual(runs[0]["nack_summary"]["providers"], {"MTS": 1})
        self.assertEqual(runs[1]["nack_summary"]["providers"], {"FH": 1})
        self.assertEqual(first["reconnect_boundaries"][0]["attempt"], 1)
        self.assertEqual(first["counter_reset_boundaries"][0]["fields"], ["nackPackets", "pli"])
        self.assertEqual(first["pli_counter_intervals"], [])
        self.assertFalse(first["route"]["isolated_route_comparison_eligible"])
        self.assertEqual(runs[1]["streams"][0]["nack"]["route"]["route_class"], "relay")

    def test_route_change_and_unknown_are_not_direct_controls(self):
        lines = self.start() + [self.route(), self.route(actual="relay", local="relay", event="changed", at=3),
            self.snapshot(), "[400 ms] Controller profile: xbox; provider=mts",
            self.route(actual="unknown", local="unknown", remote="unknown", at=401), self.snapshot(at=402)]
        run = self.parse(lines)["runs"][0]
        mixed, unknown = [stream["nack"] for stream in run["streams"]]
        self.assertEqual(mixed["route"]["route_class"], "mixed")
        self.assertTrue(mixed["route"]["route_change_observed"])
        self.assertFalse(mixed["route"]["isolated_route_comparison_eligible"])
        self.assertEqual(unknown["route"]["route_class"], "unknown")
        self.assertFalse(unknown["route"]["isolated_route_comparison_eligible"])
        self.assertEqual(run["nack_summary"]["route_comparison"]["direct"]["eligible_sessions"], 0)
        self.assertEqual(run["nack_summary"]["mixed_unknown_or_changed_sessions_excluded"], 2)

    def test_missing_and_truncated_diagnostics_are_explicit(self):
        lines = self.start() + [
            "[10 ms] Video RTP: nackPackets=2 repairedPackets=1 pli=0",
            "[20 ms] Video RTP final: nackPackets=bad repairedPackets=1",
            "[21 ms] Video RTP event: reason=1 seq=4",
            "[22 ms] Video chain resync: worker generation=bad"]
        session = self.parse(lines)["runs"][0]["streams"][0]["nack"]
        self.assertEqual(len(session["snapshots"]), 1)
        self.assertIsNone(session["latest_final"])
        self.assertGreater(session["unavailable_field_observations"]["nackArrivalP50Ms"], 0)
        self.assertEqual(session["malformed_lines"], 3)
        self.assertEqual(session["expired_hole_events"], [])

    def test_single_zero_bucket_is_a_range_not_zero_delay(self):
        session = self.parse(self.start() + [self.snapshot(nackPackets=1, repairedPackets=1,
            nackArrivalSamples=1, nackArrivalP50Ms=0, nackArrivalP95Ms=0,
            nackArrivalBeforeRtt=1)])["runs"][0]["streams"][0]["nack"]
        interpretation = session["arrival_interpretation"]
        self.assertEqual(interpretation["percentile_bucket_ranges"]["p50"],
                         {"lower_bound_ms": 0, "upper_bound_exclusive_ms": 10})
        self.assertTrue(interpretation["zero_bucket_is_not_zero_delay"])
        self.assertTrue(interpretation["before_rtt_does_not_prove_original"])
        self.assertTrue(interpretation["arrival_does_not_prove_retransmit_or_RTX"])

    def test_pli_and_display_are_windows_not_exact_event_times(self):
        lines = self.start() + [self.snapshot(at=100, pli=0),
            "[150 ms] Video RTP event: reason=1 ms=1150 seq=1",
            self.snapshot(at=200, pli=2),
            "[200 ms] Client latency: videoLatency windowMs=100 presentGap{samples=4 p50Us=10000 p95Us=90000 p99Us=120000 maxUs=130000 overflowLifetime=0}"]
        session = self.parse(lines)["runs"][0]["streams"][0]["nack"]
        event = session["expired_hole_events"][0]
        self.assertEqual(event["correlation"]["pli_intervals"][0]["pli_delta"], 2)
        self.assertEqual(event["correlation"]["pli_intervals"][0]["timing"],
                         "counter-snapshot-interval-not-exact-PLI-times")
        self.assertEqual(event["correlation"]["display_windows"][0]["overlap"], "within-window")
        self.assertEqual(session["display_windows"][0]["timing"], "rolling-window-not-a-pause-timestamp")
        self.assertNotIn("pause_ms", session["display_windows"][0])

    def test_final_drain_events_and_route_changes_are_parsed(self):
        final_route = self.route(actual="relay", local="relay", event="changed", at=101).replace(
            "Video RTP event:", "Stream final event:")
        lines = self.start() + [self.route(),
            "[100 ms] Stream final event: reason=1 ms=1090 seq=7 missing=1", final_route, self.snapshot(at=102)]
        run = self.parse(lines)["runs"][0]
        session = run["streams"][0]["nack"]
        self.assertEqual([event["seq"] for event in session["expired_hole_events"]], [7])
        self.assertEqual(session["expired_hole_events"][0]["normalized_ms"], 90)
        self.assertEqual(session["route"]["route_class"], "mixed")
        self.assertTrue(session["route"]["route_change_observed"])
        self.assertFalse(session["route"]["isolated_route_comparison_eligible"])
        self.assertEqual(run["nack_summary"]["route_comparison"]["direct"]["eligible_sessions"], 0)

    def test_route_eviction_never_improves_lifetime_eligibility(self):
        with patch.object(report.nack_report.NackSession, "RouteLimit", 2):
            lines = self.start() + [self.route(actual="relay", local="relay", event="selected"),
                self.route(event="changed", at=3)]
            lines += [self.route(event="selected", at=4 + n) for n in range(64)]
            session = self.parse(lines)["runs"][0]["streams"][0]["nack"]
        self.assertEqual(session["retention"]["routes"], {"observed": 66, "omitted": 64})
        self.assertEqual(session["route"]["route_class"], "mixed")
        self.assertEqual(session["route"]["observed_known_route_classes"], ["direct", "relay"])
        self.assertTrue(session["route"]["route_change_observed"])
        self.assertFalse(session["route"]["isolated_route_comparison_eligible"])

    def test_reconnect_profile_replacement_keeps_episode_provenance(self):
        lines = self.start() + [self.route(), self.snapshot(pli=10),
            "[110 ms] Transport reconnect attempt=1/3 rollingWindow=120s",
            "[111 ms] Controller profile: xbox; provider=mts", self.route(at=112),
            "[130 ms] Video RTP event: reason=1 ms=1120 seq=9 missing=1", self.snapshot(at=140, pli=12)]
        run = self.parse(lines)["runs"][0]
        first, replacement = [stream["nack"] for stream in run["streams"]]
        self.assertEqual(first["rental_episode"], replacement["rental_episode"])
        self.assertTrue(first["episode_has_reconnect"])
        self.assertTrue(replacement["episode_has_reconnect"])
        self.assertTrue(replacement["replacement_after_reconnect"])
        self.assertFalse(first["route"]["isolated_route_comparison_eligible"])
        self.assertFalse(replacement["route"]["isolated_route_comparison_eligible"])
        self.assertEqual(run["nack_summary"]["rental_episodes"], 1)
        self.assertEqual(run["nack_summary"]["reconnect_episodes"], 1)
        self.assertEqual(run["nack_summary"]["route_comparison"]["direct"]["eligible_sessions_with_expired_holes"], 0)

    def test_reconnect_blocks_pli_delta_even_when_counters_increase(self):
        lines = self.start() + [self.route(), self.snapshot(pli=10),
            "[110 ms] Transport reconnect attempt=1/3 rollingWindow=120s", self.snapshot(at=200, pli=12)]
        session = self.parse(lines)["runs"][0]["streams"][0]["nack"]
        self.assertEqual(session["pli_counter_intervals"], [])
        self.assertEqual(session["latest_final"]["pli"], 12)

    def test_relay_with_any_unknown_required_field_is_ineligible(self):
        originals = {"actual": "relay", "localType": "relay", "remoteType": "srflx",
                     "localProtocol": "udp", "remoteProtocol": "udp"}
        for field, original in originals.items():
            with self.subTest(field=field):
                route = self.route(actual="relay", local="relay", event="selected").replace(
                    f"{field}={original}", f"{field}=unknown")
                session = self.parse(self.start() + [route, self.snapshot()])["runs"][0]["streams"][0]["nack"]
                self.assertEqual(session["route"]["route_class"], "unknown")
                self.assertTrue(session["route"]["unknown_route_observed"])
                self.assertEqual(session["route"]["transport_signatures"], [])
                self.assertNotIn("transport_signature", session["route"]["observations"][0])
                self.assertFalse(session["route"]["isolated_route_comparison_eligible"])
                self.assertEqual(session["malformed_lines"], 0)

    def test_rental_stop_ends_pending_reconnect_before_independent_rental(self):
        lines = self.start(provider="moonlight") + [
            "[2 ms] Stream protocol: Moonlight codec=H.265",
            "[110 ms] Transport reconnect attempt=1/3 rollingWindow=120s",
            "[115 ms] Rental stop begin; reason=transport-preflight-refused screen=6",
            "[200 ms] Controller profile: xbox; provider=mts", self.route(at=210),
            "[210 ms] Video RTP event: reason=1 ms=1210 seq=2", self.snapshot(at=230)]
        run = self.parse(lines)["runs"][0]
        old, independent = [stream["nack"] for stream in run["streams"]]
        self.assertEqual((old["rental_episode"], independent["rental_episode"]), (1, 2))
        self.assertTrue(old["episode_has_reconnect"])
        self.assertFalse(independent["episode_has_reconnect"])
        self.assertFalse(independent["replacement_after_reconnect"])
        self.assertTrue(independent["route"]["isolated_route_comparison_eligible"])
        self.assertEqual(run["nack_summary"]["rental_episodes"], 2)
        self.assertEqual(run["nack_summary"]["reconnect_episodes"], 1)
        self.assertEqual(run["nack_summary"]["providers"], {"FH": 1, "MTS": 1})
        self.assertEqual(run["nack_summary"]["route_comparison"]["direct"],
                         {"eligible_sessions": 1, "eligible_sessions_with_expired_holes": 1})

    def test_legacy_truncated_clocks_permit_fractional_counterexample(self):
        lines = ["[0 ms] FogPlay NX 0.6.30 startup.", "[1 ms] Controller profile: xbox; provider=mts",
            "[99 ms] Video RTP event: reason=1 ms=1100 seq=1",
            "[100 ms] Video chain resync: worker generation=1",
            "[200 ms] Video RTP event: reason=1 ms=1100 seq=2"]
        session = self.parse(lines)["runs"][0]["streams"][0]["nack"]
        first, second = session["expired_hole_events"]
        self.assertEqual(session["legacy_offset_lower_bound_ms"], 1000)
        self.assertEqual((first["normalized_max_ms"], second["normalized_max_ms"]), (101, 101))
        self.assertGreaterEqual(first["normalized_max_ms"], 99.90)
        self.assertGreaterEqual(second["normalized_max_ms"], 100.05)
        self.assertEqual(first["correlation"]["resync"][0]["order"], "unknown")
        self.assertEqual(second["correlation"]["resync"][0]["order"], "unknown")
        self.assertEqual(first["logging_delay_min_ms"], 0)
        self.assertEqual(second["logging_delay_min_ms"], 99)
        self.assertLessEqual(second["logging_delay_min_ms"], 100.0)

    def test_route_string_values_are_whitelisted_and_redacted(self):
        originals = {"actual": "direct", "localType": "host", "remoteType": "srflx",
                     "localProtocol": "udp", "remoteProtocol": "udp", "routeEvent": "selected"}
        for field, original in originals.items():
            with self.subTest(field=field):
                secret = f"secret-{field}"
                route = self.route(event="selected").replace(f"{field}={original}", f"{field}={secret}")
                session = self.parse(self.start() + [route, self.snapshot()])["runs"][0]["streams"][0]["nack"]
                self.assertEqual(session["route"]["route_class"], "unknown")
                self.assertEqual(session["route"]["observations"][0]["invalid_fields"], [field])
                self.assertNotIn("transport_signature", session["route"]["observations"][0])
                self.assertFalse(session["route"]["isolated_route_comparison_eligible"])
                self.assertEqual(session["malformed_lines"], 1)
                self.assertNotIn(secret, json.dumps(session))

    def test_retention_is_bounded_and_omissions_counted(self):
        with patch.multiple(report.nack_report.NackSession, EventLimit=2, SnapshotLimit=1,
                            RouteLimit=1, ResyncLimit=1, LatencyLimit=1):
            lines = self.start() + [self.route(at=2), self.route(at=3),
                "[10 ms] Video chain resync: worker generation=1",
                "[11 ms] Video chain resync: worker generation=2",
                self.snapshot(at=20), self.snapshot(at=21),
                "[30 ms] Client latency: videoLatency windowMs=10 presentGap{samples=1}",
                "[31 ms] Client latency: videoLatency windowMs=10 presentGap{samples=1}"]
            lines += [f"[{40 + n} ms] Video RTP event: reason=1 ms={1100 + n} seq={n}" for n in range(4)]
            session = self.parse(lines)["runs"][0]["streams"][0]["nack"]
        self.assertEqual([event["seq"] for event in session["expired_hole_events"]], [2, 3])
        self.assertEqual(session["retention"]["events"], {"observed": 4, "omitted": 2})
        self.assertEqual(session["retention"]["snapshots"], {"observed": 2, "omitted": 1})
        self.assertEqual(session["retention"]["routes"]["omitted"], 1)
        self.assertEqual(session["retention"]["resyncs"]["omitted"], 1)
        self.assertEqual(session["retention"]["latency_windows"]["omitted"], 1)

    def test_old_idr_decoder_and_latency_fields_are_unchanged(self):
        lines = ["[0 ms] FogPlay NX 0.6.19 startup.", "[1 ms] Controller profile: xbox; provider=mts",
            "[2 ms] Moonlight IDR event=request seq=1 connection=1 wait=1 core_us=100 frame=0 value=0 totalsValid=1 requests=1 intact=0 fecFailedBlocks=0",
            "[3 ms] Video decoder: decoded_frames=10 uploaded_frames=9 presented_frames=9 superseded_frames=1",
            "[4 ms] Video decoder: decoded_frames=15 uploaded_frames=14 presented_frames=14 superseded_frames=2",
            "[5 ms] Client latency: videoLatency windowMs=5000 presentGap{samples=2 p95Us=20000}"]
        run = self.parse(lines)["runs"][0]
        stream = run["streams"][0]
        self.assertEqual(run["moonlight_idr"]["observed_event_counts"], {"request": 1})
        self.assertEqual(run["moonlight_idr"]["events_in_log_order"][0]["seq"], 1)
        self.assertEqual(stream["counters"]["observed_deltas"]["decoded_frames"], 5)
        self.assertEqual(stream["latency_windows"][0]["histograms"]["presentGap"]["p95Us"], 20000)
        self.assertEqual(stream["nack"]["snapshots"], [])

    def test_nack_output_drops_unwhitelisted_text(self):
        lines = self.start() + [self.route() + " candidate=private-host address=10.2.3.4",
            self.snapshot() + " sdp=secret-payload serverPreset=private-name"]
        stream = self.parse(lines)["runs"][0]["streams"][0]
        encoded = json.dumps(stream["nack"])
        for forbidden in ("private-host", "10.2.3.4", "secret-payload", "private-name"):
            self.assertNotIn(forbidden, encoded)

    def test_cli_emits_nack_json(self):
        path = self.directory / "cli.log"
        path.write_text("\n".join(self.start() + [self.route(), self.snapshot()]) + "\n", encoding="utf-8")
        executable = Path(report.__file__)
        completed = subprocess.run([sys.executable, str(executable), "logs", str(path)],
                                   check=True, capture_output=True, text=True)
        document = json.loads(completed.stdout)
        self.assertEqual(document["logs"][0]["runs"][0]["streams"][0]["nack"]["latest_final"]["nackPackets"], 0)


if __name__ == "__main__":
    unittest.main()
