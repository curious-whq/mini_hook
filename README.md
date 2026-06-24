# Minimal malloc/free replay hook

This directory contains the incremental OpenHarmony replay experiment.

## Current stage

The shared library currently:

- exports and forwards only `malloc` and `free`;
- does not record allocator calls yet;
- does not use TLS or create threads;
- opens `/data/local/tmp/mini_replay.bin` once in its constructor;
- leaves the descriptor open for appspawn children to inherit;
- writes a 32-byte binary file header when the file is empty;
- appends one 48-byte `PROCESS_START` event from each constructor.

The on-disk structures are defined in `replay_format.h`. Future `MALLOC` and
`FREE` events will use the same 48-byte event layout.

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
