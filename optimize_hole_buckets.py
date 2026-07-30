#!/usr/bin/env python3
"""Find size-class rules that minimize live internal fragmentation.

The optimizer consumes mini_malloc_hole_v11 CSV snapshots. It treats the
176 common histogram intervals as indivisible observations, selects at most
K interval upper bounds as explicit size classes, and keeps 4 KiB fallback
above --max-explicit-size.
"""

from __future__ import annotations

import argparse
from bisect import bisect_right
import csv
from dataclasses import dataclass
import itertools
import json
import math
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple


FORMAT = "mini_malloc_hole_v11"
DEFAULT_BUCKET_COUNTS = "32,48,64,80,96,128,160"
PAGE_SIZE = 4096


@dataclass
class FileAggregate:
    path: Path
    histogram_classes: List[int]
    labels: List[str]
    counts: List[float]
    requested: List[float]
    fallback_holes: List[float]
    live_requested: float
    mini_hole: float
    duration_ns: float
    rows: int
    malformed_rows: int
    tracking_failures: int
    estimated: bool


@dataclass
class Dataset:
    paths: List[Path]
    histogram_classes: List[int]
    labels: List[str]
    counts: List[float]
    requested: List[float]
    fallback_holes: List[float]
    live_requested: float
    mini_hole: float
    rows: int
    malformed_rows: int
    weighting: str
    tracking_failures: int
    estimated_files: int


@dataclass
class Candidate:
    bucket_budget: int
    size_classes: List[int]
    train_hole: float
    validation_hole: Optional[float] = None


def parse_metadata(line: str) -> Dict[str, str]:
    parts = [part.strip() for part in line.strip().split(",")]
    metadata = {"format": parts[0].lstrip("#") if parts else ""}
    for part in parts[1:]:
        if "=" in part:
            name, value = part.split("=", 1)
            metadata[name] = value
    return metadata


def parse_class_list(metadata: Dict[str, str], name: str) -> List[int]:
    text = metadata.get(name)
    if not text:
        raise ValueError(f"CSV metadata is missing {name}")
    try:
        values = [
            int(item)
            for item in text.split("|")
            if item and item != "4K+"
        ]
    except ValueError as error:
        raise ValueError(f"invalid {name} metadata") from error
    if values != sorted(set(values)):
        raise ValueError(f"{name} must be strictly increasing")
    return values


def align_up(value: int, alignment: int = PAGE_SIZE) -> int:
    return (value + alignment - 1) // alignment * alignment


def histogram_labels(classes: Sequence[int]) -> List[str]:
    return [str(value) for value in classes] + [
        f"{classes[-1]}_plus"
    ]


def _parse_row(
    header: Sequence[str], values: Sequence[str]
) -> Optional[Dict[str, int]]:
    if len(values) != len(header):
        return None
    try:
        return {
            name: int(value.strip() or "0")
            for name, value in zip(header, values)
        }
    except ValueError:
        return None


def read_file(
    path: Path, last_rows: Optional[int] = None
) -> FileAggregate:
    with path.open("r", encoding="utf-8", newline="") as source:
        metadata_line = source.readline()
        header_line = source.readline()
        if not metadata_line or not header_line:
            raise ValueError(f"{path}: missing metadata or header")
        metadata = parse_metadata(metadata_line)
        if metadata.get("format") != FORMAT:
            raise ValueError(
                f"{path}: expected {FORMAT}, got "
                f"{metadata.get('format', 'unknown')}"
            )
        classes = parse_class_list(
            metadata, "live_histogram_classes"
        )
        labels = histogram_labels(classes)
        header = next(csv.reader([header_line]))
        fields = set(header)
        required = {
            "start_ns", "end_ns", "live_requested", "live_hole"
        }
        for label in labels:
            required.add(f"live_hist_count_{label}")
            required.add(f"live_hist_requested_{label}")
        missing = sorted(required.difference(fields))
        if missing:
            raise ValueError(
                f"{path}: missing fields: {', '.join(missing)}"
            )

        large_start = bisect_right(classes, 16384)
        for index, label in enumerate(labels):
            if index >= large_start:
                field = f"live_hist_4k_hole_{label}"
                if field not in fields:
                    raise ValueError(f"{path}: missing field {field}")

        rows: List[Tuple[float, Dict[str, int]]] = []
        malformed = 0
        for values in csv.reader(source):
            if not values or all(not value.strip() for value in values):
                continue
            row = _parse_row(header, values)
            if row is None:
                malformed += 1
                continue
            duration = max(1, row["end_ns"] - row["start_ns"])
            rows.append((float(duration), row))

    if not rows:
        raise ValueError(f"{path}: no usable data rows")
    if last_rows is not None:
        if last_rows <= 0:
            raise ValueError("last_rows must be positive")
        rows = rows[-last_rows:]

    counts = [0.0] * len(labels)
    requested = [0.0] * len(labels)
    fallback_holes = [0.0] * len(labels)
    live_requested = 0.0
    mini_hole = 0.0
    duration_ns = 0.0
    tracking_failures = 0

    for duration, row in rows:
        duration_ns += duration
        live_requested += row["live_requested"] * duration
        mini_hole += row["live_hole"] * duration
        tracking_failures = max(
            tracking_failures,
            row.get("total_live_track_failed", 0),
        )
        lower = 0
        for index, label in enumerate(labels):
            count = row[f"live_hist_count_{label}"]
            request_sum = row[f"live_hist_requested_{label}"]
            counts[index] += count * duration
            requested[index] += request_sum * duration
            fallback_field = f"live_hist_4k_hole_{label}"
            if fallback_field in row:
                fallback = row[fallback_field]
            else:
                upper = (
                    classes[index]
                    if index < len(classes) else None
                )
                if upper is None:
                    raise ValueError(
                        f"{path}: tail bucket has no 4K hole"
                    )
                first_rounded = align_up(max(1, lower + 1))
                last_rounded = align_up(upper)
                if first_rounded != last_rounded:
                    raise ValueError(
                        f"{path}: common bucket ({lower}, {upper}] "
                        "crosses a 4K boundary without a fallback field"
                    )
                fallback = count * last_rounded - request_sum
            fallback_holes[index] += fallback * duration
            if index < len(classes):
                lower = classes[index]

    return FileAggregate(
        path=path,
        histogram_classes=classes,
        labels=labels,
        counts=counts,
        requested=requested,
        fallback_holes=fallback_holes,
        live_requested=live_requested,
        mini_hole=mini_hole,
        duration_ns=duration_ns,
        rows=len(rows),
        malformed_rows=malformed,
        tracking_failures=tracking_failures,
        estimated=metadata.get("live_values") == "estimated",
    )


def combine_files(
    paths: Sequence[Path],
    weighting: str,
    last_rows: Optional[int] = None,
) -> Dataset:
    files = [
        read_file(path, last_rows=last_rows) for path in paths
    ]
    reference = files[0].histogram_classes
    for item in files[1:]:
        if item.histogram_classes != reference:
            raise ValueError(
                f"{item.path}: common histogram differs from {files[0].path}"
            )

    size = len(files[0].labels)
    counts = [0.0] * size
    requested = [0.0] * size
    fallback_holes = [0.0] * size
    live_requested = 0.0
    mini_hole = 0.0

    if weighting == "equal-file":
        scales = [
            1.0 / item.duration_ns / len(files)
            for item in files
        ]
    elif weighting == "duration":
        total_duration = sum(item.duration_ns for item in files)
        scales = [1.0 / total_duration] * len(files)
    else:
        raise ValueError(f"unsupported weighting: {weighting}")

    for item, scale in zip(files, scales):
        for index in range(size):
            counts[index] += item.counts[index] * scale
            requested[index] += item.requested[index] * scale
            fallback_holes[index] += (
                item.fallback_holes[index] * scale
            )
        live_requested += item.live_requested * scale
        mini_hole += item.mini_hole * scale

    return Dataset(
        paths=[item.path for item in files],
        histogram_classes=list(reference),
        labels=list(files[0].labels),
        counts=counts,
        requested=requested,
        fallback_holes=fallback_holes,
        live_requested=live_requested,
        mini_hole=mini_hole,
        rows=sum(item.rows for item in files),
        malformed_rows=sum(item.malformed_rows for item in files),
        weighting=weighting,
        tracking_failures=sum(
            item.tracking_failures for item in files
        ),
        estimated_files=sum(item.estimated for item in files),
    )


def _group_cost(
    start: int,
    end: int,
    boundaries: Sequence[int],
    prefix_counts: Sequence[float],
    prefix_requested: Sequence[float],
) -> float:
    count = prefix_counts[end + 1] - prefix_counts[start]
    requested = (
        prefix_requested[end + 1] - prefix_requested[start]
    )
    return boundaries[end] * count - requested


def optimize_classes(
    boundaries: Sequence[int],
    counts: Sequence[float],
    requested: Sequence[float],
    bucket_counts: Sequence[int],
) -> Dict[int, Tuple[float, List[int]]]:
    """Return exact minimum explicit-region hole for every requested K."""
    size = len(boundaries)
    if not (
        len(counts) == size and len(requested) == size
    ):
        raise ValueError("boundary and histogram lengths differ")
    wanted = sorted({
        value for value in bucket_counts if 1 <= value <= size
    })
    if not wanted:
        raise ValueError("no bucket count fits the optimization range")

    prefix_counts = [0.0]
    prefix_requested = [0.0]
    for count, request_sum in zip(counts, requested):
        prefix_counts.append(prefix_counts[-1] + count)
        prefix_requested.append(
            prefix_requested[-1] + request_sum
        )

    maximum = max(wanted)
    infinity = float("inf")
    previous = [infinity] * (size + 1)
    previous[0] = 0.0
    back = [[-1] * (size + 1) for _ in range(maximum + 1)]
    costs: Dict[int, float] = {}

    for bucket_count in range(1, maximum + 1):
        current = [infinity] * (size + 1)
        for end in range(bucket_count, size + 1):
            best_cost = infinity
            best_start = -1
            for start in range(bucket_count - 1, end):
                prefix_cost = previous[start]
                if not math.isfinite(prefix_cost):
                    continue
                cost = prefix_cost + _group_cost(
                    start,
                    end - 1,
                    boundaries,
                    prefix_counts,
                    prefix_requested,
                )
                if cost < best_cost:
                    best_cost = cost
                    best_start = start
            current[end] = best_cost
            back[bucket_count][end] = best_start
        previous = current
        if bucket_count in wanted:
            costs[bucket_count] = current[size]

    result = {}
    for bucket_count in wanted:
        end = size
        selected = []
        for level in range(bucket_count, 0, -1):
            start = back[level][end]
            if start < 0:
                raise RuntimeError("failed to reconstruct size classes")
            selected.append(boundaries[end - 1])
            end = start
        selected.reverse()
        result[bucket_count] = (costs[bucket_count], selected)
    return result


def fallback_cost(dataset: Dataset, explicit_count: int) -> float:
    return sum(dataset.fallback_holes[explicit_count:])


def evaluate_rule(
    dataset: Dataset,
    size_classes: Sequence[int],
    cutoff: int,
) -> float:
    explicit_count = bisect_right(
        dataset.histogram_classes, cutoff
    )
    if not size_classes or size_classes[-1] != cutoff:
        raise ValueError("candidate rule must end at the cutoff")
    hole = 0.0
    class_index = 0
    for index in range(explicit_count):
        upper = dataset.histogram_classes[index]
        while size_classes[class_index] < upper:
            class_index += 1
        hole += (
            size_classes[class_index] * dataset.counts[index]
            - dataset.requested[index]
        )
    return hole + fallback_cost(dataset, explicit_count)


def memory_saving_percent(
    baseline_requested: float,
    baseline_hole: float,
    candidate_hole: float,
) -> float:
    baseline_allocated = baseline_requested + baseline_hole
    if baseline_allocated == 0:
        return 0.0
    return (
        (baseline_hole - candidate_hole)
        / baseline_allocated * 100.0
    )


def hole_reduction_percent(
    baseline_hole: float, candidate_hole: float
) -> float:
    if baseline_hole == 0:
        return 0.0
    return (
        (baseline_hole - candidate_hole)
        / baseline_hole * 100.0
    )


def choose_knee(candidates: Sequence[Candidate]) -> Candidate:
    if len(candidates) <= 2:
        return candidates[-1]
    first = candidates[0]
    last = candidates[-1]
    class_span = last.bucket_budget - first.bucket_budget
    gain_span = first.train_hole - last.train_hole
    if class_span <= 0 or gain_span <= 0:
        return first
    best = first
    best_score = -float("inf")
    for candidate in candidates:
        x = (
            (candidate.bucket_budget - first.bucket_budget)
            / class_span
        )
        y = (
            (first.train_hole - candidate.train_hole)
            / gain_span
        )
        score = y - x
        if score > best_score:
            best_score = score
            best = candidate
    return best


def human_bytes(value: float) -> str:
    units = ["B", "KiB", "MiB", "GiB", "TiB"]
    magnitude = abs(value)
    unit = 0
    while magnitude >= 1024 and unit < len(units) - 1:
        magnitude /= 1024
        unit += 1
    if value < 0:
        magnitude = -magnitude
    return f"{magnitude:.2f} {units[unit]}"


def parse_bucket_counts(text: str) -> List[int]:
    try:
        values = sorted({
            int(item.strip())
            for item in text.split(",") if item.strip()
        })
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            "bucket counts must be comma-separated integers"
        ) from error
    if not values or values[0] <= 0:
        raise argparse.ArgumentTypeError(
            "bucket counts must be positive"
        )
    return values


def build_report(
    train: Dataset,
    validation: Optional[Dataset],
    cutoff: int,
    bucket_counts: Sequence[int],
) -> Tuple[List[Candidate], Candidate, Dict[str, object]]:
    explicit_count = bisect_right(
        train.histogram_classes, cutoff
    )
    if (
        explicit_count == 0
        or train.histogram_classes[explicit_count - 1] != cutoff
    ):
        raise ValueError(
            "--max-explicit-size must be a common histogram boundary"
        )
    if validation is not None:
        if validation.histogram_classes != train.histogram_classes:
            raise ValueError(
                "training and validation histograms differ"
            )

    fitted = optimize_classes(
        train.histogram_classes[:explicit_count],
        train.counts[:explicit_count],
        train.requested[:explicit_count],
        bucket_counts,
    )
    tail = fallback_cost(train, explicit_count)
    candidates = []
    for bucket_budget in sorted(fitted):
        explicit_hole, size_classes = fitted[bucket_budget]
        candidate = Candidate(
            bucket_budget=bucket_budget,
            size_classes=size_classes,
            train_hole=explicit_hole + tail,
        )
        if validation is not None:
            candidate.validation_hole = evaluate_rule(
                validation, size_classes, cutoff
            )
        candidates.append(candidate)

    recommended = choose_knee(candidates)
    curve = []
    for candidate in candidates:
        item = {
            "bucket_budget": candidate.bucket_budget,
            "size_classes": candidate.size_classes,
            "train_hole_bytes": candidate.train_hole,
            "train_hole_rate": (
                candidate.train_hole
                / (train.live_requested + candidate.train_hole)
                if train.live_requested + candidate.train_hole else 0.0
            ),
            "train_memory_saving_percent": memory_saving_percent(
                train.live_requested,
                train.mini_hole,
                candidate.train_hole,
            ),
            "train_hole_reduction_percent": hole_reduction_percent(
                train.mini_hole, candidate.train_hole
            ),
        }
        if validation is not None:
            item.update({
                "validation_hole_bytes":
                    candidate.validation_hole,
                "validation_hole_rate": (
                    candidate.validation_hole
                    / (
                        validation.live_requested
                        + candidate.validation_hole
                    )
                    if (
                        validation.live_requested
                        + candidate.validation_hole
                    ) else 0.0
                ),
                "validation_memory_saving_percent":
                    memory_saving_percent(
                        validation.live_requested,
                        validation.mini_hole,
                        candidate.validation_hole,
                    ),
                "validation_hole_reduction_percent":
                    hole_reduction_percent(
                        validation.mini_hole,
                        candidate.validation_hole,
                    ),
            })
        curve.append(item)

    report = {
        "format": "mini_hole_bucket_optimizer_v1",
        "objective": "average_live_internal_fragmentation",
        "weighting": train.weighting,
        "max_explicit_size": cutoff,
        "fallback": "4K",
        "train": {
            "files": [str(path) for path in train.paths],
            "rows": train.rows,
            "malformed_rows": train.malformed_rows,
            "tracking_failures": train.tracking_failures,
            "estimated_files": train.estimated_files,
            "average_live_requested_bytes": train.live_requested,
            "average_mini160_hole_bytes": train.mini_hole,
        },
        "validation": (
            {
                "files": [str(path) for path in validation.paths],
                "rows": validation.rows,
                "malformed_rows": validation.malformed_rows,
                "tracking_failures":
                    validation.tracking_failures,
                "estimated_files": validation.estimated_files,
                "average_live_requested_bytes":
                    validation.live_requested,
                "average_mini160_hole_bytes":
                    validation.mini_hole,
            }
            if validation is not None else None
        ),
        "curve": curve,
        "recommended_bucket_budget":
            recommended.bucket_budget,
        "recommended_size_classes":
            recommended.size_classes,
        "recommendation_method":
            "maximum normalized elbow distance",
    }
    return candidates, recommended, report


def print_report(
    train: Dataset,
    validation: Optional[Dataset],
    candidates: Sequence[Candidate],
    recommended: Candidate,
) -> None:
    print(
        f"训练集：{len(train.paths)}个CSV，{train.rows}行，"
        f"权重={train.weighting}"
    )
    print(
        "Mini160基线："
        f"requested={human_bytes(train.live_requested)} "
        f"hole={human_bytes(train.mini_hole)} "
        f"hole_rate="
        f"{train.mini_hole / (train.live_requested + train.mini_hole) * 100 if train.live_requested + train.mini_hole else 0:.4f}%"
    )
    if train.tracking_failures:
        print(
            "警告：训练CSV累计报告"
            f"{train.tracking_failures}次存活表跟踪失败，"
            "优化结果可能低估未跟踪分配。"
        )
    if train.estimated_files:
        print(
            f"提示：训练集中有{train.estimated_files}个采样估算CSV，"
            "冷门尺寸的收益波动可能较大。"
        )
    if validation is None:
        print(
            "\n桶预算  候选空洞       空洞率      占用收益    空洞下降"
        )
    else:
        print(
            f"验证集：{len(validation.paths)}个CSV，"
            f"{validation.rows}行"
        )
        print(
            "\n桶预算  训练占用收益  验证占用收益  "
            "训练空洞下降  验证空洞下降"
        )

    for candidate in candidates:
        train_memory = memory_saving_percent(
            train.live_requested,
            train.mini_hole,
            candidate.train_hole,
        )
        train_reduction = hole_reduction_percent(
            train.mini_hole, candidate.train_hole
        )
        marker = "  <- 拐点" if candidate is recommended else ""
        if validation is None:
            allocated = train.live_requested + candidate.train_hole
            rate = (
                candidate.train_hole / allocated * 100
                if allocated else 0.0
            )
            print(
                f"{candidate.bucket_budget:>6}  "
                f"{human_bytes(candidate.train_hole):>12}  "
                f"{rate:>8.4f}%  "
                f"{train_memory:>8.4f}%  "
                f"{train_reduction:>8.2f}%{marker}"
            )
        else:
            validation_memory = memory_saving_percent(
                validation.live_requested,
                validation.mini_hole,
                candidate.validation_hole,
            )
            validation_reduction = hole_reduction_percent(
                validation.mini_hole,
                candidate.validation_hole,
            )
            print(
                f"{candidate.bucket_budget:>6}  "
                f"{train_memory:>10.4f}%  "
                f"{validation_memory:>12.4f}%  "
                f"{train_reduction:>12.2f}%  "
                f"{validation_reduction:>12.2f}%{marker}"
            )

    print(
        f"\n推荐桶预算：{recommended.bucket_budget}"
        "（数学拐点，仅供候选筛选）"
    )
    print(
        "Size Classes："
        + "|".join(str(value) for value in recommended.size_classes)
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "从一个或多个v11 CSV中寻找低空洞Size Class规则"
        )
    )
    parser.add_argument(
        "csv_paths", type=Path, nargs="+", help="训练CSV"
    )
    parser.add_argument(
        "--validation", type=Path, nargs="+",
        help="可选：独立验证CSV，只评估、不参与规则拟合",
    )
    parser.add_argument(
        "--bucket-counts",
        type=parse_bucket_counts,
        default=parse_bucket_counts(DEFAULT_BUCKET_COUNTS),
        help=f"候选桶预算，默认 {DEFAULT_BUCKET_COUNTS}",
    )
    parser.add_argument(
        "--max-explicit-size", type=int, default=16384,
        help="最后一个显式Size Class，之后按4K对齐（默认16384）",
    )
    parser.add_argument(
        "--weighting",
        choices=("equal-file", "duration"),
        default="equal-file",
        help=(
            "多CSV权重：每个文件等权，或所有时间等权"
            "（默认equal-file）"
        ),
    )
    parser.add_argument(
        "--output-json", type=Path,
        help="可选：写入完整曲线和候选规则JSON",
    )
    args = parser.parse_args()

    missing = [
        path for path in itertools.chain(
            args.csv_paths, args.validation or []
        ) if not path.is_file()
    ]
    if missing:
        parser.error(
            "CSV不存在：" + ", ".join(str(path) for path in missing)
        )

    try:
        train = combine_files(args.csv_paths, args.weighting)
        validation = (
            combine_files(args.validation, args.weighting)
            if args.validation else None
        )
        candidates, recommended, report = build_report(
            train,
            validation,
            args.max_explicit_size,
            args.bucket_counts,
        )
    except (ValueError, RuntimeError) as error:
        parser.error(str(error))

    print_report(train, validation, candidates, recommended)
    if args.output_json:
        output = args.output_json.resolve()
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        print(f"JSON：{output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
