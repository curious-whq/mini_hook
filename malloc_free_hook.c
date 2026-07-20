#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
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
typedef int (*pthread_create_fn)(
    pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
typedef int (*pthread_join_fn)(pthread_t, void **);
typedef int (*pthread_detach_fn)(pthread_t);

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
static _Atomic(pthread_create_fn) real_pthread_create;
static _Atomic(pthread_join_fn) real_pthread_join;
static _Atomic(pthread_detach_fn) real_pthread_detach;
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
static bool replay_path_exclusive;
static _Atomic uint32_t next_thread_instance = 1;
static __thread uint32_t current_thread_instance;

#define THREAD_MAP_CAPACITY (1u << 16)
enum ThreadMapState {
    THREAD_MAP_EMPTY = 0,
    THREAD_MAP_USED = 1,
    THREAD_MAP_DELETED = 2,
};

typedef struct {
    unsigned char state;
    pthread_t pthread_key;
    uint32_t instance;
} ThreadMapEntry;

static ThreadMapEntry thread_map[THREAD_MAP_CAPACITY];
static pthread_mutex_t thread_map_lock = PTHREAD_MUTEX_INITIALIZER;

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
        &real_pthread_create,
        (pthread_create_fn)dlsym(RTLD_NEXT, "pthread_create"),
        memory_order_release);
    atomic_store_explicit(
        &real_pthread_join,
        (pthread_join_fn)dlsym(RTLD_NEXT, "pthread_join"),
        memory_order_release);
    atomic_store_explicit(
        &real_pthread_detach,
        (pthread_detach_fn)dlsym(RTLD_NEXT, "pthread_detach"),
        memory_order_release);

    atomic_store_explicit(
        &resolver_complete, true, memory_order_release);
    atomic_flag_clear_explicit(&resolver_lock, memory_order_release);
}

static pid_t raw_getpid(void)
{
    return (pid_t)syscall(SYS_getpid);
}

static void capture_fatal(void)
{
    if (replay_header != NULL) {
        atomic_fetch_or_explicit(
            &replay_header->runtime_flags, MINI_REPLAY_FLAG_OVERFLOW,
            memory_order_release);
    }
    syscall(SYS_exit_group, REPLAY_OVERFLOW_EXIT_CODE);
    __builtin_unreachable();
}

static uint32_t allocate_thread_instance(void)
{
    uint32_t instance = atomic_fetch_add_explicit(
        &next_thread_instance, 1, memory_order_relaxed);
    if (instance == 0 || instance == UINT32_MAX) {
        capture_fatal();
    }
    return instance;
}

static uint32_t get_thread_instance(void)
{
    if (current_thread_instance == 0) {
        current_thread_instance = allocate_thread_instance();
    }
    return current_thread_instance;
}

static uint64_t mix_uint64(uint64_t value)
{
    value ^= value >> 33;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33;
    value *= UINT64_C(0xc4ceb9fe1a85ec53);
    return value ^ (value >> 33);
}

static size_t pthread_hash(pthread_t thread)
{
    return (size_t)(mix_uint64((uint64_t)(uintptr_t)thread) &
                    (THREAD_MAP_CAPACITY - 1));
}

static bool thread_map_put(pthread_t thread, uint32_t instance)
{
    bool inserted = false;
    pthread_mutex_lock(&thread_map_lock);
    size_t first_deleted = SIZE_MAX;
    size_t hash = pthread_hash(thread);
    for (size_t probe = 0; probe < THREAD_MAP_CAPACITY; ++probe) {
        size_t index = (hash + probe) & (THREAD_MAP_CAPACITY - 1);
        ThreadMapEntry *entry = &thread_map[index];
        if (entry->state == THREAD_MAP_USED &&
            pthread_equal(entry->pthread_key, thread)) {
            entry->instance = instance;
            inserted = true;
            break;
        }
        if (entry->state == THREAD_MAP_DELETED && first_deleted == SIZE_MAX) {
            first_deleted = index;
        }
        if (entry->state == THREAD_MAP_EMPTY) {
            if (first_deleted != SIZE_MAX) {
                entry = &thread_map[first_deleted];
            }
            entry->state = THREAD_MAP_USED;
            entry->pthread_key = thread;
            entry->instance = instance;
            inserted = true;
            break;
        }
    }
    pthread_mutex_unlock(&thread_map_lock);
    return inserted;
}

static uint32_t thread_map_get(pthread_t thread)
{
    uint32_t instance = 0;
    pthread_mutex_lock(&thread_map_lock);
    size_t hash = pthread_hash(thread);
    for (size_t probe = 0; probe < THREAD_MAP_CAPACITY; ++probe) {
        ThreadMapEntry *entry =
            &thread_map[(hash + probe) & (THREAD_MAP_CAPACITY - 1)];
        if (entry->state == THREAD_MAP_EMPTY) {
            break;
        }
        if (entry->state == THREAD_MAP_USED &&
            pthread_equal(entry->pthread_key, thread)) {
            instance = entry->instance;
            break;
        }
    }
    pthread_mutex_unlock(&thread_map_lock);
    return instance;
}

static void thread_map_remove(pthread_t thread)
{
    pthread_mutex_lock(&thread_map_lock);
    size_t hash = pthread_hash(thread);
    for (size_t probe = 0; probe < THREAD_MAP_CAPACITY; ++probe) {
        ThreadMapEntry *entry =
            &thread_map[(hash + probe) & (THREAD_MAP_CAPACITY - 1)];
        if (entry->state == THREAD_MAP_EMPTY) {
            break;
        }
        if (entry->state == THREAD_MAP_USED &&
            pthread_equal(entry->pthread_key, thread)) {
            entry->state = THREAD_MAP_DELETED;
            entry->instance = 0;
            break;
        }
    }
    pthread_mutex_unlock(&thread_map_lock);
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
    const char *environment_path = getenv("MINI_REPLAY_PATH");
    if (environment_path != NULL && environment_path[0] != '\0') {
        replay_path_exclusive = true;
        return append_text(
            replay_log_path, sizeof(replay_log_path), &length,
            environment_path);
    }

#ifdef REPLAY_LOG_PATH
    replay_path_exclusive = false;
    return append_text(
        replay_log_path, sizeof(replay_log_path), &length,
        REPLAY_LOG_PATH);
#else
    replay_path_exclusive = true;
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

    const int open_flags = O_RDWR | O_CREAT |
        (replay_path_exclusive ? O_EXCL : 0);
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
    event->tid = get_thread_instance();
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

static void write_thread_event(
    uint16_t type, uint64_t address, uint32_t thread_instance)
{
    uint64_t index = reserve_event_slot();
    if (index != UINT64_MAX) {
        commit_event(
            index, type, address, (uint64_t)thread_instance, 0, 0);
    }
}

typedef struct {
    void *(*start_routine)(void *);
    void *argument;
    uint32_t thread_instance;
    _Atomic bool parent_ready;
} ThreadStartContext;

static void record_thread_end(void *unused)
{
    (void)unused;
    write_thread_event(
        MINI_REPLAY_THREAD_END,
        (uint64_t)(uintptr_t)pthread_self(),
        get_thread_instance());
}

static void *thread_start_trampoline(void *opaque)
{
    ThreadStartContext *context = opaque;
    while (!atomic_load_explicit(
        &context->parent_ready, memory_order_acquire)) {
        syscall(SYS_sched_yield);
    }

    void *(*start_routine)(void *) = context->start_routine;
    void *argument = context->argument;
    uint32_t thread_instance = context->thread_instance;

    free_fn free_function =
        atomic_load_explicit(&real_free, memory_order_acquire);
    if (free_function != NULL) {
        free_function(context);
    }

    current_thread_instance = thread_instance;
    write_thread_event(
        MINI_REPLAY_THREAD_START,
        (uint64_t)(uintptr_t)pthread_self(), thread_instance);

    void *result;
    pthread_cleanup_push(record_thread_end, NULL);
    result = start_routine(argument);
    pthread_cleanup_pop(1);
    return result;
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
            index, MINI_REPLAY_CALLOC, (uint64_t)count,
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
            index, MINI_REPLAY_POSIX_MEMALIGN,
            (uint64_t)alignment,
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
            index, MINI_REPLAY_ALIGNED_ALLOC,
            (uint64_t)alignment,
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
            index, MINI_REPLAY_MEMALIGN,
            (uint64_t)alignment,
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

__attribute__((visibility("default")))
int pthread_create(
    pthread_t *thread, const pthread_attr_t *attribute,
    void *(*start_routine)(void *), void *argument)
{
    pthread_create_fn function = atomic_load_explicit(
        &real_pthread_create, memory_order_acquire);
    malloc_fn malloc_function = atomic_load_explicit(
        &real_malloc, memory_order_acquire);
    free_fn free_function = atomic_load_explicit(
        &real_free, memory_order_acquire);
    if (function == NULL || malloc_function == NULL) {
        resolve_real_allocators();
        function = atomic_load_explicit(
            &real_pthread_create, memory_order_acquire);
        malloc_function = atomic_load_explicit(
            &real_malloc, memory_order_acquire);
        free_function = atomic_load_explicit(
            &real_free, memory_order_acquire);
    }
    if (function == NULL || malloc_function == NULL) {
        return EAGAIN;
    }

    ThreadStartContext *context = malloc_function(sizeof(*context));
    if (context == NULL) {
        return EAGAIN;
    }
    context->start_routine = start_routine;
    context->argument = argument;
    context->thread_instance = allocate_thread_instance();
    atomic_init(&context->parent_ready, false);

    int result = function(
        thread, attribute, thread_start_trampoline, context);
    if (result != 0) {
        if (free_function != NULL) {
            free_function(context);
        }
        return result;
    }

    if (!thread_map_put(*thread, context->thread_instance)) {
        capture_fatal();
    }
    write_thread_event(
        MINI_REPLAY_THREAD_CREATE,
        (uint64_t)(uintptr_t)*thread, context->thread_instance);

    int detach_state = PTHREAD_CREATE_JOINABLE;
    if (attribute != NULL &&
        pthread_attr_getdetachstate(attribute, &detach_state) == 0 &&
        detach_state == PTHREAD_CREATE_DETACHED) {
        write_thread_event(
            MINI_REPLAY_THREAD_DETACH,
            (uint64_t)(uintptr_t)*thread, context->thread_instance);
        thread_map_remove(*thread);
    }

    atomic_store_explicit(
        &context->parent_ready, true, memory_order_release);
    return 0;
}

__attribute__((visibility("default")))
int pthread_join(pthread_t thread, void **return_value)
{
    pthread_join_fn function = atomic_load_explicit(
        &real_pthread_join, memory_order_acquire);
    if (function == NULL) {
        resolve_real_allocators();
        function = atomic_load_explicit(
            &real_pthread_join, memory_order_acquire);
    }
    if (function == NULL) {
        return ESRCH;
    }

    uint32_t thread_instance = thread_map_get(thread);
    int result = function(thread, return_value);
    if (result == 0 && thread_instance != 0) {
        write_thread_event(
            MINI_REPLAY_THREAD_JOIN,
            (uint64_t)(uintptr_t)thread, thread_instance);
        thread_map_remove(thread);
    }
    return result;
}

__attribute__((visibility("default")))
int pthread_detach(pthread_t thread)
{
    pthread_detach_fn function = atomic_load_explicit(
        &real_pthread_detach, memory_order_acquire);
    if (function == NULL) {
        resolve_real_allocators();
        function = atomic_load_explicit(
            &real_pthread_detach, memory_order_acquire);
    }
    if (function == NULL) {
        return ESRCH;
    }

    uint32_t thread_instance = thread_map_get(thread);
    int result = function(thread);
    if (result == 0 && thread_instance != 0) {
        write_thread_event(
            MINI_REPLAY_THREAD_DETACH,
            (uint64_t)(uintptr_t)thread, thread_instance);
        thread_map_remove(thread);
    }
    return result;
}
