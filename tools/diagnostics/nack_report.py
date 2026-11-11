"""Bounded, privacy-safe NACK observations for the Stage 0 offline report."""
from collections import Counter, deque
import re


KV = re.compile(r"([A-Za-z_][A-Za-z_0-9-]*)=([^\s{};]+)")
SNAPSHOT_FIELDS = (
    "nackEnabled", "nackPackets", "repairedPackets", "repairWindowMs", "gapRepairWindowMs",
    "nackRetryMs", "nackArrivalSamples", "nackLateSamples", "nackRetrySamples",
    "nackFirstArrivalLastMs", "nackFirstArrivalMeanMs", "nackFirstArrivalMaxMs",
    "nackArrivalDeviationMs", "nackArrivalP50Ms", "nackArrivalP95Ms", "nackArrivalBucketMs",
    "nackRoundTripMs", "nackArrivalBeforeRtt", "nackArrivalAfterRtt", "nackArrivalRttUnknown",
    "nackHistoryEvicted", "missing", "resync", "pli", "expiredGaps", "expiredPackets",
    "eventDrop", "repairMediaPtCount", "repairRtxPtCount", "rtxReceived", "rtxRestored",
    "nackBatches", "nackSendErrors",
)
CUMULATIVE_FIELDS = (
    "nackPackets", "repairedPackets", "nackArrivalSamples", "nackLateSamples", "nackRetrySamples",
    "nackArrivalBeforeRtt", "nackArrivalAfterRtt", "nackArrivalRttUnknown", "missing", "resync",
    "pli", "expiredGaps", "expiredPackets", "repairRtxPtCount", "rtxReceived", "rtxRestored",
)
EVENT_FIELDS = ("seq", "missing", "waitIdr", "nackAttempts", "overshootMs", "pendingDepth")
LATENCY_FIELDS = ("samples", "p50Us", "p95Us", "p99Us", "maxUs", "overflowLifetime")


def fields(text):
    result = {}
    for key, value in KV.findall(text):
        if re.fullmatch(r"-?\d+", value):
            result[key] = int(value)
        else:
            result[key] = value
    return result


def valid_nonnegative(values, key):
    return isinstance(values.get(key), int) and values[key] >= 0


class DiagnosticProfile:
    """Build profile evidence only; missing legacy metadata does not imply Standard."""
    def __init__(self):
        self.profiles = set()
        self.malformed_lines = 0

    def observe(self, text):
        prefix = "Diagnostic profile:"
        pairs = KV.findall(text[len(prefix):]) if text.startswith(prefix) else []
        keys = [key for key, _ in pairs]
        # Never let a duplicate declaration silently replace Essential or an
        # unknown schema with the last, apparently acceptable token.
        if keys.count("schema") != 1 or keys.count("profile") != 1:
            self.malformed_lines += 1
            return
        values = fields(text)
        if values.get("schema") != 1 or values.get("profile") not in ("essential", "standard", "deep"):
            self.malformed_lines += 1
            return
        self.profiles.add(values["profile"])

    def summary(self):
        status = ("malformed" if self.malformed_lines else "conflicting" if len(self.profiles) > 1
                  else "declared" if self.profiles else "legacy-unreported")
        profile = next(iter(self.profiles)) if len(self.profiles) == 1 else None
        eligible = status in ("declared", "legacy-unreported") and profile != "essential"
        return {"status": status, "profile": profile, "malformed_lines": self.malformed_lines,
                "permits_detailed_comparison": eligible,
                "note": "Essential omits periodic detail; missing legacy profile is unknown, not inferred Standard. Other route/data sufficiency gates still apply."}


class DiagnosticClock:
    def __init__(self):
        self.anchors = []
        self.malformed_lines = 0
        self.conflicting = False

    def observe(self, text, line):
        values = fields(text)
        if (values.get("schema") != 1 or values.get("clock") != "steady" or
                not valid_nonnegative(values, "startupEpochMs") or
                not isinstance(values.get("resolutionMs"), int) or values["resolutionMs"] != 1):
            self.malformed_lines += 1
            return
        anchor = {"line": line, "startupEpochMs": values["startupEpochMs"], "resolutionMs": 1}
        if self.anchors and anchor["startupEpochMs"] != self.anchors[0]["startupEpochMs"]:
            self.conflicting = True
        self.anchors.append(anchor)

    def summary(self):
        status = ("conflicting" if self.conflicting else "malformed" if self.malformed_lines else
                  "exact" if self.anchors else "unavailable")
        result = {"status": status, "clock": "steady", "coordinate": "not-UTC",
                  "malformed_lines": self.malformed_lines,
                  "anchor_lines": [anchor["line"] for anchor in self.anchors]}
        if status == "exact":
            result.update(startupEpochMs=self.anchors[0]["startupEpochMs"], resolutionMs=1,
                          normalizationPrecisionMs=1)
        return result

    @property
    def origin(self):
        summary = self.summary()
        return summary.get("startupEpochMs") if summary["status"] == "exact" else None


class NackSession:
    EventLimit = 256
    SnapshotLimit = 256
    RouteLimit = 64
    ResyncLimit = 128
    LatencyLimit = 256
    PliIntervalLimit = 256
    BoundaryLimit = 128
    CorrelationWindowMs = 1000

    def __init__(self, stream, episode=1, reconnect_episode=False, replacement=False):
        self.stream = stream
        self.episode = episode
        self.reconnect_episode = reconnect_episode
        self.replacement = replacement
        self.events = deque(maxlen=self.EventLimit)
        self.snapshots = deque(maxlen=self.SnapshotLimit)
        self.routes = deque(maxlen=self.RouteLimit)
        self.resyncs = deque(maxlen=self.ResyncLimit)
        self.latency_windows = deque(maxlen=self.LatencyLimit)
        self.pli_intervals = deque(maxlen=self.PliIntervalLimit)
        self.reconnects = deque(maxlen=self.BoundaryLimit)
        self.reset_boundaries = deque(maxlen=self.BoundaryLimit)
        self.seen = Counter()
        self.malformed_lines = 0
        self.unavailable = Counter()
        self.previous_counters = None
        self.latest_final = None
        self.route_classes = set()
        self.route_signatures = set()
        self.route_unknown = False
        self.route_changed = False

    @staticmethod
    def _append(queue, item, seen, key):
        seen[key] += 1
        if len(queue) == queue.maxlen:
            seen["omitted_" + key] += 1
        queue.append(item)

    def observe_clock_event(self, text, at_ms, line):
        values = fields(text)
        if values.get("reason") != 1:
            return
        if not valid_nonnegative(values, "ms"):
            self.malformed_lines += 1
            return
        event = {"line": line, "prefix_ms": at_ms, "event_ms": values["ms"]}
        for key in EVENT_FIELDS:
            if key in values:
                if valid_nonnegative(values, key):
                    event[key] = values[key]
                else:
                    self.malformed_lines += 1
        self._append(self.events, event, self.seen, "events")

    def observe_snapshot(self, text, at_ms, line, final):
        values = fields(text)
        if not any(key in values or key + "=" in text for key in SNAPSHOT_FIELDS) and "offerLossProtection=" not in text:
            return
        invalid = [key for key in SNAPSHOT_FIELDS if key + "=" in text and not valid_nonnegative(values, key)]
        if invalid:
            self.malformed_lines += 1
            return
        snapshot = {"line": line, "at_ms": at_ms, "final": final}
        for key in SNAPSHOT_FIELDS:
            if key in values:
                snapshot[key] = values[key]
            else:
                self.unavailable[key] += 1
        if "offerLossProtection=" in text:
            protection = values.get("offerLossProtection")
            if protection in ("none", "flexfec", "rtx", "flexfec+rtx", "unknown"):
                snapshot["offerLossProtection"] = protection
            else:
                self.malformed_lines += 1
        else:
            self.unavailable["offerLossProtection"] += 1
        current = {key: snapshot[key] for key in CUMULATIVE_FIELDS if key in snapshot}
        reset_fields = []
        if self.previous_counters is not None:
            reset_fields = sorted(key for key in current if key in self.previous_counters and
                                  current[key] < self.previous_counters[key])
            if reset_fields:
                self._append(self.reset_boundaries, {"from_line": self.previous_counters["line"],
                    "to_line": line, "from_ms": self.previous_counters["at_ms"], "to_ms": at_ms,
                    "fields": reset_fields}, self.seen, "reset_boundaries")
            elif valid_nonnegative(values, "pli"):
                previous_pli = self.previous_counters.get("pli")
                if isinstance(previous_pli, int) and values["pli"] > previous_pli:
                    previous_at = self.previous_counters["at_ms"]
                    if isinstance(previous_at, int) and isinstance(at_ms, int) and at_ms >= previous_at:
                        self._append(self.pli_intervals, {"start_ms": previous_at, "end_ms": at_ms,
                            "from_line": self.previous_counters["line"], "to_line": line,
                            "pli_delta": values["pli"] - previous_pli,
                            "timing": "counter-snapshot-interval-not-exact-PLI-times"}, self.seen, "pli_intervals")
        self.previous_counters = {**current, "line": line, "at_ms": at_ms}
        self._append(self.snapshots, snapshot, self.seen, "snapshots")
        if final:
            self.latest_final = snapshot

    def observe_route(self, text, at_ms, line):
        values = fields(text)
        if "actual" not in values:
            return
        whitelist = {
            "actual": {"direct", "relay", "turn", "unknown"},
            "localType": {"host", "srflx", "prflx", "relay", "unknown"},
            "remoteType": {"host", "srflx", "prflx", "relay", "unknown"},
            "localProtocol": {"udp", "tcp", "unknown"},
            "remoteProtocol": {"udp", "tcp", "unknown"},
            "routeEvent": {"selected", "changed"},
        }
        invalid = sorted(key for key, allowed in whitelist.items()
                         if key in values and values[key] not in allowed)
        allowed = {key: values[key] for key in whitelist if key in values and key not in invalid}
        observation = {"line": line, "at_ms": at_ms, **allowed}
        if invalid:
            observation["invalid_fields"] = invalid
            self.malformed_lines += 1
        required = ("actual", "localType", "remoteType", "localProtocol", "remoteProtocol")
        complete = (all(key in allowed and allowed[key] != "unknown" for key in required) and
                    values.get("routeKnown") in (None, 1))
        route_class = "unknown"
        if complete and not invalid:
            types = (allowed["localType"], allowed["remoteType"])
            if allowed["actual"] in ("relay", "turn") or "relay" in types:
                route_class = "relay"
            elif (allowed["actual"] == "direct" and
                    all(value in ("host", "srflx", "prflx") for value in types) and
                    all(allowed[key] in ("udp", "tcp") for key in ("localProtocol", "remoteProtocol"))):
                route_class = "direct"
        observation["route_class"] = route_class
        if route_class == "unknown":
            self.route_unknown = True
        else:
            signature = ":".join([route_class] + [allowed[key] for key in
                ("localType", "remoteType", "localProtocol", "remoteProtocol")])
            observation["transport_signature"] = signature
            self.route_classes.add(route_class)
            self.route_signatures.add(signature)
            self.route_changed |= len(self.route_signatures) > 1
        if allowed.get("routeEvent") == "changed":
            self.route_changed = True
        self._append(self.routes, observation, self.seen, "routes")

    def observe_resync(self, text, at_ms, line):
        values = fields(text)
        if not valid_nonnegative(values, "generation"):
            self.malformed_lines += 1
            return
        self._append(self.resyncs, {"line": line, "at_ms": at_ms,
            "generation": values["generation"]}, self.seen, "resyncs")

    def observe_latency(self, text, at_ms, line):
        values = fields(text)
        match = re.search(r"presentGap\{([^{}]*)\}", text)
        if not match or not valid_nonnegative(values, "windowMs") or not isinstance(at_ms, int):
            self.malformed_lines += 1
            return
        histogram = fields(match[1])
        retained = {}
        for key in LATENCY_FIELDS:
            if key in histogram:
                if valid_nonnegative(histogram, key):
                    retained[key] = histogram[key]
                else:
                    self.malformed_lines += 1
        self._append(self.latency_windows, {"line": line, "end_ms": at_ms,
            "start_ms": max(0, at_ms - values["windowMs"]), "windowMs": values["windowMs"],
            "presentGap": retained, "timing": "rolling-window-not-a-pause-timestamp"},
            self.seen, "latency_windows")

    def observe_reconnect(self, text, at_ms, line):
        values = fields(text)
        record = {"line": line, "at_ms": at_ms}
        attempt = re.fullmatch(r"(\d+)(?:/\d+)?", str(values.get("attempt", "")))
        if attempt:
            record["attempt"] = int(attempt[1])
        else:
            self.malformed_lines += 1
        self.reconnect_episode = True
        self.previous_counters = None
        self._append(self.reconnects, record, self.seen, "reconnects")

    def event_offset_lower_bound(self):
        return max((event["event_ms"] - event["prefix_ms"] - 1 for event in self.events
                    if isinstance(event["prefix_ms"], int)), default=None)

    @staticmethod
    def _normalize(event, origin, legacy_offset):
        result = {key: value for key, value in event.items()}
        if origin is not None:
            normalized = event["event_ms"] - origin
            result.update(normalized_ms=normalized, normalized_min_ms=normalized - 1,
                normalized_max_ms=normalized + 1, time_precision="steady-origin-truncation-plus-minus-1ms")
            if isinstance(event["prefix_ms"], int):
                delay = event["prefix_ms"] - normalized
                result.update(logging_delay_ms=delay, logging_delay_min_ms=delay - 1,
                    logging_delay_max_ms=delay + 2)
        else:
            upper = event["event_ms"] + 1 - legacy_offset if legacy_offset is not None else None
            result.update(normalized_ms=None, normalized_min_ms=None, normalized_max_ms=upper,
                time_precision="upper-bound-only-two-truncated-clocks")
            if isinstance(event["prefix_ms"], int) and upper is not None:
                result.update(logging_delay_min_ms=max(0, event["prefix_ms"] - upper),
                    logging_delay_max_ms=None)
        return result

    @staticmethod
    def _order(lower, upper, prefix_ms):
        if lower is not None and lower > prefix_ms + 1:
            return "after"
        if upper is not None and upper < prefix_ms:
            return "before"
        return "unknown"

    def _near(self, lower, upper, prefix_ms):
        return (lower is None or lower <= prefix_ms + 1 + self.CorrelationWindowMs) and (
            upper is None or upper + self.CorrelationWindowMs >= prefix_ms)

    def _correlations(self, event):
        lower, upper = event["normalized_min_ms"], event["normalized_max_ms"]
        if lower is None and upper is None:
            return {"resync": [], "pli_intervals": [], "display_windows": []}
        resync = []
        for item in self.resyncs:
            if not isinstance(item["at_ms"], int) or not self._near(lower, upper, item["at_ms"]):
                continue
            match = {"line": item["line"], "at_ms": item["at_ms"], "generation": item["generation"],
                     "order": self._order(lower, upper, item["at_ms"])}
            if event["normalized_ms"] is not None:
                match.update(delta_ms=event["normalized_ms"] - item["at_ms"], delta_precision_ms=2)
            else:
                match.update(delta_lower_bound_ms=None, delta_upper_bound_ms=upper - item["at_ms"])
            resync.append(match)
        pli = []
        for interval in self.pli_intervals:
            if ((lower is not None and lower > interval["end_ms"] + 1) or
                    (upper is not None and upper < interval["start_ms"])):
                continue
            pli.append({**interval, "correlation": "event-overlaps-or-can-overlap-counter-interval"})
        windows = []
        for window in self.latency_windows:
            if ((lower is not None and lower > window["end_ms"] + 1) or
                    (upper is not None and upper < window["start_ms"])):
                continue
            status = ("within-window" if lower is not None and upper is not None and
                      lower >= window["start_ms"] and upper <= window["end_ms"] + 1 else "possible-overlap")
            windows.append({"line": window["line"], "start_ms": window["start_ms"],
                "end_ms": window["end_ms"], "overlap": status})
        return {"resync": resync, "pli_intervals": pli, "display_windows": windows}

    def _route_summary(self):
        classes = sorted(self.route_classes)
        signatures = sorted(self.route_signatures)
        isolated = (len(signatures) == 1 and not self.route_unknown and not self.route_changed and
                    not self.reconnect_episode and not self.reset_boundaries)
        return {"route_class": classes[0] if len(classes) == 1 else "mixed" if classes else "unknown",
                "observed_known_route_classes": classes, "transport_signatures": signatures,
                "unknown_route_observed": self.route_unknown, "route_change_observed": self.route_changed,
                "isolated_route_comparison_eligible": isolated,
                "observations": list(self.routes)}

    @staticmethod
    def _arrival_interpretation(snapshot):
        if not snapshot:
            return None
        samples = snapshot.get("nackArrivalSamples")
        bucket = snapshot.get("nackArrivalBucketMs")
        result = {"scope": "observed-arrivals-only-not-all-NACK-requests",
                  "before_rtt_does_not_prove_original": True,
                  "arrival_does_not_prove_retransmit_or_RTX": True}
        if not isinstance(samples, int) or samples == 0:
            result["distribution"] = "unavailable-no-observed-arrival-samples"
            return result
        result["distribution"] = "bucketed-observed-arrivals"
        if isinstance(bucket, int) and bucket > 0:
            ranges = {}
            for name, key in (("p50", "nackArrivalP50Ms"), ("p95", "nackArrivalP95Ms")):
                if key in snapshot:
                    value = snapshot[key]
                    ranges[name] = {"lower_bound_ms": value, "upper_bound_exclusive_ms": value + bucket}
            result["percentile_bucket_ranges"] = ranges
            result["zero_bucket_is_not_zero_delay"] = True
        return result

    def summary(self, provider, clock, legacy_offset, profile=None):
        origin = clock.origin
        events = []
        for event in self.events:
            normalized = self._normalize(event, origin, legacy_offset)
            normalized["correlation"] = self._correlations(normalized)
            normalized["correlation_meaning"] = "observed-temporal-proximity-only-not-causality"
            events.append(normalized)
        route = self._route_summary()
        retention = {"limits": {"events": self.EventLimit, "snapshots": self.SnapshotLimit,
            "routes": self.RouteLimit, "resyncs": self.ResyncLimit,
            "latency_windows": self.LatencyLimit, "pli_intervals": self.PliIntervalLimit,
            "reconnects": self.BoundaryLimit, "reset_boundaries": self.BoundaryLimit}}
        for key in ("events", "snapshots", "routes", "resyncs", "latency_windows", "pli_intervals",
                    "reconnects", "reset_boundaries"):
            retention[key] = {"observed": self.seen[key], "omitted": self.seen["omitted_" + key]}
        profile = profile if profile is not None else DiagnosticProfile().summary()
        return {"schema": 1, "stream": self.stream, "provider": provider,
            "diagnostic_profile": profile,
            "diagnostic_comparison_eligible": profile["permits_detailed_comparison"],
            "rental_episode": self.episode, "replacement_after_reconnect": self.replacement,
            "episode_has_reconnect": self.reconnect_episode,
            "clock_status": clock.summary()["status"], "legacy_offset_lower_bound_ms": legacy_offset,
            "legacy_offset_upper_bound_ms": None,
            "route": route, "snapshots": list(self.snapshots), "latest_final": self.latest_final,
            "arrival_interpretation": self._arrival_interpretation(self.latest_final),
            "expired_hole_events": events, "resync_observations": list(self.resyncs),
            "pli_counter_intervals": list(self.pli_intervals),
            "display_windows": list(self.latency_windows),
            "reconnect_boundaries": list(self.reconnects),
            "counter_reset_boundaries": list(self.reset_boundaries),
            "malformed_lines": self.malformed_lines,
            "unavailable_field_observations": dict(self.unavailable),
            "retention": retention,
            "note": "Cumulative snapshots are never summed. Reset/reconnect boundaries are explicit. Bucketed zero is not zero delay; arrivals do not prove original/retransmit/RTX; PLI and display timings are windows, not exact event times."}


def summarize_run(sessions):
    route_eligible = [session for session in sessions if session["route"]["isolated_route_comparison_eligible"]]
    eligible = [session for session in route_eligible if session.get("diagnostic_comparison_eligible", True)]
    routes = {}
    for route in ("direct", "relay"):
        selected = [session for session in eligible if session["route"]["route_class"] == route]
        routes[route] = {"eligible_sessions": len(selected),
            "eligible_sessions_with_expired_holes": sum(bool(session["expired_hole_events"]) for session in selected)}
    return {"schema": 1, "sessions": len(sessions),
            "rental_episodes": len({session["rental_episode"] for session in sessions}),
            "reconnect_episodes": len({session["rental_episode"] for session in sessions
                                       if session["episode_has_reconnect"]}),
            "providers": dict(Counter(session["provider"] or "unknown" for session in sessions)),
            "route_comparison": routes,
            "mixed_unknown_or_changed_sessions_excluded": len(sessions) - len(route_eligible),
            "diagnostic_profile_sessions_excluded": sum(not session.get("diagnostic_comparison_eligible", True) for session in sessions),
            "policy_decision": "keep-policy", "evidence_sufficiency": "insufficient-data-for-policy-change",
            "policy_note": "At least three isolated sessions with expired holes per investigated route are required before considering one policy experiment; temporal correlation is not causality."}
