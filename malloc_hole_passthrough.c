#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dlfcn.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <unistd.h>

/*
 * Diagnostic allocator passthrough known to start appspawndf.
 * Keep this independent from malloc_hole_hook.c so future statistics changes
 * cannot accidentally change the control experiment.
 */

#define BOOTSTRAP_HEAP_CAPACITY (1024U * 1024U)

typedef void *(*malloc_fn)(size_t);
typedef void (*free_fn)(void *);

static _Atomic(malloc_fn) real_malloc;
static _Atomic(free_fn) real_free;
static atomic_bool resolver_complete;
static atomic_flag resolver_lock = ATOMIC_FLAG_INIT;

typedef union {
    max_align_t alignment;
    unsigned char bytes[BOOTSTRAP_HEAP_CAPACITY];
} BootstrapHeap;

static BootstrapHeap bootstrap_heap;
static _Atomic size_t bootstrap_offset;

static void *bootstrap_malloc(size_t size)
{
    const size_t alignment = _Alignof(max_align_t);
    size_t amount = size == 0 ? 1 : size;
    if (amount > SIZE_MAX - (alignment - 1)) {
        return NULL;
    }
    amount = ((amount + alignment - 1) / alignment) * alignment;

    size_t offset = atomic_fetch_add_explicit(
        &bootstrap_offset, amount, memory_order_relaxed);
    if (offset > sizeof(bootstrap_heap.bytes) ||
        amount > sizeof(bootstrap_heap.bytes) - offset) {
        return NULL;
    }
    return &bootstrap_heap.bytes[offset];
}

static bool is_bootstrap_pointer(const void *ptr)
{
    uintptr_t address = (uintptr_t)ptr;
    uintptr_t begin = (uintptr_t)&bootstrap_heap.bytes[0];
    uintptr_t end =
        (uintptr_t)&bootstrap_heap.bytes[sizeof(bootstrap_heap.bytes)];
    return ptr != NULL && address >= begin && address < end;
}

static void resolve_real_allocators(void)
{
    if (atomic_load_explicit(&resolver_complete, memory_order_acquire)) {
        return;
    }
    if (atomic_flag_test_and_set_explicit(
            &resolver_lock, memory_order_acquire)) {
        return;
    }
    if (atomic_load_explicit(
            &real_malloc, memory_order_relaxed) == NULL) {
        atomic_store_explicit(
            &real_malloc, (malloc_fn)dlsym(RTLD_NEXT, "malloc"),
            memory_order_release);
    }
    if (atomic_load_explicit(
            &real_free, memory_order_relaxed) == NULL) {
        atomic_store_explicit(
            &real_free, (free_fn)dlsym(RTLD_NEXT, "free"),
            memory_order_release);
    }
    atomic_store_explicit(
        &resolver_complete, true, memory_order_release);
    atomic_flag_clear_explicit(&resolver_lock, memory_order_release);
}

__attribute__((constructor)) static void initialize_passthrough(void)
{
    resolve_real_allocators();
}

__attribute__((visibility("default"))) void *malloc(size_t size)
{
    malloc_fn function =
        atomic_load_explicit(&real_malloc, memory_order_acquire);
    if (function == NULL) {
        resolve_real_allocators();
        function =
            atomic_load_explicit(&real_malloc, memory_order_acquire);
    }
    return function != NULL ? function(size) : bootstrap_malloc(size);
}

__attribute__((visibility("default"))) void free(void *ptr)
{
    if (ptr == NULL || is_bootstrap_pointer(ptr)) {
        return;
    }
    free_fn function =
        atomic_load_explicit(&real_free, memory_order_acquire);
    if (function == NULL) {
        resolve_real_allocators();
        function =
            atomic_load_explicit(&real_free, memory_order_acquire);
    }
    if (function != NULL) {
        function(ptr);
    }
}

__attribute__((visibility("default")))
void free_sized(void *ptr, size_t size)
{
    (void)size;
    free(ptr);
}
