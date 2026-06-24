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
- writes at most one `malloc` line and one `free` line per PID through the
  inherited descriptor.

Example output:

```text
pid=691 hook=constructor
pid=691 hook=malloc
pid=691 hook=free
pid=13175 hook=constructor
pid=13175 hook=malloc
pid=13175 hook=free
```

The constructor line is a diagnostic probe. If it appears for the target PID
but the `malloc` and `free` lines do not, the library is loaded and the
inherited descriptor works, but allocator calls are not resolving to this
library's exported symbols.

The hook path performs no `open`: it only formats a short stack buffer and uses
`SYS_write`. This avoids depending on the child sandbox mount during cold
start. If appspawn explicitly closes the inherited descriptor, the hook simply
does not log and does not retry.

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
