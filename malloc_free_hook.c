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
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "replay_format.h"

#define BOOTSTRAP_HEAP_CAPACITY (1024 * 1024)
#ifndef REPLAY_OUTPUT_DIR
#define REPLAY_OUTPUT_DIR "/data/local/tmp"
#endif
#ifndef REPLAY_MAPPING_SIZE
#define REPLAY_MAPPING_SIZE (3ULL * 1024ULL * 1024ULL * 1024ULL)
#endif
#define REPLAY_OVERFLOW_EXIT_CODE 75
#define REPLAY_PATH_CAPACITY 192

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
static _Atomic bool resolver_complete;
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

static MiniReplayFileHeader *replay_header;
static MiniReplayEvent *replay_events;
static char replay_log_path[REPLAY_PATH_CAPACITY];

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
            BootstrapHeader *header = (BootstrapHeader *)
                &bootstrap_heap.bytes[offset];
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

    const uintptr_t address = (uintptr_t)ptr;
    const uintptr_t begin = (uintptr_t)&bootstrap_heap.bytes[0];
    const uintptr_t end =
        (uintptr_t)&bootstrap_heap.bytes[sizeof(bootstrap_heap.bytes)];
    return address >= begin && address < end;
}

static size_t bootstrap_pointer_size(const void *ptr)
{
    const BootstrapHeader *header =
        (const BootstrapHeader *)ptr - 1;
    return header->metadata.size;
}

static void copy_bytes(
    unsigned char *destination, const unsigned char *source,
    size_t count)
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
        copy_bytes(
            new_ptr, ptr, old_size < size ? old_size : size);
    }
    return new_ptr;
}

static void resolve_real_allocators(void)
{
    if (atomic_load_explicit(
            &resolver_complete, memory_order_acquire)) {
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
    atomic_store_explicit(
        &real_calloc, (calloc_fn)dlsym(RTLD_NEXT, "calloc"),
        memory_order_release);
    atomic_store_explicit(
        &real_realloc, (realloc_fn)dlsym(RTLD_NEXT, "realloc"),
        memory_order_release);
    atomic_store_explicit(
        &real_free_sized,
        (free_sized_fn)dlsym(RTLD_NEXT, "free_sized"),
        memory_order_release);
    atomic_store_explicit(
        &real_posix_memalign,
        (posix_memalign_fn)dlsym(RTLD_NEXT, "posix_memalign"),
        memory_order_release);
    atomic_store_explicit(
        &real_aligned_alloc,
        (aligned_alloc_fn)dlsym(RTLD_NEXT, "aligned_alloc"),
        memory_order_release);
    atomic_store_explicit(
        &real_valloc, (valloc_fn)dlsym(RTLD_NEXT, "valloc"),
        memory_order_release);
    atomic_store_explicit(
        &real_memalign, (memalign_fn)dlsym(RTLD_NEXT, "memalign"),
        memory_order_release);
    atomic_store_explicit(
        &real_pvalloc, (pvalloc_fn)dlsym(RTLD_NEXT, "pvalloc"),
        memory_order_release);

    atomic_store_explicit(
        &resolver_complete, true, memory_order_release);
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

static bool append_text(
    char *destination, size_t capacity, size_t *length,
    const char *text)
{
    while (*text != '\0') {
        if (*length + 1 >= capacity) {
            return false;
        }
        destination[(*length)++] = *text++;
    }
    destination[*length] = '\0';
    return true;
}

#ifndef REPLAY_LOG_PATH
static uint64_t realtime_ns(void)
{
    struct timespec value = {0};
    if (syscall(SYS_clock_gettime, CLOCK_REALTIME, &value) != 0) {
        return 0;
    }
    return (uint64_t)value.tv_sec * 1000000000ULL +
           (uint64_t)value.tv_nsec;
}

static bool append_uint64(
    char *destination, size_t capacity, size_t *length,
    uint64_t value)
{
    char digits[20];
    size_t count = 0;

    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);

    while (count != 0) {
        if (*length + 1 >= capacity) {
            return false;
        }
        destination[(*length)++] = digits[--count];
    }
    destination[*length] = '\0';
    return true;
}
#endif

static bool build_replay_log_path(void)
{
    size_t length = 0;

#ifdef REPLAY_LOG_PATH
    return append_text(
        replay_log_path, sizeof(replay_log_path), &length,
        REPLAY_LOG_PATH);
#else
    return append_text(
               replay_log_path, sizeof(replay_log_path), &length,
               REPLAY_OUTPUT_DIR "/mini_replay_") &&
           append_uint64(
               replay_log_path, sizeof(replay_log_path), &length,
               realtime_ns()) &&
           append_text(
               replay_log_path, sizeof(replay_log_path), &length, "_") &&
           append_uint64(
               replay_log_path, sizeof(replay_log_path), &length,
               (uint64_t)(uint32_t)raw_getpid()) &&
           append_text(
               replay_log_path, sizeof(replay_log_path), &length, ".bin");
#endif
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
    const uint64_t capacity =
        (REPLAY_MAPPING_SIZE - sizeof(MiniReplayFileHeader)) /
        sizeof(MiniReplayEvent);
    if (capacity > UINT32_MAX) {
        return false;
    }

    uint32_t expected = MINI_REPLAY_INIT_EMPTY;
    if (!atomic_compare_exchange_strong_explicit(
            &replay_header->init_state, &expected,
            MINI_REPLAY_INIT_BUSY, memory_order_acq_rel,
            memory_order_acquire)) {
        return false;
    }

    replay_header->capacity = capacity;
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
           replay_header->capacity <= UINT32_MAX &&
           replay_header->capacity ==
               (REPLAY_MAPPING_SIZE - sizeof(MiniReplayFileHeader)) /
                   sizeof(MiniReplayEvent);
}

static bool initialize_replay_mapping(void)
{
    if (!build_replay_log_path()) {
        return false;
    }

#ifdef REPLAY_LOG_PATH
    const int open_flags = O_RDWR | O_CREAT;
#else
    const int open_flags = O_RDWR | O_CREAT | O_EXCL;
#endif
    int fd = (int)syscall(
        SYS_openat, AT_FDCWD, replay_log_path, open_flags, 0666);
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
    uint64_t result, uint64_t size, uint16_t flags)
{
    MiniReplayEvent *event = &replay_events[index];
    event->timestamp_ns = monotonic_time_ns();
    event->address = address;
    event->result = result;
    event->size = size;
    event->pid = (uint32_t)raw_getpid();
    event->tid = (uint32_t)raw_gettid();
    event->type = type;
    event->flags = flags;
    atomic_store_explicit(
        &event->sequence, (uint32_t)(index + 1),
        memory_order_release);
}

static void write_process_start(void)
{
    uint64_t index = reserve_event_slot();
    if (index != UINT64_MAX) {
        commit_event(
            index, MINI_REPLAY_PROCESS_START, 0, 0, 0, 0);
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
    malloc_fn function =
        atomic_load_explicit(&real_malloc, memory_order_acquire);
    if (function == NULL) {
        resolve_real_allocators();
        function = atomic_load_explicit(
            &real_malloc, memory_order_acquire);
    }

    void *ptr =
        function != NULL ? function(size) : bootstrap_malloc(size);
    uint64_t index = reserve_event_slot();
    if (index != UINT64_MAX) {
        commit_event(
            index, MINI_REPLAY_MALLOC,
            0, (uint64_t)(uintptr_t)ptr, (uint64_t)size,
            ptr == NULL ? MINI_REPLAY_EVENT_FAILED : 0);
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
        uint64_t index = reserve_event_slot();
        if (index != UINT64_MAX) {
            commit_event(
                index, MINI_REPLAY_FREE,
                (uint64_t)(uintptr_t)ptr, 0, 0, 0);
        }
        function(ptr);
    }
}

__attribute__((visibility("default")))
void *calloc(size_t count, size_t size)
{
    bool size_overflow = size != 0 && count > SIZE_MAX / size;
    uint64_t total_size =
        size_overflow ? UINT64_MAX : (uint64_t)(count * size);

    calloc_fn function =
        atomic_load_explicit(&real_calloc, memory_order_acquire);
    if (function == NULL) {
        resolve_real_allocators();
        function = atomic_load_explicit(
            &real_calloc, memory_order_acquire);
    }

    void *ptr;
    if (function != NULL) {
        ptr = function(count, size);
    } else {
        malloc_fn malloc_function =
            atomic_load_explicit(&real_malloc, memory_order_acquire);
        if (malloc_function != NULL && !size_overflow) {
            ptr = malloc_function((size_t)total_size);
            if (ptr != NULL) {
                zero_bytes(ptr, (size_t)total_size);
            }
        } else {
            ptr = bootstrap_calloc(count, size);
        }
    }

    uint64_t index = reserve_event_slot();
    if (index != UINT64_MAX) {
        commit_event(
            index, MINI_REPLAY_CALLOC, 0,
            (uint64_t)(uintptr_t)ptr, total_size,
            ptr == NULL ? MINI_REPLAY_EVENT_FAILED : 0);
    }
    return ptr;
}

__attribute__((visibility("default")))
void *realloc(void *ptr, size_t size)
{
    void *new_ptr;

    if (is_bootstrap_pointer(ptr)) {
        new_ptr = bootstrap_realloc(ptr, size);
    } else {
        realloc_fn function =
            atomic_load_explicit(&real_realloc, memory_order_acquire);
        if (function == NULL) {
            resolve_real_allocators();
            function = atomic_load_explicit(
                &real_realloc, memory_order_acquire);
        }
        new_ptr = function != NULL ? function(ptr, size) : NULL;
    }

    uint64_t index = reserve_event_slot();
    if (index != UINT64_MAX) {
        commit_event(
            index, MINI_REPLAY_REALLOC,
            (uint64_t)(uintptr_t)ptr,
            (uint64_t)(uintptr_t)new_ptr, (uint64_t)size,
            new_ptr == NULL && size != 0
                ? MINI_REPLAY_EVENT_FAILED
                : 0);
    }
    return new_ptr;
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
        fallback = atomic_load_explicit(
            &real_free, memory_order_acquire);
    }
    if (function == NULL && fallback == NULL) {
        return;
    }

    uint64_t index = reserve_event_slot();
    if (index != UINT64_MAX) {
        commit_event(
            index, MINI_REPLAY_FREE_SIZED,
            (uint64_t)(uintptr_t)ptr, 0, (uint64_t)size, 0);
    }

    if (function != NULL) {
        function(ptr, size);
    } else {
        fallback(ptr);
    }
}

__attribute__((visibility("default")))
int posix_memalign(void **memptr, size_t alignment, size_t size)
{
    posix_memalign_fn function =
        atomic_load_explicit(
            &real_posix_memalign, memory_order_acquire);
    if (function == NULL) {
        resolve_real_allocators();
        function = atomic_load_explicit(
            &real_posix_memalign, memory_order_acquire);
    }

    void *ptr = NULL;
    int result = function != NULL
        ? function(&ptr, alignment, size)
        : ENOMEM;
    if (result == 0) {
        *memptr = ptr;
    }

    uint64_t index = reserve_event_slot();
    if (index != UINT64_MAX) {
        commit_event(
            index, MINI_REPLAY_POSIX_MEMALIGN, 0,
            result == 0 ? (uint64_t)(uintptr_t)ptr : 0,
            (uint64_t)size,
            result == 0 ? 0 : MINI_REPLAY_EVENT_FAILED);
    }
    return result;
}

__attribute__((visibility("default")))
void *aligned_alloc(size_t alignment, size_t size)
{
    aligned_alloc_fn function =
        atomic_load_explicit(
            &real_aligned_alloc, memory_order_acquire);
    if (function == NULL) {
        resolve_real_allocators();
        function = atomic_load_explicit(
            &real_aligned_alloc, memory_order_acquire);
    }

    void *ptr =
        function != NULL ? function(alignment, size) : NULL;
    uint64_t index = reserve_event_slot();
    if (index != UINT64_MAX) {
        commit_event(
            index, MINI_REPLAY_ALIGNED_ALLOC, 0,
            (uint64_t)(uintptr_t)ptr, (uint64_t)size,
            ptr == NULL ? MINI_REPLAY_EVENT_FAILED : 0);
    }
    return ptr;
}

__attribute__((visibility("default"))) void *valloc(size_t size)
{
    valloc_fn function =
        atomic_load_explicit(&real_valloc, memory_order_acquire);
    if (function == NULL) {
        resolve_real_allocators();
        function = atomic_load_explicit(
            &real_valloc, memory_order_acquire);
    }

    void *ptr = function != NULL ? function(size) : NULL;
    uint64_t index = reserve_event_slot();
    if (index != UINT64_MAX) {
        commit_event(
            index, MINI_REPLAY_VALLOC, 0,
            (uint64_t)(uintptr_t)ptr, (uint64_t)size,
            ptr == NULL ? MINI_REPLAY_EVENT_FAILED : 0);
    }
    return ptr;
}

__attribute__((visibility("default")))
void *memalign(size_t alignment, size_t size)
{
    memalign_fn function =
        atomic_load_explicit(&real_memalign, memory_order_acquire);
    if (function == NULL) {
        resolve_real_allocators();
        function = atomic_load_explicit(
            &real_memalign, memory_order_acquire);
    }

    void *ptr =
        function != NULL ? function(alignment, size) : NULL;
    uint64_t index = reserve_event_slot();
    if (index != UINT64_MAX) {
        commit_event(
            index, MINI_REPLAY_MEMALIGN, 0,
            (uint64_t)(uintptr_t)ptr, (uint64_t)size,
            ptr == NULL ? MINI_REPLAY_EVENT_FAILED : 0);
    }
    return ptr;
}

__attribute__((visibility("default"))) void *pvalloc(size_t size)
{
    pvalloc_fn function =
        atomic_load_explicit(&real_pvalloc, memory_order_acquire);
    if (function == NULL) {
        resolve_real_allocators();
        function = atomic_load_explicit(
            &real_pvalloc, memory_order_acquire);
    }

    void *ptr = function != NULL ? function(size) : NULL;
    uint64_t index = reserve_event_slot();
    if (index != UINT64_MAX) {
        commit_event(
            index, MINI_REPLAY_PVALLOC, 0,
            (uint64_t)(uintptr_t)ptr, (uint64_t)size,
            ptr == NULL ? MINI_REPLAY_EVENT_FAILED : 0);
    }
    return ptr;
}
