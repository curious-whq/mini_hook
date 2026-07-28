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

#if defined(MINI_HOLE_LOADER_PROBE)

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
 * Low-overhead allocator-hole aggregation hook.
 *
 * Real allocator symbols are resolved in the constructor, matching the
 * appspawn-safe path used by malloc_free_hook.c. Allocation entry points do
 * not touch TLS until process initialization is complete.
 */

#define BOOTSTRAP_HEAP_CAPACITY (1024U * 1024U)
#define SMALL_SIZE_CLASS_COUNT 96U
#define SIZE_BUCKET_COUNT (SMALL_SIZE_CLASS_COUNT + 1U)
#define COMPARISON_ALLOCATOR_COUNT 3U
#define DFMALLOC1_CLASS_COUNT 28U
#define DFMALLOC2_CLASS_COUNT 36U
#define JEMALLOC_CLASS_COUNT 49U
#define LARGE_ALLOCATION_ALIGNMENT 4096U
#define LOCAL_FLUSH_THRESHOLD 64U
#define PID_CHECK_INTERVAL 1024U
#define MAX_INTERVAL_SECONDS (7U * 24U * 60U * 60U)
#define WRITER_STACK_SIZE (128U * 1024U)
#define OUTPUT_DIR_CAPACITY 256U
#define OUTPUT_PATH_CAPACITY 384U
#define OUTPUT_BUFFER_CAPACITY (16U * 1024U)
#define ATOMIC_STATISTICS_SHARD_COUNT 256U
#define MIN_LIVE_TABLE_CAPACITY 1024U
#define MAX_LIVE_TABLE_CAPACITY (128U * 1024U * 1024U)
#define MAX_LIVE_SAMPLE_RATE 65536U
#define MAX_CLOCK_CHECK_EVERY 65536U
#define LIVE_TABLE_MAX_PROBES 128U
#define LIVE_SLOT_EMPTY ((uintptr_t)0U)
#define LIVE_SLOT_TOMBSTONE ((uintptr_t)1U)

/*
 * Compile-time collection configuration. Change these values and rebuild;
 * the production hook intentionally does not read matching environment
 * variables.
 */
#ifndef MINI_HOLE_INTERVAL_SEC
#define MINI_HOLE_INTERVAL_SEC 10U
#endif
#ifndef MINI_HOLE_LIVE_SAMPLE_RATE
#define MINI_HOLE_LIVE_SAMPLE_RATE 1U
#endif
#ifndef MINI_HOLE_CLOCK_CHECK_EVERY
#define MINI_HOLE_CLOCK_CHECK_EVERY 1024U
#endif
#ifndef MINI_HOLE_LIVE_CAPACITY
#define MINI_HOLE_LIVE_CAPACITY (1024U * 1024U * 64U)
#endif

_Static_assert(
    MINI_HOLE_INTERVAL_SEC >= 1U &&
        MINI_HOLE_INTERVAL_SEC <= MAX_INTERVAL_SECONDS,
    "MINI_HOLE_INTERVAL_SEC must be in [1, 604800]");
_Static_assert(
    MINI_HOLE_LIVE_SAMPLE_RATE >= 1U &&
        MINI_HOLE_LIVE_SAMPLE_RATE <= MAX_LIVE_SAMPLE_RATE &&
        (MINI_HOLE_LIVE_SAMPLE_RATE &
         (MINI_HOLE_LIVE_SAMPLE_RATE - 1U)) == 0,
    "MINI_HOLE_LIVE_SAMPLE_RATE must be a power of two in [1, 65536]");
_Static_assert(
    MINI_HOLE_CLOCK_CHECK_EVERY >= 1U &&
        MINI_HOLE_CLOCK_CHECK_EVERY <= MAX_CLOCK_CHECK_EVERY &&
        (MINI_HOLE_CLOCK_CHECK_EVERY &
         (MINI_HOLE_CLOCK_CHECK_EVERY - 1U)) == 0,
    "MINI_HOLE_CLOCK_CHECK_EVERY must be a power of two in [1, 65536]");
_Static_assert(
    MINI_HOLE_LIVE_CAPACITY >= MIN_LIVE_TABLE_CAPACITY &&
        MINI_HOLE_LIVE_CAPACITY <= MAX_LIVE_TABLE_CAPACITY &&
        (MINI_HOLE_LIVE_CAPACITY &
         (MINI_HOLE_LIVE_CAPACITY - 1U)) == 0,
    "MINI_HOLE_LIVE_CAPACITY must be a power of two in "
    "[1024, 134217728]");

#ifndef MINI_HOLE_OUTPUT_DIR
#if defined(__OHOS__)
#define MINI_HOLE_OUTPUT_DIR "/data/local/tmp"
#else
#define MINI_HOLE_OUTPUT_DIR "/tmp"
#endif
#endif

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

typedef struct {
    uint64_t allocations;
    uint64_t measured_allocations;
    uint64_t requested_bytes;
    uint64_t hole_bytes;
    uint64_t failed_allocations;
    uint64_t measurement_errors;
    uint64_t bucket_allocations[SIZE_BUCKET_COUNT];
    uint64_t bucket_hole_bytes[SIZE_BUCKET_COUNT];
} HoleSnapshot;

typedef struct {
    _Atomic uint64_t allocations;
    _Atomic uint64_t measured_allocations;
    _Atomic uint64_t requested_bytes;
    _Atomic uint64_t hole_bytes;
    _Atomic uint64_t failed_allocations;
    _Atomic uint64_t measurement_errors;
    _Atomic uint64_t bucket_allocations[SIZE_BUCKET_COUNT];
    _Atomic uint64_t bucket_hole_bytes[SIZE_BUCKET_COUNT];
} AtomicHoleStatistics;

typedef struct {
    uint64_t free_events;
    uint64_t untracked_frees;
    uint64_t tracking_failures;
    uint64_t allocations;
    uint64_t requested_bytes;
    uint64_t hole_bytes;
    uint64_t comparison_hole_bytes[COMPARISON_ALLOCATOR_COUNT];
    uint64_t bucket_allocations[SIZE_BUCKET_COUNT];
    uint64_t bucket_hole_bytes[SIZE_BUCKET_COUNT];
} LiveSnapshot;

typedef struct {
    atomic_flag lock;
    uint64_t free_events;
    uint64_t untracked_frees;
    uint64_t tracking_failures;
    uint64_t allocations;
    uint64_t requested_bytes;
    uint64_t hole_bytes;
    uint64_t comparison_hole_bytes[COMPARISON_ALLOCATOR_COUNT];
    uint64_t bucket_allocations[SIZE_BUCKET_COUNT];
    uint64_t bucket_hole_bytes[SIZE_BUCKET_COUNT];
} LiveStatisticsShard;

typedef struct {
    uintptr_t pointer;
    size_t requested;
} LiveAllocationSlot;

typedef struct {
    bool observed;
    bool tracked;
    size_t requested;
} DetachedAllocation;

/*
 * Variable-step size classes:
 *   8..256:     step 8
 *   272..512:   step 16
 *   544..1024:  step 32
 *   1088..2048: step 64
 *   2176..4096: step 128
 * Requests above 4096 share one reporting bucket and round to 4 KiB.
 */
static size_t small_size_class_at(uint32_t index)
{
    if (index < 32U) {
        return (size_t)(index + 1U) * 8U;
    }
    index -= 32U;
    if (index < 16U) {
        return 256U + (size_t)(index + 1U) * 16U;
    }
    index -= 16U;
    if (index < 16U) {
        return 512U + (size_t)(index + 1U) * 32U;
    }
    index -= 16U;
    if (index < 16U) {
        return 1024U + (size_t)(index + 1U) * 64U;
    }
    index -= 16U;
    return 2048U + (size_t)(index + 1U) * 128U;
}

static const size_t dfmalloc1_size_classes[DFMALLOC1_CLASS_COUNT] = {
    8, 16, 24, 32, 40, 48, 56, 64,
    80, 96, 112, 128, 144, 176, 208, 256,
    304, 352, 384, 432, 496, 560, 656, 784,
    992, 1312, 1984, 3968,
};

static const size_t dfmalloc2_size_classes[DFMALLOC2_CLASS_COUNT] = {
    8, 16, 32, 48, 64, 80, 96, 112, 128,
    160, 192, 224, 256, 304, 352, 384, 432, 496,
    560, 640, 784, 992, 1280, 1536, 2048,
    2560, 3072, 3584, 4096, 5120, 6144, 7168,
    8192, 10240, 12288, 14336,
};

/*
 * This is the jemalloc rule supplied with the comparison worksheet. The
 * worksheet jumps from 16384 to 40960 and ends at 262144; requests above the
 * final listed class use the same 4 KiB fallback as the other supplied rules.
 */
static const size_t jemalloc_size_classes[JEMALLOC_CLASS_COUNT] = {
    8, 16, 32, 48, 64, 80, 96, 112, 128,
    160, 192, 224, 256, 320, 384, 448, 512,
    640, 768, 896, 1024, 1280, 1536, 1792, 2048,
    2560, 3072, 3584, 4096, 5120, 6144, 7168,
    8192, 10240, 12288, 14336, 16384,
    40960, 49152, 57344, 65536, 81920, 98304,
    114688, 131072, 163840, 196608, 229376, 262144,
};

#if defined(MINI_HOLE_ATOMIC_STATS)
static _Alignas(64) AtomicHoleStatistics
    statistics[ATOMIC_STATISTICS_SHARD_COUNT];
#else
static AtomicHoleStatistics statistics;
#endif
static _Alignas(64) LiveStatisticsShard
    live_statistics[ATOMIC_STATISTICS_SHARD_COUNT];
static _Atomic uint64_t owner_pid;
static atomic_bool initialization_enabled;
static atomic_bool statistics_ready;
static atomic_bool writer_started;
#if defined(MINI_HOLE_ATOMIC_STATS)
static _Atomic uint32_t statistics_suppression;
#endif
#if defined(MINI_HOLE_OPPORTUNISTIC_SNAPSHOT)
static _Atomic uint64_t next_snapshot_monotonic_ns;
#endif
static atomic_flag process_init_lock = ATOMIC_FLAG_INIT;
static atomic_flag snapshot_lock = ATOMIC_FLAG_INIT;

#if !defined(MINI_HOLE_ATOMIC_STATS)
static pthread_key_t thread_flush_key;
static atomic_bool thread_flush_key_ready;
static __thread HoleSnapshot thread_pending;
static __thread uint32_t thread_pending_events;
static __thread uint32_t pid_check_countdown;
static __thread bool thread_key_registered;
static __thread uint32_t record_suppression;
#endif

static int log_fd = -1;
static char output_dir[OUTPUT_DIR_CAPACITY];
static char output_path[OUTPUT_PATH_CAPACITY];
static char output_buffer[OUTPUT_BUFFER_CAPACITY];
static uint64_t interval_seconds = MINI_HOLE_INTERVAL_SEC;
static uint64_t process_start_realtime_ns;
static uint64_t previous_snapshot_realtime_ns;
static HoleSnapshot previous_snapshot;
static LiveSnapshot previous_live_snapshot;
static uint64_t initial_process_pid;
static bool initial_process_is_appspawndf;
static LiveAllocationSlot *live_table;
static size_t live_table_capacity = MINI_HOLE_LIVE_CAPACITY;
static size_t live_table_mapping_size;
static bool live_table_ready;
static size_t live_sample_rate = MINI_HOLE_LIVE_SAMPLE_RATE;
static size_t clock_check_every = MINI_HOLE_CLOCK_CHECK_EVERY;

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

#define RESOLVE_ALLOCATOR(target, type, name)                              \
    do {                                                                   \
        if (atomic_load_explicit(&(target), memory_order_relaxed) == NULL) {\
            atomic_store_explicit(                                         \
                &(target), (type)dlsym(RTLD_NEXT, (name)),                 \
                memory_order_release);                                     \
        }                                                                  \
    } while (0)

    RESOLVE_ALLOCATOR(real_malloc, malloc_fn, "malloc");
    RESOLVE_ALLOCATOR(real_free, free_fn, "free");
    RESOLVE_ALLOCATOR(real_calloc, calloc_fn, "calloc");
    RESOLVE_ALLOCATOR(real_realloc, realloc_fn, "realloc");
    RESOLVE_ALLOCATOR(real_free_sized, free_sized_fn, "free_sized");
    RESOLVE_ALLOCATOR(
        real_posix_memalign, posix_memalign_fn, "posix_memalign");
    RESOLVE_ALLOCATOR(
        real_aligned_alloc, aligned_alloc_fn, "aligned_alloc");
    RESOLVE_ALLOCATOR(real_valloc, valloc_fn, "valloc");
    RESOLVE_ALLOCATOR(real_memalign, memalign_fn, "memalign");
    RESOLVE_ALLOCATOR(real_pvalloc, pvalloc_fn, "pvalloc");

#undef RESOLVE_ALLOCATOR

    atomic_store_explicit(
        &resolver_complete, true, memory_order_release);
    atomic_flag_clear_explicit(&resolver_lock, memory_order_release);
}

static uint64_t raw_getpid(void)
{
    return (uint64_t)syscall(SYS_getpid);
}

static uintptr_t live_pointer_hash(const void *ptr)
{
    uintptr_t value = (uintptr_t)ptr;
#if UINTPTR_MAX > UINT32_MAX
    value ^= value >> 33;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33;
    value *= UINT64_C(0xc4ceb9fe1a85ec53);
    value ^= value >> 33;
#else
    value ^= value >> 16;
    value *= UINT32_C(0x7feb352d);
    value ^= value >> 15;
    value *= UINT32_C(0x846ca68b);
    value ^= value >> 16;
#endif
    return value;
}

static uintptr_t statistics_pointer_hash(const void *ptr)
{
    uintptr_t value = (uintptr_t)ptr;
    value ^= value >> 17U;
    value ^= value >> 9U;
    return value;
}

static void lock_live_shard(LiveStatisticsShard *shard)
{
    while (atomic_flag_test_and_set_explicit(
            &shard->lock, memory_order_acquire)) {
        (void)syscall(SYS_sched_yield);
    }
}

static void unlock_live_shard(LiveStatisticsShard *shard)
{
    atomic_flag_clear_explicit(
        &shard->lock, memory_order_release);
}

static size_t live_shard_index(uintptr_t hash)
{
    return (size_t)hash &
        (ATOMIC_STATISTICS_SHARD_COUNT - 1U);
}

static bool live_pointer_is_sampled(uintptr_t sample_hash)
{
    return live_sample_rate == 1U ||
        ((size_t)sample_hash & (live_sample_rate - 1U)) == 0U;
}

#if defined(MINI_HOLE_OPPORTUNISTIC_SNAPSHOT)
static bool clock_check_is_due(uint64_t event_index)
{
    return clock_check_every == 1U ||
        (event_index & (clock_check_every - 1U)) == 0U;
}
#endif

static void release_live_table(void)
{
    if (live_table != NULL && live_table_mapping_size != 0) {
        (void)syscall(
            SYS_munmap, live_table, live_table_mapping_size);
    }
    live_table = NULL;
    live_table_mapping_size = 0;
    live_table_ready = false;
}

static void initialize_live_table(void)
{
    release_live_table();
    if (live_table_capacity >
        SIZE_MAX / sizeof(LiveAllocationSlot)) {
        return;
    }
    size_t mapping_size =
        live_table_capacity * sizeof(LiveAllocationSlot);
    void *mapping = (void *)syscall(
        SYS_mmap, NULL, mapping_size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        return;
    }
    live_table = (LiveAllocationSlot *)mapping;
    live_table_mapping_size = mapping_size;
    live_table_ready = true;
}

static bool live_table_insert_locked(
    const void *ptr, size_t requested, uintptr_t hash)
{
    if (!live_table_ready || ptr == NULL) {
        return false;
    }
    uintptr_t pointer = (uintptr_t)ptr;
    if (pointer <= LIVE_SLOT_TOMBSTONE) {
        return false;
    }
    size_t slots_per_shard =
        live_table_capacity / ATOMIC_STATISTICS_SHARD_COUNT;
    size_t shard = live_shard_index(hash);
    size_t shard_begin = shard * slots_per_shard;
    size_t mask = slots_per_shard - 1U;
    size_t index = (size_t)(hash >>
        8U) & mask;
    size_t probes = slots_per_shard < LIVE_TABLE_MAX_PROBES ?
        slots_per_shard : LIVE_TABLE_MAX_PROBES;
    for (size_t probe = 0; probe < probes; ++probe) {
        LiveAllocationSlot *slot =
            &live_table[shard_begin + ((index + probe) & mask)];
        uintptr_t current = slot->pointer;
        if (current != LIVE_SLOT_EMPTY &&
            current != LIVE_SLOT_TOMBSTONE) {
            continue;
        }
        slot->requested = requested;
        slot->pointer = pointer;
        return true;
    }
    return false;
}

static bool live_table_remove_locked(
    const void *ptr, size_t *requested, uintptr_t hash)
{
    if (!live_table_ready || ptr == NULL) {
        return false;
    }
    uintptr_t pointer = (uintptr_t)ptr;
    size_t slots_per_shard =
        live_table_capacity / ATOMIC_STATISTICS_SHARD_COUNT;
    size_t shard = live_shard_index(hash);
    size_t shard_begin = shard * slots_per_shard;
    size_t mask = slots_per_shard - 1U;
    size_t index = (size_t)(hash >> 8U) & mask;
    size_t probes = slots_per_shard < LIVE_TABLE_MAX_PROBES ?
        slots_per_shard : LIVE_TABLE_MAX_PROBES;
    for (size_t probe = 0; probe < probes; ++probe) {
        LiveAllocationSlot *slot =
            &live_table[shard_begin + ((index + probe) & mask)];
        uintptr_t current = slot->pointer;
        if (current == LIVE_SLOT_EMPTY) {
            return false;
        }
        if (current != pointer) {
            continue;
        }
        *requested = slot->requested;
        slot->pointer = LIVE_SLOT_TOMBSTONE;
        return true;
    }
    return false;
}

static bool raw_cmdline_contains(const char *needle)
{
    char buffer[2048];
    int fd = (int)syscall(
        SYS_openat, AT_FDCWD, "/proc/self/cmdline",
        O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    long count = syscall(
        SYS_read, fd, buffer, sizeof(buffer));
    (void)syscall(SYS_close, fd);
    if (count <= 0) {
        return false;
    }

    size_t needle_size = 0;
    while (needle[needle_size] != '\0') {
        ++needle_size;
    }
    if (needle_size == 0 || (size_t)count < needle_size) {
        return false;
    }
    for (size_t i = 0; i + needle_size <= (size_t)count; ++i) {
        size_t j = 0;
        while (j < needle_size &&
               buffer[i + j] == needle[j]) {
            ++j;
        }
        if (j == needle_size) {
            return true;
        }
    }
    return false;
}

static uint64_t realtime_ns(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_REALTIME, &value) != 0) {
        return 0;
    }
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
           (uint64_t)value.tv_nsec;
}

#if defined(MINI_HOLE_OPPORTUNISTIC_SNAPSHOT)
static uint64_t monotonic_ns(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return 0;
    }
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
           (uint64_t)value.tv_nsec;
}
#endif

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

static void copy_output_dir(const char *source)
{
    size_t index = 0;
    while (source[index] != '\0' &&
           index + 1 < sizeof(output_dir)) {
        output_dir[index] = source[index];
        ++index;
    }
    output_dir[index] = '\0';
}

static void prepare_output_path(uint64_t pid)
{
    size_t offset = append_text(
        output_path, sizeof(output_path) - 1, 0, output_dir);
    if (offset != 0 && output_path[offset - 1] != '/') {
        offset = append_text(
            output_path, sizeof(output_path) - 1, offset, "/");
    }
    offset = append_text(
        output_path, sizeof(output_path) - 1, offset, "mini_hole_");
    offset = append_u64(
        output_path, sizeof(output_path) - 1, offset,
        process_start_realtime_ns);
    offset = append_text(
        output_path, sizeof(output_path) - 1, offset, "_");
    offset = append_u64(
        output_path, sizeof(output_path) - 1, offset, pid);
    offset = append_text(
        output_path, sizeof(output_path) - 1, offset, ".csv");
    output_path[offset] = '\0';
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

static void close_log(void)
{
    if (log_fd >= 0) {
        (void)syscall(SYS_close, log_fd);
        log_fd = -1;
    }
}

static size_t append_class_list_metadata(
    char *buffer, size_t offset, const char *name,
    const size_t *classes, uint32_t class_count)
{
    offset = append_text(
        buffer, OUTPUT_BUFFER_CAPACITY, offset, ",");
    offset = append_text(
        buffer, OUTPUT_BUFFER_CAPACITY, offset, name);
    offset = append_text(
        buffer, OUTPUT_BUFFER_CAPACITY, offset, "=");
    for (uint32_t i = 0; i < class_count; ++i) {
        if (i != 0) {
            offset = append_text(
                buffer, OUTPUT_BUFFER_CAPACITY, offset, "|");
        }
        offset = append_u64(
            buffer, OUTPUT_BUFFER_CAPACITY, offset, classes[i]);
    }
    return offset;
}

static size_t append_csv_header(char *buffer, size_t offset, uint64_t pid)
{
    offset = append_text(
        buffer, OUTPUT_BUFFER_CAPACITY, offset,
#if defined(MINI_HOLE_OPPORTUNISTIC_SNAPSHOT)
        "#mini_malloc_hole_v9,mode=atomic_sharded_opportunistic,"
        "clock=monotonic,initial_snapshot=1,"
        "no_tls=1,no_writer=1,clock_check_every="
#else
        "#mini_malloc_hole_v9,mode=diagnostic,"
        "clock_check_every="
#endif
    );
    offset = append_u64(
        buffer, OUTPUT_BUFFER_CAPACITY, offset,
        (uint64_t)clock_check_every);
    offset = append_text(
        buffer, OUTPUT_BUFFER_CAPACITY, offset,
        ",scope=post_process_start,inherited_allocations=excluded,"
        "comparison_rules=mini96|dfmalloc1.0|dfmalloc2.0|jemalloc,"
        "comparison_fallback=4K,"
        "jemalloc_last_explicit_class=262144,"
        "live_values=");
    offset = append_text(
        buffer, OUTPUT_BUFFER_CAPACITY, offset,
        live_sample_rate == 1U ? "exact" : "estimated");
    offset = append_text(
        buffer, OUTPUT_BUFFER_CAPACITY, offset,
        ",live_table=sharded_open_addressing,"
        "live_table_capacity=");
    offset = append_u64(
        buffer, OUTPUT_BUFFER_CAPACITY, offset,
        (uint64_t)live_table_capacity);
    offset = append_text(
        buffer, OUTPUT_BUFFER_CAPACITY, offset,
        ",live_table_max_probes=");
    offset = append_u64(
        buffer, OUTPUT_BUFFER_CAPACITY, offset,
        LIVE_TABLE_MAX_PROBES);
    offset = append_text(
        buffer, OUTPUT_BUFFER_CAPACITY, offset,
        ",live_sample_rate=");
    offset = append_u64(
        buffer, OUTPUT_BUFFER_CAPACITY, offset,
        (uint64_t)live_sample_rate);
    offset = append_text(
        buffer, OUTPUT_BUFFER_CAPACITY, offset,
        ",live_table_ready=");
    offset = append_u64(
        buffer, OUTPUT_BUFFER_CAPACITY, offset,
        live_table_ready ? 1U : 0U);
    offset = append_text(
        buffer, OUTPUT_BUFFER_CAPACITY, offset,
        ",unit=byte,pid=");
    offset = append_u64(
        buffer, OUTPUT_BUFFER_CAPACITY, offset, pid);
    offset = append_text(
        buffer, OUTPUT_BUFFER_CAPACITY, offset,
        ",interval_seconds=");
    offset = append_u64(
        buffer, OUTPUT_BUFFER_CAPACITY, offset, interval_seconds);
    offset = append_class_list_metadata(
        buffer, offset, "dfmalloc1_classes",
        dfmalloc1_size_classes, DFMALLOC1_CLASS_COUNT);
    offset = append_class_list_metadata(
        buffer, offset, "dfmalloc2_classes",
        dfmalloc2_size_classes, DFMALLOC2_CLASS_COUNT);
    offset = append_class_list_metadata(
        buffer, offset, "jemalloc_classes",
        jemalloc_size_classes, JEMALLOC_CLASS_COUNT);
    offset = append_text(
        buffer, OUTPUT_BUFFER_CAPACITY, offset,
        ",size_classes=");
    for (uint32_t i = 0; i < SMALL_SIZE_CLASS_COUNT; ++i) {
        if (i != 0) {
            offset = append_text(
                buffer, OUTPUT_BUFFER_CAPACITY, offset, "|");
        }
        offset = append_u64(
            buffer, OUTPUT_BUFFER_CAPACITY, offset,
            small_size_class_at(i));
    }
    offset = append_text(
        buffer, OUTPUT_BUFFER_CAPACITY, offset,
        "|4K+\n"
        "start_ns,end_ns,"
        "period_alloc,total_alloc,"
        "period_measured,total_measured,"
        "period_requested,total_requested,"
        "period_hole,total_hole,"
        "period_failed,total_failed,"
        "period_measure_error,total_measure_error,"
        "period_free,total_free,"
        "period_untracked_free,total_untracked_free,"
        "period_live_track_failed,total_live_track_failed,"
        "live_alloc,live_requested,live_allocated,live_hole,"
        "live_hole_dfmalloc1,live_hole_dfmalloc2,"
        "live_hole_jemalloc");
    for (uint32_t i = 0; i < SMALL_SIZE_CLASS_COUNT; ++i) {
        offset = append_text(
            buffer, OUTPUT_BUFFER_CAPACITY, offset, ",count_");
        offset = append_u64(
            buffer, OUTPUT_BUFFER_CAPACITY, offset,
            small_size_class_at(i));
        offset = append_text(
            buffer, OUTPUT_BUFFER_CAPACITY, offset, ",hole_");
        offset = append_u64(
            buffer, OUTPUT_BUFFER_CAPACITY, offset,
            small_size_class_at(i));
    }
    offset = append_text(
        buffer, OUTPUT_BUFFER_CAPACITY, offset,
        ",count_4K_plus,hole_4K_plus");
    for (uint32_t i = 0; i < SMALL_SIZE_CLASS_COUNT; ++i) {
        offset = append_text(
            buffer, OUTPUT_BUFFER_CAPACITY, offset, ",live_count_");
        offset = append_u64(
            buffer, OUTPUT_BUFFER_CAPACITY, offset,
            small_size_class_at(i));
        offset = append_text(
            buffer, OUTPUT_BUFFER_CAPACITY, offset, ",live_hole_");
        offset = append_u64(
            buffer, OUTPUT_BUFFER_CAPACITY, offset,
            small_size_class_at(i));
    }
    return append_text(
        buffer, OUTPUT_BUFFER_CAPACITY, offset,
        ",live_count_4K_plus,live_hole_4K_plus\n");
}

static void open_log(uint64_t pid)
{
    close_log();
    int fd = (int)syscall(
        SYS_openat, AT_FDCWD, output_path,
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return;
    }

    size_t size = append_csv_header(output_buffer, 0, pid);
    if (!raw_write_all(fd, output_buffer, size)) {
        (void)syscall(SYS_close, fd);
        return;
    }
    log_fd = fd;
}

static void clear_statistics(void)
{
#define CLEAR_COUNTER(counter) \
    atomic_store_explicit(&(counter), 0, memory_order_relaxed)
#if defined(MINI_HOLE_ATOMIC_STATS)
    for (uint32_t shard = 0;
         shard < ATOMIC_STATISTICS_SHARD_COUNT; ++shard) {
        CLEAR_COUNTER(statistics[shard].allocations);
        CLEAR_COUNTER(statistics[shard].measured_allocations);
        CLEAR_COUNTER(statistics[shard].requested_bytes);
        CLEAR_COUNTER(statistics[shard].hole_bytes);
        CLEAR_COUNTER(statistics[shard].failed_allocations);
        CLEAR_COUNTER(statistics[shard].measurement_errors);
        for (uint32_t i = 0; i < SIZE_BUCKET_COUNT; ++i) {
            CLEAR_COUNTER(
                statistics[shard].bucket_allocations[i]);
            CLEAR_COUNTER(
                statistics[shard].bucket_hole_bytes[i]);
        }
    }
#else
    CLEAR_COUNTER(statistics.allocations);
    CLEAR_COUNTER(statistics.measured_allocations);
    CLEAR_COUNTER(statistics.requested_bytes);
    CLEAR_COUNTER(statistics.hole_bytes);
    CLEAR_COUNTER(statistics.failed_allocations);
    CLEAR_COUNTER(statistics.measurement_errors);
    for (uint32_t i = 0; i < SIZE_BUCKET_COUNT; ++i) {
        CLEAR_COUNTER(statistics.bucket_allocations[i]);
        CLEAR_COUNTER(statistics.bucket_hole_bytes[i]);
    }
#endif
#undef CLEAR_COUNTER
}

static void clear_live_statistics(void)
{
    for (uint32_t shard = 0;
         shard < ATOMIC_STATISTICS_SHARD_COUNT; ++shard) {
        LiveStatisticsShard *current = &live_statistics[shard];
        atomic_flag_clear_explicit(
            &current->lock, memory_order_relaxed);
        current->free_events = 0;
        current->untracked_frees = 0;
        current->tracking_failures = 0;
        current->allocations = 0;
        current->requested_bytes = 0;
        current->hole_bytes = 0;
        for (uint32_t i = 0;
             i < COMPARISON_ALLOCATOR_COUNT; ++i) {
            current->comparison_hole_bytes[i] = 0;
        }
        for (uint32_t i = 0; i < SIZE_BUCKET_COUNT; ++i) {
            current->bucket_allocations[i] = 0;
            current->bucket_hole_bytes[i] = 0;
        }
    }
}

static void load_statistics(HoleSnapshot *snapshot)
{
#define LOAD_COUNTER(counter) \
    atomic_load_explicit(&(counter), memory_order_relaxed)
#if defined(MINI_HOLE_ATOMIC_STATS)
    zero_bytes((unsigned char *)snapshot, sizeof(*snapshot));
    for (uint32_t shard = 0;
         shard < ATOMIC_STATISTICS_SHARD_COUNT; ++shard) {
        snapshot->requested_bytes +=
            LOAD_COUNTER(statistics[shard].requested_bytes);
        snapshot->failed_allocations +=
            LOAD_COUNTER(statistics[shard].failed_allocations);
        snapshot->measurement_errors +=
            LOAD_COUNTER(statistics[shard].measurement_errors);
        for (uint32_t i = 0; i < SIZE_BUCKET_COUNT; ++i) {
            uint64_t count = LOAD_COUNTER(
                statistics[shard].bucket_allocations[i]);
            uint64_t hole = LOAD_COUNTER(
                statistics[shard].bucket_hole_bytes[i]);
            snapshot->bucket_allocations[i] += count;
            snapshot->bucket_hole_bytes[i] += hole;
            snapshot->measured_allocations += count;
            snapshot->hole_bytes += hole;
        }
    }
    snapshot->allocations =
        snapshot->measured_allocations +
        snapshot->measurement_errors;
#else
    snapshot->allocations = LOAD_COUNTER(statistics.allocations);
    snapshot->measured_allocations =
        LOAD_COUNTER(statistics.measured_allocations);
    snapshot->requested_bytes =
        LOAD_COUNTER(statistics.requested_bytes);
    snapshot->hole_bytes = LOAD_COUNTER(statistics.hole_bytes);
    snapshot->failed_allocations =
        LOAD_COUNTER(statistics.failed_allocations);
    snapshot->measurement_errors =
        LOAD_COUNTER(statistics.measurement_errors);
    for (uint32_t i = 0; i < SIZE_BUCKET_COUNT; ++i) {
        snapshot->bucket_allocations[i] =
            LOAD_COUNTER(statistics.bucket_allocations[i]);
        snapshot->bucket_hole_bytes[i] =
            LOAD_COUNTER(statistics.bucket_hole_bytes[i]);
    }
#endif
#undef LOAD_COUNTER
}

static uint64_t scale_live_counter(uint64_t value)
{
    if (live_sample_rate == 1U) {
        return value;
    }
    if (value > UINT64_MAX / live_sample_rate) {
        return UINT64_MAX;
    }
    return value * live_sample_rate;
}

static void load_live_statistics(LiveSnapshot *snapshot)
{
    zero_bytes((unsigned char *)snapshot, sizeof(*snapshot));
    for (uint32_t shard = 0;
         shard < ATOMIC_STATISTICS_SHARD_COUNT; ++shard) {
        LiveStatisticsShard *current = &live_statistics[shard];
        lock_live_shard(current);
        snapshot->free_events += current->free_events;
        snapshot->untracked_frees += current->untracked_frees;
        snapshot->tracking_failures += current->tracking_failures;
        snapshot->allocations += current->allocations;
        snapshot->requested_bytes += current->requested_bytes;
        snapshot->hole_bytes += current->hole_bytes;
        for (uint32_t i = 0;
             i < COMPARISON_ALLOCATOR_COUNT; ++i) {
            snapshot->comparison_hole_bytes[i] +=
                current->comparison_hole_bytes[i];
        }
        for (uint32_t i = 0; i < SIZE_BUCKET_COUNT; ++i) {
            snapshot->bucket_allocations[i] +=
                current->bucket_allocations[i];
            snapshot->bucket_hole_bytes[i] +=
                current->bucket_hole_bytes[i];
        }
        unlock_live_shard(current);
    }
    snapshot->free_events =
        scale_live_counter(snapshot->free_events);
    snapshot->untracked_frees =
        scale_live_counter(snapshot->untracked_frees);
    snapshot->tracking_failures =
        scale_live_counter(snapshot->tracking_failures);
    snapshot->allocations =
        scale_live_counter(snapshot->allocations);
    snapshot->requested_bytes =
        scale_live_counter(snapshot->requested_bytes);
    snapshot->hole_bytes =
        scale_live_counter(snapshot->hole_bytes);
    for (uint32_t i = 0;
         i < COMPARISON_ALLOCATOR_COUNT; ++i) {
        snapshot->comparison_hole_bytes[i] =
            scale_live_counter(
                snapshot->comparison_hole_bytes[i]);
    }
    for (uint32_t i = 0; i < SIZE_BUCKET_COUNT; ++i) {
        snapshot->bucket_allocations[i] =
            scale_live_counter(snapshot->bucket_allocations[i]);
        snapshot->bucket_hole_bytes[i] =
            scale_live_counter(snapshot->bucket_hole_bytes[i]);
    }
}

static void reset_current_thread(void)
{
#if !defined(MINI_HOLE_ATOMIC_STATS)
    zero_bytes(
        (unsigned char *)&thread_pending, sizeof(thread_pending));
    thread_pending_events = 0;
    pid_check_countdown = 0;
    thread_key_registered = false;
    record_suppression = 0;
#endif
}

#if !defined(MINI_HOLE_ATOMIC_STATS)
static void flush_thread_pending(void)
{
    if (thread_pending_events == 0 ||
        !atomic_load_explicit(
            &statistics_ready, memory_order_acquire)) {
        return;
    }

    ++record_suppression;
#define ADD_PENDING(counter, value)                                      \
    do {                                                                 \
        if ((value) != 0) {                                              \
            atomic_fetch_add_explicit(                                   \
                &(counter), (value), memory_order_relaxed);              \
        }                                                                \
    } while (0)
    ADD_PENDING(statistics.allocations, thread_pending.allocations);
    ADD_PENDING(
        statistics.measured_allocations,
        thread_pending.measured_allocations);
    ADD_PENDING(
        statistics.requested_bytes, thread_pending.requested_bytes);
    ADD_PENDING(statistics.hole_bytes, thread_pending.hole_bytes);
    ADD_PENDING(
        statistics.failed_allocations,
        thread_pending.failed_allocations);
    ADD_PENDING(
        statistics.measurement_errors,
        thread_pending.measurement_errors);
    for (uint32_t i = 0; i < SIZE_BUCKET_COUNT; ++i) {
        ADD_PENDING(
            statistics.bucket_allocations[i],
            thread_pending.bucket_allocations[i]);
        ADD_PENDING(
            statistics.bucket_hole_bytes[i],
            thread_pending.bucket_hole_bytes[i]);
    }
#undef ADD_PENDING

    zero_bytes(
        (unsigned char *)&thread_pending, sizeof(thread_pending));
    thread_pending_events = 0;
    --record_suppression;
}

static void thread_flush_destructor(void *value)
{
    (void)value;
    /*
     * pthread has already cleared the key before invoking this callback.
     * Keep the local flag true while flushing so the key is not installed
     * again from its own destructor.
     */
    thread_key_registered = true;
    flush_thread_pending();
    thread_key_registered = false;
}

static void ensure_thread_flush_registered(void)
{
    if (thread_key_registered ||
        !atomic_load_explicit(
            &thread_flush_key_ready, memory_order_acquire)) {
        return;
    }
    ++record_suppression;
    if (pthread_setspecific(
            thread_flush_key, (void *)(uintptr_t)1) == 0) {
        thread_key_registered = true;
    }
    --record_suppression;
}

static void finish_pending_event(void)
{
    ++thread_pending_events;
    ensure_thread_flush_registered();
    if (thread_pending_events >= LOCAL_FLUSH_THRESHOLD) {
        flush_thread_pending();
    }
}
#endif

static bool rounded_allocation_size(
    size_t requested, size_t *rounded, uint32_t *bucket)
{
    size_t value = requested == 0 ? 1U : requested;
    if (value <= 256U) {
        *rounded = (value + 7U) & ~(size_t)7U;
        *bucket = (uint32_t)(*rounded / 8U - 1U);
        return true;
    }
    if (value <= 512U) {
        *rounded = 256U + ((value - 256U + 15U) & ~(size_t)15U);
        *bucket = 31U + (uint32_t)((*rounded - 256U) / 16U);
        return true;
    }
    if (value <= 1024U) {
        *rounded = 512U + ((value - 512U + 31U) & ~(size_t)31U);
        *bucket = 47U + (uint32_t)((*rounded - 512U) / 32U);
        return true;
    }
    if (value <= 2048U) {
        *rounded = 1024U + ((value - 1024U + 63U) & ~(size_t)63U);
        *bucket = 63U + (uint32_t)((*rounded - 1024U) / 64U);
        return true;
    }
    if (value <= 4096U) {
        *rounded = 2048U + ((value - 2048U + 127U) & ~(size_t)127U);
        *bucket = 79U + (uint32_t)((*rounded - 2048U) / 128U);
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

static void initialize_process(uint64_t pid);

static bool ensure_current_process(void)
{
    uint64_t pid = raw_getpid();
    if (atomic_load_explicit(&owner_pid, memory_order_acquire) == pid &&
        atomic_load_explicit(
            &statistics_ready, memory_order_acquire)) {
        return true;
    }
    if (atomic_flag_test_and_set_explicit(
            &process_init_lock, memory_order_acquire)) {
        return false;
    }

    if (atomic_load_explicit(&owner_pid, memory_order_relaxed) != pid ||
        !atomic_load_explicit(
            &statistics_ready, memory_order_relaxed)) {
        initialize_process(pid);
    }
    atomic_flag_clear_explicit(
        &process_init_lock, memory_order_release);
    return atomic_load_explicit(
        &statistics_ready, memory_order_acquire);
}

static bool prepare_recording(void)
{
    /*
     * Do not access any __thread state before the constructor finishes.
     * This ordering is required by appspawndf's early loader path.
     */
    if (!atomic_load_explicit(
            &initialization_enabled, memory_order_acquire)) {
        return false;
    }
#if defined(MINI_HOLE_ATOMIC_STATS)
    if (atomic_load_explicit(
            &statistics_suppression, memory_order_acquire) != 0) {
        return false;
    }
    /*
     * Do not pay getpid() on every allocation. The initial appspawndf is
     * intentionally not measured; its forked child observes a different PID
     * once, initializes its own statistics, and then stays on this fast path.
     */
    if (initial_process_is_appspawndf &&
        atomic_load_explicit(
            &owner_pid, memory_order_acquire) ==
            initial_process_pid) {
        uint64_t pid = raw_getpid();
        if (pid == initial_process_pid) {
            return false;
        }
        return ensure_current_process();
    }
    return atomic_load_explicit(
        &statistics_ready, memory_order_acquire);
#else
    if (record_suppression != 0) {
        return false;
    }
    if (!atomic_load_explicit(
            &statistics_ready, memory_order_acquire)) {
        return ensure_current_process();
    }
    if (pid_check_countdown == 0) {
        pid_check_countdown = PID_CHECK_INTERVAL;
        return ensure_current_process();
    }
    --pid_check_countdown;
    return true;
#endif
}

#if defined(MINI_HOLE_OPPORTUNISTIC_SNAPSHOT)
static void maybe_write_periodic_snapshot(void);
#endif

static size_t round_by_size_classes(
    size_t requested, const size_t *classes, uint32_t class_count)
{
    size_t value = requested == 0 ? 1U : requested;
    if (value <= classes[class_count - 1U]) {
        uint32_t low = 0;
        uint32_t high = class_count;
        while (low < high) {
            uint32_t middle = low + (high - low) / 2U;
            if (classes[middle] < value) {
                low = middle + 1U;
            } else {
                high = middle;
            }
        }
        return classes[low];
    }
    return (value + LARGE_ALLOCATION_ALIGNMENT - 1U) &
        ~((size_t)LARGE_ALLOCATION_ALIGNMENT - 1U);
}

static void comparison_holes(
    size_t requested,
    uint64_t holes[COMPARISON_ALLOCATOR_COUNT])
{
    holes[0] = (uint64_t)(
        round_by_size_classes(
            requested, dfmalloc1_size_classes,
            DFMALLOC1_CLASS_COUNT) - requested);
    holes[1] = (uint64_t)(
        round_by_size_classes(
            requested, dfmalloc2_size_classes,
            DFMALLOC2_CLASS_COUNT) - requested);
    holes[2] = (uint64_t)(
        round_by_size_classes(
            requested, jemalloc_size_classes,
            JEMALLOC_CLASS_COUNT) - requested);
}

static void add_live_counters_locked(
    LiveStatisticsShard *shard,
    size_t requested, size_t rounded, uint32_t bucket,
    const uint64_t alternate_holes[COMPARISON_ALLOCATOR_COUNT])
{
    uint64_t hole = (uint64_t)(rounded - requested);
    ++shard->allocations;
    shard->requested_bytes += (uint64_t)requested;
    shard->hole_bytes += hole;
    for (uint32_t i = 0;
         i < COMPARISON_ALLOCATOR_COUNT; ++i) {
        shard->comparison_hole_bytes[i] += alternate_holes[i];
    }
    ++shard->bucket_allocations[bucket];
    shard->bucket_hole_bytes[bucket] += hole;
}

static void subtract_live_counters_locked(
    LiveStatisticsShard *shard,
    size_t requested, size_t rounded, uint32_t bucket)
{
    uint64_t hole = (uint64_t)(rounded - requested);
    uint64_t alternate_holes[COMPARISON_ALLOCATOR_COUNT];
    comparison_holes(requested, alternate_holes);
    --shard->allocations;
    shard->requested_bytes -= (uint64_t)requested;
    shard->hole_bytes -= hole;
    for (uint32_t i = 0;
         i < COMPARISON_ALLOCATOR_COUNT; ++i) {
        shard->comparison_hole_bytes[i] -= alternate_holes[i];
    }
    --shard->bucket_allocations[bucket];
    shard->bucket_hole_bytes[bucket] -= hole;
}

static bool track_live_allocation(
    const void *ptr, size_t requested, size_t rounded,
    uint32_t bucket, uintptr_t sample_hash)
{
    if (!live_pointer_is_sampled(sample_hash)) {
        return false;
    }
    uint64_t alternate_holes[COMPARISON_ALLOCATOR_COUNT];
    comparison_holes(requested, alternate_holes);
    uintptr_t hash = live_pointer_hash(ptr);
    LiveStatisticsShard *shard =
        &live_statistics[live_shard_index(hash)];
    lock_live_shard(shard);
    bool inserted =
        live_table_insert_locked(ptr, requested, hash);
    if (inserted) {
        add_live_counters_locked(
            shard, requested, rounded, bucket,
            alternate_holes);
    } else {
        ++shard->tracking_failures;
    }
    unlock_live_shard(shard);
    return inserted;
}

static DetachedAllocation detach_live_allocation(const void *ptr)
{
    DetachedAllocation detached = {
        .observed = false,
        .tracked = false,
        .requested = 0,
    };
    if (!prepare_recording()) {
        return detached;
    }
    uintptr_t sample_hash = statistics_pointer_hash(ptr);
    if (!live_pointer_is_sampled(sample_hash)) {
        return detached;
    }
    uintptr_t hash = live_pointer_hash(ptr);
    detached.observed = true;
    LiveStatisticsShard *shard =
        &live_statistics[live_shard_index(hash)];
    lock_live_shard(shard);
    size_t requested = 0;
    if (!live_table_remove_locked(ptr, &requested, hash)) {
        unlock_live_shard(shard);
        return detached;
    }

    size_t rounded;
    uint32_t bucket;
    if (!rounded_allocation_size(requested, &rounded, &bucket)) {
        ++shard->tracking_failures;
        unlock_live_shard(shard);
        return detached;
    }
    subtract_live_counters_locked(
        shard, requested, rounded, bucket);
    unlock_live_shard(shard);
    detached.tracked = true;
    detached.requested = requested;
    return detached;
}

static void restore_detached_allocation(
    const void *ptr, const DetachedAllocation *detached)
{
    if (!detached->tracked) {
        return;
    }
    size_t rounded;
    uint32_t bucket;
    if (!rounded_allocation_size(
            detached->requested, &rounded, &bucket) ||
        !track_live_allocation(
            ptr, detached->requested, rounded, bucket,
            statistics_pointer_hash(ptr))) {
        return;
    }
}

static void record_release_event(
    const void *ptr, const DetachedAllocation *detached)
{
    if (!detached->observed) {
        return;
    }
    uintptr_t hash = live_pointer_hash(ptr);
    LiveStatisticsShard *shard =
        &live_statistics[live_shard_index(hash)];
    lock_live_shard(shard);
    ++shard->free_events;
    if (!detached->tracked) {
        ++shard->untracked_frees;
    }
    unlock_live_shard(shard);
}

static void record_free_allocation(const void *ptr)
{
    if (!prepare_recording()) {
        return;
    }
    uintptr_t sample_hash = statistics_pointer_hash(ptr);
    if (!live_pointer_is_sampled(sample_hash)) {
        return;
    }
    uintptr_t hash = live_pointer_hash(ptr);
    LiveStatisticsShard *shard =
        &live_statistics[live_shard_index(hash)];
    lock_live_shard(shard);
#if defined(MINI_HOLE_OPPORTUNISTIC_SNAPSHOT)
    uint64_t free_index = shard->free_events;
#endif
    ++shard->free_events;
    size_t requested = 0;
    if (live_table_remove_locked(ptr, &requested, hash)) {
        size_t rounded;
        uint32_t bucket;
        if (rounded_allocation_size(requested, &rounded, &bucket)) {
            subtract_live_counters_locked(
                shard, requested, rounded, bucket);
        } else {
            ++shard->tracking_failures;
        }
    } else {
        ++shard->untracked_frees;
    }
    unlock_live_shard(shard);
#if defined(MINI_HOLE_OPPORTUNISTIC_SNAPSHOT)
    if (clock_check_is_due(free_index)) {
        maybe_write_periodic_snapshot();
    }
#endif
}

static void record_failed_allocation(void)
{
    if (!prepare_recording()) {
        return;
    }
#if defined(MINI_HOLE_ATOMIC_STATS)
    atomic_fetch_add_explicit(
        &statistics[0].failed_allocations, 1,
        memory_order_relaxed);
#else
    ++thread_pending.failed_allocations;
    finish_pending_event();
#endif
}

static void record_successful_allocation(
    size_t requested, const void *ptr)
{
    if (!prepare_recording()) {
        return;
    }
    uintptr_t hash = statistics_pointer_hash(ptr);
#if defined(MINI_HOLE_ATOMIC_STATS)
    AtomicHoleStatistics *shard =
        &statistics[
            hash & (ATOMIC_STATISTICS_SHARD_COUNT - 1U)];
#else
    (void)ptr;
    ++thread_pending.allocations;
#endif

    size_t rounded;
    uint32_t bucket;
    if (!rounded_allocation_size(requested, &rounded, &bucket)) {
#if defined(MINI_HOLE_ATOMIC_STATS)
        atomic_fetch_add_explicit(
            &shard->measurement_errors, 1,
            memory_order_relaxed);
#else
        ++thread_pending.measurement_errors;
        finish_pending_event();
#endif
        return;
    }

    uint64_t hole = (uint64_t)(rounded - requested);
#if defined(MINI_HOLE_ATOMIC_STATS)
    atomic_fetch_add_explicit(
        &shard->requested_bytes, (uint64_t)requested,
        memory_order_relaxed);
    uint64_t allocation_index = atomic_fetch_add_explicit(
        &shard->bucket_allocations[bucket], 1,
        memory_order_relaxed);
    (void)allocation_index;
    atomic_fetch_add_explicit(
        &shard->bucket_hole_bytes[bucket], hole,
        memory_order_relaxed);
    (void)track_live_allocation(
        ptr, requested, rounded, bucket, hash);
#if defined(MINI_HOLE_OPPORTUNISTIC_SNAPSHOT)
    if (clock_check_is_due(allocation_index)) {
        maybe_write_periodic_snapshot();
    }
#endif
#else
    ++thread_pending.measured_allocations;
    thread_pending.requested_bytes += (uint64_t)requested;
    thread_pending.hole_bytes += hole;
    ++thread_pending.bucket_allocations[bucket];
    thread_pending.bucket_hole_bytes[bucket] += hole;
    (void)track_live_allocation(
        ptr, requested, rounded, bucket, hash);
    finish_pending_event();
#endif
}

static HoleSnapshot snapshot_delta(
    const HoleSnapshot *current, const HoleSnapshot *previous)
{
    HoleSnapshot delta;
    delta.allocations = current->allocations - previous->allocations;
    delta.measured_allocations =
        current->measured_allocations -
        previous->measured_allocations;
    delta.requested_bytes =
        current->requested_bytes - previous->requested_bytes;
    delta.hole_bytes = current->hole_bytes - previous->hole_bytes;
    delta.failed_allocations =
        current->failed_allocations - previous->failed_allocations;
    delta.measurement_errors =
        current->measurement_errors -
        previous->measurement_errors;
    for (uint32_t i = 0; i < SIZE_BUCKET_COUNT; ++i) {
        delta.bucket_allocations[i] =
            current->bucket_allocations[i] -
            previous->bucket_allocations[i];
        delta.bucket_hole_bytes[i] =
            current->bucket_hole_bytes[i] -
            previous->bucket_hole_bytes[i];
    }
    return delta;
}

static LiveSnapshot live_event_delta(
    const LiveSnapshot *current, const LiveSnapshot *previous)
{
    LiveSnapshot delta;
    zero_bytes((unsigned char *)&delta, sizeof(delta));
    delta.free_events =
        current->free_events - previous->free_events;
    delta.untracked_frees =
        current->untracked_frees - previous->untracked_frees;
    delta.tracking_failures =
        current->tracking_failures - previous->tracking_failures;
    return delta;
}

static size_t append_csv_row(
    char *buffer, uint64_t start_ns, uint64_t end_ns,
    const HoleSnapshot *period, const HoleSnapshot *total,
    const LiveSnapshot *live_period, const LiveSnapshot *live)
{
    size_t offset = 0;
#define APPEND_VALUE(value)                                                \
    do {                                                                   \
        offset = append_u64(                                               \
            buffer, OUTPUT_BUFFER_CAPACITY, offset, (value));             \
        offset = append_text(                                              \
            buffer, OUTPUT_BUFFER_CAPACITY, offset, ",");                 \
    } while (0)
    APPEND_VALUE(start_ns);
    APPEND_VALUE(end_ns);
    APPEND_VALUE(period->allocations);
    APPEND_VALUE(total->allocations);
    APPEND_VALUE(period->measured_allocations);
    APPEND_VALUE(total->measured_allocations);
    APPEND_VALUE(period->requested_bytes);
    APPEND_VALUE(total->requested_bytes);
    APPEND_VALUE(period->hole_bytes);
    APPEND_VALUE(total->hole_bytes);
    APPEND_VALUE(period->failed_allocations);
    APPEND_VALUE(total->failed_allocations);
    APPEND_VALUE(period->measurement_errors);
    APPEND_VALUE(total->measurement_errors);
    APPEND_VALUE(live_period->free_events);
    APPEND_VALUE(live->free_events);
    APPEND_VALUE(live_period->untracked_frees);
    APPEND_VALUE(live->untracked_frees);
    APPEND_VALUE(live_period->tracking_failures);
    APPEND_VALUE(live->tracking_failures);
    APPEND_VALUE(live->allocations);
    APPEND_VALUE(live->requested_bytes);
    APPEND_VALUE(live->requested_bytes + live->hole_bytes);
    APPEND_VALUE(live->hole_bytes);
    APPEND_VALUE(live->comparison_hole_bytes[0]);
    APPEND_VALUE(live->comparison_hole_bytes[1]);
    APPEND_VALUE(live->comparison_hole_bytes[2]);
    for (uint32_t i = 0; i < SIZE_BUCKET_COUNT; ++i) {
        APPEND_VALUE(period->bucket_allocations[i]);
        APPEND_VALUE(period->bucket_hole_bytes[i]);
    }
    for (uint32_t i = 0; i < SIZE_BUCKET_COUNT; ++i) {
        APPEND_VALUE(live->bucket_allocations[i]);
        offset = append_u64(
            buffer, OUTPUT_BUFFER_CAPACITY, offset,
            live->bucket_hole_bytes[i]);
        if (i + 1 < SIZE_BUCKET_COUNT) {
            offset = append_text(
                buffer, OUTPUT_BUFFER_CAPACITY, offset, ",");
        }
    }
#undef APPEND_VALUE
    return append_text(
        buffer, OUTPUT_BUFFER_CAPACITY, offset, "\n");
}

static void lock_flag(atomic_flag *flag)
{
    while (atomic_flag_test_and_set_explicit(
            flag, memory_order_acquire)) {
        (void)syscall(SYS_sched_yield);
    }
}

static void write_snapshot(void)
{
    lock_flag(&snapshot_lock);
    uint64_t pid = raw_getpid();
    if (atomic_load_explicit(&owner_pid, memory_order_acquire) != pid ||
        log_fd < 0) {
        atomic_flag_clear_explicit(
            &snapshot_lock, memory_order_release);
        return;
    }

    HoleSnapshot total;
    load_statistics(&total);
    HoleSnapshot period =
        snapshot_delta(&total, &previous_snapshot);
    LiveSnapshot live;
    load_live_statistics(&live);
    LiveSnapshot live_period =
        live_event_delta(&live, &previous_live_snapshot);
    uint64_t end_ns = realtime_ns();

    size_t size = append_csv_row(
        output_buffer, previous_snapshot_realtime_ns, end_ns,
        &period, &total, &live_period, &live);
    if (raw_write_all(log_fd, output_buffer, size)) {
        previous_snapshot = total;
        previous_live_snapshot = live;
        previous_snapshot_realtime_ns = end_ns;
    }
    atomic_flag_clear_explicit(
        &snapshot_lock, memory_order_release);
}

/*
 * Diagnostic entry point used by local correctness tests and one-off device
 * checks. Production collection still uses the configured interval.
 */
__attribute__((visibility("default")))
int mini_hole_snapshot_now(void)
{
    int saved_errno = errno;
    if (!ensure_current_process()) {
        errno = saved_errno;
        return -1;
    }
    write_snapshot();
    int result = log_fd >= 0 ? 0 : -1;
    errno = saved_errno;
    return result;
}

#if defined(MINI_HOLE_OPPORTUNISTIC_SNAPSHOT)
static void maybe_write_periodic_snapshot(void)
{
    uint64_t now = monotonic_ns();
    uint64_t deadline = atomic_load_explicit(
        &next_snapshot_monotonic_ns, memory_order_acquire);
    if (now == 0 || deadline == 0 || now < deadline) {
        return;
    }

    uint64_t interval_ns =
        interval_seconds * UINT64_C(1000000000);
    uint64_t next_deadline = now + interval_ns;
    if (!atomic_compare_exchange_strong_explicit(
            &next_snapshot_monotonic_ns, &deadline, next_deadline,
            memory_order_acq_rel, memory_order_acquire)) {
        return;
    }
    write_snapshot();
}
#endif

#if !defined(MINI_HOLE_NO_WRITER)
static void *writer_main(void *argument)
{
    uint64_t pid = (uint64_t)(uintptr_t)argument;
#if !defined(MINI_HOLE_ATOMIC_STATS)
    record_suppression = 1;
#endif

    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
        return NULL;
    }
    for (;;) {
        deadline.tv_sec += (time_t)interval_seconds;
        int result;
        do {
            result = clock_nanosleep(
                CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
        } while (result == EINTR);

        if (atomic_load_explicit(
                &owner_pid, memory_order_acquire) != pid ||
            raw_getpid() != pid) {
            return NULL;
        }
        write_snapshot();
    }
}
#endif

static void start_writer(uint64_t pid)
{
#if defined(MINI_HOLE_NO_WRITER)
    (void)pid;
#else
    /*
     * appspawndf is a fork server. Creating our private thread from its DSO
     * constructor changes the server's startup threading model and has been
     * observed to terminate the service on OpenHarmony. A specialized child
     * has a different PID, reinitializes after fork, and starts its own
     * writer normally.
     */
    if (initial_process_is_appspawndf &&
        pid == initial_process_pid) {
        return;
    }

    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &writer_started, &expected, true,
            memory_order_acq_rel, memory_order_relaxed)) {
        return;
    }

#if !defined(MINI_HOLE_ATOMIC_STATS)
    ++record_suppression;
#else
    atomic_fetch_add_explicit(
        &statistics_suppression, 1, memory_order_acq_rel);
#endif
    pthread_t thread;
    pthread_attr_t attributes;
    int attr_result = pthread_attr_init(&attributes);
    bool detached = false;
    if (attr_result == 0) {
        detached = pthread_attr_setdetachstate(
            &attributes, PTHREAD_CREATE_DETACHED) == 0;
        (void)pthread_attr_setstacksize(
            &attributes, WRITER_STACK_SIZE);
    }
    int result = pthread_create(
        &thread, attr_result == 0 ? &attributes : NULL,
        writer_main, (void *)(uintptr_t)pid);
    if (attr_result == 0) {
        (void)pthread_attr_destroy(&attributes);
    }
    if (result == 0 && !detached) {
        (void)pthread_detach(thread);
    } else if (result != 0) {
        atomic_store_explicit(
            &writer_started, false, memory_order_release);
    }
#if !defined(MINI_HOLE_ATOMIC_STATS)
    --record_suppression;
#else
    atomic_fetch_sub_explicit(
        &statistics_suppression, 1, memory_order_acq_rel);
#endif
#endif
}

static void initialize_process(uint64_t pid)
{
    atomic_store_explicit(
        &statistics_ready, false, memory_order_release);
    close_log();
    clear_statistics();
    clear_live_statistics();
    zero_bytes(
        (unsigned char *)&previous_snapshot,
        sizeof(previous_snapshot));
    zero_bytes(
        (unsigned char *)&previous_live_snapshot,
        sizeof(previous_live_snapshot));
    reset_current_thread();
    if (initial_process_is_appspawndf &&
        pid == initial_process_pid) {
        release_live_table();
    } else {
        initialize_live_table();
    }

    process_start_realtime_ns = realtime_ns();
    previous_snapshot_realtime_ns = process_start_realtime_ns;
#if defined(MINI_HOLE_OPPORTUNISTIC_SNAPSHOT)
    atomic_store_explicit(
        &next_snapshot_monotonic_ns,
        monotonic_ns(),
        memory_order_release);
#endif
    prepare_output_path(pid);
    atomic_store_explicit(&owner_pid, pid, memory_order_release);
    atomic_store_explicit(
        &writer_started, false, memory_order_release);
    open_log(pid);
    atomic_store_explicit(
        &statistics_ready, true, memory_order_release);
#if !defined(MINI_HOLE_ATOMIC_STATS)
    pid_check_countdown = PID_CHECK_INTERVAL;
#endif
    start_writer(pid);
}

#if !defined(MINI_HOLE_ATOMIC_STATS)
static void atfork_prepare(void)
{
    lock_flag(&process_init_lock);
    lock_flag(&snapshot_lock);
}

static void atfork_parent(void)
{
    atomic_flag_clear_explicit(
        &snapshot_lock, memory_order_release);
    atomic_flag_clear_explicit(
        &process_init_lock, memory_order_release);
}

static void atfork_child(void)
{
    close_log();
    atomic_store_explicit(&owner_pid, 0, memory_order_release);
    atomic_store_explicit(
        &statistics_ready, false, memory_order_release);
    atomic_store_explicit(
        &writer_started, false, memory_order_release);
    atomic_flag_clear_explicit(
        &snapshot_lock, memory_order_release);
    atomic_flag_clear_explicit(
        &process_init_lock, memory_order_release);
    reset_current_thread();
}
#endif

__attribute__((constructor)) static void initialize_hook(void)
{
    /*
     * Keep this first. The working replay hook proves that eager resolution
     * is safe in appspawndf; lazy dlsym from malloc is not.
     */
    resolve_real_allocators();
    initial_process_pid = raw_getpid();
    initial_process_is_appspawndf =
        raw_cmdline_contains("appspawndf");

    const char *configured_dir = getenv("MINI_HOLE_OUTPUT_DIR");
    copy_output_dir(
        configured_dir != NULL ?
            configured_dir : MINI_HOLE_OUTPUT_DIR);

#if !defined(MINI_HOLE_ATOMIC_STATS)
    if (pthread_key_create(
            &thread_flush_key, thread_flush_destructor) == 0) {
        atomic_store_explicit(
            &thread_flush_key_ready, true, memory_order_release);
    }
    (void)pthread_atfork(
        atfork_prepare, atfork_parent, atfork_child);
#endif
    atomic_store_explicit(
        &initialization_enabled, true, memory_order_release);
    (void)ensure_current_process();
}

__attribute__((destructor)) static void finalize_hook(void)
{
    uint64_t pid = raw_getpid();
    if (atomic_load_explicit(&owner_pid, memory_order_acquire) != pid) {
        return;
    }
#if !defined(MINI_HOLE_ATOMIC_STATS)
    ++record_suppression;
    flush_thread_pending();
#endif
    write_snapshot();
    close_log();
#if !defined(MINI_HOLE_ATOMIC_STATS)
    --record_suppression;
#endif
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
    void *ptr =
        function != NULL ? function(size) : bootstrap_malloc(size);
    int saved_errno = errno;
    if (ptr != NULL && !is_bootstrap_pointer(ptr)) {
        record_successful_allocation(size, ptr);
    } else if (ptr == NULL) {
        record_failed_allocation();
    }
    errno = saved_errno;
    return ptr;
}

__attribute__((visibility("default"))) void free(void *ptr)
{
    int saved_errno = errno;
    if (ptr == NULL || is_bootstrap_pointer(ptr)) {
        errno = saved_errno;
        return;
    }
    record_free_allocation(ptr);
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
    errno = saved_errno;
}

__attribute__((visibility("default")))
void *calloc(size_t count, size_t size)
{
    bool overflow = size != 0 && count > SIZE_MAX / size;
    calloc_fn function =
        atomic_load_explicit(&real_calloc, memory_order_acquire);
    if (function == NULL) {
        resolve_real_allocators();
        function =
            atomic_load_explicit(&real_calloc, memory_order_acquire);
    }
    void *ptr = function != NULL ?
        function(count, size) : bootstrap_calloc(count, size);
    int saved_errno = errno;
    if (ptr != NULL && !is_bootstrap_pointer(ptr) && !overflow) {
        record_successful_allocation(count * size, ptr);
    } else if (ptr == NULL) {
        record_failed_allocation();
    }
    errno = saved_errno;
    return ptr;
}

__attribute__((visibility("default")))
void *realloc(void *ptr, size_t size)
{
    bool old_is_trackable =
        ptr != NULL && !is_bootstrap_pointer(ptr);
    DetachedAllocation detached = {
        .observed = false,
        .tracked = false,
        .requested = 0,
    };
    if (old_is_trackable) {
        detached = detach_live_allocation(ptr);
    }

    void *new_ptr;
    if (is_bootstrap_pointer(ptr)) {
        new_ptr = bootstrap_realloc(ptr, size);
    } else {
        realloc_fn function =
            atomic_load_explicit(&real_realloc, memory_order_acquire);
        if (function == NULL) {
            resolve_real_allocators();
            function =
                atomic_load_explicit(&real_realloc, memory_order_acquire);
        }
        new_ptr = function != NULL ? function(ptr, size) : NULL;
    }
    int saved_errno = errno;
    if (new_ptr != NULL && !is_bootstrap_pointer(new_ptr)) {
        if (old_is_trackable) {
            record_release_event(ptr, &detached);
        }
        record_successful_allocation(size, new_ptr);
    } else if (new_ptr == NULL && size != 0) {
        if (old_is_trackable) {
            restore_detached_allocation(ptr, &detached);
        }
        record_failed_allocation();
    } else if (old_is_trackable) {
        /*
         * OpenHarmony's allocator releases ptr for realloc(ptr, 0) when
         * returning NULL. This is counted as a release, not a failed
         * allocation.
         */
        record_release_event(ptr, &detached);
#if defined(MINI_HOLE_OPPORTUNISTIC_SNAPSHOT)
        if (detached.observed) {
            maybe_write_periodic_snapshot();
        }
#endif
    }
    errno = saved_errno;
    return new_ptr;
}

__attribute__((visibility("default")))
void free_sized(void *ptr, size_t size)
{
    int saved_errno = errno;
    if (ptr == NULL || is_bootstrap_pointer(ptr)) {
        errno = saved_errno;
        return;
    }
    record_free_allocation(ptr);
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
    errno = saved_errno;
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
    void *ptr = NULL;
    int result = function != NULL ?
        function(&ptr, alignment, size) : ENOMEM;
    if (result == 0) {
        *memptr = ptr;
        if (ptr != NULL) {
            record_successful_allocation(size, ptr);
        }
    } else {
        record_failed_allocation();
    }
    return result;
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
    void *ptr = function != NULL ? function(alignment, size) : NULL;
    int saved_errno = errno;
    if (ptr != NULL) {
        record_successful_allocation(size, ptr);
    } else {
        record_failed_allocation();
    }
    errno = saved_errno;
    return ptr;
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
    void *ptr = function != NULL ? function(size) : NULL;
    int saved_errno = errno;
    if (ptr != NULL) {
        record_successful_allocation(size, ptr);
    } else {
        record_failed_allocation();
    }
    errno = saved_errno;
    return ptr;
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
    void *ptr = function != NULL ? function(alignment, size) : NULL;
    int saved_errno = errno;
    if (ptr != NULL) {
        record_successful_allocation(size, ptr);
    } else {
        record_failed_allocation();
    }
    errno = saved_errno;
    return ptr;
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
    void *ptr = function != NULL ? function(size) : NULL;
    int saved_errno = errno;
    if (ptr != NULL) {
        record_successful_allocation(size, ptr);
    } else {
        record_failed_allocation();
    }
    errno = saved_errno;
    return ptr;
}

#endif
