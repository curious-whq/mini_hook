#!/usr/bin/env python3
"""End-to-end test for the compact phase-based workload generator."""

from __future__ import annotations

import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


ENTRY = struct.Struct("<QQQQQQ")


def event(timestamp, function, tid, address=0, result=0, size=0, cpu=0):
    return ENTRY.pack(
        timestamp, (function << 32) | cpu, tid, address, result, size
    )


def main() -> int:
    source = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="synthetic-mstress-test-") as text:
        temporary = Path(text)
        trace = temporary / "small.rply"
        output = temporary / "workload"
        records = [
            event(0, 100, 11, address=0xA0, result=22),
            event(0, 101, 22, address=0xA0),
            event(0, 0, 11, result=0x1000, size=16),
            event(1, 2, 11, address=2, result=0x2000, size=32),
            event(2, 0, 22, result=0x3000, size=64),
            event(3, 1, 22, address=0x1000),       # remote free
            event(5, 1, 22, address=0x3000),
            event(5, 102, 22, address=0xA0),
            event(5, 103, 11, address=0xA0, result=22),
            event(6, 100, 11, address=0xB0, result=33),
            event(6, 101, 33, address=0xB0),
            event(6, 3, 11, address=0x2000, result=0x4000, size=48),
            event(6, 0, 11, result=0x5000, size=128),
            event(7, 1, 33, address=0x4000),       # cross-wave remote free
            event(8, 1, 11, address=0x5000),
            event(8, 102, 33, address=0xB0),
            event(8, 103, 11, address=0xB0, result=33),
        ]
        with trace.open("wb") as stream:
            stream.write(struct.pack("<Q", len(records) * 6))
            for record in records:
                stream.write(record)

        generated = subprocess.run(
            [
                sys.executable,
                str(source / "rply_to_mstress.py"),
                "--phases", "3",
                "--samples-per-function", "8",
                str(trace), str(output),
            ],
            check=True,
            text=True,
            stdout=subprocess.PIPE,
        )
        if "remote_free=2" not in generated.stdout:
            raise AssertionError(generated.stdout)
        completed = subprocess.run(
            [str(output), "--json", "--seed", "7", "--touch", "full"],
            check=True,
            text=True,
            stdout=subprocess.PIPE,
        )
        result = json.loads(completed.stdout)
        expected = {
            "modeled_operations": 9,
            "actual_operations": 9,
            "threads": 3,
            "phases": 3,
            "waves": 2,
            "max_wave_threads": 2,
            "worker_launches": 4,
            "phase_barriers": 0,
            "malloc": 3,
            "free": 4,
            "calloc": 1,
            "realloc": 1,
            "remote_frees": 2,
            "allocation_failures": 0,
            "failed": False,
        }
        for key, value in expected.items():
            if result.get(key) != value:
                raise AssertionError(f"{key}: {result.get(key)!r} != {value!r}")
        benchmark = subprocess.run(
            [
                sys.executable,
                str(source / "synthetic_mstress_bench.py"),
                "--warmups", "0", "--runs", "2",
                "--seed", "7", "--touch", "none",
                str(output),
            ],
            check=True,
            text=True,
            stdout=subprocess.PIPE,
        )
        if "PASS" not in benchmark.stdout:
            raise AssertionError(benchmark.stdout)
    print("synthetic mstress test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
