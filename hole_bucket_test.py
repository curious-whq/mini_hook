#!/usr/bin/env python3
"""Validate every variable-step boundary in the v8 hole hook."""

import csv
import os
from pathlib import Path
import subprocess
import sys
import tempfile


SIZES = (
    1, 8, 9,
    255, 256, 257,
    511, 512, 513,
    1023, 1024, 1025,
    2047, 2048, 2049,
    4095, 4096, 4097,
)

EXPECTED_BUCKETS = {
    "8": (2, 7),
    "16": (1, 7),
    "256": (2, 1),
    "272": (1, 15),
    "512": (2, 1),
    "544": (1, 31),
    "1024": (2, 1),
    "1088": (1, 63),
    "2048": (2, 1),
    "2176": (1, 127),
    "4096": (2, 1),
    "4K_plus": (1, 4095),
}


def expected_classes():
    return (
        list(range(8, 257, 8))
        + list(range(272, 513, 16))
        + list(range(544, 1025, 32))
        + list(range(1088, 2049, 64))
        + list(range(2176, 4097, 128))
    )


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: hole_bucket_test.py HOOK TEST_PROGRAM")
    hook = Path(sys.argv[1]).resolve()
    program = Path(sys.argv[2]).resolve()

    with tempfile.TemporaryDirectory(prefix="mini-hole-bucket-") as temp:
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
                f"bucket test exited {result.returncode}: {result.stderr}"
            )
        logs = list(Path(temp).glob("mini_hole_*.csv"))
        if len(logs) != 1:
            raise AssertionError(f"expected one CSV, found {logs}")
        with logs[0].open(encoding="utf-8", newline="") as source:
            metadata = source.readline().strip()
            rows = [
                {key: int(value) for key, value in row.items()}
                for row in csv.DictReader(source)
            ]

    classes_text = metadata.split("size_classes=", 1)[1].split(",", 1)[0]
    actual_classes = classes_text.split("|")
    wanted_classes = [str(value) for value in expected_classes()] + ["4K+"]
    if actual_classes != wanted_classes:
        raise AssertionError("size class metadata does not match the new layout")
    if len(rows) < 2:
        raise AssertionError(f"expected baseline and data rows, got {len(rows)}")
    # v8 may emit its opportunistic initial row on the first test malloc,
    # so the explicit pre-allocation snapshot is the first row.
    baseline, allocated = rows[0], rows[-1]

    for label in [str(value) for value in expected_classes()] + ["4K_plus"]:
        wanted_count, wanted_hole = EXPECTED_BUCKETS.get(label, (0, 0))
        count = (
            allocated[f"live_count_{label}"]
            - baseline[f"live_count_{label}"]
        )
        hole = (
            allocated[f"live_hole_{label}"]
            - baseline[f"live_hole_{label}"]
        )
        if (count, hole) != (wanted_count, wanted_hole):
            raise AssertionError(
                f"bucket {label}: expected {(wanted_count, wanted_hole)}, "
                f"got {(count, hole)}"
            )

    if allocated["live_alloc"] - baseline["live_alloc"] != len(SIZES):
        raise AssertionError("live allocation total does not match test inputs")
    if allocated["live_requested"] - baseline["live_requested"] != sum(SIZES):
        raise AssertionError("live requested total does not match test inputs")
    expected_hole = sum(hole for _, hole in EXPECTED_BUCKETS.values())
    if allocated["live_hole"] - baseline["live_hole"] != expected_hole:
        raise AssertionError("live hole total does not match bucket totals")

    print("v8 variable-step size class boundaries passed")


if __name__ == "__main__":
    main()
