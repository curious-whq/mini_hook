# Minimal malloc/free replay hook

This directory contains the incremental OpenHarmony replay experiment.

## Current stage

The shared library currently:

- exports and forwards only `malloc` and `free`;
- records one fixed-size binary event after each `malloc`;
- keeps `free` as pure forwarding;
- does not use TLS or create threads;
- opens `/data/local/tmp/mini_replay.bin` once in its constructor;
- leaves the descriptor open for appspawn children to inherit;
- writes a 32-byte binary file header when the file is empty;
- appends one 48-byte `PROCESS_START` event from each constructor;
- records malloc sequence, monotonic timestamp, returned address, requested
  size, PID, and kernel TID.

The on-disk structures are defined in `replay_format.h`. `MALLOC` events use
the same 48-byte layout, and future `FREE` events will reuse it.

The malloc sequence starts at 1 for each PID. An apppool child therefore resets
its inherited sequence when its PID differs from appspawndf. During the brief
lock-free reset race, an event may be dropped rather than blocking cold start.

This stage intentionally performs one `SYS_write` per malloc event. It is a
correctness experiment, not the final performance design.

Before replacing the library on a device, remove the previous trace because
format version 1 expects its header at offset zero:

```sh
rm -f /data/local/tmp/mini_replay.bin
```

After starting the target process, pull the file and inspect it with
`mini_replay_dump`.

## Linux build and test

```sh
cmake -S mini -B mini/build
cmake --build mini/build
ctest --test-dir mini/build --output-on-failure
```

Example:

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
