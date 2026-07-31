#!/usr/bin/env python3
"""Refine dfmalloc2.0 size classes under quantized-step constraints."""

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
MIN_LARGE_STEP = 128
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
) -> Tuple[float, List[int]]:
    """Choose exactly class_count endpoints and end at ``end``."""
    values = [
        value for value in candidates
        if 0 < value <= end
    ]
    if not values or values[-1] != end:
        raise ValueError("候选边界必须包含优化终点")
    if class_count < 1 or class_count > len(values):
        raise ValueError("请求的桶数量超出候选边界范围")

    states: Dict[int, Tuple[float, List[int]]] = {}
    for index, value in enumerate(values):
        if not valid_step(0, value):
            continue
        if class_count == 1 and value != end:
            continue
        if class_count > 1 and value == end:
            continue
        states[index] = (group_cost(0, value), [value])

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
        stem += f"_plus{candidate.extra_buckets}"
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
            f"新规则 +{candidate.extra_buckets}桶",
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
    previous_hole = baseline_hole
    for candidate in candidates:
        item = {
            "extra_buckets": candidate.extra_buckets,
            "total_explicit_buckets": len(candidate.classes),
            "size_classes": candidate.classes,
            "train_hole_bytes": candidate.train_hole,
            "train_hole_rate": (
                candidate.train_hole
                / (train.live_requested + candidate.train_hole)
                if train.live_requested + candidate.train_hole else 0.0
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
            "marginal_hole_saving_bytes":
                previous_hole - candidate.train_hole,
        }
        if validation is not None:
            item.update({
                "validation_hole_bytes":
                    candidate.validation_hole,
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
        previous_hole = candidate.train_hole
    return {
        "format": "dfmalloc2_constrained_optimizer_v1",
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
        "约束：≤1280和1280～14336均可重构；"
        "所有步长为8的倍数，1280以上步长≥128；"
        "每个步长严格大于当前桶的8%；"
        ">14336保持4KiB对齐"
    )
    print(
        "dfmalloc2.0基线："
        f"显式桶={len(baseline_classes)} "
        f"hole={human_bytes(baseline_hole)} "
        f"hole_rate="
        f"{baseline_hole / (train.live_requested + baseline_hole) * 100 if train.live_requested + baseline_hole else 0:.4f}%"
    )
    if train.tracking_failures:
        print(
            f"警告：存活表累计跟踪失败"
            f"{train.tracking_failures}次，结果可能低估"
        )
    if validation is None:
        print(
            "\n新增桶  总显式桶  候选空洞       空洞率      "
            "占用收益    空洞下降    边际空洞节省"
        )
    else:
        print(
            "\n新增桶  总显式桶  训练占用收益  验证占用收益  "
            "训练空洞下降  验证空洞下降"
        )

    previous_hole = baseline_hole
    for candidate in candidates:
        train_memory = memory_saving_percent(
            train.live_requested,
            baseline_hole,
            candidate.train_hole,
        )
        train_reduction = hole_reduction_percent(
            baseline_hole, candidate.train_hole
        )
        if validation is None:
            allocated = train.live_requested + candidate.train_hole
            rate = (
                candidate.train_hole / allocated * 100
                if allocated else 0.0
            )
            print(
                f"+{candidate.extra_buckets:<5} "
                f"{len(candidate.classes):>8}  "
                f"{human_bytes(candidate.train_hole):>12}  "
                f"{rate:>8.4f}%  "
                f"{train_memory:>8.4f}%  "
                f"{train_reduction:>8.2f}%  "
                f"{human_bytes(previous_hole - candidate.train_hole):>12}"
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
            "        Size Classes："
            + "|".join(str(value) for value in candidate.classes)
        )
        previous_hole = candidate.train_hole


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "基于v11 CSV约束优化dfmalloc2.0的14336及以下桶"
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
        default=parse_nonnegative_list("0,1,2"),
        help="相对当前规则增加的桶数量，默认0,1,2",
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
            " _plusN，默认由JSON或首个训练CSV名称派生"
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
    for output in detail_outputs:
        print(f"详细表：{output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
