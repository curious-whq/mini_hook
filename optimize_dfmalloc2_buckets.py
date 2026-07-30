#!/usr/bin/env python3
"""Refine dfmalloc2.0 size classes under quantized-step constraints."""

from __future__ import annotations

import argparse
from bisect import bisect_left, bisect_right
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


def read_dfmalloc2_classes(paths: Sequence[Path]) -> List[int]:
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
        step = value
        minimum_step = (
            MIN_LARGE_STEP
            if value > LARGE_STEP_START else MIN_SMALL_STEP
        )
        if (
            step < minimum_step
            or step % STEP_QUANTUM != 0
        ):
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
                step = value - current_value
                minimum_step = (
                    MIN_LARGE_STEP
                    if value > LARGE_STEP_START
                    else MIN_SMALL_STEP
                )
                if (
                    step < minimum_step
                    or step % STEP_QUANTUM != 0
                ):
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
        "optimized_from": 0,
        "optimized_through": OPTIMIZE_LIMIT,
        "fallback_above": "4K",
        "minimum_step_through_1280": MIN_SMALL_STEP,
        "minimum_step_above_1280": MIN_LARGE_STEP,
        "step_quantum": STEP_QUANTUM,
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
) -> None:
    print(
        f"训练集：{len(train.paths)}个CSV，{train.rows}行，"
        f"权重={train.weighting}"
    )
    print(
        "约束：≤1280和1280～14336均可重构；"
        "所有步长为8的倍数，1280以上步长≥128；"
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
        "--output-json", type=Path,
        help="可选：输出完整候选规则和收益JSON",
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
    try:
        baseline_classes = read_dfmalloc2_classes(all_paths)
        train = combine_files(args.csv_paths, args.weighting)
        validation = (
            combine_files(args.validation, args.weighting)
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
        )
    except ValueError as error:
        parser.error(str(error))

    print_report(
        train, validation, baseline_classes,
        baseline_hole, validation_baseline, candidates,
    )
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
