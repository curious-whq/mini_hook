#!/usr/bin/env python3
"""Validate v10 common live buckets and offline allocator comparisons."""

import csv
from bisect import bisect_left
from collections import defaultdict
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from visualize_hole_csv import (
    build_live_model,
    enrich_live_row,
    parse_metadata,
)


SIZES = (
    1, 8, 9,
    255, 256, 257,
    511, 512, 513,
    559, 560, 561,
    655, 656, 657,
    783, 784, 785,
    1023, 1024, 1025,
    1311, 1312, 1313,
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


def verify_offline_model(model):
    row = {
        "live_alloc": 0,
        "live_requested": 0,
        "live_allocated": 0,
        "live_hole": 0,
    }
    for index, label in enumerate(model["labels"]):
        row[f"live_hist_count_{label}"] = 0
        row[f"live_hist_requested_{label}"] = 0
        if index >= model["large_start"]:
            row[f"live_hist_4k_hole_{label}"] = 0

    requests = {1, 262145, 1_000_000}
    for boundary in model["histogram_classes"]:
        requests.update(
            value for value in (
                boundary - 1, boundary, boundary + 1
            ) if value > 0
        )
    for requested in sorted(requests):
        index = bisect_left(
            model["histogram_classes"], max(requested, 1)
        )
        label = model["labels"][index]
        row[f"live_hist_count_{label}"] += 1
        row[f"live_hist_requested_{label}"] += requested
        if index >= model["large_start"]:
            row[f"live_hist_4k_hole_{label}"] += (
                align_up(requested, 4096) - requested
            )

    enrich_live_row(row, model)
    rules = (
        ("live_hole_mini96_postprocess", expected_classes()),
        ("live_hole_dfmalloc1", DFMALLOC1),
        ("live_hole_dfmalloc2", DFMALLOC2),
        ("live_hole_jemalloc", JEMALLOC),
    )
    for field, classes in rules:
        expected = sum(
            class_round(requested, classes) - requested
            for requested in requests
        )
        if row[field] != expected:
            raise AssertionError(
                f"offline {field}: expected {expected}, got {row[field]}"
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
            reader = csv.DictReader(source)
            header = reader.fieldnames
            rows = [
                {key: int(value) for key, value in row.items()}
                for row in reader
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
    expected_histogram = sorted(set(
        expected_classes() + list(DFMALLOC1)
        + list(DFMALLOC2) + list(JEMALLOC)
    ))
    if metadata_classes("live_histogram_classes") != tuple(
        expected_histogram
    ):
        raise AssertionError(
            "live histogram is not the allocator-class union"
        )
    if len(expected_histogram) != 120:
        raise AssertionError("expected exactly120 explicit histogram bins")

    classes_text = metadata.split("size_classes=", 1)[1].split(",", 1)[0]
    actual_classes = classes_text.split("|")
    wanted_classes = [str(value) for value in expected_classes()] + ["4K+"]
    if actual_classes != wanted_classes:
        raise AssertionError("size class metadata does not match the new layout")
    if len(rows) < 2:
        raise AssertionError(f"expected baseline and data rows, got {len(rows)}")
    if not metadata.startswith("#mini_malloc_hole_v10,"):
        raise AssertionError(f"unexpected metadata: {metadata}")
    model = build_live_model(parse_metadata(metadata), header)
    verify_offline_model(model)
    for row in rows:
        enrich_live_row(row, model)
        histogram_count = sum(
            row[f"live_hist_count_{label}"]
            for label in model["labels"]
        )
        histogram_requested = sum(
            row[f"live_hist_requested_{label}"]
            for label in model["labels"]
        )
        if histogram_count != row["live_alloc"]:
            raise AssertionError(
                "live histogram count does not match live_alloc"
            )
        if histogram_requested != row["live_requested"]:
            raise AssertionError(
                "live histogram requested does not match live_requested"
            )
        if row["live_hole_mini96_postprocess"] != row["live_hole"]:
            raise AssertionError(
                "postprocessed Mini96 hole does not match hook total"
            )

    # v10 may emit its opportunistic initial row on the first test malloc,
    # so the explicit pre-allocation snapshot is the first row.
    baseline, allocated = rows[0], rows[-1]

    expected_histogram_buckets = defaultdict(lambda: [0, 0, 0])
    for requested in SIZES:
        index = bisect_left(expected_histogram, max(requested, 1))
        label = (
            str(expected_histogram[index])
            if index < len(expected_histogram)
            else "262144_plus"
        )
        expected_histogram_buckets[label][0] += 1
        expected_histogram_buckets[label][1] += requested
        if index >= 100:
            expected_histogram_buckets[label][2] += (
                align_up(requested, 4096) - requested
            )
    for index, label in enumerate(model["labels"]):
        wanted_count, wanted_requested, wanted_4k_hole = (
            expected_histogram_buckets[label]
        )
        actual_count = (
            allocated[f"live_hist_count_{label}"]
            - baseline[f"live_hist_count_{label}"]
        )
        actual_requested = (
            allocated[f"live_hist_requested_{label}"]
            - baseline[f"live_hist_requested_{label}"]
        )
        if (actual_count, actual_requested) != (
            wanted_count, wanted_requested
        ):
            raise AssertionError(
                f"histogram {label}: expected "
                f"{(wanted_count, wanted_requested)}, got "
                f"{(actual_count, actual_requested)}"
            )
        if index >= 100:
            actual_4k_hole = (
                allocated[f"live_hist_4k_hole_{label}"]
                - baseline[f"live_hist_4k_hole_{label}"]
            )
            if actual_4k_hole != wanted_4k_hole:
                raise AssertionError(
                    f"histogram {label} 4K hole: expected "
                    f"{wanted_4k_hole}, got {actual_4k_hole}"
                )

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

    print("v10 common buckets and offline allocator comparisons passed")


if __name__ == "__main__":
    main()
