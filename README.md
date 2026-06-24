# Minimal malloc/free hook

This directory is an independent LD_PRELOAD test project. The shared library:

- exports only `malloc` and `free`;
- resolves the next allocator implementation once in its constructor;
- only forwards calls inside the two hooks;
- creates no files, threads, locks, mappings, signals, or logs.

## Build and test

```sh
cmake -S mini -B mini/build
cmake --build mini/build
ctest --test-dir mini/build --output-on-failure
```

Run another program with the hook:

```sh
LD_PRELOAD="$PWD/mini/build/libmini_malloc_free_hook.so" /path/to/program
```

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
