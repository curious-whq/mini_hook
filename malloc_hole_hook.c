#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(MINI_HOLE_LOADER_PROBE)

/*
 * Loader-only probe:
 *   - does not export malloc/free;
 *   - has no dlsym, atomics, TLS, pthread, timer, or destructor;
 *   - only leaves a marker proving that the dynamic loader ran this DSO's
 *     constructor.
 */

#define PROBE_PATH_CAPACITY 128U

static size_t probe_append_text(
    char *buffer, size_t capacity, size_t offset, const char *text)
{
    while (*text != '\0' && offset + 1 < capacity) {
        buffer[offset++] = *text++;
    }
    return offset;
}

static size_t probe_append_u64(
    char *buffer, size_t capacity, size_t offset, uint64_t value)
{
    char digits[20];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count != 0 && offset + 1 < capacity) {
        buffer[offset++] = digits[--count];
    }
    return offset;
}

__attribute__((constructor)) static void initialize_loader_probe(void)
{
    uint64_t pid = (uint64_t)syscall(SYS_getpid);
    char path[PROBE_PATH_CAPACITY];
    size_t offset = probe_append_text(
        path, sizeof(path), 0,
        "/data/local/tmp/mini_hole_probe_");
    offset = probe_append_u64(path, sizeof(path), offset, pid);
    offset = probe_append_text(path, sizeof(path), offset, ".log");
    path[offset] = '\0';

    int fd = (int)syscall(
        SYS_openat, AT_FDCWD, path,
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        static const char marker[] =
            "mini_hole_loader_probe_loaded\n";
        (void)syscall(
            SYS_write, fd, marker, sizeof(marker) - 1);
        (void)syscall(SYS_close, fd);
    }
}

__attribute__((visibility("default")))
int mini_hole_loader_probe(void)
{
    return 0;
}

#else

/*
 * Minimal malloc-hole hook (v0).
 *
 * This intentionally does only enough work to validate that appspawndf can
 * start with the library in LD_PRELOAD:
 *
 *   - intercept malloc/free only;
 *   - count requested bytes and allocator-size-class holes;
 *   - create a CSV "loaded" marker from the constructor;
 *   - append one aggregate row when the process exits normally.
 *
 * There is no background thread, timer, pthread TLS/key, pthread_atfork,
 * getenv, clock access, periodic file I/O, or per-allocation event log.
 *
 * Once this version is known to be safe in appspawndf, functionality can be
 * restored incrementally without mixing loader/fork-server failures with
 * the statistics logic.
 */

#define BOOTSTRAP_HEAP_CAPACITY (1024U * 1024U)
#define SMALL_SIZE_CLASS_COUNT 28U
#define SIZE_BUCKET_COUNT (SMALL_SIZE_CLASS_COUNT + 1U)
#define LARGE_ALLOCATION_ALIGNMENT 4096U
#define OUTPUT_PATH_CAPACITY 192U
#define OUTPUT_BUFFER_CAPACITY 4096U

#ifndef MINI_HOLE_OUTPUT_DIR
#if defined(__OHOS__)
#define MINI_HOLE_OUTPUT_DIR "/data/local/tmp"
#else
#define MINI_HOLE_OUTPUT_DIR "/tmp"
#endif
#endif

typedef void *(*malloc_fn)(size_t);
typedef void (*free_fn)(void *);

static _Atomic(malloc_fn) real_malloc;
static _Atomic(free_fn) real_free;
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

static const size_t small_size_classes[SMALL_SIZE_CLASS_COUNT] = {
    8, 16, 24, 32, 40, 48, 56, 64,
    80, 96, 112, 128, 144, 176, 208, 256,
    304, 352, 384, 432, 496, 560, 656, 784,
    992, 1312, 1984, 3968,
};

static _Atomic uint64_t allocation_count;
static _Atomic uint64_t failed_allocation_count;
static _Atomic uint64_t requested_bytes;
static _Atomic uint64_t hole_bytes;
static _Atomic uint64_t measurement_errors;
static _Atomic uint64_t bucket_allocation_count[SIZE_BUCKET_COUNT];
static _Atomic uint64_t bucket_hole_bytes[SIZE_BUCKET_COUNT];

static __thread unsigned int hook_depth;
static uint64_t owner_pid;
static char output_path[OUTPUT_PATH_CAPACITY];

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

static void resolve_real_allocators(void)
{
    if (atomic_load_explicit(&resolver_complete, memory_order_acquire)) {
        return;
    }
    if (atomic_flag_test_and_set_explicit(
            &resolver_lock, memory_order_acquire)) {
        return;
    }

    malloc_fn malloc_function = (malloc_fn)dlsym(RTLD_NEXT, "malloc");
    free_fn free_function = (free_fn)dlsym(RTLD_NEXT, "free");
    atomic_store_explicit(
        &real_malloc, malloc_function, memory_order_release);
    atomic_store_explicit(
        &real_free, free_function, memory_order_release);
    atomic_store_explicit(
        &resolver_complete, malloc_function != NULL, memory_order_release);
    atomic_flag_clear_explicit(&resolver_lock, memory_order_release);
}

static malloc_fn get_real_malloc(void)
{
    malloc_fn function =
        atomic_load_explicit(&real_malloc, memory_order_acquire);
    if (function == NULL) {
        resolve_real_allocators();
        function =
            atomic_load_explicit(&real_malloc, memory_order_acquire);
    }
    return function;
}

static free_fn get_real_free(void)
{
    free_fn function =
        atomic_load_explicit(&real_free, memory_order_acquire);
    if (function == NULL) {
        resolve_real_allocators();
        function =
            atomic_load_explicit(&real_free, memory_order_acquire);
    }
    return function;
}

static uint64_t raw_getpid(void)
{
    return (uint64_t)syscall(SYS_getpid);
}

static int raw_open_log(const char *path, bool truncate)
{
    int flags = O_WRONLY | O_CREAT | O_CLOEXEC;
    flags |= truncate ? O_TRUNC : O_APPEND;
    return (int)syscall(
        SYS_openat, AT_FDCWD, path, flags, 0644);
}

static bool raw_write_all(int fd, const char *buffer, size_t size)
{
    while (size != 0) {
        long result = syscall(SYS_write, fd, buffer, size);
        if (result > 0) {
            buffer += (size_t)result;
            size -= (size_t)result;
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

static void raw_close(int fd)
{
    (void)syscall(SYS_close, fd);
}

static size_t append_text(
    char *buffer, size_t capacity, size_t offset, const char *text)
{
    while (*text != '\0' && offset < capacity) {
        buffer[offset++] = *text++;
    }
    return offset;
}

static size_t append_u64(
    char *buffer, size_t capacity, size_t offset, uint64_t value)
{
    char digits[20];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);

    while (count != 0 && offset < capacity) {
        buffer[offset++] = digits[--count];
    }
    return offset;
}

static void prepare_output_path(uint64_t pid)
{
    size_t offset = append_text(
        output_path, sizeof(output_path) - 1, 0, MINI_HOLE_OUTPUT_DIR);
    if (offset != 0 && output_path[offset - 1] != '/') {
        offset = append_text(
            output_path, sizeof(output_path) - 1, offset, "/");
    }
    offset = append_text(
        output_path, sizeof(output_path) - 1, offset,
        "mini_hole_min_");
    offset = append_u64(
        output_path, sizeof(output_path) - 1, offset, pid);
    offset = append_text(
        output_path, sizeof(output_path) - 1, offset, ".csv");
    output_path[offset] = '\0';
}

static size_t append_csv_header(char *buffer, size_t offset)
{
    offset = append_text(
        buffer, OUTPUT_BUFFER_CAPACITY, offset,
        "#mini_malloc_hole_min_v0,unit=byte,scope=malloc_only\n"
        "event,pid,malloc_count,failed_count,requested_bytes,"
        "hole_bytes,measurement_errors");
    for (uint32_t i = 0; i < SMALL_SIZE_CLASS_COUNT; ++i) {
        offset = append_text(
            buffer, OUTPUT_BUFFER_CAPACITY, offset, ",count_");
        offset = append_u64(
            buffer, OUTPUT_BUFFER_CAPACITY, offset,
            small_size_classes[i]);
        offset = append_text(
            buffer, OUTPUT_BUFFER_CAPACITY, offset, ",hole_");
        offset = append_u64(
            buffer, OUTPUT_BUFFER_CAPACITY, offset,
            small_size_classes[i]);
    }
    offset = append_text(
        buffer, OUTPUT_BUFFER_CAPACITY, offset,
        ",count_4K_plus,hole_4K_plus\n");
    return offset;
}

static size_t append_csv_row(
    char *buffer, size_t offset, const char *event)
{
    offset = append_text(
        buffer, OUTPUT_BUFFER_CAPACITY, offset, event);
    offset = append_text(
        buffer, OUTPUT_BUFFER_CAPACITY, offset, ",");
    offset = append_u64(
        buffer, OUTPUT_BUFFER_CAPACITY, offset, owner_pid);
    offset = append_text(
        buffer, OUTPUT_BUFFER_CAPACITY, offset, ",");
    offset = append_u64(
        buffer, OUTPUT_BUFFER_CAPACITY, offset,
        atomic_load_explicit(&allocation_count, memory_order_relaxed));
    offset = append_text(
        buffer, OUTPUT_BUFFER_CAPACITY, offset, ",");
    offset = append_u64(
        buffer, OUTPUT_BUFFER_CAPACITY, offset,
        atomic_load_explicit(
            &failed_allocation_count, memory_order_relaxed));
    offset = append_text(
        buffer, OUTPUT_BUFFER_CAPACITY, offset, ",");
    offset = append_u64(
        buffer, OUTPUT_BUFFER_CAPACITY, offset,
        atomic_load_explicit(&requested_bytes, memory_order_relaxed));
    offset = append_text(
        buffer, OUTPUT_BUFFER_CAPACITY, offset, ",");
    offset = append_u64(
        buffer, OUTPUT_BUFFER_CAPACITY, offset,
        atomic_load_explicit(&hole_bytes, memory_order_relaxed));
    offset = append_text(
        buffer, OUTPUT_BUFFER_CAPACITY, offset, ",");
    offset = append_u64(
        buffer, OUTPUT_BUFFER_CAPACITY, offset,
        atomic_load_explicit(&measurement_errors, memory_order_relaxed));

    for (uint32_t i = 0; i < SIZE_BUCKET_COUNT; ++i) {
        offset = append_text(
            buffer, OUTPUT_BUFFER_CAPACITY, offset, ",");
        offset = append_u64(
            buffer, OUTPUT_BUFFER_CAPACITY, offset,
            atomic_load_explicit(
                &bucket_allocation_count[i], memory_order_relaxed));
        offset = append_text(
            buffer, OUTPUT_BUFFER_CAPACITY, offset, ",");
        offset = append_u64(
            buffer, OUTPUT_BUFFER_CAPACITY, offset,
            atomic_load_explicit(
                &bucket_hole_bytes[i], memory_order_relaxed));
    }
    offset = append_text(
        buffer, OUTPUT_BUFFER_CAPACITY, offset, "\n");
    return offset;
}

static void write_loaded_marker(void)
{
    int fd = raw_open_log(output_path, true);
    if (fd < 0) {
        return;
    }

    char buffer[OUTPUT_BUFFER_CAPACITY];
    size_t offset = append_csv_header(buffer, 0);
    offset = append_csv_row(buffer, offset, "loaded");
    (void)raw_write_all(fd, buffer, offset);
    raw_close(fd);
}

static void write_exit_snapshot(void)
{
    int fd = raw_open_log(output_path, false);
    if (fd < 0) {
        return;
    }

    char buffer[OUTPUT_BUFFER_CAPACITY];
    size_t offset = append_csv_row(buffer, 0, "exit");
    (void)raw_write_all(fd, buffer, offset);
    raw_close(fd);
}

#if !defined(MINI_HOLE_PASSTHROUGH)
static bool rounded_allocation_size(
    size_t requested, size_t *rounded, uint32_t *bucket)
{
    if (requested <=
        small_size_classes[SMALL_SIZE_CLASS_COUNT - 1]) {
        uint32_t low = 0;
        uint32_t high = SMALL_SIZE_CLASS_COUNT;
        while (low < high) {
            uint32_t middle = low + (high - low) / 2;
            if (small_size_classes[middle] < requested) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }
        *rounded = small_size_classes[low];
        *bucket = low;
        return true;
    }

    if (requested >
        SIZE_MAX - (LARGE_ALLOCATION_ALIGNMENT - 1U)) {
        return false;
    }
    *rounded =
        (requested + LARGE_ALLOCATION_ALIGNMENT - 1U) &
        ~((size_t)LARGE_ALLOCATION_ALIGNMENT - 1U);
    *bucket = SMALL_SIZE_CLASS_COUNT;
    return true;
}
#endif

static void record_successful_allocation(size_t requested)
{
#if defined(MINI_HOLE_PASSTHROUGH)
    (void)requested;
#else
    atomic_fetch_add_explicit(
        &allocation_count, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(
        &requested_bytes, (uint64_t)requested, memory_order_relaxed);

    size_t rounded;
    uint32_t bucket;
    if (!rounded_allocation_size(requested, &rounded, &bucket)) {
        atomic_fetch_add_explicit(
            &measurement_errors, 1, memory_order_relaxed);
        return;
    }

    uint64_t hole = (uint64_t)(rounded - requested);
    atomic_fetch_add_explicit(
        &hole_bytes, hole, memory_order_relaxed);
    atomic_fetch_add_explicit(
        &bucket_allocation_count[bucket], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(
        &bucket_hole_bytes[bucket], hole, memory_order_relaxed);
#endif
}

static void record_failed_allocation(void)
{
#if !defined(MINI_HOLE_PASSTHROUGH)
    atomic_fetch_add_explicit(
        &failed_allocation_count, 1, memory_order_relaxed);
#endif
}

__attribute__((constructor)) static void initialize_hook(void)
{
    ++hook_depth;
    owner_pid = raw_getpid();
    prepare_output_path(owner_pid);
    write_loaded_marker();
    --hook_depth;
}

__attribute__((destructor)) static void finalize_hook(void)
{
    ++hook_depth;
    if (owner_pid == raw_getpid()) {
        write_exit_snapshot();
    }
    --hook_depth;
}

__attribute__((visibility("default"))) void *malloc(size_t size)
{
    if (hook_depth != 0) {
        malloc_fn nested = atomic_load_explicit(
            &real_malloc, memory_order_acquire);
        return nested != NULL ? nested(size) : bootstrap_malloc(size);
    }

    ++hook_depth;
    malloc_fn function = get_real_malloc();
    void *ptr =
        function != NULL ? function(size) : bootstrap_malloc(size);
    int saved_errno = errno;
    if (ptr != NULL && !is_bootstrap_pointer(ptr)) {
        record_successful_allocation(size);
    } else if (ptr == NULL) {
        record_failed_allocation();
    }
    errno = saved_errno;
    --hook_depth;
    return ptr;
}

__attribute__((visibility("default"))) void free(void *ptr)
{
    int saved_errno = errno;
    if (ptr == NULL || is_bootstrap_pointer(ptr)) {
        errno = saved_errno;
        return;
    }

    if (hook_depth != 0) {
        free_fn nested = atomic_load_explicit(
            &real_free, memory_order_acquire);
        if (nested != NULL) {
            nested(ptr);
        }
        errno = saved_errno;
        return;
    }

    ++hook_depth;
    free_fn function = get_real_free();
    if (function != NULL) {
        function(ptr);
    }
    errno = saved_errno;
    --hook_depth;
}

/*
 * Keep this compatibility symbol because some callers resolve it directly.
 * It adds no separate allocator lookup or accounting path.
 */
__attribute__((visibility("default")))
void free_sized(void *ptr, size_t size)
{
    (void)size;
    free(ptr);
}

#endif
