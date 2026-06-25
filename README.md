# Minimal malloc/free replay hook

This directory contains the incremental OpenHarmony replay experiment.

## Current stage

The shared library currently:

- exports and records `malloc`, `free`, `calloc`, `realloc`, `free_sized`,
  `posix_memalign`, `aligned_alloc`, `valloc`, `memalign`, and `pvalloc`;
- creates a new 3 GiB sparse file named
  `/data/local/tmp/mini_replay_<realtime-ns>_<creator-pid>.bin`;
- maps it once with `MAP_SHARED`;
- uses one shared atomic slot index across appspawndf and its children;
- writes each event directly into its 48-byte mmap slot;
- commits a slot last with its 32-bit `sequence = index + 1`;
- uses no TLS, logger thread, or per-event `write`.

The 48-byte v3 event layout stores timestamp, old/input address, result
address, size, PID, TID, function type, flags, and a final commit sequence.
Allocation events store the returned pointer in `result`. Free events store
the released pointer in `address`. Realloc stores both its old pointer and
returned pointer in one event.

Free events are committed before calling the real allocator. Null frees and
pointers owned by the early bootstrap allocator are not sent to the real
allocator and are not recorded.

The 64-byte v3 header contains the event capacity, shared `next_index`,
initialization state, and runtime flags. The event format remains 48 bytes.

A 3 GiB mapping has capacity for roughly 67 million events. The file is
initially sparse: `ftruncate` establishes its logical size, while physical
blocks are allocated as pages are written.

No event is silently discarded. If the mapping becomes full, the hook sets the
overflow flag and terminates the process with exit code 75 before allowing
execution to continue without a trace slot.

Initialization failures are also fatal. If the file cannot be opened, resized,
mapped, or validated, the process exits instead of running with an incomplete
trace.

The default OpenHarmony build uses `O_EXCL`, so it never clears or appends to
an older experiment. Forked app processes inherit the creator's mapping and
continue writing into that same file. Old traces can be removed when they are
no longer needed:

```sh
rm -f /data/local/tmp/mini_replay_*.bin
```

After stopping the experiment, pull the file and inspect it with:

```sh
mini/build/mini_replay_dump mini_replay.bin
```

Convert the binary trace directly into a text file:

```sh
mini/build/mini_replay_dump mini_replay.bin replay.txt
```

Convert one process from BIN to the large project's RPLY format:

```sh
mini/build/mini_bin_to_rply mini_replay.bin <pid> trace.rply
```

Only allocator events whose recorded PID equals `<pid>` are copied. Incomplete
slots and unknown event types are reported to stderr, counted in the summary,
and skipped so the remaining events can still be converted. A physically
truncated event remains fatal because the next record boundary cannot be read
reliably.

Convert RPLY to readable text:

```sh
mini/build/mini_rply_to_txt trace.rply replay.txt
```

The RPLY header stores its length in 64-bit words, so the converter writes
`idx = record_count * 6`, followed by 48-byte replay entries.
The current BIN format does not record the CPU number, so converted RPLY
entries use CPU 0. PID filtering does not change event order.

## Replay

Build and replay an RPLY trace with human-readable statistics:

```sh
cmake --build mini/build
mini/build/mini_replay_main -S trace.rply
```

JSON output, which is also consumed by `bench_replay.py`:

```sh
mini/build/mini_replay_main --json trace.rply
```

Useful modes:

```sh
# Fail when the trace contains unmatched free/realloc events.
mini/build/mini_replay_main -S --unknown-policy error trace.rply

# Run all replay workers immediately without timestamp epochs.
mini/build/mini_replay_main -S --free-run trace.rply

# Abort and diagnose a slot dependency that makes no progress for 10 seconds.
mini/build/mini_replay_main -S \
  --dependency-timeout-ms 10000 trace.rply

# Touch allocated memory during replay.
mini/build/mini_replay_main -S --touch alloc trace.rply

# Show every available option.
mini/build/mini_replay_main --help
```

Replay checks every `pthread_create` and `pthread_join`. Resource exhaustion
therefore produces an error containing the worker's trace TID instead of
leaving the epoch scheduler waiting forever. Slot-generation waits default to
a 30-second diagnostic timeout; pass `--dependency-timeout-ms 0` only when an
unbounded wait is explicitly desired.

## Allocator benchmark

`bench_replay.py` runs the same RPLY repeatedly with glibc and every `.so`
found in `mini/allocator_dir`. Its default replay binary is
`mini/build/mini_replay_main`.

```sh
mkdir -p mini/allocator_dir
python3 mini/bench_replay.py trace.rply
```

Run only the glibc baseline once:

```sh
python3 mini/bench_replay.py trace.rply -n 1 \
  -a /tmp/empty-allocator-dir
```

Run selected allocator libraries and forward options to replay:

```sh
python3 mini/bench_replay.py trace.rply \
  --only jemalloc \
  --replay-arg=--free-run \
  --replay-arg=--touch \
  --replay-arg=alloc
```

For controlled tests, defining `REPLAY_LOG_PATH` at compile time keeps the
previous fixed-path behavior. The CMake test build uses this mode.

The parser reads only `next_index` slots rather than scanning the full 3 GiB.
An event is valid only when its stored sequence equals its slot index plus one.

## Linux build and test

```sh
cmake -S mini -B mini/build
cmake --build mini/build
ctest --test-dir mini/build --output-on-failure
```

For a local trace:

```sh
rm -f /tmp/mini_replay.bin
cc -std=gnu11 -fPIC -shared \
  -DREPLAY_LOG_PATH='"/tmp/mini_replay.bin"' \
  mini/malloc_free_hook.c -ldl \
  -o /tmp/libmini_replay.so
LD_PRELOAD=/tmp/libmini_replay.so /bin/true
mini/build/mini_replay_dump /tmp/mini_replay.bin
```

## OpenHarmony GN build

```gn
"//path/to/mini:libhook_anymem"
```

Build from the OpenHarmony source root using the product's normal build
command. The shared-library target is defined in `BUILD.gn`.
