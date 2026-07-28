#!/usr/bin/env python3
"""Ensure a full live table degrades statistics, never the application."""

import csv
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: hole_live_capacity_test.py HOOK TEST_PROGRAM"
        )
    hook = Path(sys.argv[1]).resolve()
    program = Path(sys.argv[2]).resolve()

    with tempfile.TemporaryDirectory(
        prefix="mini-hole-capacity-"
    ) as temp:
        environment = os.environ.copy()
        environment.update(
            {
                "LD_PRELOAD": str(hook),
                "MINI_HOLE_OUTPUT_DIR": temp,
            }
        )
        result = subprocess.run(
            [str(program)],
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        if result.returncode != 0:
            raise AssertionError(
                f"capacity test exited {result.returncode}: "
                f"{result.stderr}"
            )
        logs = list(Path(temp).glob("mini_hole_*.csv"))
        if len(logs) != 1:
            raise AssertionError(f"expected one CSV, found {logs}")
        with logs[0].open(encoding="utf-8", newline="") as source:
            source.readline()
            rows = [
                {key: int(value) for key, value in row.items()}
                for row in csv.DictReader(source)
            ]

    if len(rows) < 3:
        raise AssertionError(f"expected at least 3 snapshots, got {len(rows)}")
    baseline = rows[0]
    allocated, released = rows[-2:]
    failures = (
        allocated["total_live_track_failed"]
        - baseline["total_live_track_failed"]
    )
    if failures <= 0:
        raise AssertionError("small live table unexpectedly tracked all blocks")
    if allocated["live_alloc"] > 1024:
        raise AssertionError("live allocation count exceeded table capacity")
    if (
        released["live_alloc"] != baseline["live_alloc"]
        or released["live_requested"] != baseline["live_requested"]
        or released["live_hole"] != baseline["live_hole"]
    ):
        raise AssertionError("tracked allocations were not fully released")
    untracked = (
        released["total_untracked_free"]
        - baseline["total_untracked_free"]
    )
    if untracked != failures:
        raise AssertionError(
            f"expected {failures} untracked frees, got {untracked}"
        )

    print("v8 live-table fail-open behavior passed")


if __name__ == "__main__":
    main()
