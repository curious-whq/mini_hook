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
#include <time.h>
#include <unistd.h>

#include "replay_format.h"

#define BOOTSTRAP_HEAP_CAPACITY (1024 * 1024)
#ifndef REPLAY_LOG_PATH
#define REPLAY_LOG_PATH "/data/local/tmp/mini_replay.bin"
#endif
#define SEQUENCE_PID_INITIALIZING UINT64_MAX

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
static _Atomic uint64_t sequence_pid;
static _Atomic uint64_t event_sequence;
static int replay_fd = -1;

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

static pid_t raw_getpid(void)
{
    return (pid_t)syscall(SYS_getpid);
}

static pid_t raw_gettid(void)
{
    return (pid_t)syscall(SYS_gettid);
}

static uint64_t monotonic_time_ns(void)
{
    struct timespec value = {0};
    if (syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &value) != 0) {
        return 0;
    }
    return (uint64_t)value.tv_sec * 1000000000ULL +
           (uint64_t)value.tv_nsec;
}

static bool raw_write_all(int fd, const void *data, size_t size)
{
    const unsigned char *cursor = data;
    while (size != 0) {
        long written = syscall(SYS_write, fd, cursor, size);
        if (written <= 0) {
            return false;
        }
        cursor += written;
        size -= (size_t)written;
    }
    return true;
}

static bool replay_file_is_empty(int fd)
{
    long offset = syscall(SYS_lseek, fd, 0, SEEK_END);
    return offset == 0;
}

static void write_file_header(void)
{
    static const MiniReplayFileHeader header = {
        .magic = MINI_REPLAY_MAGIC,
        .version = MINI_REPLAY_VERSION,
        .header_size = sizeof(MiniReplayFileHeader),
        .event_size = sizeof(MiniReplayEvent),
        .pointer_size = sizeof(void *),
        .endian = MINI_REPLAY_ENDIAN_LITTLE,
        .flags = 0,
        .reserved0 = 0,
        .reserved1 = 0,
    };

    (void)raw_write_all(replay_fd, &header, sizeof(header));
}

static void write_process_start(void)
{
    MiniReplayEvent event = {
        .sequence = 0,
        .timestamp_ns = monotonic_time_ns(),
        .address = 0,
        .size = 0,
        .pid = (uint32_t)raw_getpid(),
        .tid = (uint32_t)raw_gettid(),
        .type = MINI_REPLAY_PROCESS_START,
        .flags = 0,
        .reserved = 0,
    };

    (void)raw_write_all(replay_fd, &event, sizeof(event));
}

static uint64_t next_event_sequence(pid_t pid)
{
    const uint64_t current_pid = (uint64_t)pid;
    uint64_t recorded_pid =
        atomic_load_explicit(&sequence_pid, memory_order_acquire);

    if (recorded_pid != current_pid) {
        if (recorded_pid == SEQUENCE_PID_INITIALIZING ||
            !atomic_compare_exchange_strong_explicit(
                &sequence_pid, &recorded_pid, SEQUENCE_PID_INITIALIZING,
                memory_order_acq_rel, memory_order_acquire)) {
            return 0;
        }

        atomic_store_explicit(
            &event_sequence, 0, memory_order_relaxed);
        atomic_store_explicit(
            &sequence_pid, current_pid, memory_order_release);
    }

    return atomic_fetch_add_explicit(
               &event_sequence, 1, memory_order_relaxed) +
           1;
}

static void write_malloc_event(void *ptr, size_t size)
{
    if (replay_fd < 0) {
        return;
    }

    const pid_t pid = raw_getpid();
    const uint64_t sequence = next_event_sequence(pid);
    if (sequence == 0) {
        return;
    }

    MiniReplayEvent event = {
        .sequence = sequence,
        .timestamp_ns = monotonic_time_ns(),
        .address = (uint64_t)(uintptr_t)ptr,
        .size = (uint64_t)size,
        .pid = (uint32_t)pid,
        .tid = (uint32_t)raw_gettid(),
        .type = MINI_REPLAY_MALLOC,
        .flags = ptr == NULL ? 1 : 0,
        .reserved = 0,
    };

    (void)raw_write_all(replay_fd, &event, sizeof(event));
}

__attribute__((constructor)) static void initialize_hook(void)
{
    resolve_real_allocators();

    replay_fd = (int)syscall(
        SYS_openat, AT_FDCWD, REPLAY_LOG_PATH,
        O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (replay_fd < 0) {
        return;
    }

    if (replay_file_is_empty(replay_fd)) {
        write_file_header();
    }
    write_process_start();
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
    write_malloc_event(ptr, size);
    return ptr;
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
        function = atomic_load_explicit(
            &real_free, memory_order_acquire);
    }
    if (function != NULL) {
        function(ptr);
    }
}
