#!/usr/bin/env python3
"""Compare segmented dfmalloc2.0 size-class optimization schemes."""

from __future__ import annotations

import argparse
from bisect import bisect_left, bisect_right
import csv
from dataclasses import dataclass
import itertools
import json
from pathlib import Path
from typing import Callable, Dict, List, Optional, Sequence, Tuple

from optimize_hole_buckets import (
    Dataset,
    combine_files,
    hole_reduction_percent,
    human_bytes,
    memory_saving_percent,
    parse_class_list,
    parse_metadata,
)


LARGE_STEP_START = 1280
OPTIMIZE_LIMIT = 14336
MIN_SMALL_STEP = 8
MIN_LARGE_STEP = 256
STEP_QUANTUM = 8
MIN_RELATIVE_STEP_PERCENT = 8

DETAIL_RULES = (
    ("dfmalloc1.0", "dfmalloc1_classes"),
    ("dfmalloc2.0", "dfmalloc2_classes"),
    ("jemalloc", "jemalloc_classes"),
)


@dataclass
class Candidate:
    extra_buckets: int
    classes: List[int]
    train_hole: float
    validation_hole: Optional[float] = None
    scheme: Optional["Scheme"] = None

    @property
    def display_name(self) -> str:
        if self.scheme is not None:
            return f"方案{self.scheme.number}"
        return f"新规则 +{self.extra_buckets}桶"

    @property
    def file_suffix(self) -> str:
        if self.scheme is not None:
            return f"scheme{self.scheme.number:02d}"
        return f"plus{self.extra_buckets}"


@dataclass(frozen=True)
class Scheme:
    number: int
    large_alignment: int
    small_extra_buckets: int
    large_extra_buckets: int


DEFAULT_SCHEMES = tuple(
    Scheme(number, alignment, small_extra, large_extra)
    for number, (alignment, small_extra, large_extra) in enumerate(
        (
            (512, 0, 0),
            (512, 1, 0),
            (512, 2, 0),
            (512, 0, 1),
            (512, 0, 2),
            (256, 0, 0),
            (256, 1, 0),
            (256, 2, 0),
            (256, 0, 1),
            (256, 0, 2),
        ),
        start=1,
    )
)


def parse_nonnegative_list(text: str) -> List[int]:
    try:
        values = sorted({
            int(item.strip())
            for item in text.split(",") if item.strip()
        })
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            "新增桶数量必须是逗号分隔的整数"
        ) from error
    if not values or values[0] < 0:
        raise argparse.ArgumentTypeError("新增桶数量不能为负数")
    return values


def read_allocator_classes(
    paths: Sequence[Path],
) -> Dict[str, List[int]]:
    reference: Optional[Dict[str, List[int]]] = None
    for path in paths:
        with path.open("r", encoding="utf-8") as source:
            metadata = parse_metadata(source.readline())
        classes = {
            display_name: parse_class_list(metadata, metadata_name)
            for display_name, metadata_name in DETAIL_RULES
        }
        if reference is None:
            reference = classes
        elif classes != reference:
            raise ValueError(
                f"{path}: 分配器规则与其他CSV不一致"
            )
    if reference is None:
        raise ValueError("没有CSV")
    if reference["dfmalloc2.0"][-1] != OPTIMIZE_LIMIT:
        raise ValueError(
            f"dfmalloc2最后一个显式边界必须是{OPTIMIZE_LIMIT}"
        )
    return reference


def read_dfmalloc2_classes(paths: Sequence[Path]) -> List[int]:
    """Read only the baseline rule for compatibility with older callers."""
    reference = None
    for path in paths:
        with path.open("r", encoding="utf-8") as source:
            metadata = parse_metadata(source.readline())
        classes = parse_class_list(metadata, "dfmalloc2_classes")
        if reference is None:
            reference = classes
        elif classes != reference:
            raise ValueError(
                f"{path}: dfmalloc2_classes与其他CSV不一致"
            )
    if reference is None:
        raise ValueError("没有CSV")
    if reference[-1] != OPTIMIZE_LIMIT:
        raise ValueError(
            f"dfmalloc2最后一个显式边界必须是{OPTIMIZE_LIMIT}"
        )
    return reference


def validate_baseline_rule(classes: Sequence[int]) -> None:
    if LARGE_STEP_START not in classes:
        raise ValueError(
            f"dfmalloc2原始规则必须包含分界桶{LARGE_STEP_START}"
        )
    if not classes or classes[-1] != OPTIMIZE_LIMIT:
        raise ValueError(
            f"dfmalloc2原始规则必须结束于{OPTIMIZE_LIMIT}"
        )
    previous = 0
    for value in classes:
        step = value - previous
        if step % STEP_QUANTUM != 0:
            raise ValueError(
                f"原始规则步长{step}不是{STEP_QUANTUM}的倍数"
            )
        previous = value


def valid_step(lower: int, upper: int) -> bool:
    step = upper - lower
    minimum_step = (
        MIN_LARGE_STEP
        if upper > LARGE_STEP_START else MIN_SMALL_STEP
    )
    return (
        step >= minimum_step
        and step % STEP_QUANTUM == 0
        and step * 100 > upper * MIN_RELATIVE_STEP_PERCENT
    )


def select_boundaries(
    candidates: Sequence[int],
    end: int,
    class_count: int,
    group_cost: Callable[[int, int], float],
    start: int = 0,
) -> Tuple[float, List[int]]:
    """Choose exactly ``class_count`` endpoints in ``(start, end]``."""
    values = [
        value for value in candidates
        if start < value <= end
    ]
    if not values or values[-1] != end:
        raise ValueError("候选边界必须包含优化终点")
    if class_count < 1 or class_count > len(values):
        raise ValueError("请求的桶数量超出候选边界范围")

    states: Dict[int, Tuple[float, List[int]]] = {}
    for index, value in enumerate(values):
        if not valid_step(start, value):
            continue
        if class_count == 1 and value != end:
            continue
        if class_count > 1 and value == end:
            continue
        states[index] = (group_cost(start, value), [value])

    for level in range(2, class_count + 1):
        next_states: Dict[int, Tuple[float, List[int]]] = {}
        for current_index, (cost, path) in states.items():
            current_value = path[-1]
            for next_index in range(current_index + 1, len(values)):
                value = values[next_index]
                if not valid_step(current_value, value):
                    continue
                if level == class_count and value != end:
                    continue
                if level < class_count and value == end:
                    continue
                candidate = (
                    cost + group_cost(current_value, value),
                    path + [value],
                )
                existing = next_states.get(next_index)
                if (
                    existing is None
                    or candidate[0] < existing[0]
                    or (
                        candidate[0] == existing[0]
                        and candidate[1] < existing[1]
                    )
                ):
                    next_states[next_index] = candidate
        states = next_states

    end_index = len(values) - 1
    result = states.get(end_index)
    if result is None:
        raise ValueError(
            f"找不到包含{class_count}个显式桶的合法规则"
        )
    return result


def page_aware_group_cost(
    dataset: Dataset, lower: int, upper: int
) -> float:
    start = bisect_right(dataset.histogram_classes, lower)
    stop = bisect_right(dataset.histogram_classes, upper)
    if (
        stop == 0
        or dataset.histogram_classes[stop - 1] != upper
    ):
        raise ValueError(f"候选边界{upper}不是公共桶边界")
    hole = 0.0
    for index in range(start, stop):
        class_hole = (
            upper * dataset.counts[index]
            - dataset.requested[index]
        )
        hole += min(class_hole, dataset.fallback_holes[index])
    return hole


def evaluate_page_aware_rule(
    dataset: Dataset, classes: Sequence[int]
) -> float:
    if not classes or classes[-1] != OPTIMIZE_LIMIT:
        raise ValueError(
            f"规则必须结束于{OPTIMIZE_LIMIT}"
        )
    hole = 0.0
    for index, count in enumerate(dataset.counts):
        requested = dataset.requested[index]
        upper = (
            dataset.histogram_classes[index]
            if index < len(dataset.histogram_classes)
            else None
        )
        rounded = None
        if upper is not None:
            class_index = bisect_left(classes, upper)
            if class_index < len(classes):
                rounded = classes[class_index]
        if rounded is None:
            hole += dataset.fallback_holes[index]
        else:
            class_hole = rounded * count - requested
            hole += min(
                class_hole, dataset.fallback_holes[index]
            )
    return hole


def allocator_detail_rows(
    dataset: Dataset, classes: Sequence[int]
) -> List[Dict[str, object]]:
    if not classes:
        raise ValueError("详细表中的分配器规则不能为空")
    histogram_set = set(dataset.histogram_classes)
    unavailable = [
        value for value in classes if value not in histogram_set
    ]
    if unavailable:
        raise ValueError(
            "详细表规则包含公共桶中不存在的边界："
            + "|".join(str(value) for value in unavailable)
        )

    rows = []
    lower = 0
    for upper in classes:
        rows.append({
            "range": f"{lower + 1} ~ {upper}",
            "allocated": upper,
            "step": upper - lower,
            "hole": page_aware_group_cost(
                dataset, lower, upper
            ),
        })
        lower = upper
    tail_start = bisect_right(
        dataset.histogram_classes, classes[-1]
    )
    rows.append({
        "range": f"> {classes[-1]}",
        "allocated": "4K对齐",
        "step": 4096,
        "hole": sum(dataset.fallback_holes[tail_start:]),
    })
    return rows


def detail_number(value: object) -> object:
    if not isinstance(value, float):
        return value
    rounded = round(value)
    if abs(value - rounded) < 1e-6:
        return rounded
    return f"{value:.2f}"


def detail_path_for_candidate(
    base_path: Path,
    candidate: Candidate,
    multiple: bool,
) -> Path:
    suffix = base_path.suffix or ".csv"
    stem = (
        base_path.stem
        if base_path.suffix else base_path.name
    )
    if multiple:
        stem += f"_{candidate.file_suffix}"
    return base_path.with_name(stem + suffix)


def write_detail_tables(
    base_path: Path,
    train: Dataset,
    allocator_classes: Dict[str, List[int]],
    candidates: Sequence[Candidate],
) -> List[Path]:
    outputs = []
    multiple = len(candidates) > 1
    for candidate in candidates:
        rules = [
            (
                display_name,
                allocator_classes[display_name],
            )
            for display_name, _ in DETAIL_RULES
        ]
        rules.append((
            candidate.display_name,
            candidate.classes,
        ))
        details = [
            (name, allocator_detail_rows(train, classes))
            for name, classes in rules
        ]
        output = detail_path_for_candidate(
            base_path.resolve(), candidate, multiple
        )
        output.parent.mkdir(parents=True, exist_ok=True)
        maximum_rows = max(len(rows) for _, rows in details)
        with output.open(
            "w", encoding="utf-8-sig", newline=""
        ) as destination:
            writer = csv.writer(destination)
            first_header = []
            second_header = []
            for name, _ in details:
                first_header.extend([name, "", "", ""])
                second_header.extend([
                    "用户申请范围(B)",
                    "实际分配(B)",
                    "步长(B)",
                    "训练集平均存活空洞(B)",
                ])
            writer.writerow(first_header)
            writer.writerow(second_header)
            for index in range(maximum_rows):
                output_row = []
                for _, rows in details:
                    if index >= len(rows):
                        output_row.extend(["", "", "", ""])
                        continue
                    row = rows[index]
                    output_row.extend([
                        row["range"],
                        row["allocated"],
                        row["step"],
                        detail_number(row["hole"]),
                    ])
                writer.writerow(output_row)
        outputs.append(output)
    return outputs


def optimize(
    train: Dataset,
    validation: Optional[Dataset],
    baseline_classes: Sequence[int],
    extras: Sequence[int],
) -> Tuple[float, Optional[float], List[Candidate]]:
    validate_baseline_rule(baseline_classes)
    baseline_bucket_count = len(baseline_classes)
    candidates = [
        value for value in train.histogram_classes
        if (
            0 < value <= OPTIMIZE_LIMIT
            and value % STEP_QUANTUM == 0
        )
    ]
    baseline_hole = evaluate_page_aware_rule(
        train, baseline_classes
    )
    validation_baseline = (
        evaluate_page_aware_rule(validation, baseline_classes)
        if validation is not None else None
    )
    cost_cache: Dict[Tuple[int, int], float] = {}

    def cached_group_cost(lower: int, upper: int) -> float:
        key = (lower, upper)
        if key not in cost_cache:
            cost_cache[key] = page_aware_group_cost(
                train, lower, upper
            )
        return cost_cache[key]

    results = []
    for extra in extras:
        _, classes = select_boundaries(
            candidates,
            OPTIMIZE_LIMIT,
            baseline_bucket_count + extra,
            cached_group_cost,
        )
        candidate = Candidate(
            extra_buckets=extra,
            classes=classes,
            train_hole=evaluate_page_aware_rule(train, classes),
        )
        if validation is not None:
            candidate.validation_hole = evaluate_page_aware_rule(
                validation, classes
            )
        results.append(candidate)
    return baseline_hole, validation_baseline, results


def optimize_schemes(
    train: Dataset,
    validation: Optional[Dataset],
    baseline_classes: Sequence[int],
    schemes: Sequence[Scheme] = DEFAULT_SCHEMES,
) -> Tuple[float, Optional[float], List[Candidate]]:
    """Optimize the small and large regions independently per scheme."""
    validate_baseline_rule(baseline_classes)
    small_baseline_count = bisect_right(
        baseline_classes, LARGE_STEP_START
    )
    large_baseline_count = (
        len(baseline_classes) - small_baseline_count
    )
    histogram_candidates = [
        value for value in train.histogram_classes
        if (
            0 < value <= OPTIMIZE_LIMIT
            and value % STEP_QUANTUM == 0
        )
    ]
    if LARGE_STEP_START not in histogram_candidates:
        raise ValueError(
            f"公共桶必须包含分界桶{LARGE_STEP_START}"
        )

    baseline_hole = evaluate_page_aware_rule(
        train, baseline_classes
    )
    validation_baseline = (
        evaluate_page_aware_rule(validation, baseline_classes)
        if validation is not None else None
    )
    cost_cache: Dict[Tuple[int, int], float] = {}

    def cached_group_cost(lower: int, upper: int) -> float:
        key = (lower, upper)
        if key not in cost_cache:
            cost_cache[key] = page_aware_group_cost(
                train, lower, upper
            )
        return cost_cache[key]

    small_results: Dict[int, List[int]] = {}
    large_results: Dict[Tuple[int, int], List[int]] = {}
    results = []
    for scheme in schemes:
        small_extra = scheme.small_extra_buckets
        if small_extra not in small_results:
            _, small_classes = select_boundaries(
                histogram_candidates,
                LARGE_STEP_START,
                small_baseline_count + small_extra,
                cached_group_cost,
            )
            small_results[small_extra] = small_classes

        large_key = (
            scheme.large_alignment,
            scheme.large_extra_buckets,
        )
        if large_key not in large_results:
            large_candidates = [
                value for value in histogram_candidates
                if (
                    value > LARGE_STEP_START
                    and value % scheme.large_alignment == 0
                )
            ]
            _, large_classes = select_boundaries(
                large_candidates,
                OPTIMIZE_LIMIT,
                large_baseline_count
                + scheme.large_extra_buckets,
                cached_group_cost,
                start=LARGE_STEP_START,
            )
            large_results[large_key] = large_classes

        classes = (
            small_results[small_extra]
            + large_results[large_key]
        )
        candidate = Candidate(
            extra_buckets=(
                scheme.small_extra_buckets
                + scheme.large_extra_buckets
            ),
            classes=classes,
            train_hole=evaluate_page_aware_rule(train, classes),
            scheme=scheme,
        )
        if validation is not None:
            candidate.validation_hole = evaluate_page_aware_rule(
                validation, classes
            )
        results.append(candidate)
    return baseline_hole, validation_baseline, results


def hole_rate(requested: float, hole: Optional[float]) -> float:
    if hole is None:
        return 0.0
    allocated = requested + hole
    return hole / allocated if allocated else 0.0


def write_summary_table(
    path: Path,
    train: Dataset,
    validation: Optional[Dataset],
    baseline_classes: Sequence[int],
    baseline_hole: float,
    validation_baseline: Optional[float],
    candidates: Sequence[Candidate],
) -> Path:
    """Write one row per scheme, including the dfmalloc2 baseline."""
    output = path.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    baseline_text = "|".join(str(value) for value in baseline_classes)
    header = [
        "方案",
        ">1280桶边界对齐(B)",
        "<1280区间相对dfmalloc2新增切分数",
        ">1280相对dfmalloc2新增桶",
        "≤1280区间桶数(含1280终点)",
        ">1280桶数",
        "总显式桶数",
        "方案桶",
        "训练集平均存活空洞(B)",
        "训练集空洞率",
        "dfmalloc2桶",
        "dfmalloc2训练集平均存活空洞(B)",
        "dfmalloc2训练集空洞率",
    ]
    if validation is not None:
        header.extend([
            "验证集平均存活空洞(B)",
            "验证集空洞率",
            "dfmalloc2验证集平均存活空洞(B)",
            "dfmalloc2验证集空洞率",
        ])
    with output.open(
        "w", encoding="utf-8-sig", newline=""
    ) as destination:
        writer = csv.writer(destination)
        writer.writerow(header)
        for candidate in candidates:
            scheme = candidate.scheme
            split = bisect_right(
                candidate.classes, LARGE_STEP_START
            )
            row = [
                candidate.display_name,
                scheme.large_alignment if scheme else "",
                scheme.small_extra_buckets if scheme else "",
                scheme.large_extra_buckets if scheme else "",
                split,
                len(candidate.classes) - split,
                len(candidate.classes),
                "|".join(str(value) for value in candidate.classes),
                detail_number(candidate.train_hole),
                f"{hole_rate(train.live_requested, candidate.train_hole):.6%}",
                baseline_text,
                detail_number(baseline_hole),
                f"{hole_rate(train.live_requested, baseline_hole):.6%}",
            ]
            if validation is not None:
                row.extend([
                    detail_number(candidate.validation_hole),
                    f"{hole_rate(validation.live_requested, candidate.validation_hole):.6%}",
                    detail_number(validation_baseline),
                    f"{hole_rate(validation.live_requested, validation_baseline):.6%}",
                ])
            writer.writerow(row)
    return output


def report_json(
    train: Dataset,
    validation: Optional[Dataset],
    baseline_classes: Sequence[int],
    baseline_hole: float,
    validation_baseline: Optional[float],
    candidates: Sequence[Candidate],
    last_rows: Optional[int],
) -> Dict[str, object]:
    curve = []
    for candidate in candidates:
        item = {
            "name": candidate.display_name,
            "extra_buckets": candidate.extra_buckets,
            "total_explicit_buckets": len(candidate.classes),
            "size_classes": candidate.classes,
            "train_hole_bytes": candidate.train_hole,
            "train_hole_rate": hole_rate(
                train.live_requested, candidate.train_hole
            ),
            "train_memory_saving_percent":
                memory_saving_percent(
                    train.live_requested,
                    baseline_hole,
                    candidate.train_hole,
                ),
            "train_hole_reduction_percent":
                hole_reduction_percent(
                    baseline_hole, candidate.train_hole
                ),
            "hole_saving_vs_dfmalloc2_bytes":
                baseline_hole - candidate.train_hole,
        }
        if candidate.scheme is not None:
            item.update({
                "scheme": candidate.scheme.number,
                "large_alignment":
                    candidate.scheme.large_alignment,
                "small_extra_buckets":
                    candidate.scheme.small_extra_buckets,
                "large_extra_buckets":
                    candidate.scheme.large_extra_buckets,
            })
        if validation is not None:
            item.update({
                "validation_hole_bytes":
                    candidate.validation_hole,
                "validation_hole_rate": hole_rate(
                    validation.live_requested,
                    candidate.validation_hole,
                ),
                "validation_memory_saving_percent":
                    memory_saving_percent(
                        validation.live_requested,
                        validation_baseline,
                        candidate.validation_hole,
                    ),
                "validation_hole_reduction_percent":
                    hole_reduction_percent(
                        validation_baseline,
                        candidate.validation_hole,
                    ),
            })
        curve.append(item)
    segmented = all(
        candidate.scheme is not None for candidate in candidates
    )
    return {
        "format": (
            "dfmalloc2_segmented_optimizer_v2"
            if segmented
            else "dfmalloc2_constrained_optimizer_v1"
        ),
        "objective": "average_live_page_aware_hole",
        "weighting": train.weighting,
        "last_rows_per_file": last_rows,
        "optimized_from": 0,
        "optimized_through": OPTIMIZE_LIMIT,
        "fallback_above": "4K",
        "minimum_step_through_1280": MIN_SMALL_STEP,
        "minimum_step_above_1280": MIN_LARGE_STEP,
        "step_quantum": STEP_QUANTUM,
        "minimum_relative_step_percent_exclusive":
            MIN_RELATIVE_STEP_PERCENT,
        "step_order": "unconstrained",
        "baseline_size_classes": list(baseline_classes),
        "schemes": (
            [
                {
                    "scheme": candidate.scheme.number,
                    "large_alignment":
                        candidate.scheme.large_alignment,
                    "small_extra_buckets":
                        candidate.scheme.small_extra_buckets,
                    "large_extra_buckets":
                        candidate.scheme.large_extra_buckets,
                }
                for candidate in candidates
            ]
            if segmented else None
        ),
        "train": {
            "files": [str(path) for path in train.paths],
            "rows": train.rows,
            "average_live_requested_bytes": train.live_requested,
            "baseline_dfmalloc2_hole_bytes": baseline_hole,
            "tracking_failures": train.tracking_failures,
            "estimated_files": train.estimated_files,
        },
        "validation": (
            {
                "files": [
                    str(path) for path in validation.paths
                ],
                "rows": validation.rows,
                "average_live_requested_bytes":
                    validation.live_requested,
                "baseline_dfmalloc2_hole_bytes":
                    validation_baseline,
            }
            if validation is not None else None
        ),
        "curve": curve,
        "rules": [
            {
                "name": candidate.display_name,
                "size_classes": candidate.classes,
            }
            for candidate in candidates
        ],
    }


def print_report(
    train: Dataset,
    validation: Optional[Dataset],
    baseline_classes: Sequence[int],
    baseline_hole: float,
    validation_baseline: Optional[float],
    candidates: Sequence[Candidate],
    last_rows: Optional[int],
) -> None:
    print(
        f"训练集：{len(train.paths)}个CSV，{train.rows}行，"
        f"权重={train.weighting}"
    )
    if last_rows is not None:
        print(
            f"数据窗口：每个CSV最后{last_rows}条有效快照"
        )
    print(
        "约束：≤1280和>1280两个区间独立优化并固定1280分界；"
        "所有步长为8的倍数，>1280桶按方案进行512/256边界对齐；"
        "每个步长严格大于当前桶的8%；"
        ">14336保持4KiB对齐"
    )
    print(
        "dfmalloc2.0基线："
        f"显式桶={len(baseline_classes)} "
        f"hole={human_bytes(baseline_hole)} "
        f"hole_rate="
        f"{hole_rate(train.live_requested, baseline_hole) * 100:.4f}%"
    )
    print(
        "dfmalloc2.0桶："
        + "|".join(str(value) for value in baseline_classes)
    )
    if train.tracking_failures:
        print(
            f"警告：存活表累计跟踪失败"
            f"{train.tracking_failures}次，结果可能低估"
        )
    segmented = all(
        candidate.scheme is not None for candidate in candidates
    )
    if segmented and validation is None:
        print(
            "\n方案  对齐  小桶+  大桶+  总桶  候选空洞       "
            "空洞率      占用收益    空洞下降"
        )
    elif segmented:
        print(
            "\n方案  对齐  小桶+  大桶+  总桶  训练空洞率  "
            "验证空洞率  训练占用收益  验证占用收益"
        )
    elif validation is None:
        print(
            "\n新增桶  总显式桶  候选空洞       空洞率      "
            "占用收益    空洞下降"
        )
    else:
        print(
            "\n新增桶  总显式桶  训练占用收益  验证占用收益  "
            "训练空洞下降  验证空洞下降"
        )

    for candidate in candidates:
        train_memory = memory_saving_percent(
            train.live_requested,
            baseline_hole,
            candidate.train_hole,
        )
        train_reduction = hole_reduction_percent(
            baseline_hole, candidate.train_hole
        )
        if segmented and validation is None:
            scheme = candidate.scheme
            print(
                f"{scheme.number:>4}  "
                f"{scheme.large_alignment:>4}  "
                f"{scheme.small_extra_buckets:>5}  "
                f"{scheme.large_extra_buckets:>5}  "
                f"{len(candidate.classes):>4}  "
                f"{human_bytes(candidate.train_hole):>12}  "
                f"{hole_rate(train.live_requested, candidate.train_hole) * 100:>8.4f}%  "
                f"{train_memory:>8.4f}%  "
                f"{train_reduction:>8.2f}%"
            )
        elif segmented:
            validation_memory = memory_saving_percent(
                validation.live_requested,
                validation_baseline,
                candidate.validation_hole,
            )
            validation_reduction = hole_reduction_percent(
                validation_baseline,
                candidate.validation_hole,
            )
            print(
                f"{candidate.scheme.number:>4}  "
                f"{candidate.scheme.large_alignment:>4}  "
                f"{candidate.scheme.small_extra_buckets:>5}  "
                f"{candidate.scheme.large_extra_buckets:>5}  "
                f"{len(candidate.classes):>4}  "
                f"{hole_rate(train.live_requested, candidate.train_hole) * 100:>8.4f}%  "
                f"{hole_rate(validation.live_requested, candidate.validation_hole) * 100:>8.4f}%  "
                f"{train_memory:>12.4f}%  "
                f"{validation_memory:>12.4f}%"
            )
        elif validation is None:
            print(
                f"+{candidate.extra_buckets:<5} "
                f"{len(candidate.classes):>8}  "
                f"{human_bytes(candidate.train_hole):>12}  "
                f"{hole_rate(train.live_requested, candidate.train_hole) * 100:>8.4f}%  "
                f"{train_memory:>8.4f}%  "
                f"{train_reduction:>8.2f}%"
            )
        else:
            validation_memory = memory_saving_percent(
                validation.live_requested,
                validation_baseline,
                candidate.validation_hole,
            )
            validation_reduction = hole_reduction_percent(
                validation_baseline,
                candidate.validation_hole,
            )
            print(
                f"+{candidate.extra_buckets:<5} "
                f"{len(candidate.classes):>8}  "
                f"{train_memory:>12.4f}%  "
                f"{validation_memory:>12.4f}%  "
                f"{train_reduction:>12.2f}%  "
                f"{validation_reduction:>12.2f}%"
            )
        print(
            f"        {candidate.display_name}桶："
            + "|".join(str(value) for value in candidate.classes)
        )


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "基于v11 CSV同时比较dfmalloc2.0的10种分段DP桶方案"
        )
    )
    parser.add_argument(
        "csv_paths", type=Path, nargs="+", help="训练CSV"
    )
    parser.add_argument(
        "--validation", type=Path, nargs="+",
        help="可选：独立验证CSV，只评估、不参与拟合",
    )
    parser.add_argument(
        "--extra-buckets",
        type=parse_nonnegative_list,
        help=(
            "兼容旧的全区间DP：指定相对总桶数增量；"
            "不指定时运行默认10种分段方案"
        ),
    )
    parser.add_argument(
        "--weighting",
        choices=("equal-file", "duration"),
        default="equal-file",
        help="多个CSV按文件等权或按总时长加权",
    )
    parser.add_argument(
        "--last-rows", type=int,
        help="每个CSV只使用最后N条有效快照",
    )
    parser.add_argument(
        "--output-json", type=Path,
        help="可选：输出完整候选规则和收益JSON",
    )
    parser.add_argument(
        "--output-detail-csv", type=Path,
        help=(
            "四规则横向详细表的输出路径；多个候选会自动添加"
            " _schemeNN，默认由JSON或首个训练CSV名称派生"
        ),
    )
    parser.add_argument(
        "--output-summary-csv", type=Path,
        help=(
            "10个方案、dfmalloc2基线桶和空洞率汇总表路径；"
            "默认由JSON或首个训练CSV名称派生"
        ),
    )
    args = parser.parse_args()

    all_paths = list(itertools.chain(
        args.csv_paths, args.validation or []
    ))
    missing = [path for path in all_paths if not path.is_file()]
    if missing:
        parser.error(
            "CSV不存在：" + ", ".join(str(path) for path in missing)
        )
    if args.last_rows is not None and args.last_rows <= 0:
        parser.error("--last-rows必须是正整数")
    try:
        allocator_classes = read_allocator_classes(all_paths)
        baseline_classes = allocator_classes["dfmalloc2.0"]
        train = combine_files(
            args.csv_paths, args.weighting, args.last_rows
        )
        validation = (
            combine_files(
                args.validation, args.weighting, args.last_rows
            )
            if args.validation else None
        )
        if (
            validation is not None
            and validation.histogram_classes
            != train.histogram_classes
        ):
            raise ValueError("训练集与验证集公共桶不一致")
        if args.extra_buckets is None:
            baseline_hole, validation_baseline, candidates = (
                optimize_schemes(
                    train,
                    validation,
                    baseline_classes,
                )
            )
        else:
            baseline_hole, validation_baseline, candidates = optimize(
                train,
                validation,
                baseline_classes,
                args.extra_buckets,
            )
        report = report_json(
            train, validation, baseline_classes,
            baseline_hole, validation_baseline, candidates,
            args.last_rows,
        )
        if args.output_detail_csv:
            detail_base = args.output_detail_csv
        elif args.output_json:
            detail_base = args.output_json.with_name(
                args.output_json.stem + "_details.csv"
            )
        else:
            first_csv = args.csv_paths[0]
            detail_base = first_csv.with_name(
                first_csv.stem + "_dfmalloc2_details.csv"
            )
        if args.output_summary_csv:
            summary_path = args.output_summary_csv
        elif args.output_json:
            summary_path = args.output_json.with_name(
                args.output_json.stem + "_summary.csv"
            )
        else:
            first_csv = args.csv_paths[0]
            summary_path = first_csv.with_name(
                first_csv.stem + "_dfmalloc2_schemes.csv"
            )
        summary_output = write_summary_table(
            summary_path,
            train,
            validation,
            baseline_classes,
            baseline_hole,
            validation_baseline,
            candidates,
        )
        detail_outputs = write_detail_tables(
            detail_base,
            train,
            allocator_classes,
            candidates,
        )
    except (OSError, ValueError) as error:
        parser.error(str(error))

    print_report(
        train, validation, baseline_classes,
        baseline_hole, validation_baseline, candidates,
        args.last_rows,
    )
    if args.output_json:
        output = args.output_json.resolve()
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        print(f"JSON：{output}")
    print(f"汇总表：{summary_output}")
    for output in detail_outputs:
        print(f"详细表：{output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
