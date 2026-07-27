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
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/*
 * This hook measures allocator-internal fragmentation:
 *
 *     hole_bytes = rounded_size(requested_bytes) - requested_bytes
 *
 * It records aggregate counters only. No pointer, stack, or per-allocation
 * event is retained. The allocation hot path never reads the clock, takes a
 * lock, writes a file, or allocates memory.
 */

#define BOOTSTRAP_HEAP_CAPACITY (1024U * 1024U)
#define SMALL_SIZE_CLASS_COUNT 28U
#define SIZE_BUCKET_COUNT (SMALL_SIZE_CLASS_COUNT + 1U)
#define LARGE_ALLOCATION_ALIGNMENT 4096U
#define HOLE_EXCLUSIVE_SHARDS 512U
#define HOLE_OVERFLOW_SHARDS 64U
#define LOCAL_FLUSH_THRESHOLD 64U
#define PID_CHECK_INTERVAL 1024U
#define DEFAULT_INTERVAL_SECONDS 3600U
#define MAX_INTERVAL_SECONDS (7U * 24U * 60U * 60U)
#define WRITER_STACK_SIZE (128U * 1024U)
#define OUTPUT_DIR_CAPACITY 256U
#define OUTPUT_PATH_CAPACITY 384U
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
    _Atomic uint64_t allocations;
    _Atomic uint64_t measured_allocations;
    _Atomic uint64_t requested_bytes;
    _Atomic uint64_t hole_bytes;
    _Atomic uint64_t failed_allocations;
    _Atomic uint64_t measurement_errors;
    _Atomic uint64_t bucket_allocations[SIZE_BUCKET_COUNT];
    _Atomic uint64_t bucket_hole_bytes[SIZE_BUCKET_COUNT];
} HoleShard;

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

static const size_t small_size_classes[SMALL_SIZE_CLASS_COUNT] = {
    8, 16, 24, 32, 40, 48, 56, 64,
    80, 96, 112, 128, 144, 176, 208, 256,
    304, 352, 384, 432, 496, 560, 656, 784,
    992, 1312, 1984, 3968,
};

static _Alignas(64) HoleShard exclusive_shards[HOLE_EXCLUSIVE_SHARDS];
static _Alignas(64) HoleShard overflow_shards[HOLE_OVERFLOW_SHARDS];
static _Atomic uint32_t next_exclusive_shard;
static _Atomic uint32_t next_overflow_shard;

static __thread HoleShard *thread_shard;
static __thread bool thread_shard_is_exclusive;
static __thread uint32_t pid_check_countdown;
static __thread unsigned int hook_depth;
static __thread HoleSnapshot thread_pending;
static __thread uint32_t thread_pending_events;
static __thread bool thread_key_registered;

static pthread_key_t thread_flush_key;
static atomic_bool thread_flush_key_ready;

static _Atomic uint64_t owner_pid;
static atomic_bool writer_started;
static atomic_flag snapshot_lock = ATOMIC_FLAG_INIT;
static int log_fd = -1;
static bool log_header_written;
static char output_dir[OUTPUT_DIR_CAPACITY];
static char output_path[OUTPUT_PATH_CAPACITY];
static uint64_t interval_seconds = DEFAULT_INTERVAL_SECONDS;
static uint64_t process_start_realtime_ns;
static uint64_t previous_snapshot_realtime_ns;
static HoleSnapshot previous_snapshot;

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
    const uintptr_t address = (uintptr_t)ptr;
    const uintptr_t begin = (uintptr_t)&bootstrap_heap.bytes[0];
    const uintptr_t end =
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
        copy_bytes(
            new_ptr, ptr, old_size < size ? old_size : size);
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
    atomic_store_explicit(&resolver_complete, true, memory_order_release);
    atomic_flag_clear_explicit(&resolver_lock, memory_order_release);
}

static malloc_fn get_real_malloc(void)
{
    malloc_fn function =
        atomic_load_explicit(&real_malloc, memory_order_acquire);
    if (function == NULL) {
        resolve_real_allocators();
        function = atomic_load_explicit(
            &real_malloc, memory_order_acquire);
    }
    return function;
}

static free_fn get_real_free(void)
{
    free_fn function =
        atomic_load_explicit(&real_free, memory_order_acquire);
    if (function == NULL) {
        resolve_real_allocators();
        function = atomic_load_explicit(&real_free, memory_order_acquire);
    }
    return function;
}

static pid_t raw_getpid(void)
{
    return (pid_t)syscall(SYS_getpid);
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

static void copy_output_dir(const char *source)
{
    size_t i = 0;
    while (source[i] != '\0' && i + 1 < sizeof(output_dir)) {
        output_dir[i] = source[i];
        ++i;
    }
    output_dir[i] = '\0';
}

static size_t append_text(char *buffer, size_t offset, const char *text)
{
    while (*text != '\0' && offset < OUTPUT_BUFFER_CAPACITY) {
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
    while (count != 0 && offset < OUTPUT_BUFFER_CAPACITY) {
        buffer[offset++] = digits[--count];
    }
    return offset;
}

static size_t append_path_text(
    char *buffer, size_t capacity, size_t offset, const char *text)
{
    while (*text != '\0' && offset + 1 < capacity) {
        buffer[offset++] = *text++;
    }
    return offset;
}

static size_t append_path_u64(
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

static void prepare_output_path(pid_t pid)
{
    size_t offset = append_path_text(
        output_path, sizeof(output_path), 0, output_dir);
    if (offset != 0 && output_path[offset - 1] != '/' &&
        offset + 1 < sizeof(output_path)) {
        output_path[offset++] = '/';
    }
    offset = append_path_text(
        output_path, sizeof(output_path), offset, "mini_hole_");
    offset = append_path_u64(
        output_path, sizeof(output_path), offset,
        process_start_realtime_ns);
    if (offset + 1 < sizeof(output_path)) {
        output_path[offset++] = '_';
    }
    offset = append_path_u64(
        output_path, sizeof(output_path), offset, (uint64_t)pid);
    offset = append_path_text(
        output_path, sizeof(output_path), offset, ".csv");
    output_path[offset] = '\0';
}

static bool write_all(int fd, const char *buffer, size_t size)
{
    while (size != 0) {
        ssize_t written = write(fd, buffer, size);
        if (written > 0) {
            buffer += written;
            size -= (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

static bool write_log_header(int fd, pid_t pid)
{
    char buffer[OUTPUT_BUFFER_CAPACITY];
    size_t offset = 0;
    offset = append_text(buffer, offset, "#mini_malloc_hole_v2");
    offset = append_text(buffer, offset, ",unit=byte,pid=");
    offset = append_u64(buffer, offset, (uint64_t)pid);
    offset = append_text(buffer, offset, ",interval_seconds=");
    offset = append_u64(buffer, offset, interval_seconds);
    offset = append_text(
        buffer, offset,
        ",size_classes=8|16|24|32|40|48|56|64|80|96|112|128|"
        "144|176|208|256|304|352|384|432|496|560|656|784|"
        "992|1312|1984|3968|4K+\n");
    offset = append_text(
        buffer, offset,
        "start_ns,end_ns,"
        "period_alloc,total_alloc,"
        "period_measured,total_measured,"
        "period_requested,total_requested,"
        "period_hole,total_hole,"
        "period_failed,total_failed,"
        "period_measure_error,total_measure_error");
    for (uint32_t i = 0; i < SMALL_SIZE_CLASS_COUNT; ++i) {
        offset = append_text(buffer, offset, ",count_");
        offset = append_u64(buffer, offset, small_size_classes[i]);
        offset = append_text(buffer, offset, ",hole_");
        offset = append_u64(buffer, offset, small_size_classes[i]);
    }
    offset = append_text(buffer, offset, ",count_4K_plus,hole_4K_plus");
    if (offset < sizeof(buffer)) {
        buffer[offset++] = '\n';
    }
    return write_all(fd, buffer, offset);
}

static int ensure_log_open(void)
{
    if (log_fd >= 0) {
        return log_fd;
    }

    bool new_file = !log_header_written;
    int flags = O_WRONLY | O_APPEND | O_CLOEXEC;
    if (new_file) {
        flags |= O_CREAT | O_EXCL;
    }
    int fd = open(output_path, flags, 0644);
    if (fd < 0 && new_file && errno == EEXIST) {
        fd = open(output_path, O_WRONLY | O_APPEND | O_CLOEXEC);
    }
    if (fd < 0) {
        return -1;
    }

    log_fd = fd;
    if (!log_header_written) {
        if (!write_log_header(
                fd, (pid_t)atomic_load_explicit(
                    &owner_pid, memory_order_acquire))) {
            close(fd);
            log_fd = -1;
            return -1;
        }
        log_header_written = true;
    }
    return fd;
}

static void clear_shard(HoleShard *shard)
{
    atomic_store_explicit(&shard->allocations, 0, memory_order_relaxed);
    atomic_store_explicit(
        &shard->measured_allocations, 0, memory_order_relaxed);
    atomic_store_explicit(
        &shard->requested_bytes, 0, memory_order_relaxed);
    atomic_store_explicit(&shard->hole_bytes, 0, memory_order_relaxed);
    atomic_store_explicit(
        &shard->failed_allocations, 0, memory_order_relaxed);
    atomic_store_explicit(
        &shard->measurement_errors, 0, memory_order_relaxed);
    for (uint32_t i = 0; i < SIZE_BUCKET_COUNT; ++i) {
        atomic_store_explicit(
            &shard->bucket_allocations[i], 0, memory_order_relaxed);
        atomic_store_explicit(
            &shard->bucket_hole_bytes[i], 0, memory_order_relaxed);
    }
}

static void clear_all_statistics(void)
{
    for (uint32_t i = 0; i < HOLE_EXCLUSIVE_SHARDS; ++i) {
        clear_shard(&exclusive_shards[i]);
    }
    for (uint32_t i = 0; i < HOLE_OVERFLOW_SHARDS; ++i) {
        clear_shard(&overflow_shards[i]);
    }
    atomic_store_explicit(
        &next_exclusive_shard, 0, memory_order_relaxed);
    atomic_store_explicit(
        &next_overflow_shard, 0, memory_order_relaxed);
    zero_bytes(
        (unsigned char *)&previous_snapshot, sizeof(previous_snapshot));
}

static void add_shard_to_snapshot(
    const HoleShard *shard, HoleSnapshot *snapshot)
{
    snapshot->allocations += atomic_load_explicit(
        &shard->allocations, memory_order_relaxed);
    snapshot->measured_allocations += atomic_load_explicit(
        &shard->measured_allocations, memory_order_relaxed);
    snapshot->requested_bytes += atomic_load_explicit(
        &shard->requested_bytes, memory_order_relaxed);
    snapshot->hole_bytes += atomic_load_explicit(
        &shard->hole_bytes, memory_order_relaxed);
    snapshot->failed_allocations += atomic_load_explicit(
        &shard->failed_allocations, memory_order_relaxed);
    snapshot->measurement_errors += atomic_load_explicit(
        &shard->measurement_errors, memory_order_relaxed);
    for (uint32_t i = 0; i < SIZE_BUCKET_COUNT; ++i) {
        snapshot->bucket_allocations[i] += atomic_load_explicit(
            &shard->bucket_allocations[i], memory_order_relaxed);
        snapshot->bucket_hole_bytes[i] += atomic_load_explicit(
            &shard->bucket_hole_bytes[i], memory_order_relaxed);
    }
}

static HoleSnapshot collect_snapshot(void)
{
    HoleSnapshot snapshot = {0};
    for (uint32_t i = 0; i < HOLE_EXCLUSIVE_SHARDS; ++i) {
        add_shard_to_snapshot(&exclusive_shards[i], &snapshot);
    }
    for (uint32_t i = 0; i < HOLE_OVERFLOW_SHARDS; ++i) {
        add_shard_to_snapshot(&overflow_shards[i], &snapshot);
    }
    return snapshot;
}

static size_t append_csv_pair(
    char *buffer, size_t offset, uint64_t period, uint64_t total)
{
    if (offset < OUTPUT_BUFFER_CAPACITY) {
        buffer[offset++] = ',';
    }
    offset = append_u64(buffer, offset, period);
    if (offset < OUTPUT_BUFFER_CAPACITY) {
        buffer[offset++] = ',';
    }
    return append_u64(buffer, offset, total);
}

static void write_snapshot(void)
{
    if (atomic_flag_test_and_set_explicit(
            &snapshot_lock, memory_order_acquire)) {
        return;
    }

    uint64_t now_ns = realtime_ns();
    HoleSnapshot current = collect_snapshot();
    int fd = ensure_log_open();
    if (fd >= 0) {
        char buffer[OUTPUT_BUFFER_CAPACITY];
        size_t offset = 0;
        offset = append_u64(
            buffer, offset, previous_snapshot_realtime_ns);
        if (offset < sizeof(buffer)) {
            buffer[offset++] = ',';
        }
        offset = append_u64(buffer, offset, now_ns);
        offset = append_csv_pair(
            buffer, offset,
            current.allocations - previous_snapshot.allocations,
            current.allocations);
        offset = append_csv_pair(
            buffer, offset,
            current.measured_allocations -
                previous_snapshot.measured_allocations,
            current.measured_allocations);
        offset = append_csv_pair(
            buffer, offset,
            current.requested_bytes - previous_snapshot.requested_bytes,
            current.requested_bytes);
        offset = append_csv_pair(
            buffer, offset,
            current.hole_bytes - previous_snapshot.hole_bytes,
            current.hole_bytes);
        offset = append_csv_pair(
            buffer, offset,
            current.failed_allocations -
                previous_snapshot.failed_allocations,
            current.failed_allocations);
        offset = append_csv_pair(
            buffer, offset,
            current.measurement_errors -
                previous_snapshot.measurement_errors,
            current.measurement_errors);
        for (uint32_t i = 0; i < SIZE_BUCKET_COUNT; ++i) {
            if (offset < sizeof(buffer)) {
                buffer[offset++] = ',';
            }
            offset = append_u64(
                buffer, offset,
                current.bucket_allocations[i] -
                    previous_snapshot.bucket_allocations[i]);
            if (offset < sizeof(buffer)) {
                buffer[offset++] = ',';
            }
            offset = append_u64(
                buffer, offset,
                current.bucket_hole_bytes[i] -
                    previous_snapshot.bucket_hole_bytes[i]);
        }
        if (offset < sizeof(buffer)) {
            buffer[offset++] = '\n';
        }
        if (write_all(fd, buffer, offset)) {
            previous_snapshot = current;
            previous_snapshot_realtime_ns = now_ns;
        } else {
            close(fd);
            log_fd = -1;
        }
    }

    atomic_flag_clear_explicit(&snapshot_lock, memory_order_release);
}

static void *writer_main(void *argument)
{
    const uint64_t pid = (uint64_t)(uintptr_t)argument;
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

        if (atomic_load_explicit(&owner_pid, memory_order_acquire) != pid ||
            (uint64_t)raw_getpid() != pid) {
            return NULL;
        }
        write_snapshot();
    }
}

static void start_writer(pid_t pid)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &writer_started, &expected, true,
            memory_order_acq_rel, memory_order_relaxed)) {
        return;
    }

    pthread_t thread;
    pthread_attr_t attributes;
    int attributes_result = pthread_attr_init(&attributes);
    bool create_detached = false;
    if (attributes_result == 0) {
        create_detached = pthread_attr_setdetachstate(
            &attributes, PTHREAD_CREATE_DETACHED) == 0;
        (void)pthread_attr_setstacksize(
            &attributes, WRITER_STACK_SIZE);
    }
    int result = pthread_create(
        &thread,
        attributes_result == 0 ? &attributes : NULL,
        writer_main, (void *)(uintptr_t)pid);
    if (attributes_result == 0) {
        (void)pthread_attr_destroy(&attributes);
    }
    if (result == 0 && !create_detached) {
        (void)pthread_detach(thread);
    } else if (result != 0) {
        atomic_store_explicit(
            &writer_started, false, memory_order_release);
    }
}

static void initialize_process(pid_t pid)
{
    if (log_fd >= 0) {
        close(log_fd);
        log_fd = -1;
    }
    log_header_written = false;
    process_start_realtime_ns = realtime_ns();
    previous_snapshot_realtime_ns = process_start_realtime_ns;
    clear_all_statistics();

    thread_shard = NULL;
    thread_shard_is_exclusive = false;
    zero_bytes(
        (unsigned char *)&thread_pending, sizeof(thread_pending));
    thread_pending_events = 0;
    thread_key_registered = false;
    pid_check_countdown = PID_CHECK_INTERVAL;
    prepare_output_path(pid);
    atomic_store_explicit(&writer_started, false, memory_order_release);
    (void)ensure_log_open();
    start_writer(pid);
}

static void ensure_current_process(void)
{
    pid_t pid = raw_getpid();
    uint64_t current_pid = (uint64_t)pid;
    uint64_t recorded_pid =
        atomic_load_explicit(&owner_pid, memory_order_acquire);
    if (recorded_pid == current_pid) {
        return;
    }

    if (atomic_compare_exchange_strong_explicit(
            &owner_pid, &recorded_pid, current_pid,
            memory_order_acq_rel, memory_order_acquire)) {
        initialize_process(pid);
    }
}

static void maybe_check_process(void)
{
    if (pid_check_countdown > 1) {
        --pid_check_countdown;
        return;
    }
    pid_check_countdown = PID_CHECK_INTERVAL;
    ensure_current_process();
}

static void atfork_child(void)
{
    atomic_store_explicit(&owner_pid, 0, memory_order_release);
    atomic_store_explicit(&writer_started, false, memory_order_release);
    thread_shard = NULL;
    thread_shard_is_exclusive = false;
    zero_bytes(
        (unsigned char *)&thread_pending, sizeof(thread_pending));
    thread_pending_events = 0;
    thread_key_registered = false;
    pid_check_countdown = 0;
    atomic_flag_clear_explicit(&snapshot_lock, memory_order_release);
}

static HoleShard *get_thread_shard(void)
{
    if (thread_shard != NULL) {
        return thread_shard;
    }

    uint32_t index = atomic_fetch_add_explicit(
        &next_exclusive_shard, 1, memory_order_relaxed);
    if (index < HOLE_EXCLUSIVE_SHARDS) {
        thread_shard = &exclusive_shards[index];
        thread_shard_is_exclusive = true;
    } else {
        index = atomic_fetch_add_explicit(
            &next_overflow_shard, 1, memory_order_relaxed);
        thread_shard =
            &overflow_shards[index % HOLE_OVERFLOW_SHARDS];
        thread_shard_is_exclusive = false;
    }
    if (!thread_key_registered &&
        atomic_load_explicit(
            &thread_flush_key_ready, memory_order_acquire) &&
        pthread_setspecific(thread_flush_key, (void *)(uintptr_t)1) == 0) {
        thread_key_registered = true;
    }
    return thread_shard;
}

static void counter_add(
    _Atomic uint64_t *counter, uint64_t value, bool exclusive)
{
    if (exclusive) {
        uint64_t current =
            atomic_load_explicit(counter, memory_order_relaxed);
        atomic_store_explicit(
            counter, current + value, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(counter, value, memory_order_relaxed);
    }
}

static void flush_thread_pending(void)
{
    if (thread_pending_events == 0) {
        return;
    }

    HoleShard *shard = get_thread_shard();
    bool exclusive = thread_shard_is_exclusive;
    counter_add(
        &shard->allocations, thread_pending.allocations, exclusive);
    counter_add(
        &shard->measured_allocations,
        thread_pending.measured_allocations, exclusive);
    counter_add(
        &shard->requested_bytes,
        thread_pending.requested_bytes, exclusive);
    counter_add(
        &shard->hole_bytes, thread_pending.hole_bytes, exclusive);
    counter_add(
        &shard->failed_allocations,
        thread_pending.failed_allocations, exclusive);
    counter_add(
        &shard->measurement_errors,
        thread_pending.measurement_errors, exclusive);
    for (uint32_t i = 0; i < SIZE_BUCKET_COUNT; ++i) {
        if (thread_pending.bucket_allocations[i] != 0) {
            counter_add(
                &shard->bucket_allocations[i],
                thread_pending.bucket_allocations[i], exclusive);
        }
        if (thread_pending.bucket_hole_bytes[i] != 0) {
            counter_add(
                &shard->bucket_hole_bytes[i],
                thread_pending.bucket_hole_bytes[i], exclusive);
        }
    }
    zero_bytes(
        (unsigned char *)&thread_pending, sizeof(thread_pending));
    thread_pending_events = 0;
}

static void flush_thread_destructor(void *value)
{
    (void)value;
    flush_thread_pending();
    thread_key_registered = false;
}

static void finish_pending_event(void)
{
    ++thread_pending_events;
    if (thread_pending_events >= LOCAL_FLUSH_THRESHOLD) {
        flush_thread_pending();
    }
}

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

static void record_failed_allocation(void)
{
    maybe_check_process();
    ++thread_pending.failed_allocations;
    finish_pending_event();
}

static void record_successful_allocation(size_t requested, void *ptr)
{
    maybe_check_process();
    ++thread_pending.allocations;

    (void)ptr;
    size_t actual;
    uint32_t bucket;
    if (!rounded_allocation_size(requested, &actual, &bucket)) {
        ++thread_pending.measurement_errors;
        finish_pending_event();
        return;
    }

    uint64_t hole = (uint64_t)(actual - requested);
    ++thread_pending.measured_allocations;
    thread_pending.requested_bytes += (uint64_t)requested;
    thread_pending.hole_bytes += hole;
    ++thread_pending.bucket_allocations[bucket];
    thread_pending.bucket_hole_bytes[bucket] += hole;
    finish_pending_event();
}

static uint64_t parse_interval_seconds(const char *text)
{
    if (text == NULL || *text == '\0') {
        return DEFAULT_INTERVAL_SECONDS;
    }

    uint64_t value = 0;
    while (*text >= '0' && *text <= '9') {
        uint64_t digit = (uint64_t)(*text - '0');
        if (value > (UINT64_MAX - digit) / 10) {
            return DEFAULT_INTERVAL_SECONDS;
        }
        value = value * 10 + digit;
        ++text;
    }
    if (*text != '\0' || value == 0 || value > MAX_INTERVAL_SECONDS) {
        return DEFAULT_INTERVAL_SECONDS;
    }
    return value;
}

__attribute__((constructor)) static void initialize_hook(void)
{
    ++hook_depth;
    resolve_real_allocators();

    const char *configured_dir = getenv("MINI_HOLE_OUTPUT_DIR");
    copy_output_dir(
        configured_dir != NULL ? configured_dir : MINI_HOLE_OUTPUT_DIR);
    interval_seconds = parse_interval_seconds(
        getenv("MINI_HOLE_INTERVAL_SEC"));
    if (pthread_key_create(
            &thread_flush_key, flush_thread_destructor) == 0) {
        atomic_store_explicit(
            &thread_flush_key_ready, true, memory_order_release);
    }
    (void)pthread_atfork(NULL, NULL, atfork_child);
    ensure_current_process();
    --hook_depth;
}

__attribute__((destructor)) static void finalize_hook(void)
{
    ++hook_depth;
    if (atomic_load_explicit(&owner_pid, memory_order_acquire) ==
        (uint64_t)raw_getpid()) {
        flush_thread_pending();
        write_snapshot();
    }
    if (log_fd >= 0) {
        close(log_fd);
        log_fd = -1;
    }
    --hook_depth;
}

__attribute__((visibility("default"))) void *malloc(size_t size)
{
    malloc_fn function = get_real_malloc();
    if (hook_depth != 0) {
        return function != NULL ? function(size) : bootstrap_malloc(size);
    }

    ++hook_depth;
    void *ptr =
        function != NULL ? function(size) : bootstrap_malloc(size);
    int saved_errno = errno;
    if (ptr != NULL && !is_bootstrap_pointer(ptr)) {
        record_successful_allocation(size, ptr);
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

    free_fn function = get_real_free();
    if (function != NULL) {
        function(ptr);
    }
    errno = saved_errno;
}

__attribute__((visibility("default")))
void *calloc(size_t count, size_t size)
{
    if (hook_depth != 0) {
        calloc_fn nested = atomic_load_explicit(
            &real_calloc, memory_order_acquire);
        return nested != NULL
            ? nested(count, size)
            : bootstrap_calloc(count, size);
    }

    ++hook_depth;
    resolve_real_allocators();
    calloc_fn function = atomic_load_explicit(
        &real_calloc, memory_order_acquire);
    bool overflow = size != 0 && count > SIZE_MAX / size;
    void *ptr;
    if (function != NULL) {
        ptr = function(count, size);
    } else {
        ptr = bootstrap_calloc(count, size);
    }
    int saved_errno = errno;
    if (ptr != NULL && !is_bootstrap_pointer(ptr) && !overflow) {
        record_successful_allocation(count * size, ptr);
    } else if (ptr == NULL) {
        record_failed_allocation();
    }
    errno = saved_errno;
    --hook_depth;
    return ptr;
}

__attribute__((visibility("default")))
void *realloc(void *ptr, size_t size)
{
    if (hook_depth != 0) {
        if (is_bootstrap_pointer(ptr)) {
            return bootstrap_realloc(ptr, size);
        }
        realloc_fn nested = atomic_load_explicit(
            &real_realloc, memory_order_acquire);
        return nested != NULL ? nested(ptr, size) : NULL;
    }

    ++hook_depth;
    resolve_real_allocators();
    void *new_ptr;
    if (is_bootstrap_pointer(ptr)) {
        new_ptr = bootstrap_realloc(ptr, size);
    } else {
        realloc_fn function = atomic_load_explicit(
            &real_realloc, memory_order_acquire);
        new_ptr = function != NULL ? function(ptr, size) : NULL;
    }
    int saved_errno = errno;
    if (new_ptr != NULL && !is_bootstrap_pointer(new_ptr)) {
        record_successful_allocation(size, new_ptr);
    } else if (new_ptr == NULL && size != 0) {
        record_failed_allocation();
    }
    errno = saved_errno;
    --hook_depth;
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

    resolve_real_allocators();
    free_sized_fn function = atomic_load_explicit(
        &real_free_sized, memory_order_acquire);
    free_fn fallback = atomic_load_explicit(
        &real_free, memory_order_acquire);
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
    if (hook_depth != 0) {
        posix_memalign_fn nested = atomic_load_explicit(
            &real_posix_memalign, memory_order_acquire);
        return nested != NULL
            ? nested(memptr, alignment, size)
            : ENOMEM;
    }

    ++hook_depth;
    resolve_real_allocators();
    posix_memalign_fn function = atomic_load_explicit(
        &real_posix_memalign, memory_order_acquire);
    void *ptr = NULL;
    int result = function != NULL
        ? function(&ptr, alignment, size)
        : ENOMEM;
    int saved_errno = errno;
    if (result == 0) {
        *memptr = ptr;
        record_successful_allocation(size, ptr);
    } else {
        record_failed_allocation();
    }
    errno = saved_errno;
    --hook_depth;
    return result;
}

__attribute__((visibility("default")))
void *aligned_alloc(size_t alignment, size_t size)
{
    if (hook_depth != 0) {
        aligned_alloc_fn nested = atomic_load_explicit(
            &real_aligned_alloc, memory_order_acquire);
        return nested != NULL ? nested(alignment, size) : NULL;
    }

    ++hook_depth;
    resolve_real_allocators();
    aligned_alloc_fn function = atomic_load_explicit(
        &real_aligned_alloc, memory_order_acquire);
    void *ptr =
        function != NULL ? function(alignment, size) : NULL;
    int saved_errno = errno;
    if (ptr != NULL) {
        record_successful_allocation(size, ptr);
    } else {
        record_failed_allocation();
    }
    errno = saved_errno;
    --hook_depth;
    return ptr;
}

__attribute__((visibility("default"))) void *valloc(size_t size)
{
    if (hook_depth != 0) {
        valloc_fn nested = atomic_load_explicit(
            &real_valloc, memory_order_acquire);
        return nested != NULL ? nested(size) : NULL;
    }

    ++hook_depth;
    resolve_real_allocators();
    valloc_fn function = atomic_load_explicit(
        &real_valloc, memory_order_acquire);
    void *ptr = function != NULL ? function(size) : NULL;
    int saved_errno = errno;
    if (ptr != NULL) {
        record_successful_allocation(size, ptr);
    } else {
        record_failed_allocation();
    }
    errno = saved_errno;
    --hook_depth;
    return ptr;
}

__attribute__((visibility("default")))
void *memalign(size_t alignment, size_t size)
{
    if (hook_depth != 0) {
        memalign_fn nested = atomic_load_explicit(
            &real_memalign, memory_order_acquire);
        return nested != NULL ? nested(alignment, size) : NULL;
    }

    ++hook_depth;
    resolve_real_allocators();
    memalign_fn function = atomic_load_explicit(
        &real_memalign, memory_order_acquire);
    void *ptr =
        function != NULL ? function(alignment, size) : NULL;
    int saved_errno = errno;
    if (ptr != NULL) {
        record_successful_allocation(size, ptr);
    } else {
        record_failed_allocation();
    }
    errno = saved_errno;
    --hook_depth;
    return ptr;
}

__attribute__((visibility("default"))) void *pvalloc(size_t size)
{
    if (hook_depth != 0) {
        pvalloc_fn nested = atomic_load_explicit(
            &real_pvalloc, memory_order_acquire);
        return nested != NULL ? nested(size) : NULL;
    }

    ++hook_depth;
    resolve_real_allocators();
    pvalloc_fn function = atomic_load_explicit(
        &real_pvalloc, memory_order_acquire);
    void *ptr = function != NULL ? function(size) : NULL;
    int saved_errno = errno;
    if (ptr != NULL) {
        record_successful_allocation(size, ptr);
    } else {
        record_failed_allocation();
    }
    errno = saved_errno;
    --hook_depth;
    return ptr;
}
