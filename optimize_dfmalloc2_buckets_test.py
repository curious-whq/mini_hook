#!/usr/bin/env python3
"""Correctness tests for the constrained dfmalloc2 optimizer."""

import itertools
from pathlib import Path

from optimize_dfmalloc2_buckets import (
    page_aware_group_cost,
    select_boundaries,
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


def main():
    test_against_brute_force()
    test_rejects_short_step_above_1280()
    test_decreasing_steps_are_allowed()
    test_page_aware_cost_removes_complete_pages()
    print("dfmalloc2 constrained optimizer tests passed")


if __name__ == "__main__":
    main()
