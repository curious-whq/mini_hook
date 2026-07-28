#!/usr/bin/env python3
"""Check that allocations inherited from appspawndf are excluded."""

import csv
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def read_csv(path):
    with path.open(encoding="utf-8", newline="") as source:
        metadata = source.readline()
        rows = [
            {key: int(value) for key, value in row.items()}
            for row in csv.DictReader(source)
        ]
    pid = int(
        next(
            item.split("=", 1)[1]
            for item in metadata.split(",")
            if item.startswith("pid=")
        )
    )
    return pid, rows


def main():
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: appspawndf_live_scope_test.py HOOK TEST_PROGRAM"
        )
    hook = Path(sys.argv[1]).resolve()
    program = Path(sys.argv[2]).resolve()

    with tempfile.TemporaryDirectory(
        prefix="mini-hole-appspawndf-"
    ) as temp:
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
                f"scope test exited {result.returncode}: {result.stderr}"
            )
        parsed = [
            read_csv(path)
            for path in Path(temp).glob("mini_hole_*.csv")
        ]

    child_logs = [rows for _, rows in parsed if len(rows) >= 4]
    if len(child_logs) != 1:
        raise AssertionError(
            f"expected one child log with snapshots, got {parsed}"
        )
    baseline, inherited_free, allocated, released = child_logs[0][-4:]

    if baseline["live_alloc"] != 0:
        raise AssertionError("child inherited a parent live allocation")
    if inherited_free["live_alloc"] != 0:
        raise AssertionError("freeing inherited memory changed live state")
    if inherited_free["total_untracked_free"] != 1:
        raise AssertionError("inherited free was not marked untracked")
    if (
        allocated["live_alloc"] != 1
        or allocated["live_requested"] != 9
        or allocated["live_allocated"] != 16
        or allocated["live_hole"] != 7
    ):
        raise AssertionError("post-start malloc was not tracked exactly")
    if (
        released["live_alloc"] != 0
        or released["live_requested"] != 0
        or released["live_hole"] != 0
    ):
        raise AssertionError("post-start free did not clear live state")
    if released["total_untracked_free"] != 1:
        raise AssertionError("tracked free was incorrectly marked untracked")

    print("v8 appspawndf inherited-allocation exclusion passed")


if __name__ == "__main__":
    main()
