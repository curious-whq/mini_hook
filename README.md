# Minimal malloc/free replay hook

This directory contains the incremental OpenHarmony replay experiment.

## Current stage

The shared library currently:

- exports and forwards `malloc` and `free`;
- records every `malloc`; `free` is still pure forwarding;
- creates a fixed 3 GiB sparse file at
  `/data/local/tmp/mini_replay.bin`;
- maps it once with `MAP_SHARED`;
- uses one shared atomic slot index across appspawndf and its children;
- writes each event directly into its 48-byte mmap slot;
- commits a slot last with `sequence = index + 1`;
- uses no TLS, logger thread, or per-event `write`.

The 64-byte v2 header contains the event capacity, shared `next_index`,
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

Before each device experiment, remove the old v1/v2 trace:

```sh
rm -f /data/local/tmp/mini_replay.bin
```

After stopping the experiment, pull the file and inspect it with:

```sh
mini/build/mini_replay_dump mini_replay.bin
```

Convert the binary trace directly into a text file:

```sh
mini/build/mini_replay_dump mini_replay.bin replay.txt
```

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
