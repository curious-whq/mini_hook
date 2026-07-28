#!/usr/bin/env python3
"""Compare mini_mstress performance with and without malloc hole hook."""

import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess
import tempfile
import time


def run_once(command, hook, output_dir, hook_config):
    environment = os.environ.copy()
    environment.pop("LD_PRELOAD", None)
    environment.pop("MINI_HOLE_OUTPUT_DIR", None)
    environment.pop("MINI_HOLE_INTERVAL_SEC", None)
    environment.pop("MINI_HOLE_LIVE_CAPACITY", None)
    environment.pop("MINI_HOLE_LIVE_SAMPLE_RATE", None)
    environment.pop("MINI_HOLE_CLOCK_CHECK_EVERY", None)
    if hook is not None:
        environment["LD_PRELOAD"] = str(hook)
        environment["MINI_HOLE_OUTPUT_DIR"] = str(output_dir)
        environment["MINI_HOLE_INTERVAL_SEC"] = "604800"
        environment.update(hook_config)

    start = time.perf_counter_ns()
    result = subprocess.run(
        command,
        env=environment,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    elapsed = (time.perf_counter_ns() - start) / 1_000_000_000
    if result.returncode != 0:
        raise RuntimeError(
            f"{'hook' if hook else 'baseline'} run failed "
            f"with exit code {result.returncode}: {result.stderr.strip()}"
        )
    return elapsed


def summary(values):
    average = statistics.mean(values)
    deviation = statistics.stdev(values) if len(values) > 1 else 0.0
    return {
        "runs": len(values),
        "mean_s": average,
        "median_s": statistics.median(values),
        "min_s": min(values),
        "max_s": max(values),
        "stdev_s": deviation,
        "cv_pct": deviation / average * 100 if average else 0.0,
    }


def main():
    directory = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser()
    parser.add_argument("--runs", type=int, default=10)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--cpus", default="")
    parser.add_argument("--threads", type=int, default=16)
    parser.add_argument("--scale", type=int, default=100)
    parser.add_argument("--iterations", type=int, default=20)
    parser.add_argument("--live-capacity", type=int, default=1048576)
    parser.add_argument("--live-sample-rate", type=int, default=1)
    parser.add_argument("--clock-check-every", type=int, default=1)
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--benchmark",
        type=Path,
        default=directory / "build" / "mini_mstress_bench",
    )
    parser.add_argument(
        "--hook",
        type=Path,
        default=directory / "build" / "libmini_malloc_hole_hook.so",
    )
    arguments = parser.parse_args()

    benchmark = arguments.benchmark.resolve()
    hook = arguments.hook.resolve()
    if arguments.runs < 2 or arguments.warmups < 0:
        parser.error("--runs must be >= 2 and --warmups must be >= 0")
    if not benchmark.is_file() or not hook.is_file():
        parser.error("build mini_mstress_bench and mini_malloc_hole_hook first")
    for name, value, minimum, maximum in (
        ("--live-capacity", arguments.live_capacity, 1024, 16777216),
        ("--live-sample-rate", arguments.live_sample_rate, 1, 65536),
        ("--clock-check-every", arguments.clock_check_every, 1, 65536),
    ):
        if (
            value < minimum
            or value > maximum
            or value & (value - 1)
        ):
            parser.error(
                f"{name} must be a power of two in "
                f"[{minimum}, {maximum}]"
            )

    command = [
        str(benchmark),
        str(arguments.threads),
        str(arguments.scale),
        str(arguments.iterations),
    ]
    if arguments.cpus:
        command = ["taskset", "-c", arguments.cpus, *command]
    hook_config = {
        "MINI_HOLE_LIVE_CAPACITY": str(arguments.live_capacity),
        "MINI_HOLE_LIVE_SAMPLE_RATE": str(arguments.live_sample_rate),
        "MINI_HOLE_CLOCK_CHECK_EVERY": str(arguments.clock_check_every),
    }

    baseline = []
    hooked = []
    paired_overhead = []
    with tempfile.TemporaryDirectory(prefix="mini-hole-bench-") as temp:
        output_dir = Path(temp)
        for _ in range(arguments.warmups):
            run_once(command, None, output_dir, hook_config)
            run_once(command, hook, output_dir, hook_config)

        for index in range(arguments.runs):
            if index % 2 == 0:
                baseline_time = run_once(
                    command, None, output_dir, hook_config
                )
                hook_time = run_once(
                    command, hook, output_dir, hook_config
                )
            else:
                hook_time = run_once(
                    command, hook, output_dir, hook_config
                )
                baseline_time = run_once(
                    command, None, output_dir, hook_config
                )
            baseline.append(baseline_time)
            hooked.append(hook_time)
            paired_overhead.append(
                (hook_time / baseline_time - 1.0) * 100.0
            )
            print(
                f"pair {index + 1:02d}: baseline={baseline_time:.6f}s "
                f"hook={hook_time:.6f}s "
                f"overhead={paired_overhead[-1]:+.2f}%",
                flush=True,
            )

    baseline_summary = summary(baseline)
    hook_summary = summary(hooked)
    result = {
        "command": command,
        "hook_config": hook_config,
        "baseline": baseline_summary,
        "hook": hook_summary,
        "mean_overhead_pct":
            (hook_summary["mean_s"] / baseline_summary["mean_s"] - 1.0)
            * 100.0,
        "median_overhead_pct":
            (hook_summary["median_s"] / baseline_summary["median_s"] - 1.0)
            * 100.0,
        "paired_overhead_pct": summary(paired_overhead),
        "baseline_samples_s": baseline,
        "hook_samples_s": hooked,
    }
    print(json.dumps(result, indent=2))
    if arguments.output is not None:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(
            json.dumps(result, indent=2) + "\n", encoding="utf-8"
        )


if __name__ == "__main__":
    main()
