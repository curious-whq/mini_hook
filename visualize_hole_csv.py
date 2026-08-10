#!/usr/bin/env python3
"""Generate a self-contained HTML dashboard from a mini hole CSV."""

from __future__ import annotations

import argparse
from bisect import bisect_left
import csv
import html
import json
import math
from pathlib import Path
from typing import Dict, Iterator, List, Optional, Sequence, Tuple


PERIOD_FIELDS = (
    "period_alloc",
    "period_measured",
    "period_requested",
    "period_hole",
    "period_failed",
    "period_measure_error",
)

LIVE_SOURCE_FIELDS = (
    "live_alloc",
    "live_requested",
    "live_allocated",
    "live_hole",
)

LIVE_EVENT_FIELDS = (
    "period_free",
    "period_untracked_free",
    "period_live_track_failed",
)

PAGE_SIZE = 4096

BUILTIN_ALLOCATOR_INFO = (
    {
        "id": "mini160",
        "name": "Mini 160 变长桶",
        "color": "#ff9b57",
        "metadata": "size_classes",
    },
    {
        "id": "dfmalloc1",
        "name": "dfmalloc1.0",
        "color": "#53a7ff",
        "metadata": "dfmalloc1_classes",
    },
    {
        "id": "dfmalloc2",
        "name": "dfmalloc2.0",
        "color": "#52d6a0",
        "metadata": "dfmalloc2_classes",
    },
    {
        "id": "jemalloc",
        "name": "jemalloc",
        "color": "#a98bff",
        "metadata": "jemalloc_classes",
    },
)

CUSTOM_COLORS = (
    "#f2cc60",
    "#ef6f9c",
    "#55c2ff",
    "#7bd88f",
    "#c792ea",
    "#ffcb6b",
    "#89ddff",
    "#f78c6c",
)


def parse_metadata(line: str) -> Dict[str, str]:
    parts = [part.strip() for part in line.strip().split(",")]
    metadata = {"format": parts[0].lstrip("#") if parts else "unknown"}
    for part in parts[1:]:
        if "=" in part:
            key, value = part.split("=", 1)
            metadata[key] = value
    return metadata


def read_preamble(path: Path) -> Tuple[Dict[str, str], List[str]]:
    with path.open("r", encoding="utf-8", newline="") as source:
        metadata_line = source.readline()
        header_line = source.readline()
    metadata = parse_metadata(metadata_line)
    if metadata.get("format") != "mini_malloc_hole_v11":
        raise ValueError("只支持最新的 mini_malloc_hole_v11 CSV")
    if not header_line:
        raise ValueError("CSV 缺少字段表头")
    header = next(csv.reader([header_line]))
    required = {
        "start_ns",
        "end_ns",
        *PERIOD_FIELDS,
        *LIVE_SOURCE_FIELDS,
        *LIVE_EVENT_FIELDS,
    }
    missing = sorted(required.difference(header))
    if missing:
        raise ValueError(f"CSV 缺少必要字段：{', '.join(missing)}")
    return metadata, header


def discover_buckets(header: List[str]) -> List[Tuple[str, str, str]]:
    fields = set(header)
    buckets = []
    for field in header:
        if not field.startswith("count_"):
            continue
        label = field[len("count_") :]
        hole_field = f"hole_{label}"
        if hole_field in fields:
            buckets.append((label, field, hole_field))
    return buckets


def parse_int(value: str) -> int:
    return int(value.strip() or "0")


def metadata_classes(metadata: Dict[str, str], name: str) -> List[int]:
    value = metadata.get(name)
    if not value:
        raise ValueError(f"CSV 元信息缺少 {name}")
    try:
        return [
            int(item)
            for item in value.split("|")
            if item and item != "4K+"
        ]
    except ValueError as error:
        raise ValueError(f"CSV 元信息 {name} 无效") from error


def validate_class_list(
    name: str, classes: Sequence[object]
) -> List[int]:
    try:
        values = [int(value) for value in classes]
    except (TypeError, ValueError) as error:
        raise ValueError(
            f"自定义规则“{name}”的 size_classes 必须全是整数"
        ) from error
    if not values:
        raise ValueError(f"自定义规则“{name}”没有 size_classes")
    if values[0] <= 0 or values != sorted(set(values)):
        raise ValueError(
            f"自定义规则“{name}”的 size_classes 必须为正整数、"
            "严格递增且不能重复"
        )
    return values


def parse_custom_rule(text: str) -> Dict[str, object]:
    if "=" not in text:
        raise ValueError(
            "--custom-rule 格式应为“名称=8|16|24|...”"
        )
    name, class_text = text.split("=", 1)
    name = name.strip()
    if not name:
        raise ValueError("--custom-rule 的名称不能为空")
    tokens = [
        item.strip()
        for item in class_text.replace(",", "|").split("|")
        if item.strip()
    ]
    return {
        "name": name,
        "size_classes": validate_class_list(name, tokens),
        "source": "command_line",
    }


def custom_rules_from_json(path: Path) -> List[Dict[str, object]]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except OSError as error:
        raise ValueError(f"无法读取自定义规则 JSON：{path}") from error
    except json.JSONDecodeError as error:
        raise ValueError(
            f"自定义规则 JSON 格式错误：{path}:{error.lineno}"
        ) from error

    raw_rules: object
    if isinstance(document, dict) and document.get("format") == (
        "dfmalloc2_constrained_optimizer_v1"
    ):
        curve = document.get("curve")
        if not isinstance(curve, list) or not curve:
            raise ValueError(f"{path} 的 curve 为空或不是数组")
        raw_rules = [
            {
                "name": (
                    "dfmalloc2 优化 "
                    f"+{item.get('extra_buckets', index)}桶"
                ),
                "size_classes": item.get("size_classes"),
                "source": str(path),
            }
            for index, item in enumerate(curve)
            if isinstance(item, dict)
        ]
    elif isinstance(document, dict) and "rules" in document:
        raw_rules = document["rules"]
    elif isinstance(document, dict) and "size_classes" in document:
        raw_rules = [document]
    elif isinstance(document, list):
        raw_rules = document
    else:
        raise ValueError(
            f"{path} 应是优化器 JSON、规则对象或 rules 数组"
        )

    if not isinstance(raw_rules, list) or not raw_rules:
        raise ValueError(f"{path} 没有可用规则")
    rules = []
    for index, item in enumerate(raw_rules, start=1):
        if not isinstance(item, dict):
            raise ValueError(f"{path} 的第 {index} 条规则不是对象")
        name = str(item.get("name", "")).strip()
        if not name:
            raise ValueError(f"{path} 的第 {index} 条规则缺少 name")
        classes = item.get("size_classes")
        if not isinstance(classes, list):
            raise ValueError(
                f"{path} 中规则“{name}”的 size_classes 不是数组"
            )
        rules.append(
            {
                "name": name,
                "size_classes": validate_class_list(name, classes),
                "source": str(item.get("source") or path),
            }
        )
    return rules


def collect_custom_rules(
    inline_rules: Sequence[str], json_paths: Sequence[Path]
) -> List[Dict[str, object]]:
    rules = [parse_custom_rule(text) for text in inline_rules]
    for path in json_paths:
        rules.extend(custom_rules_from_json(path.resolve()))
    names = [str(rule["name"]) for rule in rules]
    duplicates = sorted({
        name for name in names if names.count(name) > 1
    })
    if duplicates:
        raise ValueError(
            "自定义规则名称不能重复：" + "、".join(duplicates)
        )
    return rules


def build_live_model(
    metadata: Dict[str, str],
    header: List[str],
    custom_rules: Optional[Sequence[Dict[str, object]]] = None,
) -> Dict[str, object]:
    histogram_classes = metadata_classes(
        metadata, "live_histogram_classes"
    )
    if histogram_classes != sorted(set(histogram_classes)):
        raise ValueError(
            "live_histogram_classes 必须严格递增且不能重复"
        )

    mini_classes = metadata_classes(metadata, "size_classes")
    allocators = {
        str(item["id"]): metadata_classes(
            metadata, str(item["metadata"])
        )
        for item in BUILTIN_ALLOCATOR_INFO
    }
    allocator_info = [
        {
            "id": item["id"],
            "name": item["name"],
            "color": item["color"],
            "custom": False,
        }
        for item in BUILTIN_ALLOCATOR_INFO
    ]
    expected_union = sorted({
        size_class
        for classes in allocators.values()
        for size_class in classes
    })
    if histogram_classes != expected_union:
        raise ValueError(
            "公共桶边界不是四套显式 Size Class 的完整并集"
        )

    if len(mini_classes) != 160 or mini_classes[-1] != 16384:
        raise ValueError(
            "Mini规则应包含160个显式类并结束于16384"
        )
    if len(histogram_classes) != 176:
        raise ValueError(
            "live_histogram_classes 应包含176个显式边界"
        )

    histogram_set = set(histogram_classes)
    for index, rule in enumerate(custom_rules or (), start=1):
        name = str(rule["name"])
        classes = validate_class_list(
            name, rule.get("size_classes", [])
        )
        unavailable = [
            value for value in classes if value not in histogram_set
        ]
        if unavailable:
            preview = "|".join(str(value) for value in unavailable[:12])
            suffix = "…" if len(unavailable) > 12 else ""
            raise ValueError(
                f"自定义规则“{name}”包含 CSV 公共桶中不存在的边界："
                f"{preview}{suffix}；当前 CSV 无法精确重放该规则，"
                "请使用包含这些边界的更细粒度 CSV"
            )
        allocator_id = f"custom_{index}"
        allocators[allocator_id] = classes
        allocator_info.append(
            {
                "id": allocator_id,
                "name": name,
                "color": CUSTOM_COLORS[
                    (index - 1) % len(CUSTOM_COLORS)
                ],
                "custom": True,
                "source": str(rule.get("source", "")),
            }
        )

    labels = [str(value) for value in histogram_classes]
    labels.append(f"{histogram_classes[-1]}_plus")
    fields = set(header)
    missing = []
    for index, label in enumerate(labels):
        for prefix in (
            "live_hist_count_", "live_hist_requested_"
        ):
            field = f"{prefix}{label}"
            if field not in fields:
                missing.append(field)
        if index >= 164:
            field = f"live_hist_4k_hole_{label}"
            if field not in fields:
                missing.append(field)
    if missing:
        raise ValueError(
            "v11 CSV 缺少公共存活桶字段："
            + ", ".join(missing)
        )
    return {
        "histogram_classes": histogram_classes,
        "labels": labels,
        "allocators": allocators,
        "allocator_info": allocator_info,
        "large_start": 164,
    }


def rounded_class_for_interval(
    upper: Optional[int],
    lower: int,
    classes: List[int],
) -> Optional[int]:
    if upper is None or lower >= classes[-1]:
        return None
    index = bisect_left(classes, upper)
    if index >= len(classes):
        return None
    return classes[index]


def page_aware_interval_hole(
    count: int,
    requested: int,
    upper: Optional[int],
    rounded: Optional[int],
    fallback_hole: Optional[int],
) -> int:
    """Remove complete unused pages while retaining the final-page hole."""
    if fallback_hole is not None:
        page_hole = fallback_hole
    elif upper is not None:
        page_rounded = (
            (upper + PAGE_SIZE - 1) // PAGE_SIZE * PAGE_SIZE
        )
        page_hole = count * page_rounded - requested
    else:
        raise ValueError("tail histogram bucket is missing its 4K hole")

    if rounded is None:
        return page_hole
    class_hole = count * rounded - requested
    return min(class_hole, page_hole)


def enrich_live_row(
    row: Dict[str, int], model: Dict[str, object]
) -> None:
    histogram_classes = model["histogram_classes"]
    labels = model["labels"]
    allocators = model["allocators"]
    large_start = model["large_start"]

    holes = {name: 0 for name in allocators}
    allocator_bucket_totals = {}
    for name, classes in allocators.items():
        totals = {
            str(value): [0, 0, 0] for value in classes
        }
        totals["4K_plus"] = [0, 0, 0]
        allocator_bucket_totals[name] = totals
    lower = 0

    for index, label in enumerate(labels):
        upper = (
            histogram_classes[index]
            if index < len(histogram_classes)
            else None
        )
        count = row[f"live_hist_count_{label}"]
        requested = row[f"live_hist_requested_{label}"]
        fallback_hole = (
            row[f"live_hist_4k_hole_{label}"]
            if index >= large_start else None
        )

        for name, classes in allocators.items():
            rounded = rounded_class_for_interval(
                upper, lower, classes
            )
            if rounded is not None:
                bucket_label = str(rounded)
            else:
                bucket_label = "4K_plus"
            bucket_hole = page_aware_interval_hole(
                count, requested, upper, rounded, fallback_hole
            )
            holes[name] += bucket_hole
            totals = allocator_bucket_totals[name][bucket_label]
            totals[0] += count
            totals[1] += requested
            totals[2] += bucket_hole
        if upper is not None:
            lower = upper

    for name, hole in holes.items():
        row[f"live_rule_total_hole_{name}"] = hole
    # Keep the legacy names for callers that consume enriched rows directly.
    row["live_hole_dfmalloc1"] = holes["dfmalloc1"]
    row["live_hole_dfmalloc2"] = holes["dfmalloc2"]
    row["live_hole_jemalloc"] = holes["jemalloc"]
    row["live_hole_mini160_postprocess"] = holes["mini160"]
    for name, totals in allocator_bucket_totals.items():
        for label, (count, requested, hole) in totals.items():
            row[f"live_rule_count_{name}_{label}"] = count
            row[f"live_rule_requested_{name}_{label}"] = requested
            row[f"live_rule_hole_{name}_{label}"] = hole
            if name == "mini160":
                row[f"live_count_{label}"] = count
                row[f"live_hole_{label}"] = hole


def iter_rows(
    path: Path,
    header: List[str],
    selected_pid: Optional[int],
    live_model: Dict[str, object],
) -> Iterator[Tuple[int, Dict[str, int]]]:
    with path.open("r", encoding="utf-8", newline="") as source:
        source.readline()
        source.readline()
        reader = csv.reader(source)
        for line_number, values in enumerate(reader, start=3):
            if not values or all(not value.strip() for value in values):
                continue
            if len(values) != len(header):
                yield line_number, {}
                continue
            try:
                row = {
                    name: parse_int(value)
                    for name, value in zip(header, values)
                }
            except ValueError:
                yield line_number, {}
                continue
            if selected_pid is not None and "pid" in row:
                if row["pid"] != selected_pid:
                    continue
            enrich_live_row(row, live_model)
            yield line_number, row


def empty_totals() -> Dict[str, int]:
    return {field: 0 for field in PERIOD_FIELDS}


def bucket_capacity(label: str) -> Optional[int]:
    try:
        return int(label)
    except ValueError:
        return None


def aggregate_period_buckets(
    rows: Sequence[Dict[str, int]],
    buckets: Sequence[Tuple[str, str, str]],
    period_requested: int,
) -> Dict[str, List[int]]:
    """Aggregate Mini160 traffic buckets and recover requested bytes."""
    counts = [
        sum(row[count_field] for row in rows)
        for _, count_field, _ in buckets
    ]
    holes = [
        sum(row[hole_field] for row in rows)
        for _, _, hole_field in buckets
    ]
    requested: List[Optional[int]] = []
    unknown_indexes = []
    known_requested = 0
    for index, ((label, _, _), count, hole) in enumerate(
        zip(buckets, counts, holes)
    ):
        capacity = bucket_capacity(label)
        if capacity is None:
            requested.append(None)
            unknown_indexes.append(index)
            continue
        value = capacity * count - hole
        requested.append(value)
        known_requested += value

    # v11 has one 4K+ traffic bucket. Its requested bytes are the exact
    # period total minus all explicit Mini160 buckets.
    if len(unknown_indexes) == 1:
        requested[unknown_indexes[0]] = max(
            0, period_requested - known_requested
        )
    return {
        "counts": counts,
        "requested": [value or 0 for value in requested],
        "holes": holes,
    }


def first_pass(
    path: Path,
    metadata: Dict[str, str],
    header: List[str],
    buckets: List[Tuple[str, str, str]],
    selected_pid: Optional[int],
    live_model: Dict[str, object],
) -> Dict[str, object]:
    totals = empty_totals()
    bucket_totals = {
        label: {"count": 0, "hole": 0}
        for label, _, _ in buckets
    }
    row_count = 0
    malformed = 0
    start_ns = None
    end_ns = None
    pids = set()
    peak_hole = 0
    peak_alloc = 0
    peak_ratio = 0.0
    live_supported = all(
        field in header for field in LIVE_SOURCE_FIELDS
    )
    live_event_totals = {
        field: 0 for field in LIVE_EVENT_FIELDS if field in header
    }
    live_latest = {
        field: 0 for field in LIVE_SOURCE_FIELDS
    }
    live_allocator_latest = {
        allocator: 0
        for allocator in live_model["allocators"]
    }
    live_bucket_latest = {
        label: {"count": 0, "hole": 0}
        for label, _, _ in buckets
    }
    peak_live_hole = 0
    peak_live_allocated = 0
    peak_live_ratio = 0.0

    for _, row in iter_rows(
        path, header, selected_pid, live_model
    ):
        if not row:
            malformed += 1
            continue
        row_count += 1
        row_start = row["start_ns"]
        row_end = row["end_ns"]
        start_ns = row_start if start_ns is None else min(start_ns, row_start)
        end_ns = row_end if end_ns is None else max(end_ns, row_end)
        if "pid" in row:
            pids.add(row["pid"])

        for field in PERIOD_FIELDS:
            totals[field] += row[field]
        for label, count_field, hole_field in buckets:
            bucket_totals[label]["count"] += row[count_field]
            bucket_totals[label]["hole"] += row[hole_field]

        actual = row["period_requested"] + row["period_hole"]
        ratio = row["period_hole"] / actual if actual else 0.0
        peak_hole = max(peak_hole, row["period_hole"])
        peak_alloc = max(peak_alloc, row["period_alloc"])
        peak_ratio = max(peak_ratio, ratio)
        for field in live_event_totals:
            live_event_totals[field] += row[field]
        if live_supported:
            for field in LIVE_SOURCE_FIELDS:
                live_latest[field] = row[field]
            for allocator in live_allocator_latest:
                live_allocator_latest[allocator] = row[
                    f"live_rule_total_hole_{allocator}"
                ]
            for label, _, _ in buckets:
                live_count = f"live_count_{label}"
                live_hole = f"live_hole_{label}"
                if live_count in row and live_hole in row:
                    live_bucket_latest[label]["count"] = row[live_count]
                    live_bucket_latest[label]["hole"] = row[live_hole]
            live_actual = row["live_allocated"]
            live_ratio = (
                row["live_hole"] / live_actual if live_actual else 0.0
            )
            peak_live_hole = max(peak_live_hole, row["live_hole"])
            peak_live_allocated = max(
                peak_live_allocated, live_actual
            )
            peak_live_ratio = max(peak_live_ratio, live_ratio)

    if not pids and selected_pid is not None:
        pids.add(selected_pid)
    elif not pids:
        metadata_pid = metadata.get("pid") or metadata.get("creator_pid")
        if metadata_pid and metadata_pid.isdigit():
            pids.add(int(metadata_pid))

    requested = totals["period_requested"]
    hole = totals["period_hole"]
    actual = requested + hole
    live_actual = live_latest["live_allocated"]
    live_summary = {
        "allocations": live_latest["live_alloc"],
        "requested_bytes": live_latest["live_requested"],
        "allocated_bytes": live_actual,
        "hole_bytes": live_latest["live_hole"],
        "hole_ratio": (
            live_latest["live_hole"] / live_actual
            if live_actual else 0.0
        ),
        "total_free": live_event_totals.get("period_free", 0),
        "total_untracked_free": live_event_totals.get(
            "period_untracked_free", 0
        ),
        "total_tracking_failures": live_event_totals.get(
            "period_live_track_failed", 0
        ),
        "peak_hole_bytes": peak_live_hole,
        "peak_allocated_bytes": peak_live_allocated,
        "peak_hole_ratio": peak_live_ratio,
        "bucket_totals": live_bucket_latest,
        "allocators": [
            {
                **info,
                "hole_bytes": live_allocator_latest[info["id"]],
            }
            for info in live_model["allocator_info"]
        ],
    }
    summary = {
        "row_count": row_count,
        "malformed_rows": malformed,
        "start_ns": start_ns or 0,
        "end_ns": end_ns or 0,
        "duration_ns": max(0, (end_ns or 0) - (start_ns or 0)),
        "pids": sorted(pids),
        "totals": totals,
        "actual_bytes": actual,
        "hole_ratio": hole / actual if actual else 0.0,
        "hole_over_requested": hole / requested if requested else 0.0,
        "average_hole": (
            hole / totals["period_measured"]
            if totals["period_measured"]
            else 0.0
        ),
        "peak_period_hole": peak_hole,
        "peak_period_alloc": peak_alloc,
        "peak_period_ratio": peak_ratio,
        "bucket_totals": bucket_totals,
        "live_supported": live_supported,
        "live": live_summary,
    }
    return summary


def aggregate_points(
    path: Path,
    header: List[str],
    buckets: List[Tuple[str, str, str]],
    selected_pid: Optional[int],
    row_count: int,
    max_points: int,
    live_model: Dict[str, object],
) -> List[Dict[str, object]]:
    if row_count == 0:
        return []
    group_size = max(1, math.ceil(row_count / max_points))
    points: List[Dict[str, object]] = []
    group: List[Dict[str, int]] = []

    def finish_group(rows: List[Dict[str, int]]) -> None:
        if not rows:
            return
        start_ns = min(row["start_ns"] for row in rows)
        end_ns = max(row["end_ns"] for row in rows)
        duration_seconds = max(0, end_ns - start_ns) / 1_000_000_000
        duration_seconds = max(duration_seconds, 1e-9)
        alloc = sum(row["period_alloc"] for row in rows)
        measured = sum(row["period_measured"] for row in rows)
        requested = sum(row["period_requested"] for row in rows)
        hole = sum(row["period_hole"] for row in rows)
        failed = sum(row["period_failed"] for row in rows)
        actual = requested + hole
        latest = rows[-1]
        live_allocated = latest["live_allocated"]
        points.append(
            {
                "startNs": start_ns,
                "endNs": end_ns,
                "rows": len(rows),
                "allocRate": alloc / duration_seconds,
                "requestedRate": requested / duration_seconds,
                "holeRate": hole / duration_seconds,
                "failedRate": failed / duration_seconds,
                "holeRatio": hole / actual if actual else 0.0,
                "averageHole": hole / alloc if alloc else 0.0,
                "periodAlloc": alloc,
                "periodMeasured": measured,
                "periodRequested": requested,
                "periodHole": hole,
                "periodFailed": failed,
                "durationSeconds": duration_seconds,
                "periodBuckets": aggregate_period_buckets(
                    rows, buckets, requested
                ),
                "periodFree": sum(
                    row.get("period_free", 0) for row in rows
                ),
                "liveAlloc": latest["live_alloc"],
                "liveRequested": latest["live_requested"],
                "liveAllocated": live_allocated,
                "liveHole": latest["live_hole"],
                "liveHoleRatio": (
                    latest["live_hole"] / live_allocated
                    if live_allocated else 0.0
                ),
                "liveHoleDfmalloc1":
                    latest["live_hole_dfmalloc1"],
                "liveHoleDfmalloc2":
                    latest["live_hole_dfmalloc2"],
                "liveHoleJemalloc":
                    latest["live_hole_jemalloc"],
                "liveHoleRatioDfmalloc1": (
                    latest["live_hole_dfmalloc1"] /
                    (latest["live_requested"] +
                     latest["live_hole_dfmalloc1"])
                    if latest["live_requested"] +
                    latest["live_hole_dfmalloc1"] else 0.0
                ),
                "liveHoleRatioDfmalloc2": (
                    latest["live_hole_dfmalloc2"] /
                    (latest["live_requested"] +
                     latest["live_hole_dfmalloc2"])
                    if latest["live_requested"] +
                    latest["live_hole_dfmalloc2"] else 0.0
                ),
                "liveHoleRatioJemalloc": (
                    latest["live_hole_jemalloc"] /
                    (latest["live_requested"] +
                     latest["live_hole_jemalloc"])
                    if latest["live_requested"] +
                    latest["live_hole_jemalloc"] else 0.0
                ),
                "liveAllocatorHoles": {
                    allocator: {
                        "hole": latest[
                            f"live_rule_total_hole_{allocator}"
                        ],
                        "ratio": (
                            latest[
                                f"live_rule_total_hole_{allocator}"
                            ] / (
                                latest["live_requested"]
                                + latest[
                                    f"live_rule_total_hole_{allocator}"
                                ]
                            )
                            if (
                                latest["live_requested"]
                                + latest[
                                    f"live_rule_total_hole_{allocator}"
                                ]
                            ) else 0.0
                        ),
                    }
                    for allocator in live_model["allocators"]
                },
                "liveAllocatorBuckets": {
                    allocator: {
                        "counts": [
                            latest[
                                f"live_rule_count_{allocator}_{label}"
                            ]
                            for label in (
                                [str(value) for value in classes]
                                + ["4K_plus"]
                            )
                        ],
                        "requested": [
                            latest[
                                f"live_rule_requested_{allocator}_{label}"
                            ]
                            for label in (
                                [str(value) for value in classes]
                                + ["4K_plus"]
                            )
                        ],
                        "holes": [
                            latest[
                                f"live_rule_hole_{allocator}_{label}"
                            ]
                            for label in (
                                [str(value) for value in classes]
                                + ["4K_plus"]
                            )
                        ],
                    }
                    for allocator, classes
                    in live_model["allocators"].items()
                },
                "liveHistogram": {
                    "counts": [
                        latest[f"live_hist_count_{label}"]
                        for label in live_model["labels"]
                    ],
                    "requested": [
                        latest[f"live_hist_requested_{label}"]
                        for label in live_model["labels"]
                    ],
                    "fallbackHoles": [
                        (
                            latest[f"live_hist_4k_hole_{label}"]
                            if index >= live_model["large_start"]
                            else None
                        )
                        for index, label
                        in enumerate(live_model["labels"])
                    ],
                },
            }
        )

    for _, row in iter_rows(
        path, header, selected_pid, live_model
    ):
        if not row:
            continue
        group.append(row)
        if len(group) >= group_size:
            finish_group(group)
            group = []
    finish_group(group)
    return points


def make_allocator_bucket_rows(
    live_model: Dict[str, object],
) -> Dict[str, List[Dict[str, object]]]:
    result = {}
    for allocator, classes in live_model["allocators"].items():
        labels = [str(value) for value in classes] + ["4K_plus"]
        result[allocator] = [
            {
                "label": (
                    f">{classes[-1]}（4K对齐）"
                    if label == "4K_plus"
                    else label
                ),
                "rawLabel": label,
                "bucketSize": bucket_capacity(label),
            }
            for label in labels
        ]
    return result


def json_for_html(value: object) -> str:
    return json.dumps(
        value, ensure_ascii=False, separators=(",", ":")
    ).replace("</", "<\\/")


def build_html(
    source_path: Path,
    title: str,
    metadata: Dict[str, str],
    summary: Dict[str, object],
    points: List[Dict[str, object]],
    bucket_rows: Dict[str, List[Dict[str, object]]],
    live_model: Dict[str, object],
) -> str:
    payload = {
        "source": str(source_path),
        "metadata": metadata,
        "summary": summary,
        "points": points,
        "buckets": bucket_rows,
        "periodBucketInfo": bucket_rows.get("mini160", []),
        "allocatorInfo": live_model["allocator_info"],
        "histogram": {
            "classes": live_model["histogram_classes"],
            "labels": live_model["labels"],
            "largeStart": live_model["large_start"],
        },
    }
    safe_title = html.escape(title)
    data_json = json_for_html(payload)
    return f"""<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{safe_title}</title>
<style>
:root {{
  color-scheme: dark;
  --bg:#0b1020; --panel:#131a2b; --panel2:#182238; --line:#2a3651;
  --text:#eef3ff; --muted:#93a4bf; --blue:#53a7ff; --orange:#ff9b57;
  --green:#52d6a0; --red:#ff6b7a; --violet:#a98bff;
}}
* {{ box-sizing:border-box }}
body {{
  margin:0; background:
    radial-gradient(circle at 15% -10%,#1d335d 0,transparent 35%),
    radial-gradient(circle at 100% 0,#3a1f47 0,transparent 28%),var(--bg);
  color:var(--text); font:14px/1.5 Inter,ui-sans-serif,system-ui,-apple-system,
    BlinkMacSystemFont,"Segoe UI","Microsoft YaHei",sans-serif;
}}
.wrap {{ max-width:1500px; margin:auto; padding:28px }}
header {{ display:flex; gap:20px; justify-content:space-between; align-items:end;
  margin-bottom:22px }}
h1 {{ margin:0; font-size:28px; letter-spacing:.2px }}
.subtitle {{ color:var(--muted); margin-top:7px; word-break:break-all }}
.badge {{ border:1px solid #40547c; background:#15233d; color:#b9d8ff;
  padding:6px 10px; border-radius:999px; white-space:nowrap }}
.cards {{ display:grid; grid-template-columns:repeat(auto-fit,minmax(210px,1fr));
  gap:12px; margin-bottom:16px }}
.card,.panel {{ background:linear-gradient(145deg,rgba(24,34,56,.96),
  rgba(16,23,39,.96)); border:1px solid var(--line);
  box-shadow:0 14px 35px rgba(0,0,0,.18); border-radius:14px }}
.card {{ padding:16px; border-top:3px solid var(--accent,var(--line)) }}
.label {{ color:var(--muted); font-size:12px; text-transform:uppercase;
  letter-spacing:.7px }}
.value {{ font-size:24px; font-weight:750; margin-top:4px }}
.hint {{ color:var(--muted); font-size:12px; margin-top:3px }}
.warning {{ display:none; margin:0 0 16px; padding:11px 14px;
  border:1px solid #8d6032; background:#3a2718; color:#ffd8a8;
  border-radius:10px }}
.grid {{ display:grid; grid-template-columns:repeat(2,minmax(0,1fr)); gap:16px }}
.panel {{ padding:17px; min-width:0 }}
.panel h2 {{ font-size:15px; margin:0 0 11px }}
.chart {{ position:relative; height:250px }}
canvas {{ width:100%; height:100%; display:block }}
.tooltip {{ display:none; position:absolute; pointer-events:none; z-index:4;
  background:#070b14ee; border:1px solid #40506d; border-radius:9px;
  padding:8px 10px; color:#eaf2ff; min-width:230px; box-shadow:0 8px 24px #0008 }}
.wide {{ grid-column:1/-1 }}
.bucket-time {{ display:grid; grid-template-columns:minmax(220px,1fr) auto;
  gap:16px; align-items:center; margin:0 0 16px; padding:12px 14px;
  border:1px solid var(--line); background:#10182a; border-radius:10px }}
.bucket-time input {{ width:100%; accent-color:var(--orange) }}
.bucket-moment {{ color:#c8d7ed; text-align:right; font-variant-numeric:tabular-nums }}
.period-summary {{ display:grid;
  grid-template-columns:repeat(auto-fit,minmax(170px,1fr)); gap:9px;
  margin:0 0 14px }}
.period-stat {{ padding:11px 12px; border:1px solid var(--line);
  background:#10182a; border-radius:9px }}
.period-stat .value {{ font-size:18px; margin-top:2px }}
.period-note {{ color:var(--muted); font-size:12px; margin:0 0 10px }}
.period-table {{ max-height:390px }}
.allocator-compare {{ display:grid;
  grid-template-columns:repeat(auto-fit,minmax(230px,1fr));
  gap:10px; margin-bottom:16px }}
.compare-card {{ padding:13px; border:1px solid var(--line);
  border-left:3px solid var(--accent); background:#10182a; border-radius:10px;
  cursor:pointer }}
.compare-card.active {{ border-color:var(--accent);
  box-shadow:0 0 0 1px var(--accent) inset }}
.compare-name {{ font-weight:700; margin-bottom:7px }}
.compare-value {{ font-size:20px; font-weight:750 }}
.compare-detail {{ color:var(--muted); font-size:12px; margin-top:4px }}
.allocator-switch {{ display:flex; flex-wrap:wrap; gap:8px; margin:0 0 14px }}
.allocator-switch button {{ color:var(--muted); background:#10182a;
  border:1px solid var(--line); border-radius:999px; padding:7px 13px;
  cursor:pointer }}
.allocator-switch button.active {{ color:#fff; border-color:var(--accent);
  background:#1b2942; box-shadow:0 0 0 1px var(--accent) inset }}
.pie-grid {{ display:grid; grid-template-columns:repeat(2,minmax(0,1fr));
  gap:14px; margin:0 0 18px }}
.pie-card {{ padding:14px; border:1px solid var(--line); background:#10182a;
  border-radius:10px; min-width:0 }}
.pie-card h3 {{ font-size:14px; margin:0 0 9px }}
.pie-layout {{ display:grid; grid-template-columns:minmax(220px,.9fr)
  minmax(220px,1.1fr); gap:12px; align-items:center }}
.donut-canvas {{ position:relative; height:260px }}
.pie-legend {{ overflow:auto; max-height:260px; padding-right:4px }}
.legend-row {{ display:grid; grid-template-columns:10px minmax(54px,1fr)
  auto auto; gap:7px; align-items:center; padding:4px 2px;
  color:#c9d5e8; font-size:12px }}
.legend-dot {{ width:9px; height:9px; border-radius:50% }}
.legend-share {{ color:var(--muted); min-width:54px; text-align:right }}
.bucket-grid {{ display:grid; grid-template-columns:minmax(0,1.1fr)
  minmax(400px,1fr); gap:18px }}
.bar-row {{ display:grid; grid-template-columns:72px 1fr 95px; gap:10px;
  align-items:center; margin:7px 0 }}
.bar-track {{ height:12px; background:#0d1424; border-radius:8px; overflow:hidden }}
.bar-fill {{ height:100%; border-radius:8px;
  background:linear-gradient(90deg,var(--orange),#ffd05b) }}
.num {{ text-align:right; font-variant-numeric:tabular-nums }}
.scroll {{ overflow:auto; max-height:470px; border:1px solid var(--line);
  border-radius:10px }}
table {{ border-collapse:collapse; width:100%; font-variant-numeric:tabular-nums }}
th,td {{ padding:9px 11px; border-bottom:1px solid #253049; text-align:right;
  white-space:nowrap }}
th:first-child,td:first-child {{ text-align:left }}
th {{ position:sticky; top:0; background:#182238; color:#b9c9e2; cursor:pointer }}
tr:hover td {{ background:#1b2740 }}
.relation-controls {{ display:flex; flex-wrap:wrap; gap:10px; align-items:end;
  margin-bottom:12px }}
.relation-control {{ display:grid; gap:4px; color:var(--muted); font-size:12px }}
.relation-control select {{ min-width:150px; color:var(--text); background:#10182a;
  border:1px solid var(--line); border-radius:8px; padding:7px 10px }}
.relation-arrow {{ color:var(--muted); font-size:20px; padding-bottom:4px }}
.relation-summary {{ padding:9px 12px; margin-bottom:9px;
  background:#10182a; border:1px solid var(--line); border-radius:9px;
  color:#c9d5e8 }}
.relation-legend {{ display:flex; flex-wrap:wrap; gap:14px; color:var(--muted);
  font-size:12px; margin:0 0 8px }}
.relation-key {{ display:inline-flex; align-items:center; gap:5px }}
.relation-key i {{ width:18px; height:4px; border-radius:4px; display:inline-block }}
.relation-chart {{ overflow:auto; border:1px solid var(--line);
  border-radius:10px; background:#0d1424; margin-bottom:12px }}
.relation-chart svg {{ display:block; width:100%; min-width:760px; min-height:560px }}
.relation-table {{ max-height:360px }}
.empty {{ padding:35px; text-align:center; color:var(--muted) }}
footer {{ color:var(--muted); margin-top:18px; font-size:12px }}
@media (max-width:1100px) {{
  .cards,.allocator-compare {{ grid-template-columns:repeat(2,1fr) }}
  .pie-grid {{ grid-template-columns:1fr }}
  .bucket-grid {{ grid-template-columns:1fr }}
}}
@media (max-width:720px) {{
  .wrap {{ padding:16px }} header {{ align-items:start; flex-direction:column }}
  .cards {{ grid-template-columns:repeat(2,1fr) }}
  .allocator-compare {{ grid-template-columns:1fr }}
  .grid {{ grid-template-columns:1fr }} .wide {{ grid-column:auto }}
  .bucket-time {{ grid-template-columns:1fr }}
  .bucket-moment {{ text-align:left }}
  .pie-layout {{ grid-template-columns:1fr }}
  .relation-arrow {{ display:none }}
}}
</style>
</head>
<body>
<div class="wrap">
  <header>
    <div><h1>{safe_title}</h1><div class="subtitle" id="source"></div></div>
    <div class="badge" id="formatBadge"></div>
  </header>
  <section class="cards" id="cards"></section>
  <div class="warning" id="warning"></div>
  <section class="grid">
    <div class="panel"><h2 id="primaryHoleTitle">各规则当前存活空洞（剔除完整4KiB页）</h2><div class="chart">
      <canvas id="holeRate"></canvas><div class="tooltip"></div></div></div>
    <div class="panel"><h2 id="primaryRatioTitle">各规则当前存活空洞率（剔除完整4KiB页）</h2><div class="chart">
      <canvas id="holeRatio"></canvas><div class="tooltip"></div></div></div>
    <div class="panel wide"><h2>分配次数速率</h2><div class="chart">
      <canvas id="allocRate"></canvas><div class="tooltip"></div></div>
      <div class="bucket-time">
        <input id="allocTime" type="range" min="0" max="0" value="0" step="1"
          aria-label="选择分配次数速率时间片">
        <div class="bucket-moment" id="allocMoment"></div>
      </div>
      <div class="period-summary" id="periodSummary"></div>
      <div class="period-note">按 Mini160 Size Class 汇总本时间片的新申请；表格默认按申请次数降序。申请总量由次数与桶空洞精确还原，不是当前存活量。</div>
      <div class="scroll period-table"><table id="periodTable">
        <thead></thead><tbody></tbody></table></div>
    </div>
    <div class="panel wide">
      <h2 id="bucketTitle">所选时点：分配器规则对比与 Mini 160 桶明细</h2>
      <div class="bucket-time">
        <input id="bucketTime" type="range" min="0" max="0" value="0" step="1"
          aria-label="选择存活桶明细时间点">
        <div class="bucket-moment" id="bucketMoment"></div>
      </div>
      <div class="allocator-compare" id="allocatorCompare"></div>
      <div class="allocator-switch" id="allocatorSwitch"
        aria-label="切换 Size Class 明细规则"></div>
      <div class="pie-grid">
        <div class="pie-card">
          <h3 id="allocatedPieTitle">各 Size Class 当前分配空间</h3>
          <div class="pie-layout">
            <div class="donut-canvas"><canvas id="allocatedPie"></canvas>
              <div class="tooltip"></div></div>
            <div class="pie-legend" id="allocatedPieLegend"></div>
          </div>
        </div>
        <div class="pie-card">
          <h3 id="holePieTitle">各 Size Class 当前空洞</h3>
          <div class="pie-layout">
            <div class="donut-canvas"><canvas id="holePie"></canvas>
              <div class="tooltip"></div></div>
            <div class="pie-legend" id="holePieLegend"></div>
          </div>
        </div>
      </div>
      <div class="bucket-grid">
        <div id="bucketBars"></div>
        <div class="scroll"><table id="bucketTable">
          <thead></thead><tbody></tbody>
        </table></div>
      </div>
    </div>
    <div class="panel wide">
      <h2>所选时点：两两分配器 Size Class 对应关系</h2>
      <div class="relation-controls">
        <label class="relation-control">左侧分配器
          <select id="relationLeft"></select>
        </label>
        <span class="relation-arrow">→</span>
        <label class="relation-control">右侧分配器
          <select id="relationRight"></select>
        </label>
        <label class="relation-control">连线粗细
          <select id="relationMetric">
            <option value="requested">用户申请量</option>
            <option value="count">存活分配数量</option>
            <option value="impact">空洞差绝对值</option>
          </select>
        </label>
        <label class="relation-control">显示关系
          <select id="relationLimit">
            <option value="20">Top 20</option>
            <option value="40">Top 40</option>
            <option value="0">全部</option>
          </select>
        </label>
      </div>
      <div class="relation-summary" id="relationSummary"></div>
      <div class="relation-legend">
        <span class="relation-key"><i style="background:#ff6b7a"></i>右侧空洞更多</span>
        <span class="relation-key"><i style="background:#52d6a0"></i>右侧空洞更少</span>
        <span class="relation-key"><i style="background:#70809c"></i>两侧空洞相同</span>
        <span>连线代表同一批存活申请在两套规则中的桶映射</span>
      </div>
      <div class="relation-chart" id="relationChart"></div>
      <div class="scroll relation-table"><table id="relationTable">
        <thead></thead><tbody></tbody>
      </table></div>
    </div>
    <div class="panel wide"><h2>最近聚合时间点</h2>
      <div class="scroll"><table id="timeTable">
        <thead></thead><tbody></tbody></table></div>
    </div>
  </section>
  <footer id="footer"></footer>
</div>
<script>
const D={data_json};
const S=D.summary, P=D.points, AB=D.buckets, HM=D.histogram;
const PBI=D.periodBucketInfo;
const L=S.live;
let selectedTimeIndex=Math.max(0,P.length-1);
const chartDrawers={{}};
const liveEstimated=D.metadata.live_values==="estimated";
const nf=new Intl.NumberFormat("zh-CN",{{maximumFractionDigits:2}});
function bytes(v){{
  if(!Number.isFinite(v)) return "—";
  const u=["B","KiB","MiB","GiB","TiB","PiB"]; let i=0,n=Math.abs(v);
  while(n>=1024&&i<u.length-1){{n/=1024;i++}}
  return (v<0?"-":"")+nf.format(n)+" "+u[i];
}}
function pct(v){{return Number.isFinite(v)?nf.format(v*100)+"%":"—"}}
function integer(v){{return new Intl.NumberFormat("zh-CN").format(v||0)}}
function timeNs(v){{return v?new Date(v/1e6).toLocaleString():"—"}}
function xml(v){{return String(v).replace(/[&<>"']/g,ch=>({{
  "&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"
}}[ch]))}}
document.getElementById("source").textContent=D.source;
document.getElementById("formatBadge").textContent=
  (D.metadata.format||"unknown")+" · "+S.row_count+" 行";
const ALLOCATORS=D.allocatorInfo;
function allocatorHole(point,id){{
  return Number(point?.liveAllocatorHoles?.[id]?.hole)||0;
}}
function allocatorRatio(point,id){{
  return Number(point?.liveAllocatorHoles?.[id]?.ratio)||0;
}}
const summaryAllocators=Object.fromEntries(
  L.allocators.map(item=>[item.id,item]));
document.getElementById("primaryHoleTitle").textContent=
  ALLOCATORS.length+"套规则当前存活空洞（剔除完整4KiB页）";
document.getElementById("primaryRatioTitle").textContent=
  ALLOCATORS.length+"套规则当前存活空洞率（剔除完整4KiB页）";
document.getElementById("cards").innerHTML=ALLOCATORS.map((allocator,index)=>{{
  const hole=summaryAllocators[allocator.id]?.hole_bytes||0;
  const allocated=L.requested_bytes+hole;
  const ratio=allocated?hole/allocated:0;
  const prefix=index===0?
    "当前活着的分配："+integer(L.allocations)+" 个 · ":"";
  return `<div class="card" style="--accent:${{allocator.color}}">
    <div class="label">${{xml(allocator.name)}} 当前存活空洞</div>
    <div class="value">${{bytes(hole)}}</div>
    <div class="hint">${{prefix}}空洞率 ${{pct(ratio)}} · 占用 ${{bytes(allocated)}}</div>
  </div>`;
}}
).join("");
if(L.total_tracking_failures>0){{
  const warning=document.getElementById("warning");
  warning.style.display="block";
  warning.textContent="存活表跟踪失败 "+integer(L.total_tracking_failures)+
    " 次；当前存活值存在低估，请增大编译期 MINI_HOLE_LIVE_CAPACITY 或启用采样。";
}}

function chart(id,series,format,selectable=false){{
  const canvas=document.getElementById(id), tip=canvas.nextElementSibling;
  const parent=canvas.parentElement;
  function draw(){{
    const r=parent.getBoundingClientRect(),dpr=devicePixelRatio||1;
    canvas.width=Math.max(1,r.width*dpr); canvas.height=Math.max(1,r.height*dpr);
    const c=canvas.getContext("2d"); c.scale(dpr,dpr);
    const w=r.width,h=r.height,pad={{l:58,r:15,t:34,b:30}};
    c.clearRect(0,0,w,h);
    if(!P.length){{c.fillStyle="#93a4bf";c.fillText("暂无周期数据",20,35);return}}
    c.font="11px system-ui";
    let legendRows=1,measureX=pad.l;
    series.forEach(item=>{{
      const itemWidth=c.measureText(item.label).width+34;
      if(measureX+itemWidth>w-pad.r&&measureX>pad.l){{
        legendRows++;measureX=pad.l;
      }}
      measureX+=itemWidth;
    }});
    pad.t=18+legendRows*16;
    const valueOf=(item,point)=>Number(
      item.value?item.value(point):point[item.key]
    )||0;
    const values=series.map(item=>P.map(x=>valueOf(item,x)));
    const rawMax=Math.max(...values.flat());
    const max=rawMax>0?rawMax:1;
    c.strokeStyle="#2a3651";c.fillStyle="#8496b3";c.font="11px system-ui";
    for(let i=0;i<=4;i++){{const y=pad.t+(h-pad.t-pad.b)*i/4;
      c.beginPath();c.moveTo(pad.l,y);c.lineTo(w-pad.r,y);c.stroke();
      const val=max*(1-i/4);c.fillText(format(val),4,y+4)}}
    const x=i=>pad.l+(w-pad.l-pad.r)*(P.length===1?.5:i/(P.length-1));
    const y=v=>h-pad.b-(h-pad.t-pad.b)*v/max;
    series.forEach((item,seriesIndex)=>{{
      c.beginPath();
      values[seriesIndex].forEach((v,i)=>
        i?c.lineTo(x(i),y(v)):c.moveTo(x(i),y(v)));
      c.strokeStyle=item.color;c.lineWidth=2;c.stroke();
    }});
    if(selectable&&P.length){{
      const index=Math.max(0,Math.min(P.length-1,selectedTimeIndex));
      const markerX=x(index);
      c.save();
      c.strokeStyle="#eef3ff88";c.lineWidth=1;c.setLineDash([4,4]);
      c.beginPath();c.moveTo(markerX,pad.t);c.lineTo(markerX,h-pad.b);c.stroke();
      c.setLineDash([]);
      series.forEach((item,seriesIndex)=>{{
        c.beginPath();c.arc(markerX,y(values[seriesIndex][index]),5,0,Math.PI*2);
        c.fillStyle=item.color;c.fill();c.strokeStyle="#fff";c.lineWidth=2;c.stroke();
      }});
      c.restore();
    }}
    let legendX=pad.l,legendY=9;
    c.font="11px system-ui";
    series.forEach(item=>{{
      const itemWidth=c.measureText(item.label).width+34;
      if(legendX+itemWidth>w-pad.r&&legendX>pad.l){{
        legendX=pad.l;legendY+=16;
      }}
      c.fillStyle=item.color;c.fillRect(legendX,legendY,9,9);
      c.fillStyle="#b9c9e2";c.fillText(item.label,legendX+14,legendY+9);
      legendX+=itemWidth;
    }});
    canvas._geom={{x,pad,w,h}};
  }}
  canvas.addEventListener("mousemove",e=>{{
    if(!P.length||!canvas._geom)return;
    const r=canvas.getBoundingClientRect(),g=canvas._geom;
    let i=P.length===1?0:Math.round((e.clientX-r.left-g.pad.l)/
      (g.w-g.pad.l-g.pad.r)*(P.length-1));
    i=Math.max(0,Math.min(P.length-1,i));const p=P[i];
    tip.style.display="block";tip.style.left=Math.min(e.offsetX+12,r.width-190)+"px";
    tip.style.top=Math.max(8,e.offsetY-75)+"px";
    tip.innerHTML=`<b>${{timeNs(p.endNs)}}</b><br>`+
      series.map(item=>`<span style="color:${{item.color}}">●</span> ${{
        item.label}}：${{format(valueOf(item,p))}}`).join("<br>");
  }});
  canvas.addEventListener("mouseleave",()=>tip.style.display="none");
  if(selectable){{
    canvas.style.cursor="pointer";
    canvas.addEventListener("click",event=>{{
      if(!P.length||!canvas._geom)return;
      const rect=canvas.getBoundingClientRect(),g=canvas._geom;
      let index=P.length===1?0:Math.round((event.clientX-rect.left-g.pad.l)/
        (g.w-g.pad.l-g.pad.r)*(P.length-1));
      selectBucketPoint(index);
    }});
  }}
  chartDrawers[id]=draw;
  new ResizeObserver(draw).observe(parent);draw();
}}
const holeSeries=ALLOCATORS.map(item=>({{
  value:point=>allocatorHole(point,item.id),
  label:item.name,color:item.color
}}));
const ratioSeries=ALLOCATORS.map(item=>({{
  value:point=>allocatorRatio(point,item.id),
  label:item.name,color:item.color
}}));
chart("holeRate",holeSeries,bytes);
chart("holeRatio",ratioSeries,pct);
chart("allocRate",[
  {{key:"allocRate",label:"分配次数速率",color:"#53a7ff"}}
],v=>nf.format(v)+"/s",true);

const donutState={{}};
function bucketColor(index){{
  return `hsl(${{(index*137.508+18)%360}} 72% 60%)`;
}}
function drawDonut(id){{
  const state=donutState[id];
  if(!state)return;
  const canvas=document.getElementById(id);
  const parent=canvas.parentElement;
  const r=parent.getBoundingClientRect(),dpr=devicePixelRatio||1;
  canvas.width=Math.max(1,r.width*dpr);canvas.height=Math.max(1,r.height*dpr);
  const c=canvas.getContext("2d");c.scale(dpr,dpr);
  const w=r.width,h=r.height,cx=w/2,cy=h/2;
  const radius=Math.max(20,Math.min(w,h)*.39);
  const inner=radius*.58;
  c.clearRect(0,0,w,h);
  const entries=state.rows
    .map((row,index)=>({{row,index,value:Number(row[state.key])||0}}))
    .filter(item=>item.value>0)
    .sort((a,b)=>
      (a.row.bucketSize??Number.POSITIVE_INFINITY)-
      (b.row.bucketSize??Number.POSITIVE_INFINITY));
  const legendEntries=[...entries].sort((a,b)=>b.value-a.value);
  const total=entries.reduce((sum,item)=>sum+item.value,0);
  let angle=-Math.PI/2;
  const slices=[];
  entries.forEach(item=>{{
    const end=angle+item.value/total*Math.PI*2;
    c.beginPath();c.moveTo(cx,cy);c.arc(cx,cy,radius,angle,end);
    c.closePath();c.fillStyle=bucketColor(item.index);c.fill();
    c.strokeStyle="#10182a";c.lineWidth=1;c.stroke();
    slices.push({{...item,start:angle,end,color:bucketColor(item.index)}});
    angle=end;
  }});
  c.beginPath();c.arc(cx,cy,inner,0,Math.PI*2);
  c.fillStyle="#10182a";c.fill();
  c.textAlign="center";c.textBaseline="middle";
  c.fillStyle="#eef3ff";c.font="700 15px system-ui";
  c.fillText(total?bytes(total):"暂无数据",cx,cy-7);
  c.fillStyle="#93a4bf";c.font="11px system-ui";
  c.fillText(state.centerLabel,cx,cy+13);
  canvas._donut={{cx,cy,inner,radius,slices,total}};
  const legend=document.getElementById(state.legendId);
  legend.innerHTML=legendEntries.length?legendEntries.map(item=>{{
    const share=total?item.value/total:0;
    return `<div class="legend-row"><i class="legend-dot" style="background:${{
      bucketColor(item.index)}}"></i><span>${{item.row.label}}</span>
      <span>${{bytes(item.value)}}</span><span class="legend-share">${{
      pct(share)}}</span></div>`;
  }}).join(""):`<div class="empty">没有非零数据</div>`;
}}
function initDonut(id){{
  const canvas=document.getElementById(id),tip=canvas.nextElementSibling;
  canvas.addEventListener("mousemove",event=>{{
    const g=canvas._donut;if(!g||!g.slices.length)return;
    const rect=canvas.getBoundingClientRect();
    const x=event.clientX-rect.left-g.cx,y=event.clientY-rect.top-g.cy;
    const distance=Math.hypot(x,y);
    if(distance<g.inner||distance>g.radius){{
      tip.style.display="none";return;
    }}
    let angle=Math.atan2(y,x);
    if(angle<-Math.PI/2)angle+=Math.PI*2;
    const slice=g.slices.find(item=>angle>=item.start&&angle<=item.end);
    if(!slice){{tip.style.display="none";return}}
    tip.style.display="block";
    tip.style.left=Math.min(event.offsetX+12,rect.width-205)+"px";
    tip.style.top=Math.max(8,event.offsetY-65)+"px";
    tip.innerHTML=`<b>${{slice.row.label}}</b><br>${{
      donutState[id].valueLabel}}：${{bytes(slice.value)}}<br>占比 ${{
      pct(slice.value/g.total)}}`;
  }});
  canvas.addEventListener("mouseleave",()=>tip.style.display="none");
  new ResizeObserver(()=>drawDonut(id)).observe(canvas.parentElement);
}}
function setDonut(id,legendId,rows,key,centerLabel,valueLabel){{
  donutState[id]={{legendId,rows,key,centerLabel,valueLabel}};
  drawDonut(id);
}}
initDonut("allocatedPie");
initDonut("holePie");

const BK={{
  count:"liveCount",hole:"liveHole",average:"liveAverageHole",
  ratio:"liveRatio"
}};
document.querySelector("#bucketTable thead").innerHTML=
  `<tr><th data-key="bucketSize">桶</th><th data-key="${{BK.count}}">存活分配</th>
   <th data-key="${{BK.hole}}">存活空洞</th>
   <th data-key="${{BK.average}}">平均空洞</th>
   <th data-key="${{BK.ratio}}">平均空洞率</th></tr>`;
function renderBuckets(rows){{
  const ranked=[...rows]
    .filter(x=>x[BK.count]>0||x[BK.hole]>0)
    .sort((a,b)=>b[BK.hole]-a[BK.hole]);
  const top=ranked.slice(0,12);
  const max=Math.max(...top.map(x=>x[BK.hole]),1);
  document.getElementById("bucketBars").innerHTML=top.length?top.map(x=>{{
    const width=Math.sqrt(x[BK.hole]/max)*100;
    return `<div class="bar-row"><span>${{x.label}}</span>
      <div class="bar-track"><div class="bar-fill" style="width:${{width}}%"></div></div>
      <span class="num">${{bytes(x[BK.hole])}}</span></div>`;
  }}).join(""):`<div class="empty">没有桶数据</div>`;
  const body=document.querySelector("#bucketTable tbody");
  body.innerHTML=rows.map(x=>`<tr><td>${{x.label}}</td>
    <td>${{integer(x[BK.count])}}</td>
    <td>${{bytes(x[BK.hole])}}</td><td>${{bytes(x[BK.average])}}</td>
    <td>${{x[BK.ratio]===null?"—":pct(x[BK.ratio])}}</td></tr>`).join("");
}}
let bucketRows=[];
let bucketSort={{key:BK.hole,desc:true}};
let selectedAllocator="mini160";
let selectedPoint=null;
function rowsAtPoint(point){{
  const definitions=AB[selectedAllocator]||[];
  const values=point?.liveAllocatorBuckets?.[selectedAllocator]||{{}};
  return definitions.map((bucket,index)=>{{
    const liveCount=values.counts?.[index]||0;
    const liveRequested=values.requested?.[index]||0;
    const liveHole=values.holes?.[index]||0;
    const liveAllocated=liveRequested+liveHole;
    return {{
      ...bucket,
      liveCount,
      liveRequested,
      liveAllocated,
      liveHole,
      liveAverageHole:liveCount?liveHole/liveCount:0,
      liveRatio:liveAllocated?liveHole/liveAllocated:null,
    }};
  }});
}}
function renderSizePies(){{
  const allocator=ALLOCATORS.find(item=>item.id===selectedAllocator);
  const name=allocator?.name||selectedAllocator;
  document.getElementById("allocatedPieTitle").textContent=
    name+"：各 Size Class 当前分配空间";
  document.getElementById("holePieTitle").textContent=
    name+"：各 Size Class 当前空洞";
  setDonut("allocatedPie","allocatedPieLegend",bucketRows,
    "liveAllocated","实际分配空间","分配空间");
  setDonut("holePie","holePieLegend",bucketRows,
    "liveHole","内部空洞","空洞");
}}
function renderSortedBuckets(){{
  const key=bucketSort.key;
  const rows=[...bucketRows].sort((a,b)=>{{
    const av=a[key]??-1,bv=b[key]??-1;
    return (typeof av==="string"?av.localeCompare(bv):av-bv)*
      (bucketSort.desc?-1:1);
  }});
  renderBuckets(rows);
}}
function sortBuckets(key){{
  if(bucketSort.key===key)bucketSort.desc=!bucketSort.desc;
  else bucketSort={{key,desc:key!=="bucketSize"}};
  renderSortedBuckets();
}}
document.querySelectorAll("#bucketTable th[data-key]").forEach(th=>
  th.addEventListener("click",()=>sortBuckets(th.dataset.key)));
const bucketTime=document.getElementById("bucketTime");
const bucketMoment=document.getElementById("bucketMoment");
const allocTime=document.getElementById("allocTime");
const allocMoment=document.getElementById("allocMoment");
const periodSummary=document.getElementById("periodSummary");
const periodTable=document.getElementById("periodTable");
const allocatorCompare=document.getElementById("allocatorCompare");
const allocatorSwitch=document.getElementById("allocatorSwitch");
const relationLeft=document.getElementById("relationLeft");
const relationRight=document.getElementById("relationRight");
const relationMetric=document.getElementById("relationMetric");
const relationLimit=document.getElementById("relationLimit");
const relationSummary=document.getElementById("relationSummary");
const relationChart=document.getElementById("relationChart");
const relationTable=document.getElementById("relationTable");
bucketTime.max=String(Math.max(0,P.length-1));
bucketTime.value=String(Math.max(0,P.length-1));
bucketTime.disabled=!P.length;
allocTime.max=String(Math.max(0,P.length-1));
allocTime.value=String(Math.max(0,P.length-1));
allocTime.disabled=!P.length;
periodTable.querySelector("thead").innerHTML=
  `<tr><th>Mini160 Size Class</th><th>申请次数</th><th>次数占比</th>
   <th>用户申请总量</th><th>平均申请</th><th>实际分配</th>
   <th>内部空洞</th><th>平均空洞</th></tr>`;
function periodRowsAtPoint(point){{
  const values=point?.periodBuckets||{{}};
  return PBI.map((bucket,index)=>{{
    const count=Number(values.counts?.[index])||0;
    const requested=Number(values.requested?.[index])||0;
    const hole=Number(values.holes?.[index])||0;
    return {{...bucket,count,requested,hole,allocated:requested+hole,
      averageRequested:count?requested/count:0,
      averageHole:count?hole/count:0}};
  }});
}}
function periodPercentile(rows,quantile){{
  const ordered=[...rows].sort((a,b)=>(a.bucketSize??Infinity)-
    (b.bucketSize??Infinity));
  const total=ordered.reduce((sum,row)=>sum+row.count,0);
  if(!total)return "—";
  const target=total*quantile;let cumulative=0;
  for(const row of ordered){{
    cumulative+=row.count;
    if(cumulative>=target)return row.label;
  }}
  return ordered[ordered.length-1]?.label||"—";
}}
function renderPeriod(point){{
  if(!point){{
    allocMoment.textContent="CSV 目前没有可查看的时间片";
    periodSummary.innerHTML="";
    periodTable.querySelector("tbody").innerHTML=
      `<tr><td colspan="8" class="empty">没有时间片分配数据</td></tr>`;
    return;
  }}
  const rows=periodRowsAtPoint(point);
  const active=rows.filter(row=>row.count>0);
  const ranked=[...active].sort((a,b)=>b.count-a.count||
    (a.bucketSize??Infinity)-(b.bucketSize??Infinity));
  const top=ranked[0];
  const measured=Number(point.periodMeasured)||0;
  const average=measured?point.periodRequested/measured:0;
  const allocated=point.periodRequested+point.periodHole;
  const holeRatio=allocated?point.periodHole/allocated:0;
  allocMoment.textContent=timeNs(point.startNs)+" — "+timeNs(point.endNs)+
    " · 聚合 "+integer(point.rows)+" 个原始时间片";
  const cards=[
    ["本时间片申请",integer(point.periodAlloc),
      "速率 "+nf.format(point.allocRate)+"/s · 可测量 "+integer(measured)],
    ["用户申请总量",bytes(point.periodRequested),
      "平均每次 "+bytes(average)],
    ["活跃 Size Class",integer(active.length),top?
      "Top "+top.label+" · "+pct(top.count/(measured||point.periodAlloc||1)):
      "没有成功申请"],
    ["按次数的 Size Class 分位数",
      "P50 "+periodPercentile(rows,.5),
      "P90 "+periodPercentile(rows,.9)],
    ["本时间片内部空洞",bytes(point.periodHole),
      "空洞率 "+pct(holeRatio)],
    ["其他事件",integer(point.periodFailed)+" 次失败",
      integer(point.periodFree)+" 次 free"],
  ];
  periodSummary.innerHTML=cards.map(card=>
    `<div class="period-stat"><div class="label">${{xml(card[0])}}</div>
     <div class="value">${{xml(card[1])}}</div><div class="hint">${{
       xml(card[2])}}</div></div>`).join("");
  periodTable.querySelector("tbody").innerHTML=ranked.map(row=>
    `<tr><td>${{xml(row.label)}}</td><td>${{integer(row.count)}}</td>
     <td>${{pct(row.count/(measured||point.periodAlloc||1))}}</td>
     <td>${{bytes(row.requested)}}</td><td>${{bytes(row.averageRequested)}}</td>
     <td>${{bytes(row.allocated)}}</td><td>${{bytes(row.hole)}}</td>
     <td>${{bytes(row.averageHole)}}</td></tr>`
  ).join("")||`<tr><td colspan="8" class="empty">本时间片没有成功申请</td></tr>`;
}}
function renderAllocatorComparison(point){{
  if(!point){{
    allocatorCompare.innerHTML=`<div class="empty">没有分配器对比数据</div>`;
    return;
  }}
  const baseline=ALLOCATORS[0];
  const miniHole=allocatorHole(point,baseline.id);
  allocatorCompare.innerHTML=ALLOCATORS.map((allocator,index)=>{{
    const hole=allocatorHole(point,allocator.id);
    const allocated=point.liveRequested+hole;
    const ratio=allocated?hole/allocated:0;
    const average=point.liveAlloc?hole/point.liveAlloc:0;
    const delta=hole-miniHole;
    const deltaText=index===0?"比较基准":
      "比 "+baseline.name+" "+(delta>=0?"+":"")+bytes(delta);
    return `<div class="compare-card ${{selectedAllocator===allocator.id?
      "active":""}}" data-allocator="${{allocator.id}}"
      style="--accent:${{allocator.color}}">
      <div class="compare-name">${{xml(allocator.name)}}</div>
      <div class="compare-value">${{bytes(hole)}}</div>
      <div class="compare-detail">空洞率 ${{pct(ratio)}} · 平均每个活分配 ${{
        bytes(average)}}</div>
      <div class="compare-detail">预计实际占用 ${{bytes(allocated)}} · ${{
        xml(deltaText)}}</div>
    </div>`;
  }}).join("");
  allocatorCompare.querySelectorAll("[data-allocator]").forEach(card=>
    card.addEventListener("click",()=>selectAllocator(card.dataset.allocator)));
}}
function renderAllocatorSwitch(){{
  allocatorSwitch.innerHTML=ALLOCATORS.map(allocator=>
    `<button type="button" data-allocator="${{allocator.id}}"
      class="${{selectedAllocator===allocator.id?"active":""}}"
      style="--accent:${{allocator.color}}">${{
        xml(allocator.name)}} 明细</button>`
  ).join("");
  allocatorSwitch.querySelectorAll("[data-allocator]").forEach(button=>
    button.addEventListener("click",()=>
      selectAllocator(button.dataset.allocator)));
}}
function selectAllocator(allocatorId){{
  if(!ALLOCATORS.some(item=>item.id===allocatorId))return;
  selectedAllocator=allocatorId;
  const allocator=ALLOCATORS.find(item=>item.id===allocatorId);
  document.getElementById("bucketTitle").textContent=
    "所选时点：分配器规则对比与 "+allocator.name+" Size Class 明细";
  bucketRows=rowsAtPoint(selectedPoint);
  renderAllocatorSwitch();
  renderAllocatorComparison(selectedPoint);
  renderSortedBuckets();
  renderSizePies();
}}
function allocatorBucketForInterval(allocatorId,upper){{
  const definitions=AB[allocatorId]||[];
  if(upper!==null){{
    const explicit=definitions.find(bucket=>
      bucket.bucketSize!==null&&bucket.bucketSize>=upper);
    if(explicit)return explicit;
  }}
  return definitions[definitions.length-1];
}}
function intervalHole(bucket,count,requested,fallbackHole,upper){{
  let pageHole;
  if(fallbackHole!==null&&fallbackHole!==undefined)
    pageHole=fallbackHole;
  else if(upper!==null)
    pageHole=Math.ceil(upper/4096)*4096*count-requested;
  else return 0;
  if(bucket?.bucketSize===null||bucket?.bucketSize===undefined)
    return pageHole;
  const classHole=bucket.bucketSize*count-requested;
  return Math.min(classHole,pageHole);
}}
function relationFlows(point,leftId,rightId){{
  const histogram=point?.liveHistogram;
  if(!histogram)return [];
  const grouped=new Map();
  for(let index=0;index<HM.labels.length;index++){{
    const count=Number(histogram.counts?.[index])||0;
    const requested=Number(histogram.requested?.[index])||0;
    if(count===0&&requested===0)continue;
    const upper=index<HM.classes.length?HM.classes[index]:null;
    const fallbackHole=histogram.fallbackHoles?.[index];
    const left=allocatorBucketForInterval(leftId,upper);
    const right=allocatorBucketForInterval(rightId,upper);
    if(!left||!right)continue;
    const key=left.rawLabel+"\\u0000"+right.rawLabel;
    let flow=grouped.get(key);
    if(!flow){{
      flow={{
        leftKey:left.rawLabel,rightKey:right.rawLabel,
        leftLabel:left.label,rightLabel:right.label,
        leftSize:left.bucketSize??Number.POSITIVE_INFINITY,
        rightSize:right.bucketSize??Number.POSITIVE_INFINITY,
        count:0,requested:0,holeLeft:0,holeRight:0,
      }};
      grouped.set(key,flow);
    }}
    flow.count+=count;
    flow.requested+=requested;
    flow.holeLeft+=intervalHole(
      left,count,requested,fallbackHole,upper);
    flow.holeRight+=intervalHole(
      right,count,requested,fallbackHole,upper);
  }}
  return [...grouped.values()].map(flow=>({{
    ...flow,delta:flow.holeRight-flow.holeLeft,
    impact:Math.abs(flow.holeRight-flow.holeLeft),
  }}));
}}
function validateRelationTotals(point){{
  for(const left of ALLOCATORS){{
    for(const right of ALLOCATORS){{
      if(left.id===right.id)continue;
      const flows=relationFlows(point,left.id,right.id);
      const leftHole=flows.reduce((sum,flow)=>sum+flow.holeLeft,0);
      const rightHole=flows.reduce((sum,flow)=>sum+flow.holeRight,0);
      if(leftHole!==allocatorHole(point,left.id)||
         rightHole!==allocatorHole(point,right.id)){{
        return `${{left.id}}→${{right.id}} 空洞汇总不一致`;
      }}
    }}
  }}
  return "";
}}
function relationWeight(flow,metric){{
  if(metric==="count")return flow.count;
  if(metric==="impact")return flow.impact;
  return flow.requested;
}}
function relationColor(delta){{
  return delta>0?"#ff6b7a":delta<0?"#52d6a0":"#70809c";
}}
function signedBytes(value){{
  return (value>0?"+":"")+bytes(value);
}}
function renderRelationSvg(flows,leftName,rightName,metric){{
  if(!flows.length){{
    relationChart.innerHTML=`<div class="empty">当前条件下没有非零关系</div>`;
    return;
  }}
  const leftTotals=new Map(),rightTotals=new Map();
  flows.forEach(flow=>{{
    const weight=relationWeight(flow,metric);
    const add=(map,key,label,size)=>{{
      const node=map.get(key)||{{key,label,size,total:0}};
      node.total+=weight;map.set(key,node);
    }};
    add(leftTotals,flow.leftKey,flow.leftLabel,flow.leftSize);
    add(rightTotals,flow.rightKey,flow.rightLabel,flow.rightSize);
  }});
  const sortNodes=map=>[...map.values()].sort((a,b)=>
    a.size-b.size||a.label.localeCompare(b.label));
  const leftNodes=sortNodes(leftTotals),rightNodes=sortNodes(rightTotals);
  const maxNodes=Math.max(leftNodes.length,rightNodes.length);
  const width=1200,height=Math.max(560,maxNodes*18+48);
  const top=24,bottom=24,gap=6,leftX=180,rightX=1008,nodeWidth=12;
  const total=flows.reduce((sum,flow)=>sum+relationWeight(flow,metric),0);
  const usable=Math.max(80,height-top-bottom-gap*Math.max(0,maxNodes-1));
  const scale=total?usable/total:1;
  function layout(nodes){{
    let y=top;const map=new Map();
    nodes.forEach(node=>{{
      node.y=y;node.height=Math.max(1,node.total*scale);
      node.offset=0;map.set(node.key,node);y+=node.height+gap;
    }});
    return map;
  }}
  const leftMap=layout(leftNodes),rightMap=layout(rightNodes);
  const metricName=metric==="count"?"存活数量":
    metric==="impact"?"空洞影响":"用户申请量";
  const metricFormat=metric==="count"?integer:bytes;
  let paths="";
  [...flows].sort((a,b)=>relationWeight(b,metric)-relationWeight(a,metric))
    .forEach(flow=>{{
      const weight=relationWeight(flow,metric);
      const source=leftMap.get(flow.leftKey),target=rightMap.get(flow.rightKey);
      const thickness=Math.max(1,weight*scale);
      const sy=source.y+source.offset+thickness/2;
      const ty=target.y+target.offset+thickness/2;
      source.offset+=thickness;target.offset+=thickness;
      const title=`${{leftName}} ${{flow.leftLabel}} → ${{rightName}} ${{
        flow.rightLabel}}；存活 ${{integer(flow.count)}} 个；申请 ${{
        bytes(flow.requested)}}；左侧空洞 ${{bytes(flow.holeLeft)}}；右侧空洞 ${{
        bytes(flow.holeRight)}}；右-左 ${{signedBytes(flow.delta)}}`;
      paths+=`<path d="M ${{leftX+nodeWidth}} ${{sy}} C 470 ${{sy}},718 ${{
        ty}},${{rightX}} ${{ty}}" fill="none" stroke="${{
        relationColor(flow.delta)}}" stroke-opacity=".48" stroke-width="${{
        thickness}}"><title>${{xml(title)}}</title></path>`;
    }});
  function nodeSvg(nodes,x,leftSide){{
    return nodes.map(node=>{{
      const labelX=leftSide?x-8:x+nodeWidth+8;
      const anchor=leftSide?"end":"start";
      const y=node.y+node.height/2;
      const title=`${{node.label}}；${{metricName}} ${{
        metricFormat(node.total)}}`;
      return `<g><rect x="${{x}}" y="${{node.y}}" width="${{nodeWidth}}"
        height="${{Math.max(2,node.height)}}" rx="2" fill="${{
        leftSide?"#ff9b57":"#a98bff"}}"><title>${{
        xml(title)}}</title></rect><text x="${{labelX}}" y="${{y+4}}"
        text-anchor="${{anchor}}" fill="#c9d5e8" font-size="11">${{
        xml(node.label)}}</text></g>`;
    }}).join("");
  }}
  relationChart.innerHTML=`<svg viewBox="0 0 ${{width}} ${{height}}"
    style="height:${{height}}px" role="img"
    aria-label="${{xml(leftName+" 与 "+rightName+" 的 Size Class 对应关系")}}">
    ${{paths}}${{nodeSvg(leftNodes,leftX,true)}}${{
      nodeSvg(rightNodes,rightX,false)}}</svg>`;
}}
function renderRelation(){{
  const leftId=relationLeft.value||"mini160";
  const rightId=relationRight.value||"jemalloc";
  const left=ALLOCATORS.find(item=>item.id===leftId);
  const right=ALLOCATORS.find(item=>item.id===rightId);
  if(!selectedPoint||!left||!right){{
    relationSummary.textContent="没有可用于分配器映射的时间点";
    relationChart.innerHTML=`<div class="empty">暂无对应关系数据</div>`;
    relationTable.querySelector("thead").innerHTML="";
    relationTable.querySelector("tbody").innerHTML="";
    return;
  }}
  const all=relationFlows(selectedPoint,leftId,rightId);
  const metric=relationMetric.value;
  const sorted=[...all].sort((a,b)=>
    relationWeight(b,metric)-relationWeight(a,metric));
  const limit=Number(relationLimit.value)||0;
  const visible=(limit?sorted.slice(0,limit):sorted)
    .filter(flow=>relationWeight(flow,metric)>0);
  const leftHole=all.reduce((sum,flow)=>sum+flow.holeLeft,0);
  const rightHole=all.reduce((sum,flow)=>sum+flow.holeRight,0);
  const totalWeight=sorted.reduce((sum,flow)=>sum+relationWeight(flow,metric),0);
  const shownWeight=visible.reduce(
    (sum,flow)=>sum+relationWeight(flow,metric),0);
  relationSummary.innerHTML=`<b>${{xml(left.name)}}</b> 空洞 ${{bytes(leftHole)}} →
    <b>${{xml(right.name)}}</b> 空洞 ${{bytes(rightHole)}}；右侧变化
    <b style="color:${{relationColor(rightHole-leftHole)}}">${{
      signedBytes(rightHole-leftHole)}}</b>。图中显示 ${{visible.length}} / ${{
      all.length}} 组关系，覆盖当前指标的 ${{pct(totalWeight?
      shownWeight/totalWeight:0)}}。`;
  renderRelationSvg(visible,left.name,right.name,metric);
  relationTable.querySelector("thead").innerHTML=
    `<tr><th>${{xml(left.name)}} 桶</th><th>${{xml(right.name)}} 桶</th>
     <th>存活数量</th><th>用户申请量</th><th>左侧空洞</th>
     <th>右侧空洞</th><th>右-左</th></tr>`;
  relationTable.querySelector("tbody").innerHTML=sorted.map(flow=>
    `<tr><td>${{flow.leftLabel}}</td><td>${{flow.rightLabel}}</td>
     <td>${{integer(flow.count)}}</td><td>${{bytes(flow.requested)}}</td>
     <td>${{bytes(flow.holeLeft)}}</td><td>${{bytes(flow.holeRight)}}</td>
     <td style="color:${{relationColor(flow.delta)}}">${{
       signedBytes(flow.delta)}}</td></tr>`
  ).join("")||`<tr><td colspan="7" class="empty">没有关系数据</td></tr>`;
}}
function selectBucketPoint(index){{
  if(!P.length){{
    selectedPoint=null;
    bucketRows=rowsAtPoint(null);
    bucketMoment.textContent="CSV 目前只有表头，没有可查看的时间点";
    renderAllocatorComparison(null);
    renderSortedBuckets();
    renderSizePies();
    renderRelation();
    renderPeriod(null);
    chartDrawers.allocRate?.();
    return;
  }}
  index=Math.max(0,Math.min(P.length-1,Number(index)||0));
  selectedTimeIndex=index;
  bucketTime.value=String(index);
  allocTime.value=String(index);
  const point=P[index];
  selectedPoint=point;
  const relationError=validateRelationTotals(point);
  document.documentElement.dataset.relationCheck=relationError||"ok";
  if(relationError)console.error(relationError);
  bucketRows=rowsAtPoint(point);
  bucketMoment.textContent=timeNs(point.endNs)+" · 当前活着的分配 "+
    integer(point.liveAlloc)+" 个 · 原始请求 "+bytes(point.liveRequested);
  renderAllocatorComparison(point);
  renderSortedBuckets();
  renderSizePies();
  renderRelation();
  renderPeriod(point);
  chartDrawers.allocRate?.();
}}
bucketTime.addEventListener("input",event=>
  selectBucketPoint(event.target.value));
allocTime.addEventListener("input",event=>
  selectBucketPoint(event.target.value));
const relationOptions=ALLOCATORS.map(allocator=>
  `<option value="${{allocator.id}}">${{xml(allocator.name)}}</option>`).join("");
relationLeft.innerHTML=relationOptions;
relationRight.innerHTML=relationOptions;
relationLeft.value="mini160";
relationRight.value=ALLOCATORS.some(item=>item.id==="jemalloc")?
  "jemalloc":ALLOCATORS[Math.min(1,ALLOCATORS.length-1)].id;
function changeRelation(changed){{
  if(relationLeft.value===relationRight.value){{
    const current=ALLOCATORS.findIndex(item=>
      item.id===relationLeft.value);
    const replacement=ALLOCATORS[(current+1)%ALLOCATORS.length].id;
    if(changed==="left")relationRight.value=replacement;
    else relationLeft.value=replacement;
  }}
  renderRelation();
}}
relationLeft.addEventListener("change",()=>changeRelation("left"));
relationRight.addEventListener("change",()=>changeRelation("right"));
relationMetric.addEventListener("change",renderRelation);
relationLimit.addEventListener("change",renderRelation);
renderAllocatorSwitch();
selectBucketPoint(bucketTime.value);

const timeTable=document.getElementById("timeTable");
timeTable.querySelector("thead").innerHTML=
  `<tr><th>结束时间</th><th>存活分配</th><th>存活请求</th>
   ${{ALLOCATORS.map(item=>`<th>${{
     xml(item.name)}} 空洞</th>`).join("")}}</tr>`;
timeTable.querySelector("tbody").innerHTML=P.slice(-20).reverse().map(p=>
  `<tr><td>${{timeNs(p.endNs)}}</td><td>${{integer(p.liveAlloc)}}</td>
   <td>${{bytes(p.liveRequested)}}</td>${{
     ALLOCATORS.map(item=>`<td>${{
       bytes(allocatorHole(p,item.id))}}</td>`).join("")
   }}</tr>`
).join("") || `<tr><td colspan="${{3+ALLOCATORS.length}}" class="empty">
  CSV 目前只有表头，没有周期数据</td></tr>`;
document.getElementById("footer").textContent=
  `时间范围：${{timeNs(S.start_ns)}} — ${{timeNs(S.end_ns)}} · `+
  `原始数据行 ${{S.row_count}} · 图表点 ${{P.length}} · `+
  `跳过异常行 ${{S.malformed_rows}} · `+
  `滑块可查看 ${{P.length}} 个保留时间点 · `+
  `${{ALLOCATORS.length}}套规则由177个公共存活桶后处理 · `+
  `规则空洞已剔除未使用的完整4KiB页 · `+
  `存活口径：进程启动后新分配，继承内存不计`+
  (liveEstimated?" · 采样估算 1/"+D.metadata.live_sample_rate:"");
</script>
</body>
</html>
"""


def main() -> int:
    parser = argparse.ArgumentParser(
        description="把 mini malloc hole CSV 转成自包含 HTML 仪表盘"
    )
    parser.add_argument("csv_path", type=Path, help="输入 CSV")
    parser.add_argument(
        "-o", "--output", type=Path,
        help="输出 HTML；默认与 CSV 同目录同名",
    )
    parser.add_argument(
        "--max-points", type=int, default=1500,
        help="图表和桶明细滑块最多保留的聚合点数（默认 1500）",
    )
    parser.add_argument(
        "--pid", type=int,
        help="共享 CSV 只分析指定 PID",
    )
    parser.add_argument(
        "--title", default="Mini 内存空洞分析",
        help="报告标题",
    )
    parser.add_argument(
        "--summary-json", type=Path,
        help="可选：同时输出机器可读汇总 JSON",
    )
    parser.add_argument(
        "--custom-rule", action="append", default=[],
        metavar="名称=8|16|24|...",
        help=(
            "增加一套自定义规则；可重复使用，边界必须已存在于"
            " CSV 公共桶中"
        ),
    )
    parser.add_argument(
        "--custom-rule-json", action="append", default=[],
        type=Path, metavar="FILE",
        help=(
            "加载规则 JSON；支持 dfmalloc2 优化器输出、单条"
            " {name,size_classes} 或 {rules:[...]}，可重复使用"
        ),
    )
    args = parser.parse_args()

    path = args.csv_path.resolve()
    if not path.is_file():
        parser.error(f"CSV 不存在：{path}")
    if args.max_points < 50:
        parser.error("--max-points 必须 >= 50")

    metadata, header = read_preamble(path)
    if args.pid is not None and "pid" not in header:
        metadata_pid = metadata.get("pid")
        if metadata_pid != str(args.pid):
            parser.error(
                f"该单 PID CSV 的 PID 是 {metadata_pid}，不是 {args.pid}"
            )
    try:
        custom_rules = collect_custom_rules(
            args.custom_rule, args.custom_rule_json
        )
        live_model = build_live_model(
            metadata, header, custom_rules
        )
    except ValueError as error:
        parser.error(str(error))
    buckets = discover_buckets(header)
    summary = first_pass(
        path, metadata, header, buckets, args.pid, live_model
    )
    points = aggregate_points(
        path, header, buckets, args.pid,
        int(summary["row_count"]), args.max_points, live_model,
    )
    bucket_rows = make_allocator_bucket_rows(live_model)

    output = (
        args.output.resolve()
        if args.output
        else path.with_suffix(".html")
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    report = build_html(
        path, args.title, metadata, summary, points, bucket_rows,
        live_model,
    )
    output.write_text(report, encoding="utf-8")

    if args.summary_json:
        summary_path = args.summary_json.resolve()
        summary_path.parent.mkdir(parents=True, exist_ok=True)
        summary_path.write_text(
            json.dumps(
                {
                    "source": str(path),
                    "metadata": metadata,
                    "summary": summary,
                    "allocator_info": live_model["allocator_info"],
                    "buckets": bucket_rows,
                },
                ensure_ascii=False,
                indent=2,
            ) + "\n",
            encoding="utf-8",
        )

    totals = summary["totals"]
    print(f"报告：{output}")
    print(
        "汇总："
        f"alloc={totals['period_alloc']} "
        f"requested={totals['period_requested']} "
        f"hole={totals['period_hole']} "
        f"hole_ratio={summary['hole_ratio'] * 100:.4f}% "
        f"rows={summary['row_count']} points={len(points)}"
    )
    if summary["live_supported"]:
        live = summary["live"]
        print(
            "存活："
            f"alloc={live['allocations']} "
            f"requested={live['requested_bytes']} "
            f"allocated={live['allocated_bytes']} "
            f"hole={live['hole_bytes']} "
            f"hole_ratio={live['hole_ratio'] * 100:.4f}% "
            f"untracked_free={live['total_untracked_free']} "
            f"track_failed={live['total_tracking_failures']}"
        )
        allocator_text = []
        for allocator in live["allocators"]:
            hole = allocator["hole_bytes"]
            allocated = live["requested_bytes"] + hole
            ratio = hole / allocated * 100 if allocated else 0.0
            allocator_text.append(
                f"{allocator['id']}={hole}({ratio:.4f}%)"
            )
        print("规则对比：" + " ".join(allocator_text))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
