# Minimal malloc/free hook

This directory is an independent LD_PRELOAD test project. The shared library:

- exports only `malloc` and `free`;
- resolves the next allocator implementation once in its constructor;
- counts calls to the two hooks with atomic counters;
- writes one snapshot per second from a background thread;
- detects a PID change and restarts the writer in an appspawn child, including
  process creation paths that do not invoke `pthread_atfork` callbacks.
- blocks recursive instrumentation with a thread-local hook depth;
- uses a fixed bootstrap buffer if `dlsym` recursively calls `malloc` before
  the real allocator has been resolved.

By default, snapshots are written through the path visible inside the
OpenHarmony application sandbox:

```text
/data/storage/el2/base/haps/entry/files/<PID>
```

If that open fails, the current Douyin test build also tries the physical
system-side path
`/data/app/el2/100/base/com.ss.hm.ugc.aweme/haps/entry/files/<PID>`.

Each snapshot contains:

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
testing and for selecting another application directory. When it is unset, the
OpenHarmony Douyin application files directory above is used.

The values are cumulative hook-entry counts. They are intended to prove that
symbol interposition is active, not to provide an exact application allocation
profile: startup and injected-library activity can also contribute calls.

For an appspawn injection, the child inherits library state but not the
appspawn writer thread. The hook compares the current PID with the recorded
writer PID on every `malloc` and `free`. After a PID change it clears inherited
counters and starts a writer for the app process.

The target process must have normal DAC write permission on the output
directory. Disabling SELinux does not grant Unix directory permissions. The
configured default is the writable `files` directory owned by the target app.

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
