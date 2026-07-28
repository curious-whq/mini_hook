#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dlfcn.h>
#include <stdlib.h>
#include <unistd.h>

#define POINTER_COUNT 4096

typedef int (*snapshot_fn)(void);

static void *pointers[POINTER_COUNT];

int main(void)
{
    snapshot_fn snapshot =
        (snapshot_fn)dlsym(RTLD_DEFAULT, "mini_hole_snapshot_now");
    if (snapshot == NULL || snapshot() != 0) {
        _exit(1);
    }
    for (size_t i = 0; i < POINTER_COUNT; ++i) {
        pointers[i] = malloc(9);
        if (pointers[i] == NULL) {
            _exit(2);
        }
    }
    if (snapshot() != 0) {
        _exit(3);
    }
    for (size_t i = 0; i < POINTER_COUNT; ++i) {
        free(pointers[i]);
    }
    if (snapshot() != 0) {
        _exit(4);
    }
    _exit(0);
}
