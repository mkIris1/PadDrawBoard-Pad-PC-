#!/usr/bin/env python3
"""Analyze PadDrawBoard control telemetry and soak-test logs.

The input format is JSON Lines.  See docs/testing.md for the accepted record
shapes.  The implementation deliberately uses only the Python standard
library so it can run on a clean Windows, Linux, or macOS machine.
"""

from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence


SCHEMA_VERSION = 1
FRAME_MS = 1000.0 / 60.0
EXIT_OK = 0
EXIT_USAGE = 2
EXIT_GATE_FAILED = 3
EXIT_NO_DATA = 4


class InputError(ValueError):
    """Raised for malformed telemetry input."""


def _number(value: Any, name: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise InputError(f"{name} must be numeric")
    value = float(value)
    if not math.isfinite(value):
        raise InputError(f"{name} must be finite")
    return value


def _timestamp_ns(record: Mapping[str, Any], stem: str = "timestamp") -> float | None:
    for suffix, multiplier in (("_ns", 1.0), ("_us", 1_000.0), ("_ms", 1_000_000.0), ("_s", 1_000_000_000.0)):
        key = stem + suffix
        if key in record:
            return _number(record[key], key) * multiplier
    if stem in record:
        # Bare timestamps are documented as nanoseconds.  This is useful for
        # the protocol's native monotonic clock fields.
        return _number(record[stem], stem)
    return None


def _first(record: Mapping[str, Any], names: Sequence[str]) -> Any | None:
    for name in names:
        if name in record:
            return record[name]
    return None


def _timestamp_alias(record: Mapping[str, Any], names: Sequence[str]) -> float | None:
    for name in names:
        if name.endswith(("_ns", "_us", "_ms", "_s")):
            if name in record:
                suffix = name.rsplit("_", 1)[1]
                multiplier = {"ns": 1.0, "us": 1_000.0, "ms": 1_000_000.0, "s": 1_000_000_000.0}[suffix]
                return _number(record[name], name) * multiplier
        elif name in record:
            return _number(record[name], name)
    return None


def percentile(values: Sequence[float], fraction: float) -> float | None:
    if not values:
        return None
    if not 0.0 <= fraction <= 1.0:
        raise ValueError("fraction must be in [0, 1]")
    ordered = sorted(values)
    index = (len(ordered) - 1) * fraction
    lower = math.floor(index)
    upper = math.ceil(index)
    if lower == upper:
        return ordered[lower]
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (index - lower)


def stats(values: Sequence[float], unit: str = "") -> dict[str, Any]:
    if not values:
        return {"count": 0, "unit": unit, "min": None, "median": None, "p95": None, "max": None}
    return {
        "count": len(values),
        "unit": unit,
        "min": min(values),
        "median": statistics.median(values),
        "p95": percentile(values, 0.95),
        "max": max(values),
    }


def _kind(record: Mapping[str, Any]) -> str:
    return str(record.get("kind", "")).lower().replace("-", "_")


def _clock_sample(record: Mapping[str, Any]) -> tuple[float, float] | None:
    device_send = _timestamp_alias(record, ("device_send_ns", "client_send_timestamp_ns", "client_send_ns"))
    host_receive = _timestamp_alias(record, ("host_receive_ns", "host_receive_timestamp_ns"))
    host_send = _timestamp_alias(record, ("host_send_ns", "host_send_timestamp_ns"))
    device_receive = _timestamp_alias(record, ("device_receive_ns", "client_receive_timestamp_ns", "client_receive_ns"))
    if any(value is None for value in (device_send, host_receive, host_send, device_receive)):
        return None
    assert device_send is not None and host_receive is not None and host_send is not None and device_receive is not None
    rtt = (device_receive - device_send) - (host_send - host_receive)
    offset = ((host_receive - device_send) + (host_send - device_receive)) / 2.0
    if rtt < 0:
        raise InputError("clock sync record has negative RTT")
    return offset, rtt


def _latency_ms(record: Mapping[str, Any], category: str) -> float | None:
    if category == "glass":
        # Desktop video_sample ends at encoder completion, not physical display.
        # Only explicitly classified glass measurements may satisfy release gates.
        if _kind(record) not in {"glass_sample", "glass_to_glass"}:
            return None
        direct = _first(record, ("glass_to_glass_ms", "glass_latency_ms"))
        if direct is not None:
            return _number(direct, "glass latency")
        capture = _timestamp_alias(record, ("capture_timestamp_ns", "capture_ns", "capture_time_ns"))
        present = _timestamp_alias(record, ("presentation_timestamp_ns", "present_timestamp_ns", "present_ns", "displayed_ns"))
        if capture is not None and present is not None:
            return (present - capture) / 1_000_000.0
        if "latency_ms" in record:
            return _number(record["latency_ms"], "latency_ms")
    else:
        direct = _first(record, ("input_transport_ms", "transport_latency_ms"))
        if direct is not None:
            return _number(direct, "input transport latency")
        if "latency_us" in record:
            return _number(record["latency_us"], "latency_us") / 1000.0
        sent = _timestamp_alias(record, ("input_send_ns",))
        received = _timestamp_alias(record, ("input_receive_ns",))
        if _kind(record) in {"input", "input_sample", "input_transport"}:
            sent = sent if sent is not None else _timestamp_alias(record, ("send_ns", "device_send_ns"))
            received = received if received is not None else _timestamp_alias(record, ("receive_ns", "host_receive_ns"))
        if sent is not None and received is not None:
            return (received - sent) / 1_000_000.0
        if _kind(record) in {"input", "input_sample", "input_transport"} and "latency_ms" in record:
            return _number(record["latency_ms"], "latency_ms")
    return None


def _gate(value: float | None, limit: float, relation: str = "le") -> bool | None:
    if value is None:
        return None
    return value <= limit if relation == "le" else value >= limit


def analyze_clock(records: Iterable[Mapping[str, Any]]) -> dict[str, Any]:
    offsets: list[float] = []
    rtts: list[float] = []
    for record in records:
        sample = _clock_sample(record)
        if sample is not None:
            offset, rtt = sample
            offsets.append(offset / 1_000_000.0)
            rtts.append(rtt / 1_000_000.0)
    return {
        "samples": stats(offsets, "ms"),
        "rtt": stats(rtts, "ms"),
        "offset_sign": "positive means host clock is ahead of device clock",
        "pass": bool(offsets),
    }


def analyze_latency(records: Iterable[Mapping[str, Any]], target_ms: float, release_median_ms: float, release_p95_ms: float, input_p95_ms: float) -> dict[str, Any]:
    glass: list[float] = []
    input_transport: list[float] = []
    for record in records:
        value = _latency_ms(record, "glass")
        if value is not None:
            if value < 0:
                raise InputError("glass-to-glass latency cannot be negative")
            glass.append(value)
        value = _latency_ms(record, "input")
        if value is not None:
            if value < 0:
                raise InputError("input transport latency cannot be negative")
            input_transport.append(value)

    glass_stats = stats(glass, "ms")
    input_stats = stats(input_transport, "ms")
    median = glass_stats["median"]
    p95 = glass_stats["p95"]
    input_p95 = input_stats["p95"]
    optimization_target_met = _gate(median, target_ms)
    gates = {
        "release_median_le_50ms": _gate(median, release_median_ms),
        "release_p95_le_70ms": _gate(p95, release_p95_ms),
        "input_transport_p95_le_8ms": _gate(input_p95, input_p95_ms),
    }
    required = [gates["release_median_le_50ms"], gates["release_p95_le_70ms"], gates["input_transport_p95_le_8ms"]]
    release_pass = bool(glass and input_transport and all(required))
    return {
        "glass_to_glass": glass_stats,
        "input_transport": input_stats,
        "thresholds_ms": {"optimization_target_median": target_ms, "release_median": release_median_ms, "release_p95": release_p95_ms, "input_transport_p95": input_p95_ms},
        "optimizationTargetMet": optimization_target_met,
        "releasePass": release_pass,
        "gates": gates,
        "pass": release_pass,
    }


@dataclass
class _SoakPoint:
    timestamp_ns: float
    queue_depth: float | None
    active_pointers: int | None
    crashes: float | None
    latency_ms: float | None


def _soak_point(record: Mapping[str, Any]) -> _SoakPoint | None:
    if _kind(record) not in {"soak", "health", "soak_sample"} and not any(key in record for key in ("queue_depth", "active_pointers", "active_pointer_ids", "crashes")):
        return None
    timestamp = _timestamp_ns(record)
    if timestamp is None:
        raise InputError("soak record requires timestamp_ns, timestamp_us, timestamp_ms, or timestamp_s")
    pointer_value = _first(record, ("active_pointers", "unreleased_pointers"))
    if pointer_value is None and "active_pointer_ids" in record:
        ids = record["active_pointer_ids"]
        if not isinstance(ids, list):
            raise InputError("active_pointer_ids must be a list")
        pointer_value = len(ids)
    pointers = None if pointer_value is None else int(_number(pointer_value, "active_pointers"))
    queue = None if "queue_depth" not in record else _number(record["queue_depth"], "queue_depth")
    crashes = None if "crashes" not in record else _number(record["crashes"], "crashes")
    latency = None if "latency_ms" not in record else _number(record["latency_ms"], "latency_ms")
    return _SoakPoint(timestamp, queue, pointers, crashes, latency)


def analyze_soak(records: Iterable[Mapping[str, Any]], min_duration_s: float = 3600.0, frame_ms: float = FRAME_MS) -> dict[str, Any]:
    points = [point for record in records if (point := _soak_point(record)) is not None]
    if not points:
        return {"samples": 0, "duration_s": None, "pass": False, "gates": {"duration": False, "queue_growth": False, "pointers_released": False, "no_crashes": False, "drift_le_one_frame": False}}
    points.sort(key=lambda point: point.timestamp_ns)
    duration_s = (points[-1].timestamp_ns - points[0].timestamp_ns) / 1_000_000_000.0
    queues = [point.queue_depth for point in points if point.queue_depth is not None]
    pointers = [point.active_pointers for point in points if point.active_pointers is not None]
    crashes = [point.crashes for point in points if point.crashes is not None]
    latencies = [point.latency_ms for point in points if point.latency_ms is not None]
    queue_growth = (queues[-1] - queues[0]) if len(queues) >= 2 else None
    pointer_end = pointers[-1] if pointers else None
    crash_count = max(crashes) if crashes else 0.0
    drift = (max(latencies) - min(latencies)) if latencies else None
    gates = {
        "duration": duration_s >= min_duration_s,
        "queue_growth": queue_growth is not None and queue_growth <= 0.0,
        "pointers_released": pointer_end is not None and pointer_end == 0,
        "no_crashes": crash_count == 0,
        "drift_le_one_frame": drift is not None and drift <= frame_ms,
    }
    return {
        "samples": len(points),
        "duration_s": duration_s,
        "required_duration_s": min_duration_s,
        "queue_depth": stats(queues, "entries"),
        "queue_growth": queue_growth,
        "active_pointers_end": pointer_end,
        "crashes": crash_count,
        "latency_ms": stats(latencies, "ms"),
        "latency_drift_ms": drift,
        "frame_ms": frame_ms,
        "gates": gates,
        "pass": all(gates.values()),
    }


def analyze(records: Sequence[Mapping[str, Any]], *, target_ms: float = 35.0, release_median_ms: float = 50.0, release_p95_ms: float = 70.0, input_p95_ms: float = 8.0, min_duration_s: float = 3600.0, frame_ms: float = FRAME_MS) -> dict[str, Any]:
    # Each analyzer iterates its own view, which keeps record classification
    # deliberately permissive for exports that combine all control telemetry.
    clock = analyze_clock(records)
    latency = analyze_latency(records, target_ms, release_median_ms, release_p95_ms, input_p95_ms)
    soak = analyze_soak(records, min_duration_s, frame_ms)
    failures: list[str] = []
    for name, result in (("clock", clock), ("latency", latency), ("soak", soak)):
        if not result["pass"] and name == "clock":
            failures.append("clock.no_data")
        if not result["pass"] and name in {"latency", "soak"} and result.get("samples", result.get("glass_to_glass", {}).get("count", 0)) == 0:
            failures.append(f"{name}.no_data")
        for gate, passed in result.get("gates", {}).items():
            if passed is not True:
                failures.append(f"{name}.{gate}")
    return {"schema_version": SCHEMA_VERSION, "clock": clock, "latency": latency, "soak": soak, "overall_pass": not failures, "failures": failures}


def load_jsonl(path: str) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    source = Path(path)
    try:
        with source.open("r", encoding="utf-8") as stream:
            for line_number, line in enumerate(stream, 1):
                if not line.strip() or line.lstrip().startswith("#"):
                    continue
                try:
                    record = json.loads(line)
                except json.JSONDecodeError as exc:
                    raise InputError(f"{source}:{line_number}: invalid JSON: {exc.msg}") from exc
                if not isinstance(record, dict):
                    raise InputError(f"{source}:{line_number}: record must be a JSON object")
                records.append(record)
    except OSError as exc:
        raise InputError(f"cannot read {source}: {exc}") from exc
    if not records:
        raise InputError("telemetry input contains no records")
    return records


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="PadDrawBoard latency and one-hour soak-test analyzer")
    parser.add_argument("input", help="JSONL telemetry export (use - for stdin)")
    parser.add_argument("--mode", choices=("all", "clock", "latency", "soak"), default="all", help="gate a complete export or one telemetry class")
    parser.add_argument("--output", "-o", help="write the JSON report to this file as well as stdout")
    parser.add_argument("--min-duration-s", type=float, default=3600.0, help="minimum soak duration (default: 3600)")
    parser.add_argument("--frame-ms", type=float, default=FRAME_MS, help="display frame budget (default: 16.6667)")
    parser.add_argument("--target-ms", type=float, default=35.0)
    parser.add_argument("--release-median-ms", type=float, default=50.0)
    parser.add_argument("--release-p95-ms", type=float, default=70.0)
    parser.add_argument("--input-p95-ms", type=float, default=8.0)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = _parser()
    args = parser.parse_args(argv)
    try:
        if args.input == "-":
            records = []
            for line_number, line in enumerate(sys.stdin, 1):
                if line.strip():
                    try:
                        record = json.loads(line)
                    except json.JSONDecodeError as exc:
                        raise InputError(f"stdin:{line_number}: invalid JSON: {exc.msg}") from exc
                    if not isinstance(record, dict):
                        raise InputError(f"stdin:{line_number}: record must be an object")
                    records.append(record)
            if not records:
                raise InputError("telemetry input contains no records")
        else:
            records = load_jsonl(args.input)
        if args.mode == "all":
            report = analyze(records, target_ms=args.target_ms, release_median_ms=args.release_median_ms, release_p95_ms=args.release_p95_ms, input_p95_ms=args.input_p95_ms, min_duration_s=args.min_duration_s, frame_ms=args.frame_ms)
        elif args.mode == "clock":
            clock = analyze_clock(records)
            report = {"schema_version": SCHEMA_VERSION, "mode": "clock", "clock": clock, "overall_pass": clock["pass"], "failures": [] if clock["pass"] else ["clock.no_data"]}
        elif args.mode == "latency":
            latency = analyze_latency(records, args.target_ms, args.release_median_ms, args.release_p95_ms, args.input_p95_ms)
            failures = [f"latency.{name}" for name, passed in latency["gates"].items() if passed is not True]
            report = {"schema_version": SCHEMA_VERSION, "mode": "latency", "latency": latency, "overall_pass": not failures, "failures": failures}
        else:
            soak = analyze_soak(records, args.min_duration_s, args.frame_ms)
            failures = [f"soak.{name}" for name, passed in soak["gates"].items() if passed is not True]
            report = {"schema_version": SCHEMA_VERSION, "mode": "soak", "soak": soak, "overall_pass": not failures, "failures": failures}
        encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
        if args.output:
            try:
                Path(args.output).write_text(encoded, encoding="utf-8")
            except OSError as exc:
                raise InputError(f"cannot write report {args.output}: {exc}") from exc
        sys.stdout.write(encoded)
        return EXIT_OK if report["overall_pass"] else EXIT_GATE_FAILED
    except InputError as exc:
        print(f"latency-tool: error: {exc}", file=sys.stderr)
        return EXIT_NO_DATA if "no records" in str(exc) else EXIT_USAGE


if __name__ == "__main__":
    raise SystemExit(main())
