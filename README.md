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
- opens `/data/local/tmp/mini_hook_hits.log` once in the appspawndf constructor;
- leaves that descriptor open without `O_CLOEXEC`, so appspawn children can
  inherit it;
- maintains independent atomic `malloc` and `free` call counts after detecting
  a new PID;
- writes only at powers of two (`1, 2, 4, 8...`) through the inherited
  descriptor.

Example output:

```text
pid=691 hook=constructor
pid=691 hook=malloc count=1
pid=691 hook=malloc count=2
pid=691 hook=free count=1
pid=13175 hook=constructor
pid=13175 hook=malloc count=1
pid=13175 hook=malloc count=2
pid=13175 hook=malloc count=4
pid=13175 hook=free count=1
```

The constructor line is a diagnostic probe. If it appears for the target PID
but the `malloc` and `free` lines do not, the library is loaded and the
inherited descriptor works, but allocator calls are not resolving to this
library's exported symbols.

The hook path performs no `open`: it uses relaxed atomic increments and only
formats a short stack buffer before a sparse `SYS_write`. Even one billion
calls produce only 30 count lines per hook. If appspawn explicitly closes the
inherited descriptor, counting continues but logging is silently dropped.

When a forked child first enters a hook, the inherited counter is reset for its
new PID. The initialization path never waits on a lock; concurrent calls may
drop a very small number of early samples instead of risking a cold-start
deadlock.

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
