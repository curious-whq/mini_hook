#!/usr/bin/env python3
"""Correctness tests for the constrained dfmalloc2 optimizer."""

import csv
import itertools
from pathlib import Path
import tempfile

from optimize_dfmalloc2_buckets import (
    Candidate,
    DEFAULT_SCHEMES,
    allocator_detail_rows,
    optimize_schemes,
    page_aware_group_cost,
    report_json,
    select_boundaries,
    valid_step,
    write_detail_tables,
    write_summary_table,
)
from optimize_hole_buckets import Dataset


DFMALLOC2 = [
    8, 16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 224,
    256, 304, 352, 384, 432, 496, 560, 640, 784, 992, 1280,
    1536, 2048, 2560, 3072, 3584, 4096, 5120, 6144, 7168,
    8192, 10240, 12288, 14336,
]


def test_against_brute_force():
    end = 2048
    candidates = [
        8, 16, 24, 32, 1280, 1536, 1792, 2048
    ]

    def group_cost(start, stop):
        return float((stop - start) * stop + start // 8)

    for class_count in (3, 4, 5):
        feasible = []
        for prefix in itertools.combinations(
            candidates[:-1], class_count - 1
        ):
            selected = list(prefix) + [end]
            boundaries = [0] + selected
            steps = [
                boundaries[index] - boundaries[index - 1]
                for index in range(1, len(boundaries))
            ]
            if any(
                step % 8
                or step < (256 if stop > 1280 else 8)
                or step * 100 <= stop * 8
                for step, stop in zip(steps, selected)
            ):
                continue
            cost = 0.0
            start = 0
            for stop in selected:
                cost += group_cost(start, stop)
                start = stop
            feasible.append((cost, selected))

        wanted = min(feasible, key=lambda item: (item[0], item[1]))
        actual = select_boundaries(
            candidates,
            end,
            class_count,
            group_cost,
        )
        if actual != wanted:
            raise AssertionError(
                f"K={class_count}: expected {wanted}, got {actual}"
            )


def test_segmented_start_against_brute_force():
    candidates = [1536, 1792, 2048, 2304, 2560]

    def group_cost(start, stop):
        return float(stop - start + stop // 256)

    feasible = []
    for prefix in itertools.combinations(candidates[:-1], 2):
        selected = list(prefix) + [2560]
        boundaries = [1280] + selected
        if any(
            not valid_step(start, stop)
            for start, stop in zip(boundaries, selected)
        ):
            continue
        cost = sum(
            group_cost(start, stop)
            for start, stop in zip(boundaries, selected)
        )
        feasible.append((cost, selected))
    wanted = min(feasible, key=lambda item: (item[0], item[1]))
    actual = select_boundaries(
        candidates,
        end=2560,
        class_count=3,
        group_cost=group_cost,
        start=1280,
    )
    if actual != wanted:
        raise AssertionError(
            f"segmented DP: expected {wanted}, got {actual}"
        )


def test_rejects_short_step_above_1280():
    try:
        select_boundaries(
            [1280, 1408],
            end=1408,
            class_count=2,
            group_cost=lambda start, stop: float(stop - start),
        )
    except ValueError:
        return
    raise AssertionError("128-byte step above1280 should be infeasible")


def test_accepts_256_step_above_1280():
    if not valid_step(1280, 1536):
        raise AssertionError("256-byte step above1280 should be feasible")


def test_decreasing_steps_are_allowed():
    _, selected = select_boundaries(
        [8, 24, 32],
        end=32,
        class_count=3,
        group_cost=lambda start, stop: 0.0,
    )
    if selected != [8, 24, 32]:
        raise AssertionError("decreasing steps should be allowed")


def test_relative_step_must_be_strictly_above_eight_percent():
    if valid_step(184, 200):
        raise AssertionError("a step equal to8% must be rejected")
    if not valid_step(176, 192):
        raise AssertionError("a step above8% should be accepted")
    try:
        select_boundaries(
            [1024, 1088],
            end=1088,
            class_count=2,
            group_cost=lambda start, stop: float(stop - start),
        )
    except ValueError:
        return
    raise AssertionError("a step below8% should be infeasible")


def test_page_aware_cost_removes_complete_pages():
    dataset = Dataset(
        paths=[Path("synthetic.csv")],
        histogram_classes=[1280, 4096, 8192, 14336],
        labels=["1280", "4096", "8192", "14336", "14336_plus"],
        counts=[0.0, 1.0, 1.0, 0.0, 0.0],
        requested=[0.0, 2000.0, 5000.0, 0.0, 0.0],
        fallback_holes=[0.0, 2096.0, 3192.0, 0.0, 0.0],
        live_requested=7000.0,
        mini_hole=0.0,
        rows=1,
        malformed_rows=0,
        weighting="equal-file",
        tracking_failures=0,
        estimated_files=0,
    )
    actual = page_aware_group_cost(dataset, 1280, 14336)
    if actual != 5288.0:
        raise AssertionError(
            f"expected page-aware hole 5288, got {actual}"
        )


def test_detail_table_matches_rule_cost():
    dataset = Dataset(
        paths=[Path("synthetic.csv")],
        histogram_classes=[8, 16, 24, 32],
        labels=["8", "16", "24", "32", "32_plus"],
        counts=[1.0, 1.0, 1.0, 1.0, 1.0],
        requested=[7.0, 15.0, 20.0, 30.0, 40.0],
        fallback_holes=[
            4089.0, 4081.0, 4076.0, 4066.0, 4056.0
        ],
        live_requested=112.0,
        mini_hole=0.0,
        rows=1,
        malformed_rows=0,
        weighting="equal-file",
        tracking_failures=0,
        estimated_files=0,
    )
    classes = [16, 32]
    details = allocator_detail_rows(dataset, classes)
    holes = [row["hole"] for row in details]
    if holes != [10.0, 14.0, 4056.0]:
        raise AssertionError(f"unexpected detail holes: {holes}")

    allocator_classes = {
        "dfmalloc1.0": classes,
        "dfmalloc2.0": classes,
        "jemalloc": classes,
    }
    candidate = Candidate(0, classes, sum(holes))
    with tempfile.TemporaryDirectory(
        prefix="mini-dfmalloc2-detail-"
    ) as temporary:
        output = Path(temporary) / "details.csv"
        paths = write_detail_tables(
            output, dataset, allocator_classes, [candidate]
        )
        if paths != [output]:
            raise AssertionError(f"unexpected detail paths: {paths}")
        with output.open(
            encoding="utf-8-sig", newline=""
        ) as source:
            rows = list(csv.reader(source))
        if rows[0][0] != "dfmalloc1.0":
            raise AssertionError("missing allocator group header")
        if rows[1][3] != "训练集平均存活空洞(B)":
            raise AssertionError("missing hole column")
        if rows[2][0:4] != ["1 ~ 16", "16", "16", "10"]:
            raise AssertionError(
                f"unexpected first detail row: {rows[2][0:4]}"
            )
        if rows[2][12:16] != ["1 ~ 16", "16", "16", "10"]:
            raise AssertionError("new-rule detail block is incorrect")


def scheme_dataset():
    histogram_classes = (
        list(range(8, 257, 8))
        + list(range(272, 513, 16))
        + list(range(544, 1025, 32))
        + list(range(1088, 2049, 64))
        + list(range(2176, 8193, 128))
        + list(range(8448, 16385, 256))
    )
    counts = [1.0] * len(histogram_classes) + [0.0]
    requested = [float(value) for value in histogram_classes] + [0.0]
    fallback_holes = [
        float((-value) % 4096) for value in histogram_classes
    ] + [0.0]
    return Dataset(
        paths=[Path("schemes.csv")],
        histogram_classes=histogram_classes,
        labels=[str(value) for value in histogram_classes] + ["tail"],
        counts=counts,
        requested=requested,
        fallback_holes=fallback_holes,
        live_requested=sum(requested),
        mini_hole=0.0,
        rows=1,
        malformed_rows=0,
        weighting="equal-file",
        tracking_failures=0,
        estimated_files=0,
    )


def test_ten_segmented_schemes_and_summary():
    dataset = scheme_dataset()
    baseline_hole, validation_hole, candidates = optimize_schemes(
        dataset, None, DFMALLOC2
    )
    if validation_hole is not None or len(candidates) != 10:
        raise AssertionError("the default run must produce ten schemes")
    report = report_json(
        dataset,
        None,
        DFMALLOC2,
        baseline_hole,
        None,
        candidates,
        None,
    )
    if report["format"] != "dfmalloc2_segmented_optimizer_v2":
        raise AssertionError("segmented JSON format is missing")
    if len(report["rules"]) != 10:
        raise AssertionError("JSON must expose all schemes to visualizer")
    baseline_small = sum(value <= 1280 for value in DFMALLOC2)
    baseline_large = len(DFMALLOC2) - baseline_small
    for candidate, scheme in zip(candidates, DEFAULT_SCHEMES):
        if candidate.scheme != scheme:
            raise AssertionError("scheme order changed")
        small = [value for value in candidate.classes if value <= 1280]
        large = [value for value in candidate.classes if value > 1280]
        if len(small) != baseline_small + scheme.small_extra_buckets:
            raise AssertionError(f"scheme {scheme.number}: small count")
        if len(large) != baseline_large + scheme.large_extra_buckets:
            raise AssertionError(f"scheme {scheme.number}: large count")
        if small[-1] != 1280 or large[-1] != 14336:
            raise AssertionError(f"scheme {scheme.number}: fixed endpoint")
        if any(value % scheme.large_alignment for value in large):
            raise AssertionError(f"scheme {scheme.number}: alignment")

    with tempfile.TemporaryDirectory(
        prefix="mini-dfmalloc2-summary-"
    ) as temporary:
        output = Path(temporary) / "summary.csv"
        write_summary_table(
            output,
            dataset,
            None,
            DFMALLOC2,
            baseline_hole,
            None,
            candidates,
        )
        with output.open(
            encoding="utf-8-sig", newline=""
        ) as source:
            rows = list(csv.DictReader(source))
    if len(rows) != 10 or rows[0]["方案"] != "方案1":
        raise AssertionError("summary must contain one row per scheme")
    if rows[0]["dfmalloc2桶"] != "|".join(map(str, DFMALLOC2)):
        raise AssertionError("summary is missing the dfmalloc2 baseline")
    if not rows[0]["训练集空洞率"].endswith("%"):
        raise AssertionError("summary is missing the hole rate")


def main():
    test_against_brute_force()
    test_segmented_start_against_brute_force()
    test_rejects_short_step_above_1280()
    test_accepts_256_step_above_1280()
    test_decreasing_steps_are_allowed()
    test_relative_step_must_be_strictly_above_eight_percent()
    test_page_aware_cost_removes_complete_pages()
    test_detail_table_matches_rule_cost()
    test_ten_segmented_schemes_and_summary()
    print("dfmalloc2 constrained optimizer tests passed")


if __name__ == "__main__":
    main()
