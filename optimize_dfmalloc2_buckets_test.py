#!/usr/bin/env python3
"""Correctness tests for the constrained dfmalloc2 optimizer."""

import csv
import itertools
from pathlib import Path
import tempfile

from optimize_dfmalloc2_buckets import (
    Candidate,
    allocator_detail_rows,
    page_aware_group_cost,
    select_boundaries,
    valid_step,
    write_detail_tables,
)
from optimize_hole_buckets import Dataset


def test_against_brute_force():
    end = 1536
    candidates = [
        8, 16, 24, 32, 1280, 1344, 1408, 1536
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
                or step < (128 if stop > 1280 else 8)
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


def test_rejects_short_step_above_1280():
    try:
        select_boundaries(
            [1280, 1344],
            end=1344,
            class_count=2,
            group_cost=lambda start, stop: float(stop - start),
        )
    except ValueError:
        return
    raise AssertionError("64-byte step above1280 should be infeasible")


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


def main():
    test_against_brute_force()
    test_rejects_short_step_above_1280()
    test_decreasing_steps_are_allowed()
    test_relative_step_must_be_strictly_above_eight_percent()
    test_page_aware_cost_removes_complete_pages()
    test_detail_table_matches_rule_cost()
    print("dfmalloc2 constrained optimizer tests passed")


if __name__ == "__main__":
    main()
