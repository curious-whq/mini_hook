#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dlfcn.h>
#include <stddef.h>

typedef void *(*malloc_fn)(size_t);
typedef void (*free_fn)(void *);

static malloc_fn real_malloc;
static free_fn real_free;

__attribute__((constructor)) static void resolve_allocator(void)
{
    real_malloc = (malloc_fn)dlsym(RTLD_NEXT, "malloc");
    real_free = (free_fn)dlsym(RTLD_NEXT, "free");
}

__attribute__((visibility("default"))) void *malloc(size_t size)
{
    return real_malloc(size);
}

__attribute__((visibility("default"))) void free(void *ptr)
{
    real_free(ptr);
}
