# Minimal malloc/free hook

This directory is an independent LD_PRELOAD test project. The shared library:

- exports only `malloc` and `free`;
- resolves the next allocator implementation once in its constructor;
- counts calls to the two hooks with atomic counters;
- writes one snapshot per second from a background thread;
- restarts the writer in a child created from a preloaded appspawn process.

By default, snapshots are written to `/data/local/tmp/<PID>`:

```text
malloc=12345
free=12001
```

## Build and test

```sh
cmake -S mini -B mini/build
cmake --build mini/build
ctest --test-dir mini/build --output-on-failure
```

Run another program with the hook:

```sh
MINI_HOOK_OUTPUT_DIR=/tmp \
LD_PRELOAD="$PWD/mini/build/libmini_malloc_free_hook.so" \
/path/to/program
```

Then inspect `/tmp/<PID>`. `MINI_HOOK_OUTPUT_DIR` is intended for local Linux
testing; when it is unset, the OpenHarmony path `/data/local/tmp` is used.

The values are cumulative hook-entry counts. They are intended to prove that
symbol interposition is active, not to provide an exact application allocation
profile: startup and injected-library activity can also contribute calls.

For an appspawn injection, the app process is created by `fork`. The hook
clears inherited counters in the child and starts a new writer when that child
first calls `malloc` or `free`, so the filename uses the app process PID rather
than only the appspawn PID.

For an OpenHarmony cross build, use the same commands with the OpenHarmony
SDK CMake toolchain file. The resulting `.so` has no dependency on the parent
project.

## OpenHarmony GN build

Place or link `mini` inside the OpenHarmony source tree, then add this target to
the relevant product or module dependency list:

```gn
"//path/to/mini:mini_malloc_free_hook"
```

Build it directly from the OpenHarmony source root:

```sh
./build.sh --product-name <product> \
  --build-target //path/to/mini:mini_malloc_free_hook
```

The GN output is `libmini_malloc_free_hook.so`.

This standalone target does not set `part_name`, `subsystem_name`, or install
rules. If the library must be included in a system image, add the target to an
existing OpenHarmony component and let that component provide the packaging
metadata.
# mini_hook
