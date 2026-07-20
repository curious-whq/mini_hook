#!/usr/bin/env python3
"""Build a compact, phase-based mstress-like workload from an RPLY trace.

The generated executable contains statistical phase profiles rather than one
runtime record per trace event.  Every worker advances through its own phases
without a phase barrier.  Function quotas and remote-free routes are exact;
allocation sizes are sampled from bounded per-phase/function reservoirs.
"""

from __future__ import annotations

import argparse
import math
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile


RPLY_ENTRY = struct.Struct("<QQQQQQ")
RPLY_ENTRY_WORDS = 6
FUNC_COUNT = 10
FUNC_FREE = 1
FUNC_REALLOC = 3
FUNC_FREE_SIZED = 4
ALLOC_FUNCTIONS = (0, 2, 3, 5, 6, 7, 8, 9)

IMAGE_MAGIC = b"MSRUN001"
TRAILER_MAGIC = b"MSEND001"
VERSION = 2
CPU_NONE = 0xFFFFFFFF
FUNC_THREAD_CREATE = 100
FUNC_THREAD_START = 101
FUNC_THREAD_END = 102
FUNC_THREAD_JOIN = 103
FUNC_THREAD_DETACH = 104

# Keep these layouts in sync with synthetic_mstress_runtime.c.
HEADER = struct.Struct("<8sIIIIIIQQQQQQQ")
WAVE = struct.Struct("<IIQ")
THREAD = struct.Struct("<QQIIQQ")
PHASE = struct.Struct("<Q" + "II" * FUNC_COUNT)
MODEL = struct.Struct("<" + "Q" * FUNC_COUNT + "QQQQII")
ROUTE = struct.Struct("<IIQ")
TRAILER = struct.Struct("<8sQQ")


class WorkloadError(RuntimeError):
    pass


class Reservoir:
    __slots__ = ("limit", "seen", "values")

    def __init__(self, limit: int) -> None:
        self.limit = limit
        self.seen = 0
        self.values: list[int] = []

    @staticmethod
    def _mix(value: int) -> int:
        value = (value + 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
        value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
        value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
        return value ^ (value >> 31)

    def add(self, value: int, key: int) -> None:
        self.seen += 1
        if len(self.values) < self.limit:
            self.values.append(value)
            return
        mixed = self._mix(key)
        selected = mixed % self.seen
        if selected < self.limit:
            self.values[selected] = value


class ThreadStats:
    __slots__ = (
        "tid", "cpu", "index", "models", "live", "peak_live", "total_ops"
    )

    def __init__(self, tid: int, cpu: int, index: int, phases: int) -> None:
        self.tid = tid
        self.cpu = cpu
        self.index = index
        self.models = [PhaseModel() for _ in range(phases)]
        self.live = 0
        self.peak_live = 0
        self.total_ops = 0


class PhaseModel:
    __slots__ = (
        "counts", "remote_free", "remote_publish", "realloc_null", "routes"
    )

    def __init__(self) -> None:
        self.counts = [0] * FUNC_COUNT
        self.remote_free = 0
        self.remote_publish = 0
        self.realloc_null = 0
        self.routes: dict[int, int] = {}


def read_count(trace) -> int:
    raw = trace.read(8)
    if len(raw) != 8:
        raise WorkloadError("missing or incomplete RPLY header")
    (words,) = struct.unpack("<Q", raw)
    if words % RPLY_ENTRY_WORDS:
        raise WorkloadError(f"invalid RPLY index {words}")
    count = words // RPLY_ENTRY_WORDS
    expected = 8 + count * RPLY_ENTRY.size
    actual = os.fstat(trace.fileno()).st_size
    if actual < expected:
        raise WorkloadError(
            f"truncated RPLY: header needs {expected} bytes, file has {actual}"
        )
    return count


def scan_layout(trace, record_count: int):
    trace.seek(8)
    allocator_count = 0
    wave_counts = [0]
    pending_children: set[int] = set()
    wave_has_children = False
    lifecycle = {
        "create": 0,
        "start": 0,
        "end": 0,
        "join": 0,
        "detach": 0,
    }
    for index in range(record_count):
        raw = trace.read(RPLY_ENTRY.size)
        if len(raw) != RPLY_ENTRY.size:
            raise WorkloadError(f"RPLY record {index} is truncated")
        _, func_cpu, _, _, result, _ = RPLY_ENTRY.unpack(raw)
        function = func_cpu >> 32
        if function < FUNC_COUNT:
            allocator_count += 1
            wave_counts[-1] += 1
        elif function == FUNC_THREAD_CREATE:
            lifecycle["create"] += 1
            if wave_has_children and not pending_children:
                wave_counts.append(0)
                wave_has_children = False
            wave_has_children = True
            if result != 0:
                pending_children.add(result)
        elif function == FUNC_THREAD_START:
            lifecycle["start"] += 1
        elif function == FUNC_THREAD_END:
            lifecycle["end"] += 1
        elif function == FUNC_THREAD_JOIN:
            lifecycle["join"] += 1
            pending_children.discard(result)
        elif function == FUNC_THREAD_DETACH:
            lifecycle["detach"] += 1
            pending_children.discard(result)
        elif not 100 <= function <= 104:
            raise WorkloadError(f"record {index}: unsupported function {function}")
    if allocator_count == 0:
        raise WorkloadError("RPLY contains no allocator events")
    # Only treat create/join-separated groups as waves. Traces without both
    # sides of the lifecycle retain the safe one-wave behavior.
    if (lifecycle["create"] == 0 or lifecycle["join"] == 0 or
            lifecycle["detach"] != 0):
        wave_counts = [allocator_count]
    wave_counts = [count for count in wave_counts if count != 0]
    if sum(wave_counts) != allocator_count:
        raise WorkloadError("internal lifecycle wave accounting mismatch")
    return allocator_count, wave_counts, lifecycle


def distribute_phases(wave_counts: list[int], requested: int) -> list[int]:
    wave_count = len(wave_counts)
    total_phases = max(requested, wave_count)
    result = [1] * wave_count
    remaining = total_phases - wave_count
    if remaining == 0:
        return result
    total_ops = sum(wave_counts)
    exact = [remaining * count / total_ops for count in wave_counts]
    floors = [int(value) for value in exact]
    for index, value in enumerate(floors):
        result[index] += value
    left = remaining - sum(floors)
    order = sorted(
        range(wave_count), key=lambda i: exact[i] - floors[i], reverse=True
    )
    for index in order[:left]:
        result[index] += 1
    return result


def analyze(
    trace,
    record_count: int,
    allocator_events: int,
    wave_counts: list[int],
    wave_phase_counts: list[int],
    sample_limit: int,
    capacity_factor: float,
):
    phase_count = sum(wave_phase_counts)
    phase_offsets = []
    next_phase = 0
    for count in wave_phase_counts:
        phase_offsets.append(next_phase)
        next_phase += count
    threads: dict[int, ThreadStats] = {}
    ordered_threads: list[ThreadStats] = []
    reservoirs = [
        [Reservoir(sample_limit) for _ in range(FUNC_COUNT)]
        for _ in range(phase_count)
    ]
    phase_totals = [0] * phase_count
    # address -> (origin thread, origin phase)
    live: dict[int, tuple[ThreadStats, int]] = {}
    unknown_free = 0
    unknown_realloc = 0
    remote_realloc = 0
    event_index = 0
    wave_index = 0
    wave_event_index = 0

    def thread_for(tid: int, cpu: int) -> ThreadStats:
        found = threads.get(tid)
        if found is None:
            found = ThreadStats(tid, cpu, len(ordered_threads), phase_count)
            threads[tid] = found
            ordered_threads.append(found)
        return found

    def add_live(address: int, owner: ThreadStats, phase: int) -> None:
        if address == 0:
            return
        previous = live.get(address)
        if previous is not None:
            previous[0].live -= 1
        live[address] = (owner, phase)
        owner.live += 1
        owner.peak_live = max(owner.peak_live, owner.live)

    def remove_live(address: int):
        allocation = live.pop(address, None)
        if allocation is not None:
            allocation[0].live -= 1
        return allocation

    trace.seek(8)
    for record_index in range(record_count):
        raw = trace.read(RPLY_ENTRY.size)
        if len(raw) != RPLY_ENTRY.size:
            raise WorkloadError(f"RPLY record {record_index} is truncated")
        _, func_cpu, tid, address, result, size = RPLY_ENTRY.unpack(raw)
        function = func_cpu >> 32
        if function >= FUNC_COUNT:
            continue
        while wave_event_index >= wave_counts[wave_index]:
            wave_index += 1
            wave_event_index = 0
        phase = phase_offsets[wave_index] + min(
            (wave_event_index * wave_phase_counts[wave_index]) //
            wave_counts[wave_index],
            wave_phase_counts[wave_index] - 1,
        )
        event_index += 1
        wave_event_index += 1
        worker = thread_for(tid, func_cpu & 0xFFFFFFFF)
        model = worker.models[phase]
        model.counts[function] += 1
        worker.total_ops += 1
        phase_totals[phase] += 1

        if function in ALLOC_FUNCTIONS:
            reservoirs[phase][function].add(size, record_index)

        if function in (FUNC_FREE, FUNC_FREE_SIZED):
            allocation = remove_live(address)
            if allocation is None:
                unknown_free += 1
            else:
                origin, origin_phase = allocation
                if origin is not worker:
                    model.remote_free += 1
                    origin_model = origin.models[origin_phase]
                    origin_model.remote_publish += 1
                    origin_model.routes[worker.index] = (
                        origin_model.routes.get(worker.index, 0) + 1
                    )
            continue

        if function == FUNC_REALLOC:
            if address == 0:
                model.realloc_null += 1
                if result != 0:
                    add_live(result, worker, phase)
                continue
            allocation = remove_live(address)
            if allocation is None:
                unknown_realloc += 1
                model.realloc_null += 1
            elif allocation[0] is not worker:
                remote_realloc += 1
            if result != 0:
                add_live(result, worker, phase)
            elif size != 0 and allocation is not None:
                # A failed realloc retains the original object and origin.
                add_live(address, allocation[0], allocation[1])
            continue

        if result != 0:
            add_live(result, worker, phase)

    if event_index != allocator_events:
        raise WorkloadError("allocator event count changed between passes")

    capacities = []
    for worker in ordered_threads:
        scaled = int(math.ceil(worker.peak_live * capacity_factor))
        capacities.append(max(scaled + 64, 64))
    if sum(capacities) >= 0xFFFFFFFF:
        raise WorkloadError("descriptor capacity exceeds uint32 index space")

    summary = {
        "unknown_free": unknown_free,
        "unknown_realloc": unknown_realloc,
        "remote_realloc": remote_realloc,
        "remote_free": sum(
            model.remote_free
            for worker in ordered_threads
            for model in worker.models
        ),
        "live_at_end": len(live),
        "peak_descriptors": sum(worker.peak_live for worker in ordered_threads),
    }
    return (
        ordered_threads, reservoirs, phase_totals, capacities, summary,
        phase_offsets,
    )


def write_image(
    path: Path,
    threads: list[ThreadStats],
    reservoirs,
    phase_totals: list[int],
    capacities: list[int],
    total_ops: int,
    wave_counts: list[int],
    wave_phase_counts: list[int],
    phase_offsets: list[int],
) -> int:
    phase_count = len(phase_totals)
    routes: list[tuple[int, int, int]] = []
    route_ranges: dict[tuple[int, int], tuple[int, int]] = {}
    for worker in threads:
        for phase_index, model in enumerate(worker.models):
            first = len(routes)
            for target, count in sorted(model.routes.items()):
                routes.append((target, 0, count))
            route_ranges[(worker.index, phase_index)] = (first, len(routes) - first)

    samples: list[int] = []
    sample_ranges: list[list[tuple[int, int]]] = []
    for phase_reservoirs in reservoirs:
        ranges = []
        for reservoir in phase_reservoirs:
            first = len(samples)
            samples.extend(reservoir.values)
            ranges.append((first, len(reservoir.values)))
        sample_ranges.append(ranges)
    if len(samples) > 0xFFFFFFFF:
        raise WorkloadError(
            "size sample table exceeds uint32 offsets; reduce phases or samples"
        )

    wave_offset = HEADER.size
    thread_offset = wave_offset + len(wave_counts) * WAVE.size
    phase_offset = thread_offset + len(threads) * THREAD.size
    model_offset = phase_offset + phase_count * PHASE.size
    model_count = len(threads) * phase_count
    route_offset = model_offset + model_count * MODEL.size
    sample_offset = route_offset + len(routes) * ROUTE.size
    image_size = sample_offset + len(samples) * 8

    with path.open("wb") as output:
        output.write(HEADER.pack(
            IMAGE_MAGIC, VERSION, HEADER.size, len(threads), phase_count,
            FUNC_COUNT, len(wave_counts), total_ops, thread_offset, phase_offset,
            model_offset, route_offset, sample_offset, image_size,
        ))
        for first, phase_count_for_wave, operations in zip(
            phase_offsets, wave_phase_counts, wave_counts
        ):
            output.write(WAVE.pack(first, phase_count_for_wave, operations))
        for worker, capacity in zip(threads, capacities):
            seed = Reservoir._mix(worker.tid ^ 0xD1B54A32D192ED03)
            output.write(THREAD.pack(
                worker.tid, seed, capacity,
                worker.cpu if worker.cpu <= 0xFFFFFFFF else CPU_NONE,
                worker.total_ops, worker.index * phase_count,
            ))
        for phase_index, total in enumerate(phase_totals):
            flattened = []
            for first, count in sample_ranges[phase_index]:
                flattened.extend((first, count))
            output.write(PHASE.pack(total, *flattened))
        for worker in threads:
            for phase_index, model in enumerate(worker.models):
                first, count = route_ranges[(worker.index, phase_index)]
                output.write(MODEL.pack(
                    *model.counts, model.remote_free, model.remote_publish,
                    model.realloc_null, first, count, 0,
                ))
        for route in routes:
            output.write(ROUTE.pack(*route))
        for value in samples:
            output.write(struct.pack("<Q", value))
    if path.stat().st_size != image_size:
        raise WorkloadError("internal profile image size mismatch")
    return image_size


def build_executable(
    output: Path,
    image_path: Path,
    image_size: int,
    compiler: str,
    cflags: list[str],
) -> None:
    runtime = Path(__file__).with_name("synthetic_mstress_runtime.c")
    if not runtime.is_file():
        raise WorkloadError(f"synthetic runtime source is missing: {runtime}")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="rply-mstress-build-", dir=output.parent
    ) as temporary_text:
        temporary = Path(temporary_text)
        runtime_elf = temporary / "runtime"
        command = shlex.split(compiler) + [
            "-O3", "-DNDEBUG", "-std=gnu11", "-pthread",
            "-Wall", "-Wextra", "-Werror", *cflags,
            str(runtime), "-o", str(runtime_elf),
        ]
        try:
            subprocess.run(command, check=True)
        except (OSError, subprocess.CalledProcessError) as error:
            raise WorkloadError(f"synthetic runtime compilation failed: {error}") from error
        assembled = temporary / "assembled"
        with assembled.open("wb") as destination:
            with runtime_elf.open("rb") as source:
                shutil.copyfileobj(source, destination, 1024 * 1024)
            padding = (-destination.tell()) % 8
            destination.write(b"\0" * padding)
            with image_path.open("rb") as source:
                shutil.copyfileobj(source, destination, 1024 * 1024)
            destination.write(TRAILER.pack(TRAILER_MAGIC, image_size, VERSION))
        assembled.chmod(0o755)
        os.replace(assembled, output)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate a compact phase-based mstress-like ELF from RPLY."
    )
    parser.add_argument("trace", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--phases", type=int, default=64,
                        help="normalized progress phases (default: 64)")
    parser.add_argument("--samples-per-function", type=int, default=256,
                        help="size reservoir per phase/function (default: 256)")
    parser.add_argument("--capacity-factor", type=float, default=1.25,
                        help="descriptor headroom over traced owner peak (default: 1.25)")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--cflag", action="append", default=[])
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.phases < 1 or args.phases > 4096:
        raise WorkloadError("--phases must be between 1 and 4096")
    if args.samples_per_function < 1 or args.samples_per_function > 65536:
        raise WorkloadError("--samples-per-function must be between 1 and 65536")
    if not math.isfinite(args.capacity_factor) or args.capacity_factor < 1.0:
        raise WorkloadError("--capacity-factor must be finite and at least 1")
    if args.trace.resolve() == args.output.resolve():
        raise WorkloadError("input trace and output executable must differ")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.trace.open("rb") as trace, tempfile.TemporaryDirectory(
        prefix="rply-mstress-profile-", dir=args.output.parent
    ) as temporary_text:
        record_count = read_count(trace)
        allocator_events, wave_counts, lifecycle = scan_layout(trace, record_count)
        requested_phases = min(args.phases, allocator_events)
        wave_phase_counts = distribute_phases(wave_counts, requested_phases)
        result = analyze(
            trace, record_count, allocator_events, wave_counts,
            wave_phase_counts,
            args.samples_per_function, args.capacity_factor,
        )
        (threads, reservoirs, phase_totals, capacities, summary,
         phase_offsets) = result
        image_path = Path(temporary_text) / "profile.bin"
        image_size = write_image(
            image_path, threads, reservoirs, phase_totals, capacities,
            allocator_events, wave_counts, wave_phase_counts, phase_offsets,
        )
        build_executable(
            args.output, image_path, image_size, args.cc, args.cflag
        )

    print(
        f"generated={args.output} records={record_count} operations={allocator_events} "
        f"historical_threads={len(threads)} waves={len(wave_counts)} "
        f"phases={sum(wave_phase_counts)} profile_bytes={image_size} "
        f"size_samples={sum(len(r.values) for p in reservoirs for r in p)}"
    )
    print(
        "thread_events=create:{create},start:{start},end:{end},join:{join},"
        "detach:{detach} wave_operations={waves}".format(
            **lifecycle, waves=",".join(str(value) for value in wave_counts)
        )
    )
    print(
        "remote_free={remote_free} remote_realloc={remote_realloc} "
        "unknown_free={unknown_free} unknown_realloc={unknown_realloc} "
        "live_at_end={live_at_end} traced_peak_descriptors={peak_descriptors} "
        "runtime_capacity={capacity}".format(
            **summary, capacity=sum(capacities)
        )
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, WorkloadError) as error:
        print(f"rply_to_mstress: {error}", file=sys.stderr)
        raise SystemExit(1)
