#!/usr/bin/env python3
"""Repeat a generated synthetic mstress workload and report CV."""

from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import sys
import time


class BenchError(RuntimeError):
    pass


def positive_int(text: str) -> int:
    value = int(text)
    if value < 1:
        raise argparse.ArgumentTypeError("must be positive")
    return value


def nonnegative_int(text: str) -> int:
    value = int(text)
    if value < 0:
        raise argparse.ArgumentTypeError("must be non-negative")
    return value


def parse_cpu_list(text: str) -> set[int]:
    result: set[int] = set()
    try:
        for item in text.split(","):
            if not item:
                raise ValueError
            if "-" in item:
                first_text, last_text = item.split("-", 1)
                first, last = int(first_text), int(last_text)
                if first < 0 or last < first:
                    raise ValueError
                result.update(range(first, last + 1))
            else:
                value = int(item)
                if value < 0:
                    raise ValueError
                result.add(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            "expected comma-separated CPUs/ranges, for example 0-15,32-47"
        ) from error
    if not result:
        raise argparse.ArgumentTypeError("CPU list cannot be empty")
    return result


def finite_nonnegative(text: str) -> float:
    value = float(text)
    if not math.isfinite(value) or value < 0:
        raise argparse.ArgumentTypeError("must be finite and non-negative")
    return value


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Benchmark a generated phase/wave synthetic mstress ELF."
    )
    parser.add_argument("executable", type=Path)
    parser.add_argument("--warmups", type=nonnegative_int, default=3)
    parser.add_argument("--runs", type=positive_int, default=30)
    parser.add_argument("--cpu-list", type=parse_cpu_list,
                        help="allowed CPU mask, for example 0-31")
    parser.add_argument("--seed", default="1")
    parser.add_argument(
        "--touch", choices=("none", "first", "pages", "full"),
        default="first",
    )
    parser.add_argument("--allocator", type=Path,
                        help="allocator shared library used as LD_PRELOAD")
    parser.add_argument("--output", type=Path,
                        help="write measurements and summary as JSON")
    parser.add_argument("--max-cv-pct", type=finite_nonnegative,
                        help="return status 4 when internal elapsed CV exceeds this")
    return parser.parse_args()


def summarize(values: list[float]) -> dict[str, float]:
    mean = statistics.mean(values)
    stdev = statistics.stdev(values) if len(values) > 1 else 0.0
    median = statistics.median(values)
    mad = statistics.median(abs(value - median) for value in values)
    trim = len(values) // 10
    trimmed = sorted(values)[trim:len(values) - trim] if trim else values
    trimmed_mean = statistics.mean(trimmed)
    trimmed_stdev = statistics.stdev(trimmed) if len(trimmed) > 1 else 0.0
    return {
        "mean_ms": mean,
        "median_ms": median,
        "stdev_ms": stdev,
        "cv_pct": 100.0 * stdev / mean if mean else 0.0,
        "minimum_ms": min(values),
        "maximum_ms": max(values),
        "max_deviation_pct": (
            100.0 * max(abs(value - mean) for value in values) / mean
            if mean else 0.0
        ),
        "mad_ms": mad,
        "robust_cv_pct": 100.0 * 1.4826 * mad / median if median else 0.0,
        "trimmed_cv_pct": (
            100.0 * trimmed_stdev / trimmed_mean if trimmed_mean else 0.0
        ),
    }


def main() -> int:
    args = parse_args()
    executable = args.executable.resolve()
    if not executable.is_file() or not os.access(executable, os.X_OK):
        raise BenchError(f"not an executable file: {executable}")
    environment = os.environ.copy()
    if args.allocator is not None:
        allocator = args.allocator.resolve()
        if not allocator.is_file():
            raise BenchError(f"allocator does not exist: {allocator}")
        existing = environment.get("LD_PRELOAD")
        environment["LD_PRELOAD"] = (
            f"{allocator}:{existing}" if existing else str(allocator)
        )

    expected = None
    measurements = []
    for index in range(args.warmups + args.runs):
        started = time.monotonic_ns()
        result = subprocess.run(
            [str(executable), "--json", "--seed", args.seed,
             "--touch", args.touch],
            env=environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            preexec_fn=(
                (lambda: os.sched_setaffinity(0, args.cpu_list))
                if args.cpu_list is not None else None
            ),
        )
        wall_ms = (time.monotonic_ns() - started) / 1_000_000.0
        if result.returncode != 0:
            raise BenchError(
                f"run {index + 1} exited with {result.returncode}: "
                f"{result.stderr.strip() or result.stdout.strip()}"
            )
        try:
            data = json.loads(result.stdout)
        except json.JSONDecodeError as error:
            raise BenchError(f"invalid workload JSON: {result.stdout[:500]!r}") from error
        required = (
            "elapsed_ms", "modeled_operations", "actual_operations",
            "remote_frees", "allocation_failures", "local_free_underflows",
            "failed", "max_wave_threads", "waves",
        )
        if any(key not in data for key in required):
            raise BenchError(f"workload JSON is missing fields: {data}")
        signature = (
            data["modeled_operations"], data["actual_operations"],
            data["remote_frees"], data["max_wave_threads"], data["waves"],
        )
        if expected is None:
            expected = signature
        elif signature != expected:
            raise BenchError("workload signature changed between runs")
        if (data["failed"] or
                data["modeled_operations"] != data["actual_operations"] or
                data["allocation_failures"] != 0 or
                data["local_free_underflows"] != 0):
            raise BenchError(f"workload fidelity check failed: {data}")

        warmup = index < args.warmups
        ordinal = index + 1 if warmup else index - args.warmups + 1
        print(
            f"{'warmup' if warmup else 'measured'} {ordinal}/"
            f"{args.warmups if warmup else args.runs}: "
            f"elapsed_ms={data['elapsed_ms']:.6f} wall_ms={wall_ms:.3f} "
            f"deferred={data.get('deferred_remote_frees', 0)}",
            flush=True,
        )
        if not warmup:
            measurements.append({**data, "wall_ms": wall_ms})

    internal = summarize([item["elapsed_ms"] for item in measurements])
    wall = summarize([item["wall_ms"] for item in measurements])
    print(
        "internal: mean_ms={mean_ms:.6f} median_ms={median_ms:.6f} "
        "stdev_ms={stdev_ms:.6f} cv={cv_pct:.3f}% "
        "robust_cv={robust_cv_pct:.3f}% trimmed_cv={trimmed_cv_pct:.3f}% "
        "min_ms={minimum_ms:.6f} max_ms={maximum_ms:.6f}".format(**internal)
    )
    print(
        "wall: mean_ms={mean_ms:.6f} stdev_ms={stdev_ms:.6f} "
        "cv={cv_pct:.3f}%".format(**wall)
    )
    report = {
        "executable": str(executable),
        "allocator": str(args.allocator.resolve()) if args.allocator else "system",
        "cpu_list": sorted(args.cpu_list) if args.cpu_list else None,
        "seed": args.seed,
        "touch": args.touch,
        "warmups": args.warmups,
        "runs": args.runs,
        "signature": expected,
        "internal": internal,
        "wall": wall,
        "measurements": measurements,
    }
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            json.dumps(report, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
    if args.max_cv_pct is not None and internal["cv_pct"] > args.max_cv_pct:
        print(
            f"FAIL: CV {internal['cv_pct']:.3f}% > {args.max_cv_pct:.3f}%"
        )
        return 4
    print("PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, BenchError) as error:
        print(f"synthetic_mstress_bench: {error}", file=sys.stderr)
        raise SystemExit(2)
