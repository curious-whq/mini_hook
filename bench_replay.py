#!/usr/bin/env python3
"""Allocator Replay Benchmark - compare multiple allocators against one trace.

stdout is captured and parsed as JSON; stderr is inherited so replay's
progress bar is visible in the terminal in real time.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path
from statistics import mean
from typing import Any


def find_allocators(allocator_dir: Path, include_glibc: bool = True) -> list[tuple[str, Path | None]]:
    allocators: list[tuple[str, Path | None]] = []
    if include_glibc:
        allocators.append(("glibc", None))
    if allocator_dir.is_dir():
        for so in sorted(allocator_dir.glob("*.so")):
            name = so.stem.removeprefix("lib")
            allocators.append((name, so.resolve()))
    return allocators


def run_once(
    replay_bin: Path,
    trace: Path,
    preload: Path | None,
    replay_args: list[str],
) -> dict[str, Any]:
    env = os.environ.copy()
    if preload is not None:
        env["LD_PRELOAD"] = str(preload)
    else:
        env.pop("LD_PRELOAD", None)

    cmd = [str(replay_bin)] + replay_args + ["--json", str(trace)]

    start = time.time()
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=sys.stderr,
        text=True,
        env=env,
    )
    stdout, _ = proc.communicate()
    wall_s = time.time() - start

    print(flush=True)

    if proc.returncode != 0:
        print(
            f"    ERROR: replay failed, exit={proc.returncode}, wall={wall_s:.2f}s",
            file=sys.stderr,
            flush=True,
        )
        return {}

    try:
        data = json.loads(stdout)
    except json.JSONDecodeError as exc:
        print(f"    ERROR: failed to parse replay JSON: {exc}", file=sys.stderr)
        print("    First 2000 bytes of stdout:", file=sys.stderr)
        print(stdout[:2000], file=sys.stderr)
        return {}

    print(
        f"    Done: replay_time={data.get('replay_time_ms', 0):.0f} ms, "
        f"wall={wall_s:.2f}s, workers={data.get('workers', 0)}, "
        f"peak={data.get('peak_memory_mb', 0):.2f} MB, "
        f"remote_free={data.get('remote_frees', 0)}",
        flush=True,
    )
    return data


def run_benchmark(
    replay_bin: Path,
    trace: Path,
    preload: Path | None,
    runs: int,
    replay_args: list[str],
) -> list[dict[str, Any]]:
    results: list[dict[str, Any]] = []
    for i in range(1, runs + 1):
        data = run_once(
            replay_bin=replay_bin,
            trace=trace,
            preload=preload,
            replay_args=replay_args,
        )
        if data:
            results.append(data)
    return results


def avg_numeric(values: list[Any]) -> float:
    nums = [v for v in values if isinstance(v, (int, float))]
    return mean(nums) if nums else 0.0


def aggregate(results: list[dict[str, Any]]) -> dict[str, Any]:
    if not results:
        return {}

    def avg_field(key: str) -> float:
        return avg_numeric([r.get(key, 0) for r in results])

    agg: dict[str, Any] = {
        "replay_time_ms": avg_field("replay_time_ms"),
        "peak_memory_mb": avg_field("peak_memory_mb"),
        "total_records": results[0].get("total_records", 0),
        "total_ops": results[0].get("total_ops", 0),
        "workers": avg_field("workers"),
        "slots": avg_field("slots"),
        "allocs": avg_field("allocs"),
        "frees": avg_field("frees"),
        "reallocs": avg_field("reallocs"),
        "unknown_frees": avg_field("unknown_frees"),
        "local_frees": avg_field("local_frees"),
        "remote_frees": avg_field("remote_frees"),
        "failed_allocs": avg_field("failed_allocs"),
        "failed_reallocs": avg_field("failed_reallocs"),
        "waits": avg_field("waits"),
        "wait_yields": avg_field("wait_yields"),
        "runs": len(results),
    }

    if agg["local_frees"] + agg["remote_frees"] > 0:
        agg["remote_free_pct"] = 100.0 * agg["remote_frees"] / (agg["local_frees"] + agg["remote_frees"])
    else:
        agg["remote_free_pct"] = 0.0

    if agg["allocs"] + agg["frees"] > 0 and agg["replay_time_ms"] > 0:
        agg["throughput_ops_per_s"] = (agg["allocs"] + agg["frees"]) / (agg["replay_time_ms"] / 1000.0)
    else:
        agg["throughput_ops_per_s"] = 0.0

    return agg


def print_report(rows: list[dict[str, Any]]) -> None:
    if not rows:
        return

    print()
    print("=" * 100)
    print("  Allocator Replay Benchmark Report")
    print("=" * 100)

    print(
        f"{'Allocator':<18} {'Time(ms)':>10} {'Workers':>8} "
        f"{'Allocs':>10} {'Frees':>10} {'Reallocs':>10} "
        f"{'Peak(MB)':>10} {'Remote%':>9} {'Unknown':>10} {'OPS/s':>12}"
    )
    print("-" * 100)

    baseline_time = next((r.get("replay_time_ms", 0) for r in rows if r.get("name") == "glibc"), None)

    for row in rows:
        name = row.get("name", "?")
        t = row.get("replay_time_ms", 0)
        speedup = ""
        if baseline_time and baseline_time > 0 and t > 0 and name != "glibc":
            ratio = baseline_time / t
            speedup = f" ({ratio:.2f}x)"

        print(
            f"{name:<18} {t:>10.0f} {row.get('workers', 0):>8.0f} "
            f"{row.get('allocs', 0):>10.0f} {row.get('frees', 0):>10.0f} "
            f"{row.get('reallocs', 0):>10.0f} "
            f"{row.get('peak_memory_mb', 0):>10.2f} "
            f"{row.get('remote_free_pct', 0):>8.1f}% "
            f"{row.get('unknown_frees', 0):>10.0f} "
            f"{row.get('throughput_ops_per_s', 0):>12.0f}{speedup}"
        )

    print()
    print("--- Sync overhead ---")
    print(
        f"{'Allocator':<18} {'Waits':>10} {'Yields':>10} "
        f"{'FailedAlloc':>12} {'FailedRealloc':>14} {'LiveLeft':>10}"
    )
    print("-" * 74)
    for row in rows:
        print(
            f"{row.get('name', '?'):<18} "
            f"{row.get('waits', 0):>10.0f} {row.get('wait_yields', 0):>10.0f} "
            f"{row.get('failed_allocs', 0):>12.0f} {row.get('failed_reallocs', 0):>14.0f} "
            f"{row.get('live_left_at_end', 0):>10.0f}"
        )

    print()


def save_json(path: Path, data: Any) -> None:
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)


def main() -> None:
    script_dir = Path(__file__).resolve().parent
    default_allocator_dir = script_dir / "allocator_dir"
    default_replay = script_dir / "build" / "mini_replay_main"
    default_output = script_dir / "bench_results"

    parser = argparse.ArgumentParser(
        description="Benchmark multiple allocators against a replay trace"
    )
    parser.add_argument("trace", type=Path, help="Path to .rply trace file")
    parser.add_argument(
        "-a", "--allocator-dir",
        type=Path,
        default=default_allocator_dir,
        help=f"Directory containing allocator .so files (default: {default_allocator_dir})",
    )
    parser.add_argument(
        "-r", "--replay",
        type=Path,
        default=default_replay,
        help=f"Path to replay binary (default: {default_replay})",
    )
    parser.add_argument(
        "-n", "--runs",
        type=int,
        default=3,
        help="Number of runs per allocator (default: 3)",
    )
    parser.add_argument(
        "-o", "--output",
        type=Path,
        default=default_output,
        help=f"Output directory for JSON results (default: {default_output})",
    )
    parser.add_argument(
        "--only",
        action="append",
        default=[],
        help="Only run allocator names containing this string. Can be used multiple times.",
    )
    parser.add_argument(
        "--skip-glibc",
        action="store_true",
        help="Do not run the system glibc allocator baseline.",
    )
    parser.add_argument(
        "--access-profile",
        type=Path,
        help="Load .macc_profile binary for memory access simulation",
    )
    parser.add_argument(
        "--access-fidelity",
        type=float,
        default=0.1,
        help="Access simulation fraction 0.0-1.0 (default: 0.1)",
    )
    parser.add_argument(
        "--replay-arg",
        action="append",
        default=[],
        dest="replay_args",
        help=(
            "Extra argument forwarded to replay binary before --json. "
            "Use multiple times, e.g. --replay-arg=--touch --replay-arg=alloc"
        ),
    )
    args = parser.parse_args()

    if args.runs <= 0:
        print("Error: --runs must be positive", file=sys.stderr)
        sys.exit(1)

    trace = args.trace.resolve()
    replay_bin = args.replay.resolve()
    allocator_dir = args.allocator_dir.resolve()
    output_dir = args.output.resolve()

    if not trace.exists():
        print(f"Error: trace file not found: {trace}", file=sys.stderr)
        sys.exit(1)
    if not replay_bin.exists():
        print(f"Error: replay binary not found: {replay_bin}", file=sys.stderr)
        print("Build first: cmake --build build", file=sys.stderr)
        sys.exit(1)

    if args.access_profile and not args.access_profile.exists():
        print(f"Error: access profile not found: {args.access_profile}", file=sys.stderr)
        sys.exit(1)

    extra_replay_args: list[str] = list(args.replay_args)
    if args.access_profile:
        extra_replay_args += [
            "--access-profile", str(args.access_profile.resolve()),
            "--access-fidelity", str(args.access_fidelity),
        ]

    allocators = find_allocators(allocator_dir, include_glibc=not args.skip_glibc)
    if args.only:
        filters = args.only
        allocators = [
            (name, so) for name, so in allocators
            if any(f in name for f in filters)
        ]
    if not allocators:
        print(f"Error: no allocators selected from {allocator_dir}", file=sys.stderr)
        sys.exit(1)

    output_dir.mkdir(parents=True, exist_ok=True)

    print("=" * 70)
    print("  Allocator Replay Benchmark")
    print("=" * 70)
    print(f"Trace:       {trace}")
    print(f"Replay bin:  {replay_bin}")
    print(f"Allocators:  {allocator_dir}")
    print(f"Runs:        {args.runs}")
    print(f"Output:      {output_dir}")
    print(f"Replay args: {' '.join(extra_replay_args) if extra_replay_args else '(default)'}")
    if args.access_profile:
        print(f"Access profile: {args.access_profile} (fidelity={args.access_fidelity})")
    print()

    all_rows: list[dict[str, Any]] = []

    for name, preload in allocators:
        label = name + (f" ({preload.name})" if preload else " (system)")
        print(f"Benchmarking {label} ...")

        raw_results = run_benchmark(
            replay_bin=replay_bin,
            trace=trace,
            preload=preload,
            runs=args.runs,
            replay_args=extra_replay_args,
        )

        save_json(output_dir / f"{name}.runs.json", raw_results)

        agg = aggregate(raw_results)
        if not agg:
            print(f"  FAILED: {label}")
            continue

        agg["name"] = name
        save_json(output_dir / f"{name}.json", agg)

        t = agg.get("replay_time_ms", 0)
        print(f"  => {t:.0f} ms, workers={agg.get('workers', 0):.0f}, "
              f"peak={agg.get('peak_memory_mb', 0):.2f} MB, "
              f"remote_free={agg.get('remote_free_pct', 0):.1f}%")
        all_rows.append(agg)

    save_json(output_dir / "summary.json", all_rows)
    print_report(all_rows)
    print(f"Full JSON results saved to: {output_dir}/")


if __name__ == "__main__":
    main()
