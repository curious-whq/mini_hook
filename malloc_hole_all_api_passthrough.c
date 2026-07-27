#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dlfcn.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Diagnostic control group:
 *   - forwards every allocator API used by malloc_hole_hook.c;
 *   - performs no statistics, file I/O, TLS access, pthread initialization,
 *     atfork registration, or writer creation;
 *   - resolves symbols eagerly, using the same bootstrap strategy as the
 *     appspawndf-compatible malloc_free_hook.c.
 */

#define BOOTSTRAP_HEAP_CAPACITY (1024U * 1024U)

typedef void *(*malloc_fn)(size_t);
typedef void (*free_fn)(void *);
typedef void *(*calloc_fn)(size_t, size_t);
typedef void *(*realloc_fn)(void *, size_t);
typedef void (*free_sized_fn)(void *, size_t);
typedef int (*posix_memalign_fn)(void **, size_t, size_t);
typedef void *(*aligned_alloc_fn)(size_t, size_t);
typedef void *(*valloc_fn)(size_t);
typedef void *(*memalign_fn)(size_t, size_t);
typedef void *(*pvalloc_fn)(size_t);

static _Atomic(malloc_fn) real_malloc;
static _Atomic(free_fn) real_free;
static _Atomic(calloc_fn) real_calloc;
static _Atomic(realloc_fn) real_realloc;
static _Atomic(free_sized_fn) real_free_sized;
static _Atomic(posix_memalign_fn) real_posix_memalign;
static _Atomic(aligned_alloc_fn) real_aligned_alloc;
static _Atomic(valloc_fn) real_valloc;
static _Atomic(memalign_fn) real_memalign;
static _Atomic(pvalloc_fn) real_pvalloc;
static atomic_bool resolver_complete;
static atomic_flag resolver_lock = ATOMIC_FLAG_INIT;

typedef union {
    max_align_t alignment;
    struct {
        size_t size;
    } metadata;
} BootstrapHeader;

static union {
    max_align_t alignment;
    unsigned char bytes[BOOTSTRAP_HEAP_CAPACITY];
} bootstrap_heap;
static _Atomic size_t bootstrap_offset;

static void copy_bytes(
    unsigned char *destination, const unsigned char *source, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        destination[i] = source[i];
    }
}

static void zero_bytes(unsigned char *destination, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        destination[i] = 0;
    }
}

static void *bootstrap_malloc(size_t size)
{
    const size_t alignment = _Alignof(max_align_t);
    size_t allocation_size = size == 0 ? 1 : size;
    if (allocation_size > SIZE_MAX - (alignment - 1) ||
        allocation_size + alignment - 1 >
            SIZE_MAX - sizeof(BootstrapHeader)) {
        return NULL;
    }
    allocation_size =
        ((allocation_size + alignment - 1) / alignment) * alignment;
    allocation_size += sizeof(BootstrapHeader);

    size_t offset = atomic_load_explicit(
        &bootstrap_offset, memory_order_relaxed);
    for (;;) {
        if (offset > sizeof(bootstrap_heap.bytes) ||
            allocation_size > sizeof(bootstrap_heap.bytes) - offset) {
            return NULL;
        }
        if (atomic_compare_exchange_weak_explicit(
                &bootstrap_offset, &offset, offset + allocation_size,
                memory_order_relaxed, memory_order_relaxed)) {
            BootstrapHeader *header =
                (BootstrapHeader *)&bootstrap_heap.bytes[offset];
            header->metadata.size = size;
            return header + 1;
        }
    }
}

static bool is_bootstrap_pointer(const void *ptr)
{
    if (ptr == NULL) {
        return false;
    }
    uintptr_t address = (uintptr_t)ptr;
    uintptr_t begin = (uintptr_t)&bootstrap_heap.bytes[0];
    uintptr_t end =
        (uintptr_t)&bootstrap_heap.bytes[sizeof(bootstrap_heap.bytes)];
    return address >= begin && address < end;
}

static size_t bootstrap_pointer_size(const void *ptr)
{
    const BootstrapHeader *header = (const BootstrapHeader *)ptr - 1;
    return header->metadata.size;
}

static void *bootstrap_calloc(size_t count, size_t size)
{
    if (size != 0 && count > SIZE_MAX / size) {
        return NULL;
    }
    size_t total = count * size;
    void *ptr = bootstrap_malloc(total);
    if (ptr != NULL) {
        zero_bytes(ptr, total);
    }
    return ptr;
}

static void *bootstrap_realloc(void *ptr, size_t size)
{
    if (ptr == NULL) {
        return bootstrap_malloc(size);
    }
    if (!is_bootstrap_pointer(ptr) || size == 0) {
        return NULL;
    }
    void *new_ptr = bootstrap_malloc(size);
    if (new_ptr != NULL) {
        size_t old_size = bootstrap_pointer_size(ptr);
        copy_bytes(new_ptr, ptr, old_size < size ? old_size : size);
    }
    return new_ptr;
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

#define RESOLVE(target, type, name)                                       \
    do {                                                                  \
        if (atomic_load_explicit(&(target), memory_order_relaxed) == NULL) {\
            atomic_store_explicit(                                        \
                &(target), (type)dlsym(RTLD_NEXT, (name)),                \
                memory_order_release);                                    \
        }                                                                 \
    } while (0)
    RESOLVE(real_malloc, malloc_fn, "malloc");
    RESOLVE(real_free, free_fn, "free");
    RESOLVE(real_calloc, calloc_fn, "calloc");
    RESOLVE(real_realloc, realloc_fn, "realloc");
    RESOLVE(real_free_sized, free_sized_fn, "free_sized");
    RESOLVE(real_posix_memalign, posix_memalign_fn, "posix_memalign");
    RESOLVE(real_aligned_alloc, aligned_alloc_fn, "aligned_alloc");
    RESOLVE(real_valloc, valloc_fn, "valloc");
    RESOLVE(real_memalign, memalign_fn, "memalign");
    RESOLVE(real_pvalloc, pvalloc_fn, "pvalloc");
#undef RESOLVE

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
void *calloc(size_t count, size_t size)
{
    calloc_fn function =
        atomic_load_explicit(&real_calloc, memory_order_acquire);
    if (function == NULL) {
        resolve_real_allocators();
        function =
            atomic_load_explicit(&real_calloc, memory_order_acquire);
    }
    return function != NULL ?
        function(count, size) : bootstrap_calloc(count, size);
}

__attribute__((visibility("default")))
void *realloc(void *ptr, size_t size)
{
    if (is_bootstrap_pointer(ptr)) {
        return bootstrap_realloc(ptr, size);
    }
    realloc_fn function =
        atomic_load_explicit(&real_realloc, memory_order_acquire);
    if (function == NULL) {
        resolve_real_allocators();
        function =
            atomic_load_explicit(&real_realloc, memory_order_acquire);
    }
    return function != NULL ? function(ptr, size) : NULL;
}

__attribute__((visibility("default")))
void free_sized(void *ptr, size_t size)
{
    if (ptr == NULL || is_bootstrap_pointer(ptr)) {
        return;
    }
    free_sized_fn function =
        atomic_load_explicit(&real_free_sized, memory_order_acquire);
    free_fn fallback =
        atomic_load_explicit(&real_free, memory_order_acquire);
    if (function == NULL && fallback == NULL) {
        resolve_real_allocators();
        function = atomic_load_explicit(
            &real_free_sized, memory_order_acquire);
        fallback =
            atomic_load_explicit(&real_free, memory_order_acquire);
    }
    if (function != NULL) {
        function(ptr, size);
    } else if (fallback != NULL) {
        fallback(ptr);
    }
}

__attribute__((visibility("default")))
int posix_memalign(void **memptr, size_t alignment, size_t size)
{
    posix_memalign_fn function = atomic_load_explicit(
        &real_posix_memalign, memory_order_acquire);
    if (function == NULL) {
        resolve_real_allocators();
        function = atomic_load_explicit(
            &real_posix_memalign, memory_order_acquire);
    }
    if (function == NULL) {
        return ENOMEM;
    }
    return function(memptr, alignment, size);
}

__attribute__((visibility("default")))
void *aligned_alloc(size_t alignment, size_t size)
{
    aligned_alloc_fn function = atomic_load_explicit(
        &real_aligned_alloc, memory_order_acquire);
    if (function == NULL) {
        resolve_real_allocators();
        function = atomic_load_explicit(
            &real_aligned_alloc, memory_order_acquire);
    }
    return function != NULL ? function(alignment, size) : NULL;
}

__attribute__((visibility("default")))
void *valloc(size_t size)
{
    valloc_fn function =
        atomic_load_explicit(&real_valloc, memory_order_acquire);
    if (function == NULL) {
        resolve_real_allocators();
        function =
            atomic_load_explicit(&real_valloc, memory_order_acquire);
    }
    return function != NULL ? function(size) : NULL;
}

__attribute__((visibility("default")))
void *memalign(size_t alignment, size_t size)
{
    memalign_fn function =
        atomic_load_explicit(&real_memalign, memory_order_acquire);
    if (function == NULL) {
        resolve_real_allocators();
        function =
            atomic_load_explicit(&real_memalign, memory_order_acquire);
    }
    return function != NULL ? function(alignment, size) : NULL;
}

__attribute__((visibility("default")))
void *pvalloc(size_t size)
{
    pvalloc_fn function =
        atomic_load_explicit(&real_pvalloc, memory_order_acquire);
    if (function == NULL) {
        resolve_real_allocators();
        function =
            atomic_load_explicit(&real_pvalloc, memory_order_acquire);
    }
    return function != NULL ? function(size) : NULL;
}
