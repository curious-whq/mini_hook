#!/usr/bin/env python3
"""Correctness tests for optimize_hole_buckets.py."""

import csv
import itertools
from pathlib import Path
import tempfile

from optimize_hole_buckets import (
    combine_files,
    optimize_classes,
)


def direct_cost(boundaries, counts, requested, selected):
    total = 0.0
    class_index = 0
    for upper, count, request_sum in zip(
        boundaries, counts, requested
    ):
        while selected[class_index] < upper:
            class_index += 1
        total += selected[class_index] * count - request_sum
    return total


def test_dynamic_programming():
    boundaries = [8, 16, 24, 32, 40]
    counts = [10.0, 4.0, 8.0, 2.0, 1.0]
    requested = [61.0, 57.0, 141.0, 59.0, 37.0]
    optimized = optimize_classes(
        boundaries, counts, requested, [1, 2, 3, 4, 5]
    )

    for bucket_count in range(1, len(boundaries) + 1):
        brute_force = []
        for prefix in itertools.combinations(
            boundaries[:-1], bucket_count - 1
        ):
            selected = list(prefix) + [boundaries[-1]]
            brute_force.append((
                direct_cost(
                    boundaries, counts, requested, selected
                ),
                selected,
            ))
        wanted_cost = min(item[0] for item in brute_force)
        actual_cost, actual_classes = optimized[bucket_count]
        if actual_cost != wanted_cost:
            raise AssertionError(
                f"K={bucket_count}: expected {wanted_cost}, "
                f"got {actual_cost}"
            )
        if direct_cost(
            boundaries, counts, requested, actual_classes
        ) != wanted_cost:
            raise AssertionError(
                f"K={bucket_count}: reconstructed classes are not optimal"
            )


def write_test_csv(path: Path):
    labels = ["8", "16", "16_plus"]
    header = [
        "start_ns", "end_ns", "live_requested", "live_hole"
    ]
    for index, label in enumerate(labels):
        header.extend([
            f"live_hist_count_{label}",
            f"live_hist_requested_{label}",
        ])
        if index == 2:
            header.append(f"live_hist_4k_hole_{label}")
    rows = [
        [0, 10, 8, 0, 1, 8, 0, 0, 0, 0, 0],
        [10, 40, 15, 1, 0, 0, 1, 15, 0, 0, 0],
    ]
    with path.open("w", encoding="utf-8", newline="") as output:
        output.write(
            "#mini_malloc_hole_v11,"
            "live_histogram_classes=8|16\n"
        )
        writer = csv.writer(output)
        writer.writerow(header)
        writer.writerows(rows)


def test_time_weighting():
    with tempfile.TemporaryDirectory(
        prefix="mini-hole-optimize-"
    ) as temp:
        path = Path(temp) / "trace.csv"
        write_test_csv(path)
        dataset = combine_files([path], "equal-file")
    if abs(dataset.live_requested - 13.25) > 1e-12:
        raise AssertionError(
            f"unexpected requested average: {dataset.live_requested}"
        )
    if abs(dataset.mini_hole - 0.75) > 1e-12:
        raise AssertionError(
            f"unexpected hole average: {dataset.mini_hole}"
        )
    if dataset.counts != [0.25, 0.75, 0.0]:
        raise AssertionError(
            f"unexpected time-weighted counts: {dataset.counts}"
        )


def main():
    test_dynamic_programming()
    test_time_weighting()
    print("bucket optimizer dynamic programming and weighting passed")


if __name__ == "__main__":
    main()
