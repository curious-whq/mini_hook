#!/usr/bin/env python3
"""Validate v8 live-hole accounting through the public snapshot probe."""

import csv
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def load_rows(path):
    with path.open(encoding="utf-8", newline="") as source:
        metadata = source.readline().strip()
        reader = csv.DictReader(source)
        rows = [
            {key: int(value) for key, value in row.items()}
            for row in reader
        ]
    return metadata, rows


def difference(row, baseline, field):
    return row[field] - baseline[field]


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: hole_live_test.py HOOK TEST_PROGRAM")
    hook = Path(sys.argv[1]).resolve()
    program = Path(sys.argv[2]).resolve()

    with tempfile.TemporaryDirectory(prefix="mini-hole-live-") as temp:
        environment = os.environ.copy()
        environment.update(
            {
                "LD_PRELOAD": str(hook),
                "MINI_HOLE_OUTPUT_DIR": temp,
                "MINI_HOLE_INTERVAL_SEC": "604800",
                "MINI_HOLE_LIVE_CAPACITY": "1024",
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
                f"live test exited {result.returncode}: {result.stderr}"
            )
        logs = list(Path(temp).glob("mini_hole_*.csv"))
        if len(logs) != 1:
            raise AssertionError(f"expected one CSV, found {logs}")
        metadata, rows = load_rows(logs[0])

    if not metadata.startswith("#mini_malloc_hole_v8,"):
        raise AssertionError(f"unexpected metadata: {metadata}")
    if len(rows) < 5:
        raise AssertionError(f"expected at least 5 snapshots, got {len(rows)}")
    # The first allocation may also emit v8's opportunistic initial snapshot,
    # so use the explicit baseline and the final four explicit phase rows.
    baseline = rows[0]
    allocated, resized, freed_seven, freed_all = rows[-4:]

    expected = (
        (allocated, "live_alloc", 2),
        (allocated, "live_requested", 16),
        (allocated, "live_allocated", 24),
        (allocated, "live_hole", 8),
        (resized, "live_alloc", 2),
        (resized, "live_requested", 23),
        (resized, "live_allocated", 24),
        (resized, "live_hole", 1),
        (freed_seven, "live_alloc", 1),
        (freed_seven, "live_requested", 16),
        (freed_seven, "live_allocated", 16),
        (freed_seven, "live_hole", 0),
        (freed_all, "live_alloc", 0),
        (freed_all, "live_requested", 0),
        (freed_all, "live_allocated", 0),
        (freed_all, "live_hole", 0),
    )
    for row, field, value in expected:
        actual = difference(row, baseline, field)
        if actual != value:
            raise AssertionError(
                f"{field}: expected delta {value}, got {actual}"
            )

    if difference(resized, baseline, "total_free") != 1:
        raise AssertionError("successful realloc must release one old block")
    if difference(freed_seven, baseline, "total_free") != 2:
        raise AssertionError("first explicit free was not counted")
    if difference(freed_all, baseline, "total_free") != 3:
        raise AssertionError("second explicit free was not counted")
    if difference(freed_all, baseline, "total_untracked_free") != 0:
        raise AssertionError("tracked test pointers became untracked")
    if difference(freed_all, baseline, "total_live_track_failed") != 0:
        raise AssertionError("live table unexpectedly rejected an allocation")

    print("v8 live allocation/free/realloc accounting passed")


if __name__ == "__main__":
    main()
