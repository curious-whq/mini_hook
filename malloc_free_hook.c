#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dlfcn.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <unistd.h>

#define BOOTSTRAP_HEAP_CAPACITY (1024 * 1024)
#ifndef HIT_LOG_PATH
#define HIT_LOG_PATH "/data/local/tmp/mini_hook_hits.log"
#endif
#define HIT_BUFFER_CAPACITY 96
#define PID_STATE_INITIALIZING UINT64_MAX

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
static _Atomic uint64_t malloc_count;
static _Atomic uint64_t free_count;
static _Atomic uint64_t malloc_count_pid;
static _Atomic uint64_t free_count_pid;
static int hit_log_fd = -1;

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

static pid_t raw_getpid(void)
{
    return (pid_t)syscall(SYS_getpid);
}

static void raw_write(int fd, const char *buffer, size_t size)
{
    while (size != 0) {
        long written = syscall(SYS_write, fd, buffer, size);
        if (written <= 0) {
            return;
        }
        buffer += written;
        size -= (size_t)written;
    }
}

static bool prepare_process_counter(
    _Atomic uint64_t *counter_pid, _Atomic uint64_t *counter, pid_t pid)
{
    const uint64_t current_pid = (uint64_t)pid;
    uint64_t recorded_pid =
        atomic_load_explicit(counter_pid, memory_order_acquire);

    if (recorded_pid == current_pid) {
        return true;
    }
    if (recorded_pid == PID_STATE_INITIALIZING ||
        !atomic_compare_exchange_strong_explicit(
            counter_pid, &recorded_pid, PID_STATE_INITIALIZING,
            memory_order_acq_rel, memory_order_acquire)) {
        return false;
    }

    atomic_store_explicit(counter, 0, memory_order_relaxed);
    atomic_store_explicit(
        counter_pid, current_pid, memory_order_release);
    return true;
}

static bool is_power_of_two(uint64_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

static void write_count(pid_t pid, const char *hook_name, uint64_t count)
{
    if (hit_log_fd < 0 || !is_power_of_two(count)) {
        return;
    }

    char buffer[HIT_BUFFER_CAPACITY];
    size_t offset = append_text(buffer, 0, "pid=");
    offset = append_u64(buffer, offset, (uint64_t)pid);
    offset = append_text(buffer, offset, " hook=");
    offset = append_text(buffer, offset, hook_name);
    offset = append_text(buffer, offset, " count=");
    offset = append_u64(buffer, offset, count);
    buffer[offset++] = '\n';
    raw_write(hit_log_fd, buffer, offset);
}

static void record_hook(
    _Atomic uint64_t *counter_pid, _Atomic uint64_t *counter,
    pid_t pid, const char *hook_name)
{
    if (!prepare_process_counter(counter_pid, counter, pid)) {
        return;
    }

    uint64_t count = atomic_fetch_add_explicit(
        counter, 1, memory_order_relaxed) + 1;
    write_count(pid, hook_name, count);
}

static void write_probe(pid_t pid, const char *probe_name)
{
    if (hit_log_fd < 0) {
        return;
    }

    char buffer[HIT_BUFFER_CAPACITY];
    size_t offset = append_text(buffer, 0, "pid=");
    offset = append_u64(buffer, offset, (uint64_t)pid);
    offset = append_text(buffer, offset, " hook=");
    offset = append_text(buffer, offset, probe_name);
    buffer[offset++] = '\n';
    raw_write(hit_log_fd, buffer, offset);
}

__attribute__((constructor)) static void initialize_hook(void)
{
    resolve_real_allocators();
    hit_log_fd = (int)syscall(
        SYS_openat, AT_FDCWD, HIT_LOG_PATH,
        O_WRONLY | O_CREAT | O_APPEND, 0666);
    write_probe(raw_getpid(), "constructor");
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
    record_hook(
        &malloc_count_pid, &malloc_count, raw_getpid(), "malloc");
    return ptr;
}

__attribute__((visibility("default"))) void free(void *ptr)
{
    record_hook(
        &free_count_pid, &free_count, raw_getpid(), "free");

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
