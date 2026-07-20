#!/usr/bin/env python3
"""Compile an RPLY allocator trace into a standalone minimal ELF replay."""

from __future__ import annotations

import argparse
import math
import mmap
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile


RPLY_ENTRY_WORDS = 6
RPLY_ENTRY = struct.Struct("<QQQQQQ")
IMAGE_HEADER = struct.Struct("<8sIIIIIIQQQQ")
THREAD_RECORD = struct.Struct("<QQQII")
OP_RECORD = struct.Struct("<QQIIIIHBBI")
SPOOL_RECORD_SIZE = 8 + OP_RECORD.size
TRAILER = struct.Struct("<8sQQ")

IMAGE_MAGIC = b"MNRUN001"
TRAILER_MAGIC = b"MNEND001"
NATIVE_VERSION = 1
SLOT_NONE = 0xFFFFFFFF
CPU_NONE = 0xFFFFFFFF

FUNC_MALLOC = 0
FUNC_FREE = 1
FUNC_CALLOC = 2
FUNC_REALLOC = 3
FUNC_FREE_SIZED = 4
FUNC_POSIX_MEMALIGN = 5
FUNC_ALIGNED_ALLOC = 6
FUNC_VALLOC = 7
FUNC_MEMALIGN = 8
FUNC_PVALLOC = 9
FUNC_COUNT = 10

OP_ALLOC = 0
OP_FREE = 1
OP_REALLOC = 2
OP_REALLOC_NULL = 3
OP_UNKNOWN_FREE = 4

FLAG_TRACE_SUCCEEDED = 1
IMAGE_FLAG_HAS_UNKNOWN = 1


class TraceError(RuntimeError):
    pass


class ThreadInfo:
    __slots__ = ("tid", "count", "cpu", "first", "cursor")

    def __init__(self, tid: int, cpu: int) -> None:
        self.tid = tid
        self.count = 0
        self.cpu = cpu
        self.first = 0
        self.cursor = 0


def read_header(trace) -> int:
    raw = trace.read(8)
    if len(raw) != 8:
        raise TraceError("missing or incomplete RPLY header")
    (index_words,) = struct.unpack("<Q", raw)
    if index_words % RPLY_ENTRY_WORDS != 0:
        raise TraceError(
            f"invalid RPLY idx {index_words}: not divisible by "
            f"{RPLY_ENTRY_WORDS}"
        )
    count = index_words // RPLY_ENTRY_WORDS
    expected = 8 + count * RPLY_ENTRY.size
    actual = os.fstat(trace.fileno()).st_size
    if actual < expected:
        raise TraceError(
            f"truncated RPLY: header needs {expected} bytes, file has {actual}"
        )
    return count


def scan_timestamp_range(trace, record_count: int) -> tuple[int, int]:
    minimum = None
    maximum = 0
    for index in range(record_count):
        raw = trace.read(RPLY_ENTRY.size)
        if len(raw) != RPLY_ENTRY.size:
            raise TraceError(f"RPLY record {index} is truncated")
        timestamp, func_cpu, _, _, _, _ = RPLY_ENTRY.unpack(raw)
        function = func_cpu >> 32
        if function < FUNC_COUNT:
            minimum = timestamp if minimum is None else min(minimum, timestamp)
            maximum = max(maximum, timestamp)
    if minimum is None:
        raise TraceError("RPLY contains no allocator events")
    return minimum, maximum


def pack_op(
    target_ns: int,
    size: int,
    slot: int,
    wait_generation: int,
    output_generation: int,
    aux: int,
    function: int,
    kind: int,
    flags: int,
) -> bytes:
    if size > 0xFFFFFFFFFFFFFFFF:
        raise TraceError(f"allocation size does not fit uint64: {size}")
    if aux > 0xFFFFFFFF:
        aux = 0
    return OP_RECORD.pack(
        target_ns,
        size,
        slot,
        wait_generation,
        output_generation,
        aux,
        function,
        kind,
        flags,
        0,
    )


def transform_trace(
    trace,
    record_count: int,
    minimum_timestamp: int,
    timestamp_multiplier: int,
    quantum_ns: int,
    allow_unknown: bool,
    spool,
) -> tuple[dict[int, ThreadInfo], int, int, dict[str, int]]:
    # address -> (slot, generation, publishing trace tid)
    live: dict[int, tuple[int, int, int]] = {}
    threads: dict[int, ThreadInfo] = {}
    next_slot = 0
    # A completed slot is reused only by the worker that freed it.  Therefore
    # slot reuse cannot introduce a new cross-thread ordering edge.  Runtime
    # slot memory is proportional to peak per-worker liveness, not total calls.
    available_slots: dict[int, list[tuple[int, int]]] = {}
    image_flags = 0
    counts = {
        "unknown_free": 0,
        "unknown_realloc": 0,
        "remote_free": 0,
        "remote_realloc": 0,
        "live_at_end": 0,
    }

    def acquire_slot(tid: int) -> tuple[int, int, int]:
        nonlocal next_slot
        available = available_slots.get(tid)
        if available:
            slot, completed_generation = available.pop()
            if completed_generation == 0xFFFFFFFF:
                raise TraceError(f"generation overflow for slot {slot}")
            return slot, completed_generation, completed_generation + 1
        if next_slot >= SLOT_NONE:
            raise TraceError("native replay slot count exceeds uint32")
        slot = next_slot
        next_slot += 1
        return slot, 0, 1

    def release_slot(tid: int, slot: int, generation: int) -> None:
        available_slots.setdefault(tid, []).append((slot, generation))

    def remember_live(
        address: int, value: tuple[int, int, int], record_index: int
    ) -> None:
        if address in live:
            raise TraceError(
                f"record {record_index}: allocator result 0x{address:x} "
                "is already live"
            )
        live[address] = value

    trace.seek(8)
    for record_index in range(record_count):
        raw = trace.read(RPLY_ENTRY.size)
        if len(raw) != RPLY_ENTRY.size:
            raise TraceError(f"RPLY record {record_index} is truncated")
        timestamp, func_cpu, tid, address, result, size = RPLY_ENTRY.unpack(raw)
        function = func_cpu >> 32
        cpu = func_cpu & 0xFFFFFFFF
        if function >= FUNC_COUNT:
            if 100 <= function <= 104:
                continue
            raise TraceError(
                f"record {record_index} has unsupported function {function}"
            )

        thread = threads.get(tid)
        if thread is None:
            thread = ThreadInfo(tid, cpu)
            threads[tid] = thread
        target_ns = max(timestamp - minimum_timestamp, 0)
        if target_ns > 0xFFFFFFFFFFFFFFFF // timestamp_multiplier:
            raise TraceError(
                f"record {record_index}: normalized timestamp exceeds uint64"
            )
        target_ns *= timestamp_multiplier
        if quantum_ns:
            target_ns = (target_ns // quantum_ns) * quantum_ns
        op_flags = FLAG_TRACE_SUCCEEDED if result != 0 else 0

        if function not in (FUNC_FREE, FUNC_FREE_SIZED, FUNC_REALLOC):
            slot = SLOT_NONE
            wait_generation = 0
            output_generation = 0
            if result != 0:
                slot, wait_generation, output_generation = acquire_slot(tid)
                remember_live(
                    result, (slot, output_generation, tid), record_index
                )
            aux = address if function in (
                FUNC_CALLOC,
                FUNC_POSIX_MEMALIGN,
                FUNC_ALIGNED_ALLOC,
                FUNC_MEMALIGN,
            ) else 0
            op = pack_op(
                target_ns, size, slot, wait_generation, output_generation,
                aux, function, OP_ALLOC, op_flags,
            )

        elif function in (FUNC_FREE, FUNC_FREE_SIZED):
            allocation = live.pop(address, None)
            if allocation is None:
                counts["unknown_free"] += 1
                if not allow_unknown:
                    raise TraceError(
                        f"record {record_index}: free of unmatched trace "
                        f"address 0x{address:x}; use --allow-unknown only if "
                        "free(NULL) substitution is acceptable"
                    )
                image_flags |= IMAGE_FLAG_HAS_UNKNOWN
                op = pack_op(
                    target_ns, size, SLOT_NONE, 0, 0, 0,
                    function, OP_UNKNOWN_FREE, 0,
                )
            else:
                slot, generation, owner = allocation
                if owner != tid:
                    counts["remote_free"] += 1
                if generation == 0xFFFFFFFF:
                    raise TraceError(f"generation overflow for slot {slot}")
                completed_generation = generation + 1
                release_slot(tid, slot, completed_generation)
                op = pack_op(
                    target_ns, size, slot, generation,
                    completed_generation, 0,
                    function, OP_FREE, 0,
                )

        else:
            if address == 0:
                slot = SLOT_NONE
                wait_generation = 0
                output_generation = 0
                if result != 0:
                    slot, wait_generation, output_generation = acquire_slot(tid)
                    remember_live(
                        result, (slot, output_generation, tid), record_index
                    )
                op = pack_op(
                    target_ns, size, slot, wait_generation,
                    output_generation, 0,
                    function, OP_REALLOC_NULL, op_flags,
                )
            else:
                allocation = live.pop(address, None)
                if allocation is None:
                    counts["unknown_realloc"] += 1
                    if not allow_unknown:
                        raise TraceError(
                            f"record {record_index}: realloc of unmatched trace "
                            f"address 0x{address:x}; use --allow-unknown only "
                            "if realloc(NULL, size) substitution is acceptable"
                        )
                    image_flags |= IMAGE_FLAG_HAS_UNKNOWN
                    slot = SLOT_NONE
                    wait_generation = 0
                    output_generation = 0
                    if result != 0:
                        slot, wait_generation, output_generation = acquire_slot(tid)
                        remember_live(
                            result, (slot, output_generation, tid), record_index
                        )
                    op = pack_op(
                        target_ns, size, slot, wait_generation,
                        output_generation, 0,
                        function, OP_REALLOC_NULL, op_flags,
                    )
                else:
                    slot, generation, owner = allocation
                    if owner != tid:
                        counts["remote_realloc"] += 1
                    output_generation = generation + 1
                    if output_generation > 0xFFFFFFFF:
                        raise TraceError(
                            f"generation overflow for trace address 0x{address:x}"
                        )
                    if result != 0:
                        remember_live(
                            result, (slot, output_generation, tid), record_index
                        )
                    elif size != 0:
                        # Failed trace realloc leaves the old object live, but
                        # generation still advances because this worker will
                        # publish whichever pointer the replay allocator keeps.
                        live[address] = (slot, output_generation, tid)
                    else:
                        release_slot(tid, slot, output_generation)
                    op = pack_op(
                        target_ns, size, slot, generation,
                        output_generation, 0, function, OP_REALLOC,
                        op_flags,
                    )

        spool.write(struct.pack("<Q", tid))
        spool.write(op)
        thread.count += 1

    counts["live_at_end"] = len(live)
    return threads, next_slot, image_flags, counts


def write_image(
    path: Path,
    spool,
    threads: dict[int, ThreadInfo],
    slot_count: int,
    image_flags: int,
    minimum_timestamp: int,
    maximum_timestamp: int,
) -> int:
    thread_offset = IMAGE_HEADER.size
    op_offset = thread_offset + len(threads) * THREAD_RECORD.size
    total_ops = sum(thread.count for thread in threads.values())
    if total_ops > 0xFFFFFFFF:
        raise TraceError("native replay operation count exceeds uint32")
    image_size = op_offset + total_ops * OP_RECORD.size

    first = 0
    for thread in threads.values():
        thread.first = first
        first += thread.count

    with path.open("w+b") as output:
        output.truncate(image_size)
        output.seek(0)
        output.write(IMAGE_HEADER.pack(
            IMAGE_MAGIC,
            NATIVE_VERSION,
            IMAGE_HEADER.size,
            len(threads),
            image_flags,
            slot_count,
            total_ops,
            minimum_timestamp,
            maximum_timestamp,
            thread_offset,
            op_offset,
        ))
        for thread in threads.values():
            output.write(THREAD_RECORD.pack(
                thread.tid,
                thread.first,
                thread.count,
                thread.cpu if thread.cpu <= 0xFFFFFFFF else CPU_NONE,
                0,
            ))
        output.flush()

        image = mmap.mmap(output.fileno(), image_size, access=mmap.ACCESS_WRITE)
        try:
            spool.flush()
            spool.seek(0)
            while True:
                record = spool.read(SPOOL_RECORD_SIZE)
                if not record:
                    break
                if len(record) != SPOOL_RECORD_SIZE:
                    raise TraceError("internal transformed-operation spool is truncated")
                (tid,) = struct.unpack_from("<Q", record)
                thread = threads[tid]
                destination = op_offset + (
                    thread.first + thread.cursor
                ) * OP_RECORD.size
                image[destination:destination + OP_RECORD.size] = record[8:]
                thread.cursor += 1
            image.flush()
        finally:
            image.close()
    return image_size


def build_executable(
    output: Path,
    image_path: Path,
    image_size: int,
    compiler: str,
    cflags: list[str],
) -> None:
    runtime = Path(__file__).with_name("native_replay_runtime.c")
    if not runtime.is_file():
        raise TraceError(f"native runtime source is missing: {runtime}")

    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="native-replay-build-", dir=output.parent
    ) as build_dir_text:
        build_dir = Path(build_dir_text)
        temporary_executable = build_dir / "runtime"
        command = shlex.split(compiler) + [
            "-O3",
            "-DNDEBUG",
            "-std=gnu11",
            "-pthread",
            "-Wall",
            "-Wextra",
            "-Werror",
            *cflags,
            str(runtime),
            "-o",
            str(temporary_executable),
        ]
        try:
            subprocess.run(command, check=True)
        except (OSError, subprocess.CalledProcessError) as error:
            raise TraceError(f"native runtime compilation failed: {error}") from error

        assembled = build_dir / "assembled"
        with assembled.open("wb") as destination:
            with temporary_executable.open("rb") as source:
                shutil.copyfileobj(source, destination, length=1024 * 1024)
            padding = (-destination.tell()) % 8
            if padding:
                destination.write(b"\0" * padding)
            with image_path.open("rb") as source:
                shutil.copyfileobj(source, destination, length=1024 * 1024)
            destination.write(TRAILER.pack(
                TRAILER_MAGIC, image_size, NATIVE_VERSION
            ))
        assembled.chmod(0o755)
        os.replace(assembled, output)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Offline-compile an RPLY allocator trace into a standalone, "
            "low-overhead replay executable."
        )
    )
    parser.add_argument("trace", type=Path, help="input .rply trace")
    parser.add_argument("output", type=Path, help="output executable")
    parser.add_argument(
        "--quantum-us",
        type=float,
        default=100.0,
        help=(
            "timestamp bucket width in microseconds (default: 100); "
            "use 0 for one target per exact timestamp"
        ),
    )
    parser.add_argument(
        "--timestamp-unit",
        choices=("ns", "us", "ms"),
        default="ns",
        help=(
            "unit stored in RPLY timestamps (default: ns for mini BIN-to-RPLY; "
            "older large-project traces may use ms)"
        ),
    )
    parser.add_argument(
        "--allow-unknown",
        action="store_true",
        help="replace unmatched free/realloc inputs with NULL instead of failing",
    )
    parser.add_argument(
        "--cc",
        default=os.environ.get("CC", "cc"),
        help="C compiler command, including a cross compiler if needed",
    )
    parser.add_argument(
        "--cflag",
        action="append",
        default=[],
        help="additional runtime compiler flag (repeatable)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if not math.isfinite(args.quantum_us) or args.quantum_us < 0:
        raise TraceError("--quantum-us must be non-negative")
    quantum_ns = int(args.quantum_us * 1000.0)
    timestamp_multiplier = {
        "ns": 1,
        "us": 1000,
        "ms": 1000000,
    }[args.timestamp_unit]
    if args.trace.resolve() == args.output.resolve():
        raise TraceError("input trace and output executable must differ")
    args.output.parent.mkdir(parents=True, exist_ok=True)

    with args.trace.open("rb") as trace, tempfile.TemporaryDirectory(
        prefix="trace-to-native-", dir=args.output.parent
    ) as temporary_text:
        record_count = read_header(trace)
        minimum, maximum = scan_timestamp_range(trace, record_count)
        if maximum > 0xFFFFFFFFFFFFFFFF // timestamp_multiplier:
            raise TraceError("normalized absolute timestamp exceeds uint64")
        temporary = Path(temporary_text)
        spool_path = temporary / "operations.spool"
        image_path = temporary / "image.bin"
        with spool_path.open("w+b") as spool:
            threads, slots, image_flags, counts = transform_trace(
                trace,
                record_count,
                minimum,
                timestamp_multiplier,
                quantum_ns,
                args.allow_unknown,
                spool,
            )
            image_size = write_image(
                image_path,
                spool,
                threads,
                slots,
                image_flags,
                minimum * timestamp_multiplier,
                maximum * timestamp_multiplier,
            )
        build_executable(
            args.output,
            image_path,
            image_size,
            args.cc,
            args.cflag,
        )

    total_ops = sum(thread.count for thread in threads.values())
    print(
        f"generated={args.output} records={record_count} "
        f"operations={total_ops} threads={len(threads)} slots={slots} "
        f"span_ms={(maximum - minimum) * timestamp_multiplier / 1_000_000.0:.3f} "
        f"timestamp_unit={args.timestamp_unit} quantum_us={args.quantum_us:g}"
    )
    print(
        "cross_thread_free={remote_free} "
        "cross_thread_realloc={remote_realloc} "
        "unknown_free={unknown_free} unknown_realloc={unknown_realloc} "
        "live_at_end={live_at_end}".format(**counts)
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, TraceError) as error:
        print(f"trace_to_native: {error}", file=sys.stderr)
        raise SystemExit(1)
