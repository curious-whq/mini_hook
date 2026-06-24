#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dlfcn.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#define BOOTSTRAP_HEAP_CAPACITY (1024 * 1024)
#ifndef MARKER_DIRECTORY
#define MARKER_DIRECTORY "/data/storage/el2/base/haps/entry/files"
#endif
#define MARKER_PATH_CAPACITY 320

typedef void *(*malloc_fn)(size_t);
typedef void (*free_fn)(void *);

static _Atomic(malloc_fn) real_malloc;
static _Atomic(free_fn) real_free;
static atomic_flag resolver_lock = ATOMIC_FLAG_INIT;

static union {
    max_align_t alignment;
    unsigned char bytes[BOOTSTRAP_HEAP_CAPACITY];
} bootstrap_heap;
static _Atomic size_t bootstrap_offset;
static _Atomic uint64_t malloc_marker_pid;
static _Atomic uint64_t free_marker_pid;

static void *bootstrap_malloc(size_t size)
{
    const size_t alignment = _Alignof(max_align_t);
    size_t allocation_size = size == 0 ? 1 : size;

    if (allocation_size > SIZE_MAX - (alignment - 1)) {
        return NULL;
    }
    allocation_size =
        ((allocation_size + alignment - 1) / alignment) * alignment;

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
            return &bootstrap_heap.bytes[offset];
        }
    }
}

static bool is_bootstrap_pointer(const void *ptr)
{
    if (ptr == NULL) {
        return false;
    }

    const uintptr_t address = (uintptr_t)ptr;
    const uintptr_t begin = (uintptr_t)&bootstrap_heap.bytes[0];
    const uintptr_t end =
        (uintptr_t)&bootstrap_heap.bytes[sizeof(bootstrap_heap.bytes)];
    return address >= begin && address < end;
}

static void resolve_real_allocators(void)
{
    if (atomic_load_explicit(&real_malloc, memory_order_acquire) != NULL &&
        atomic_load_explicit(&real_free, memory_order_acquire) != NULL) {
        return;
    }

    if (atomic_flag_test_and_set_explicit(
            &resolver_lock, memory_order_acquire)) {
        return;
    }

    if (atomic_load_explicit(&real_malloc, memory_order_relaxed) == NULL) {
        atomic_store_explicit(
            &real_malloc, (malloc_fn)dlsym(RTLD_NEXT, "malloc"),
            memory_order_release);
    }
    if (atomic_load_explicit(&real_free, memory_order_relaxed) == NULL) {
        atomic_store_explicit(
            &real_free, (free_fn)dlsym(RTLD_NEXT, "free"),
            memory_order_release);
    }

    atomic_flag_clear_explicit(&resolver_lock, memory_order_release);
}

static size_t append_text(char *buffer, size_t offset, const char *text)
{
    while (*text != '\0') {
        buffer[offset++] = *text++;
    }
    return offset;
}

static size_t append_u64(char *buffer, size_t offset, uint64_t value)
{
    char digits[20];
    size_t count = 0;

    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);

    while (count != 0) {
        buffer[offset++] = digits[--count];
    }
    return offset;
}

static void mark_hook_once(
    _Atomic uint64_t *marker_pid, pid_t pid, const char *suffix)
{
    const uint64_t current_pid = (uint64_t)pid;
    uint64_t recorded_pid =
        atomic_load_explicit(marker_pid, memory_order_acquire);

    if (recorded_pid == current_pid ||
        !atomic_compare_exchange_strong_explicit(
            marker_pid, &recorded_pid, current_pid,
            memory_order_acq_rel, memory_order_acquire)) {
        return;
    }

    char path[MARKER_PATH_CAPACITY];
    size_t offset = append_text(path, 0, MARKER_DIRECTORY);
    path[offset++] = '/';
    offset = append_u64(path, offset, current_pid);
    offset = append_text(path, offset, suffix);
    path[offset] = '\0';

    int fd = open(
        path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd >= 0) {
        close(fd);
    }
}

__attribute__((constructor)) static void initialize_hook(void)
{
    resolve_real_allocators();
}

__attribute__((visibility("default"))) void *malloc(size_t size)
{
    malloc_fn function =
        atomic_load_explicit(&real_malloc, memory_order_acquire);
    if (function == NULL) {
        resolve_real_allocators();
        function = atomic_load_explicit(
            &real_malloc, memory_order_acquire);
    }

    void *ptr =
        function != NULL ? function(size) : bootstrap_malloc(size);
    mark_hook_once(&malloc_marker_pid, getpid(), ".malloc");
    return ptr;
}

__attribute__((visibility("default"))) void free(void *ptr)
{
    mark_hook_once(&free_marker_pid, getpid(), ".free");

    if (ptr == NULL || is_bootstrap_pointer(ptr)) {
        return;
    }

    free_fn function =
        atomic_load_explicit(&real_free, memory_order_acquire);
    if (function == NULL) {
        resolve_real_allocators();
        function = atomic_load_explicit(
            &real_free, memory_order_acquire);
    }
    if (function != NULL) {
        function(ptr);
    }
}
