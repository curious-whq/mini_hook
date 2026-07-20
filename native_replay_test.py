#!/usr/bin/env python3
"""End-to-end check for offline pointer matching and native replay."""

from pathlib import Path
import json
import os
import struct
import subprocess
import sys
import tempfile


ENTRY = struct.Struct("<QQQQQQ")


def event(timestamp, function, cpu, tid, address, result, size):
    return ENTRY.pack(
        timestamp, (function << 32) | cpu, tid, address, result, size
    )


def main():
    source_dir = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="native-replay-test-") as text:
        directory = Path(text)
        trace = directory / "cross-thread.rply"
        output = directory / "cross-thread-replay"
        base = 10_000_000_000
        events = [
            event(base + 0,       0, 0, 11, 0,      0x1000, 64),
            event(base + 100_000, 0, 1, 22, 0,      0x2000, 128),
            event(base + 200_000, 1, 1, 22, 0x1000, 0,      0),
            event(base + 300_000, 3, 0, 11, 0x2000, 0x3000, 256),
            event(base + 400_000, 1, 0, 11, 0x3000, 0,      0),
            event(base + 500_000, 2, 0, 11, 4,      0x4000, 32),
            event(base + 600_000, 1, 0, 11, 0x4000, 0,      0),
        ]
        with trace.open("wb") as stream:
            stream.write(struct.pack("<Q", len(events) * 6))
            stream.writelines(events)

        generated = subprocess.run(
            [
                sys.executable,
                str(source_dir / "trace_to_native.py"),
                "--quantum-us", "100",
                str(trace),
                str(output),
            ],
            check=True,
            text=True,
            stdout=subprocess.PIPE,
        )
        if "cross_thread_free=1" not in generated.stdout:
            raise RuntimeError(generated.stdout)
        if "cross_thread_realloc=1" not in generated.stdout:
            raise RuntimeError(generated.stdout)

        replayed = subprocess.run(
            [str(output)],
            check=True,
            text=True,
            stdout=subprocess.PIPE,
        )
        required = (
            "operations=7",
            "threads=2",
            "malloc=2 free=3 calloc=1 realloc=1",
            "runtime_allocation_failures=0",
        )
        for text in required:
            if text not in replayed.stdout:
                raise RuntimeError(replayed.stdout)

        allowed_cpus = sorted(os.sched_getaffinity(0))
        affinity_args = ["--cpu-list", ",".join(
            str(cpu) for cpu in allowed_cpus[:2]
        )]
        if len(allowed_cpus) >= 3:
            affinity_args += ["--controller-cpu", str(allowed_cpus[2])]
        affinity_run = subprocess.run(
            [str(output), *affinity_args, "--json"],
            check=True,
            text=True,
            stdout=subprocess.PIPE,
        )
        affinity_json = json.loads(affinity_run.stdout)
        if affinity_json["operations"] != 7:
            raise RuntimeError(affinity_run.stdout)
        if affinity_json["timing_buckets"] != 7:
            raise RuntimeError(affinity_run.stdout)

        report = directory / "repeatability.json"
        benchmark = subprocess.run(
            [
                sys.executable,
                str(source_dir / "native_replay_bench.py"),
                "--warmups", "1",
                "--runs", "3",
                "--max-cv-pct", "1000",
                "--max-deviation-pct", "1000",
                "--output", str(report),
                str(output),
                "--",
                *affinity_args,
                "--scale", "10",
            ],
            check=True,
            text=True,
            stdout=subprocess.PIPE,
        )
        report_json = json.loads(report.read_text(encoding="utf-8"))
        if not report_json["passed"] or "PASS:" not in benchmark.stdout:
            raise RuntimeError(benchmark.stdout)
        print(replayed.stdout, end="")


if __name__ == "__main__":
    main()
