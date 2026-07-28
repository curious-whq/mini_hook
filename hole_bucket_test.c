#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dlfcn.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>

typedef int (*snapshot_fn)(void);

int main(void)
{
    static const size_t sizes[] = {
        1, 8, 9,
        255, 256, 257,
        511, 512, 513,
        559, 560, 561,
        655, 656, 657,
        783, 784, 785,
        1023, 1024, 1025,
        1311, 1312, 1313,
        2047, 2048, 2049,
        4095, 4096, 4097,
        4223, 4224, 4225,
        8191, 8192, 8193,
        14335, 14336, 14337,
        16383, 16384, 16385,
        40959, 40960, 40961,
        262143, 262144, 262145,
    };
    void *pointers[sizeof(sizes) / sizeof(sizes[0])] = {0};
    snapshot_fn snapshot =
        (snapshot_fn)dlsym(RTLD_DEFAULT, "mini_hole_snapshot_now");
    if (snapshot == NULL || snapshot() != 0) {
        _exit(1);
    }

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        pointers[i] = malloc(sizes[i]);
        if (pointers[i] == NULL) {
            _exit(2);
        }
        ((volatile unsigned char *)pointers[i])[0] =
            (unsigned char)i;
    }
    if (snapshot() != 0) {
        _exit(3);
    }

    _exit(0);
}
