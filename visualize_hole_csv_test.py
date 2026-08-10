#!/usr/bin/env python3
"""Focused tests for dashboard time-slice traffic aggregation."""

from visualize_hole_csv import aggregate_period_buckets


def test_period_bucket_totals_and_4k_tail():
    buckets = [
        ("8", "count_8", "hole_8"),
        ("16", "count_16", "hole_16"),
        ("4K_plus", "count_4K_plus", "hole_4K_plus"),
    ]
    rows = [
        {
            "count_8": 2,
            "hole_8": 3,
            "count_16": 1,
            "hole_16": 4,
            "count_4K_plus": 1,
            "hole_4K_plus": 96,
        },
        {
            "count_8": 1,
            "hole_8": 1,
            "count_16": 0,
            "hole_16": 0,
            "count_4K_plus": 1,
            "hole_4K_plus": 192,
        },
    ]
    metrics = aggregate_period_buckets(
        rows, buckets, period_requested=8000
    )
    if metrics["counts"] != [3, 1, 2]:
        raise AssertionError(metrics["counts"])
    if metrics["holes"] != [4, 4, 288]:
        raise AssertionError(metrics["holes"])
    # Explicit buckets recover requested = class * count - hole;
    # the single 4K+ bucket receives the exact remaining period bytes.
    if metrics["requested"] != [20, 12, 7968]:
        raise AssertionError(metrics["requested"])
    if sum(metrics["requested"]) != 8000:
        raise AssertionError("period requested bytes are not conserved")


def main():
    test_period_bucket_totals_and_4k_tail()
    print("dashboard period bucket aggregation tests passed")


if __name__ == "__main__":
    main()
