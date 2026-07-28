#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dlfcn.h>
#include <stdlib.h>
#include <unistd.h>

typedef int (*snapshot_fn)(void);

static void finish(int code)
{
    _exit(code);
}

int main(void)
{
    snapshot_fn snapshot =
        (snapshot_fn)dlsym(RTLD_DEFAULT, "mini_hole_snapshot_now");
    if (snapshot == NULL || snapshot() != 0) {
        finish(1);
    }

    volatile unsigned char *seven = malloc(7);
    volatile unsigned char *nine = malloc(9);
    if (seven == NULL || nine == NULL) {
        finish(2);
    }
    seven[0] = 7;
    nine[0] = 9;
    if (snapshot() != 0) {
        finish(3);
    }

    volatile unsigned char *sixteen =
        realloc((void *)nine, 16);
    if (sixteen == NULL) {
        finish(4);
    }
    sixteen[0] = 16;
    if (snapshot() != 0) {
        finish(5);
    }

    free((void *)seven);
    if (snapshot() != 0) {
        finish(6);
    }

    free((void *)sixteen);
    if (snapshot() != 0) {
        finish(7);
    }
    finish(0);
    return 0;
}
