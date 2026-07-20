#!/usr/bin/env python3
"""Repeat a generated native replay and enforce repeatability thresholds."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import statistics
import subprocess
import sys
import time


class BenchError(RuntimeError):
    pass


def nonnegative_int(text: str) -> int:
    value = int(text)
    if value < 0:
        raise argparse.ArgumentTypeError("must be non-negative")
    return value


def at_least_two(text: str) -> int:
    value = int(text)
    if value < 2:
        raise argparse.ArgumentTypeError("must be at least 2")
    return value


def nonnegative_float(text: str) -> float:
    value = float(text)
    if not math.isfinite(value) or value < 0:
        raise argparse.ArgumentTypeError("must be a finite non-negative number")
    return value


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run a generated native replay in fresh processes, discard "
            "warmups, and check CV/max-deviation thresholds."
        )
    )
    parser.add_argument("executable", type=Path)
    parser.add_argument("--warmups", type=nonnegative_int, default=2)
    parser.add_argument("--runs", type=at_least_two, default=10)
    parser.add_argument("--max-cv-pct", type=nonnegative_float, default=1.0)
    parser.add_argument(
        "--max-deviation-pct", type=nonnegative_float, default=1.0
    )
    parser.add_argument(
        "--max-late-bucket-pct",
        type=nonnegative_float,
        help="also fail if any measured run exceeds this late-bucket percentage",
    )
    parser.add_argument("--output", type=Path, help="write full JSON report")
    parser.add_argument(
        "runtime_args",
        nargs=argparse.REMAINDER,
        help="runtime arguments after --, for example -- --cpu-list 4-11",
    )
    args = parser.parse_args()
    if args.runtime_args and args.runtime_args[0] == "--":
        args.runtime_args = args.runtime_args[1:]
    if "--json" in args.runtime_args:
        parser.error("do not pass --json; the benchmark adds it automatically")
    return args


def run_once(executable: Path, runtime_args: list[str]) -> tuple[dict, float]:
    command = [str(executable), *runtime_args, "--json"]
    started = time.monotonic()
    result = subprocess.run(
        command,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    wall_ms = (time.monotonic() - started) * 1000.0
    if result.returncode != 0:
        raise BenchError(
            f"replay exited with {result.returncode}: "
            f"{result.stderr.strip() or result.stdout.strip()}"
        )
    try:
        data = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise BenchError(
            f"invalid replay JSON: {error}: {result.stdout[:500]!r}"
        ) from error
    required = (
        "replay_time_ms",
        "operations",
        "runtime_allocation_failures",
        "late_bucket_pct",
    )
    if any(key not in data for key in required):
        raise BenchError(f"replay JSON is missing required fields: {data}")
    if data["runtime_allocation_failures"] != 0:
        raise BenchError(
            "runtime allocator did not reproduce all successful trace allocations"
        )
    return data, wall_ms


def main() -> int:
    args = parse_arguments()
    executable = args.executable.resolve()
    if not executable.is_file():
        raise BenchError(f"replay executable does not exist: {executable}")

    expected_operations = None
    measured: list[dict] = []
    for index in range(args.warmups + args.runs):
        is_warmup = index < args.warmups
        ordinal = index + 1 if is_warmup else index - args.warmups + 1
        total = args.warmups if is_warmup else args.runs
        label = "warmup" if is_warmup else "measured"
        data, wall_ms = run_once(executable, args.runtime_args)
        if expected_operations is None:
            expected_operations = data["operations"]
        elif data["operations"] != expected_operations:
            raise BenchError("operation count changed between runs")
        print(
            f"{label} {ordinal}/{total}: "
            f"replay_ms={data['replay_time_ms']:.6f} "
            f"wall_ms={wall_ms:.3f} "
            f"late={data['late_bucket_pct']:.3f}% "
            f"yields={data.get('dependency_yields', 0)}"
        )
        if not is_warmup:
            data["wall_time_ms"] = wall_ms
            measured.append(data)

    samples = [float(item["replay_time_ms"]) for item in measured]
    mean_ms = statistics.mean(samples)
    median_ms = statistics.median(samples)
    stdev_ms = statistics.stdev(samples) if len(samples) > 1 else 0.0
    cv_pct = 100.0 * stdev_ms / mean_ms if mean_ms > 0 else 0.0
    max_deviation_pct = (
        100.0 * max(abs(value - mean_ms) for value in samples) / mean_ms
        if mean_ms > 0 else 0.0
    )
    maximum_late_pct = max(
        float(item["late_bucket_pct"]) for item in measured
    )

    failures = []
    if cv_pct > args.max_cv_pct:
        failures.append(
            f"CV {cv_pct:.3f}% > {args.max_cv_pct:.3f}%"
        )
    if max_deviation_pct > args.max_deviation_pct:
        failures.append(
            "max deviation "
            f"{max_deviation_pct:.3f}% > {args.max_deviation_pct:.3f}%"
        )
    if (args.max_late_bucket_pct is not None and
            maximum_late_pct > args.max_late_bucket_pct):
        failures.append(
            "late buckets "
            f"{maximum_late_pct:.3f}% > {args.max_late_bucket_pct:.3f}%"
        )

    report = {
        "executable": str(executable),
        "runtime_args": args.runtime_args,
        "warmups": args.warmups,
        "runs": args.runs,
        "mean_ms": mean_ms,
        "median_ms": median_ms,
        "stdev_ms": stdev_ms,
        "cv_pct": cv_pct,
        "max_deviation_pct": max_deviation_pct,
        "min_ms": min(samples),
        "max_ms": max(samples),
        "maximum_late_bucket_pct": maximum_late_pct,
        "passed": not failures,
        "failures": failures,
        "measurements": measured,
    }
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            json.dumps(report, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )

    print(
        f"summary mean_ms={mean_ms:.6f} median_ms={median_ms:.6f} "
        f"stdev_ms={stdev_ms:.6f} cv={cv_pct:.3f}% "
        f"max_deviation={max_deviation_pct:.3f}% "
        f"max_late_buckets={maximum_late_pct:.3f}%"
    )
    if failures:
        print("FAIL: " + "; ".join(failures))
        return 4
    print("PASS: repeatability thresholds satisfied")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, BenchError) as error:
        print(f"native_replay_bench: {error}", file=sys.stderr)
        raise SystemExit(2)
