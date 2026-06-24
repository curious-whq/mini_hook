# Minimal malloc/free hook

This directory contains a phase-one LD_PRELOAD test library for Linux and
OpenHarmony.

The shared library:

- exports only `malloc` and `free`;
- does not use TLS;
- does not count calls;
- does not create threads;
- resolves the real allocator through `RTLD_NEXT`;
- uses a fixed emergency buffer if allocator resolution recursively enters
  `malloc`;
- ignores `free` calls for pointers from that emergency buffer.
- creates one marker per process after entering each hook:
  `/data/storage/el2/base/haps/entry/files/<PID>.malloc` and
  `/data/storage/el2/base/haps/entry/files/<PID>.free`.

The PID marker state is inherited across appspawn forks, but a child PID differs
from its parent PID, so each child creates its own markers. The marker is
claimed atomically before `open`, preventing recursive marker creation if file
operations internally allocate memory.

Each process attempts each marker only once. A failed `open` is not retried from
later allocator calls. This is intentional: the application sandbox path may
not be mounted during early appspawn cold start, and retrying from every
`malloc` or `free` can create an `open` storm and cause launch timeout.

## Linux build and test

```sh
cmake -S mini -B mini/build
cmake --build mini/build
ctest --test-dir mini/build --output-on-failure
```

Run another program:

```sh
LD_PRELOAD="$PWD/mini/build/libmini_malloc_free_hook.so" /path/to/program
```

## OpenHarmony GN build

Add the target to the appropriate product or module dependency list:

```gn
"//path/to/mini:mini_malloc_free_hook"
```

Build it from the OpenHarmony source root:

```sh
./build.sh --product-name <product> \
  --build-target //path/to/mini:mini_malloc_free_hook
```

The output is `libmini_malloc_free_hook.so`.
