#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dlfcn.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "replay_format.h"

#define BOOTSTRAP_HEAP_CAPACITY (1024 * 1024)
#ifndef REPLAY_LOG_PATH
#define REPLAY_LOG_PATH "/data/local/tmp/mini_replay.bin"
#endif
#ifndef REPLAY_MAPPING_SIZE
#define REPLAY_MAPPING_SIZE (3ULL * 1024ULL * 1024ULL * 1024ULL)
#endif
#define REPLAY_OVERFLOW_EXIT_CODE 75

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

static MiniReplayFileHeader *replay_header;
static MiniReplayEvent *replay_events;

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

static bool magic_matches(const uint8_t magic[8])
{
    static const uint8_t expected[8] = MINI_REPLAY_MAGIC;
    for (size_t i = 0; i < sizeof(expected); ++i) {
        if (magic[i] != expected[i]) {
            return false;
        }
    }
    return true;
}

static void copy_magic(uint8_t destination[8])
{
    static const uint8_t source[8] = MINI_REPLAY_MAGIC;
    for (size_t i = 0; i < sizeof(source); ++i) {
        destination[i] = source[i];
    }
}

static bool initialize_new_mapping(void)
{
    uint32_t expected = MINI_REPLAY_INIT_EMPTY;
    if (!atomic_compare_exchange_strong_explicit(
            &replay_header->init_state, &expected,
            MINI_REPLAY_INIT_BUSY, memory_order_acq_rel,
            memory_order_acquire)) {
        return false;
    }

    replay_header->capacity =
        (REPLAY_MAPPING_SIZE - sizeof(MiniReplayFileHeader)) /
        sizeof(MiniReplayEvent);
    atomic_store_explicit(
        &replay_header->next_index, 0, memory_order_relaxed);
    replay_header->version = MINI_REPLAY_VERSION;
    replay_header->header_size = sizeof(MiniReplayFileHeader);
    replay_header->event_size = sizeof(MiniReplayEvent);
    replay_header->pointer_size = sizeof(void *);
    replay_header->endian = MINI_REPLAY_ENDIAN_LITTLE;
    replay_header->flags = 0;
    atomic_store_explicit(
        &replay_header->runtime_flags, 0, memory_order_relaxed);
    replay_header->reserved0 = 0;
    replay_header->reserved1[0] = 0;
    replay_header->reserved1[1] = 0;
    copy_magic(replay_header->magic);

    atomic_store_explicit(
        &replay_header->init_state, MINI_REPLAY_INIT_READY,
        memory_order_release);
    return true;
}

static bool mapping_is_valid(void)
{
    return atomic_load_explicit(
               &replay_header->init_state, memory_order_acquire) ==
               MINI_REPLAY_INIT_READY &&
           magic_matches(replay_header->magic) &&
           replay_header->version == MINI_REPLAY_VERSION &&
           replay_header->header_size == sizeof(MiniReplayFileHeader) &&
           replay_header->event_size == sizeof(MiniReplayEvent) &&
           replay_header->capacity ==
               (REPLAY_MAPPING_SIZE - sizeof(MiniReplayFileHeader)) /
                   sizeof(MiniReplayEvent);
}

static bool initialize_replay_mapping(void)
{
    int fd = (int)syscall(
        SYS_openat, AT_FDCWD, REPLAY_LOG_PATH,
        O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        return false;
    }

    long old_size = syscall(SYS_lseek, fd, 0, SEEK_END);
    if (old_size < 0 ||
        syscall(SYS_ftruncate, fd, (off_t)REPLAY_MAPPING_SIZE) != 0) {
        syscall(SYS_close, fd);
        return false;
    }

    void *mapping = mmap(
        NULL, (size_t)REPLAY_MAPPING_SIZE,
        PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    syscall(SYS_close, fd);
    if (mapping == MAP_FAILED) {
        return false;
    }

    replay_header = mapping;
    replay_events = (MiniReplayEvent *)(
        (unsigned char *)mapping + sizeof(MiniReplayFileHeader));

    if (old_size == 0) {
        (void)initialize_new_mapping();
    }
    if (!mapping_is_valid()) {
        replay_header = NULL;
        replay_events = NULL;
        munmap(mapping, (size_t)REPLAY_MAPPING_SIZE);
        return false;
    }
    return true;
}

static uint64_t reserve_event_slot(void)
{
    if (replay_header == NULL) {
        return UINT64_MAX;
    }

    uint64_t index = atomic_fetch_add_explicit(
        &replay_header->next_index, 1, memory_order_relaxed);
    if (index < replay_header->capacity) {
        return index;
    }

    atomic_fetch_or_explicit(
        &replay_header->runtime_flags, MINI_REPLAY_FLAG_OVERFLOW,
        memory_order_release);
    syscall(SYS_exit_group, REPLAY_OVERFLOW_EXIT_CODE);
    __builtin_unreachable();
}

static void commit_event(
    uint64_t index, uint16_t type, uint64_t address,
    uint64_t size, uint16_t flags)
{
    MiniReplayEvent *event = &replay_events[index];
    event->timestamp_ns = monotonic_time_ns();
    event->address = address;
    event->size = size;
    event->pid = (uint32_t)raw_getpid();
    event->tid = (uint32_t)raw_gettid();
    event->type = type;
    event->flags = flags;
    event->reserved = 0;
    atomic_store_explicit(
        &event->sequence, index + 1, memory_order_release);
}

static void write_process_start(void)
{
    uint64_t index = reserve_event_slot();
    if (index != UINT64_MAX) {
        commit_event(
            index, MINI_REPLAY_PROCESS_START, 0, 0, 0);
    }
}

__attribute__((constructor)) static void initialize_hook(void)
{
    resolve_real_allocators();
    if (!initialize_replay_mapping()) {
        syscall(SYS_exit_group, REPLAY_OVERFLOW_EXIT_CODE);
        __builtin_unreachable();
    }
    write_process_start();
}

__attribute__((visibility("default"))) void *malloc(size_t size)
{
    uint64_t index = reserve_event_slot();

    malloc_fn function =
        atomic_load_explicit(&real_malloc, memory_order_acquire);
    if (function == NULL) {
        resolve_real_allocators();
        function = atomic_load_explicit(
            &real_malloc, memory_order_acquire);
    }

    void *ptr =
        function != NULL ? function(size) : bootstrap_malloc(size);
    if (index != UINT64_MAX) {
        commit_event(
            index, MINI_REPLAY_MALLOC,
            (uint64_t)(uintptr_t)ptr, (uint64_t)size,
            ptr == NULL ? 1 : 0);
    }
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
