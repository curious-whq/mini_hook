#!/usr/bin/env python3
"""Validate v9 variable-step buckets and allocator comparisons."""

import csv
from collections import defaultdict
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
    14335, 14336, 14337,
    16383, 16384, 16385,
    40959, 40960, 40961,
    262143, 262144, 262145,
)

DFMALLOC1 = (
    8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 176,
    208, 256, 304, 352, 384, 432, 496, 560, 656, 784, 992, 1312,
    1984, 3968,
)
DFMALLOC2 = (
    8, 16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 304,
    352, 384, 432, 496, 560, 640, 784, 992, 1280, 1536, 2048, 2560,
    3072, 3584, 4096, 5120, 6144, 7168, 8192, 10240, 12288, 14336,
)
JEMALLOC = (
    8, 16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320,
    384, 448, 512, 640, 768, 896, 1024, 1280, 1536, 1792, 2048,
    2560, 3072, 3584, 4096, 5120, 6144, 7168, 8192, 10240, 12288,
    14336, 16384, 40960, 49152, 57344, 65536, 81920, 98304, 114688,
    131072, 163840, 196608, 229376, 262144,
)


def expected_classes():
    return (
        list(range(8, 257, 8))
        + list(range(272, 513, 16))
        + list(range(544, 1025, 32))
        + list(range(1088, 2049, 64))
        + list(range(2176, 4097, 128))
    )


def align_up(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def mini_round(value):
    value = max(value, 1)
    if value <= 256:
        return align_up(value, 8)
    if value <= 512:
        return 256 + align_up(value - 256, 16)
    if value <= 1024:
        return 512 + align_up(value - 512, 32)
    if value <= 2048:
        return 1024 + align_up(value - 1024, 64)
    if value <= 4096:
        return 2048 + align_up(value - 2048, 128)
    return align_up(value, 4096)


def class_round(value, classes):
    value = max(value, 1)
    for size_class in classes:
        if value <= size_class:
            return size_class
    return align_up(value, 4096)


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

    def metadata_classes(name):
        text = metadata.split(f"{name}=", 1)[1].split(",", 1)[0]
        return tuple(int(value) for value in text.split("|"))

    if metadata_classes("dfmalloc1_classes") != DFMALLOC1:
        raise AssertionError("dfmalloc1 metadata does not match its rule")
    if metadata_classes("dfmalloc2_classes") != DFMALLOC2:
        raise AssertionError("dfmalloc2 metadata does not match its rule")
    if metadata_classes("jemalloc_classes") != JEMALLOC:
        raise AssertionError("jemalloc metadata does not match its rule")

    classes_text = metadata.split("size_classes=", 1)[1].split(",", 1)[0]
    actual_classes = classes_text.split("|")
    wanted_classes = [str(value) for value in expected_classes()] + ["4K+"]
    if actual_classes != wanted_classes:
        raise AssertionError("size class metadata does not match the new layout")
    if len(rows) < 2:
        raise AssertionError(f"expected baseline and data rows, got {len(rows)}")
    # v9 may emit its opportunistic initial row on the first test malloc,
    # so the explicit pre-allocation snapshot is the first row.
    baseline, allocated = rows[0], rows[-1]

    expected_buckets = defaultdict(lambda: [0, 0])
    for requested in SIZES:
        rounded = mini_round(requested)
        label = str(rounded) if rounded <= 4096 else "4K_plus"
        expected_buckets[label][0] += 1
        expected_buckets[label][1] += rounded - requested

    for label in [str(value) for value in expected_classes()] + ["4K_plus"]:
        wanted_count, wanted_hole = expected_buckets[label]
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
    expected_hole = sum(mini_round(size) - size for size in SIZES)
    if allocated["live_hole"] - baseline["live_hole"] != expected_hole:
        raise AssertionError("live hole total does not match bucket totals")

    comparison_rules = (
        ("live_hole_dfmalloc1", DFMALLOC1),
        ("live_hole_dfmalloc2", DFMALLOC2),
        ("live_hole_jemalloc", JEMALLOC),
    )
    for field, classes in comparison_rules:
        wanted = sum(class_round(size, classes) - size for size in SIZES)
        actual = allocated[field] - baseline[field]
        if actual != wanted:
            raise AssertionError(
                f"{field}: expected {wanted}, got {actual}"
            )

    print("v9 buckets and allocator comparison boundaries passed")


if __name__ == "__main__":
    main()
