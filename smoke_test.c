#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dlfcn.h>
#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>

typedef void (*free_sized_fn)(void *, size_t);

static int pointer_is_aligned(const void *ptr, size_t alignment)
{
    return (uintptr_t)ptr % alignment == 0;
}

int main(void)
{
    volatile unsigned char *ptr = malloc(64);
    if (ptr == NULL) {
        return 1;
    }

    ptr[0] = 0x5a;
    free((void *)ptr);

    ptr = calloc(8, 16);
    if (ptr == NULL || ptr[0] != 0) {
        return 2;
    }

    ptr = realloc((void *)ptr, 256);
    if (ptr == NULL) {
        return 3;
    }
    free((void *)ptr);

    ptr = malloc(32);
    free_sized_fn free_sized_function =
        (free_sized_fn)dlsym(RTLD_DEFAULT, "free_sized");
    if (ptr == NULL || free_sized_function == NULL) {
        return 4;
    }
    free_sized_function((void *)ptr, 32);

    void *aligned_ptr = NULL;
    if (posix_memalign(&aligned_ptr, 64, 96) != 0 ||
        aligned_ptr == NULL || !pointer_is_aligned(aligned_ptr, 64)) {
        return 5;
    }
    free(aligned_ptr);

    aligned_ptr = aligned_alloc(64, 128);
    if (aligned_ptr == NULL ||
        !pointer_is_aligned(aligned_ptr, 64)) {
        return 6;
    }
    free(aligned_ptr);

    aligned_ptr = valloc(77);
    if (aligned_ptr == NULL) {
        return 7;
    }
    free(aligned_ptr);

    aligned_ptr = memalign(128, 99);
    if (aligned_ptr == NULL ||
        !pointer_is_aligned(aligned_ptr, 128)) {
        return 8;
    }
    free(aligned_ptr);

    aligned_ptr = pvalloc(123);
    if (aligned_ptr == NULL) {
        return 9;
    }
    free(aligned_ptr);
    return 0;
}
