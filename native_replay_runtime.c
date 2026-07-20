/* Minimal runtime used by trace_to_native.py.
 *
 * The generated executable has a compact replay image and a trailer appended
 * to this ELF.  All expensive trace parsing and pointer matching happens in
 * the generator.  The timed path contains only timestamp waits, allocator
 * calls, and the atomics required to hand objects between trace threads.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <malloc.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define NATIVE_IMAGE_MAGIC "MNRUN001"
#define NATIVE_TRAILER_MAGIC "MNEND001"
#define NATIVE_VERSION 1u
#define NATIVE_SLOT_NONE UINT32_MAX
#define NATIVE_FLAG_HAS_UNKNOWN 1u
#define TRACE_RESULT_SUCCEEDED 1u
#define LATE_THRESHOLD_NS 10000u

enum FunctionId {
    FUNC_MALLOC = 0,
    FUNC_FREE = 1,
    FUNC_CALLOC = 2,
    FUNC_REALLOC = 3,
    FUNC_FREE_SIZED = 4,
    FUNC_POSIX_MEMALIGN = 5,
    FUNC_ALIGNED_ALLOC = 6,
    FUNC_VALLOC = 7,
    FUNC_MEMALIGN = 8,
    FUNC_PVALLOC = 9,
    FUNC_COUNT = 10,
};

enum OpKind {
    OP_ALLOC = 0,
    OP_FREE = 1,
    OP_REALLOC = 2,
    OP_REALLOC_NULL = 3,
    OP_UNKNOWN_FREE = 4,
};

typedef struct {
    uint8_t magic[8];
    uint32_t version;
    uint32_t header_size;
    uint32_t thread_count;
    uint32_t flags;
    uint32_t slot_count;
    uint32_t op_count;
    uint64_t min_timestamp_ns;
    uint64_t max_timestamp_ns;
    uint64_t thread_offset;
    uint64_t op_offset;
} NativeImageHeader;

typedef struct {
    uint64_t trace_tid;
    uint64_t first_op;
    uint64_t op_count;
    uint32_t recorded_cpu;
    uint32_t reserved;
} NativeThread;

typedef struct {
    uint64_t target_ns;
    uint64_t size;
    uint32_t slot;
    uint32_t wait_generation;
    uint32_t output_generation;
    uint32_t aux;
    uint16_t function;
    uint8_t kind;
    uint8_t flags;
    uint32_t reserved;
} NativeOp;

typedef struct {
    uint8_t magic[8];
    uint64_t image_size;
    uint64_t version;
} NativeTrailer;

_Static_assert(sizeof(NativeImageHeader) == 64, "native header layout");
_Static_assert(sizeof(NativeThread) == 32, "native thread layout");
_Static_assert(sizeof(NativeOp) == 40, "native op layout");
_Static_assert(sizeof(NativeTrailer) == 24, "native trailer layout");

typedef struct {
    _Atomic uintptr_t pointer;
    _Atomic uint32_t generation;
} ReplaySlot;

typedef struct {
    pthread_t thread;
    const NativeThread *description;
    uint64_t function_counts[FUNC_COUNT];
    uint64_t timing_buckets;
    uint64_t late_events;
    uint64_t total_lateness_ns;
    uint64_t maximum_lateness_ns;
    uint64_t dependency_yields;
    uint64_t runtime_allocation_failures;
    uint64_t finish_ns;
} Worker;

static const NativeImageHeader *g_header;
static const NativeThread *g_threads;
static const NativeOp *g_ops;
static ReplaySlot *g_slots;
static Worker *g_workers;
static _Atomic uint32_t g_ready;
static _Atomic bool g_start;
static _Atomic uint64_t g_last_finish_ns;
static uint64_t g_start_ns;
static bool g_timing_enabled = true;
static bool g_use_recorded_cpu;
static bool g_json_output;
static int g_cpu_list[CPU_SETSIZE];
static size_t g_cpu_count;
static int g_controller_cpu = -1;
static cpu_set_t g_allowed_cpus;
static uint64_t g_spin_ns = 50000;
static double g_time_scale = 1.0;

extern void free_sized(void *, size_t) __attribute__((weak));

static uint64_t monotonic_ns(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        perror("clock_gettime");
        _Exit(1);
    }
    return (uint64_t)value.tv_sec * 1000000000ULL +
           (uint64_t)value.tv_nsec;
}

static void cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

static void *map_anonymous(size_t bytes)
{
    if (bytes == 0) {
        bytes = 1;
    }
    void *mapping = mmap(
        NULL, bytes, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    return mapping;
}

static size_t replay_alignment(const NativeOp *op)
{
    size_t alignment = op->aux;
    if (alignment < sizeof(void *) ||
        (alignment & (alignment - 1)) != 0) {
        alignment = 64;
        if (op->function == FUNC_ALIGNED_ALLOC) {
            while (alignment > sizeof(void *) &&
                   op->size % alignment != 0) {
                alignment /= 2;
            }
        }
    }
    return alignment;
}

static void *run_allocation(const NativeOp *op)
{
    size_t size = (size_t)op->size;
    size_t alignment;
    void *pointer = NULL;

    switch (op->function) {
        case FUNC_MALLOC:
            return malloc(size);
        case FUNC_CALLOC:
            if (op->aux != 0 && op->size % op->aux == 0) {
                return calloc(op->aux, (size_t)(op->size / op->aux));
            }
            return calloc(1, size);
        case FUNC_POSIX_MEMALIGN:
            alignment = replay_alignment(op);
            if (posix_memalign(&pointer, alignment, size) != 0) {
                return NULL;
            }
            return pointer;
        case FUNC_ALIGNED_ALLOC:
            alignment = replay_alignment(op);
            return aligned_alloc(alignment, size);
        case FUNC_VALLOC:
            return valloc(size);
        case FUNC_MEMALIGN:
            return memalign(replay_alignment(op), size);
        case FUNC_PVALLOC:
            return pvalloc(size);
        default:
            return malloc(size);
    }
}

static void publish_slot(uint32_t slot, uint32_t generation, void *pointer)
{
    if (slot == NATIVE_SLOT_NONE) {
        return;
    }
    atomic_store_explicit(
        &g_slots[slot].pointer, (uintptr_t)pointer,
        memory_order_relaxed);
    atomic_store_explicit(
        &g_slots[slot].generation, generation,
        memory_order_release);
}

static void *wait_for_slot(Worker *worker, const NativeOp *op)
{
    uint64_t spins = 0;
    while (atomic_load_explicit(
               &g_slots[op->slot].generation,
               memory_order_acquire) < op->wait_generation) {
        if (++spins < 4096) {
            cpu_relax();
        } else {
            spins = 0;
            ++worker->dependency_yields;
            sched_yield();
        }
    }
    return (void *)atomic_load_explicit(
        &g_slots[op->slot].pointer, memory_order_relaxed);
}

static uint64_t scaled_target(uint64_t relative_ns)
{
    long double scaled = (long double)relative_ns * g_time_scale;
    if (scaled >= (long double)(UINT64_MAX - g_start_ns)) {
        return UINT64_MAX;
    }
    return g_start_ns + (uint64_t)scaled;
}

static uint64_t wait_until(uint64_t relative_ns)
{
    uint64_t target = scaled_target(relative_ns);
    for (;;) {
        uint64_t now = monotonic_ns();
        if (now >= target) {
            return now - target;
        }
        uint64_t remaining = target - now;
        if (remaining > g_spin_ns + 1000) {
            uint64_t wake = target - g_spin_ns;
            struct timespec value = {
                .tv_sec = (time_t)(wake / 1000000000ULL),
                .tv_nsec = (long)(wake % 1000000000ULL),
            };
            int result;
            do {
                result = clock_nanosleep(
                    CLOCK_MONOTONIC, TIMER_ABSTIME, &value, NULL);
            } while (result == EINTR);
            if (result != 0) {
                errno = result;
                perror("clock_nanosleep");
                _Exit(1);
            }
        } else {
            cpu_relax();
        }
    }
}

static void execute_operation(Worker *worker, const NativeOp *op)
{
    void *old_pointer;
    void *new_pointer;

    ++worker->function_counts[op->function];
    switch (op->kind) {
        case OP_ALLOC:
            if (op->wait_generation != 0) {
                (void)wait_for_slot(worker, op);
            }
            new_pointer = run_allocation(op);
            if ((op->flags & TRACE_RESULT_SUCCEEDED) != 0 &&
                new_pointer == NULL) {
                ++worker->runtime_allocation_failures;
            }
            publish_slot(
                op->slot, op->output_generation, new_pointer);
            break;
        case OP_FREE:
            old_pointer = wait_for_slot(worker, op);
            if (op->function == FUNC_FREE_SIZED && free_sized != NULL) {
                free_sized(old_pointer, (size_t)op->size);
            } else {
                free(old_pointer);
            }
            publish_slot(op->slot, op->output_generation, NULL);
            break;
        case OP_REALLOC_NULL:
            if (op->wait_generation != 0) {
                (void)wait_for_slot(worker, op);
            }
            new_pointer = realloc(NULL, (size_t)op->size);
            if ((op->flags & TRACE_RESULT_SUCCEEDED) != 0 &&
                new_pointer == NULL) {
                ++worker->runtime_allocation_failures;
            }
            publish_slot(
                op->slot, op->output_generation, new_pointer);
            break;
        case OP_REALLOC:
            old_pointer = wait_for_slot(worker, op);
            new_pointer = realloc(old_pointer, (size_t)op->size);
            if (new_pointer == NULL && op->size != 0) {
                if ((op->flags & TRACE_RESULT_SUCCEEDED) != 0) {
                    ++worker->runtime_allocation_failures;
                }
                new_pointer = old_pointer;
            }
            publish_slot(
                op->slot, op->output_generation, new_pointer);
            break;
        case OP_UNKNOWN_FREE:
            /* Keep the call count exact without ever passing a trace address
             * from another process to the allocator. */
            free(NULL);
            break;
        default:
            break;
    }
}

static void pin_current_thread(
    int cpu, const char *role, uint64_t trace_tid)
{
    if (cpu < 0) {
        return;
    }
    if (cpu >= CPU_SETSIZE ||
        !CPU_ISSET((unsigned)cpu, &g_allowed_cpus)) {
        fprintf(
            stderr, "CPU is not available: role=%s trace_tid=%" PRIu64
            " cpu=%d\n", role, trace_tid, cpu);
        _Exit(2);
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET((unsigned)cpu, &set);
    int result = pthread_setaffinity_np(
        pthread_self(), sizeof(set), &set);
    if (result != 0) {
        fprintf(
            stderr, "affinity failed: role=%s trace_tid=%" PRIu64
            " cpu=%d: %s\n", role, trace_tid, cpu, strerror(result));
        _Exit(2);
    }
}

static int worker_cpu(const Worker *worker)
{
    if (g_cpu_count != 0) {
        size_t index = (size_t)(worker - g_workers);
        return g_cpu_list[index % g_cpu_count];
    }
    if (g_use_recorded_cpu &&
        worker->description->recorded_cpu != UINT32_MAX) {
        return (int)worker->description->recorded_cpu;
    }
    return -1;
}

static void *worker_main(void *argument)
{
    Worker *worker = argument;
    const NativeThread *thread = worker->description;
    pin_current_thread(worker_cpu(worker), "worker", thread->trace_tid);

    atomic_fetch_add_explicit(&g_ready, 1, memory_order_release);
    while (!atomic_load_explicit(&g_start, memory_order_acquire)) {
        cpu_relax();
    }

    uint64_t previous_target = UINT64_MAX;
    for (uint64_t index = 0; index < thread->op_count; ++index) {
        const NativeOp *op = &g_ops[thread->first_op + index];
        if (g_timing_enabled && op->target_ns != previous_target) {
            uint64_t lateness = wait_until(op->target_ns);
            ++worker->timing_buckets;
            worker->total_lateness_ns += lateness;
            if (lateness > worker->maximum_lateness_ns) {
                worker->maximum_lateness_ns = lateness;
            }
            if (lateness >= LATE_THRESHOLD_NS) {
                ++worker->late_events;
            }
            previous_target = op->target_ns;
        }
        execute_operation(worker, op);
    }
    worker->finish_ns = monotonic_ns();
    uint64_t observed = atomic_load_explicit(
        &g_last_finish_ns, memory_order_relaxed);
    while (observed < worker->finish_ns &&
           !atomic_compare_exchange_weak_explicit(
               &g_last_finish_ns, &observed, worker->finish_ns,
               memory_order_release, memory_order_relaxed)) {
    }
    return NULL;
}

static bool range_is_valid(uint64_t offset, uint64_t count,
                           uint64_t element_size, uint64_t image_size)
{
    return offset <= image_size &&
           count <= (image_size - offset) / element_size;
}

static void load_embedded_image(void)
{
    int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        perror("open /proc/self/exe");
        exit(1);
    }
    struct stat status;
    if (fstat(fd, &status) != 0 ||
        status.st_size < (off_t)sizeof(NativeTrailer)) {
        perror("fstat /proc/self/exe");
        close(fd);
        exit(1);
    }
    size_t file_size = (size_t)status.st_size;
    void *mapping = mmap(
        NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mapping == MAP_FAILED) {
        perror("mmap /proc/self/exe");
        exit(1);
    }

    const NativeTrailer *trailer = (const NativeTrailer *)(
        (const uint8_t *)mapping + file_size - sizeof(*trailer));
    if (memcmp(trailer->magic, NATIVE_TRAILER_MAGIC, 8) != 0 ||
        trailer->version != NATIVE_VERSION ||
        trailer->image_size > file_size - sizeof(*trailer)) {
        fprintf(stderr, "missing or invalid embedded replay image\n");
        exit(1);
    }
    uint64_t image_offset =
        file_size - sizeof(*trailer) - trailer->image_size;
    g_header = (const NativeImageHeader *)(
        (const uint8_t *)mapping + image_offset);
    if (memcmp(g_header->magic, NATIVE_IMAGE_MAGIC, 8) != 0 ||
        g_header->version != NATIVE_VERSION ||
        g_header->header_size != sizeof(*g_header) ||
        !range_is_valid(
            g_header->thread_offset, g_header->thread_count,
            sizeof(NativeThread), trailer->image_size) ||
        !range_is_valid(
            g_header->op_offset,
            g_header->op_count,
            sizeof(NativeOp), trailer->image_size)) {
        fprintf(stderr, "invalid embedded replay image layout\n");
        exit(1);
    }
    g_threads = (const NativeThread *)(
        (const uint8_t *)g_header + g_header->thread_offset);
    g_ops = (const NativeOp *)(
        (const uint8_t *)g_header + g_header->op_offset);

    uint64_t image_op_count = g_header->op_count;
    for (uint32_t index = 0; index < g_header->thread_count; ++index) {
        const NativeThread *thread = &g_threads[index];
        if (thread->first_op > image_op_count ||
            thread->op_count > image_op_count - thread->first_op) {
            fprintf(stderr, "invalid operation range for thread %" PRIu32 "\n", index);
            exit(1);
        }
    }
}

static double parse_positive_double(const char *text, const char *name)
{
    errno = 0;
    char *end = NULL;
    double value = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' ||
        !isfinite(value) || value <= 0.0) {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        exit(2);
    }
    return value;
}

static uint64_t parse_u64(const char *text, const char *name)
{
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        exit(2);
    }
    return value;
}

static int parse_cpu_id(const char *text, const char *name)
{
    errno = 0;
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value < 0 || value >= CPU_SETSIZE) {
        fprintf(stderr, "invalid %s CPU: %s\n", name, text);
        exit(2);
    }
    return (int)value;
}

static void append_cpu(int cpu)
{
    for (size_t index = 0; index < g_cpu_count; ++index) {
        if (g_cpu_list[index] == cpu) {
            fprintf(stderr, "duplicate CPU in --cpu-list: %d\n", cpu);
            exit(2);
        }
    }
    if (g_cpu_count >= CPU_SETSIZE) {
        fprintf(stderr, "too many CPUs in --cpu-list\n");
        exit(2);
    }
    g_cpu_list[g_cpu_count++] = cpu;
}

static void parse_cpu_list(const char *text)
{
    char *copy = strdup(text);
    if (copy == NULL) {
        perror("strdup --cpu-list");
        exit(1);
    }
    char *save = NULL;
    for (char *token = strtok_r(copy, ",", &save);
         token != NULL; token = strtok_r(NULL, ",", &save)) {
        char *dash = strchr(token, '-');
        if (dash == NULL) {
            append_cpu(parse_cpu_id(token, "--cpu-list"));
            continue;
        }
        *dash = '\0';
        int first = parse_cpu_id(token, "--cpu-list");
        int last = parse_cpu_id(dash + 1, "--cpu-list");
        if (last < first) {
            fprintf(stderr, "descending CPU range: %d-%d\n", first, last);
            exit(2);
        }
        for (int cpu = first; cpu <= last; ++cpu) {
            append_cpu(cpu);
        }
    }
    free(copy);
    if (g_cpu_count == 0) {
        fprintf(stderr, "--cpu-list must not be empty\n");
        exit(2);
    }
}

static void validate_cpu_options(void)
{
    if (g_use_recorded_cpu && g_cpu_count != 0) {
        fprintf(stderr, "--recorded-cpu and --cpu-list are mutually exclusive\n");
        exit(2);
    }
    if (g_controller_cpu >= 0) {
        if (g_cpu_count == 0 && !g_use_recorded_cpu) {
            fprintf(stderr,
                    "--controller-cpu requires --cpu-list or --recorded-cpu\n");
            exit(2);
        }
        for (size_t index = 0; index < g_cpu_count; ++index) {
            if (g_cpu_list[index] == g_controller_cpu) {
                fprintf(stderr,
                        "controller CPU %d is also in --cpu-list\n",
                        g_controller_cpu);
                exit(2);
            }
        }
    }
}

static void usage(const char *program)
{
    fprintf(
        stderr,
        "Usage: %s [--no-timing] [--scale F] [--spin-us N]\n"
        "          [--cpu-list LIST | --recorded-cpu] "
        "[--controller-cpu N] [--json]\n",
        program);
}

int main(int argc, char **argv)
{
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--no-timing") == 0) {
            g_timing_enabled = false;
        } else if (strcmp(argv[index], "--recorded-cpu") == 0) {
            g_use_recorded_cpu = true;
        } else if (strcmp(argv[index], "--cpu-list") == 0 &&
                   index + 1 < argc) {
            parse_cpu_list(argv[++index]);
        } else if (strcmp(argv[index], "--controller-cpu") == 0 &&
                   index + 1 < argc) {
            g_controller_cpu = parse_cpu_id(
                argv[++index], "--controller-cpu");
        } else if (strcmp(argv[index], "--json") == 0) {
            g_json_output = true;
        } else if (strcmp(argv[index], "--scale") == 0 && index + 1 < argc) {
            g_time_scale = parse_positive_double(argv[++index], "scale");
        } else if (strcmp(argv[index], "--spin-us") == 0 && index + 1 < argc) {
            uint64_t value = parse_u64(argv[++index], "spin-us");
            if (value > UINT64_MAX / 1000) {
                fprintf(stderr, "spin-us is too large\n");
                return 2;
            }
            g_spin_ns = value * 1000;
        } else if (strcmp(argv[index], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    validate_cpu_options();
    CPU_ZERO(&g_allowed_cpus);
    if (sched_getaffinity(0, sizeof(g_allowed_cpus), &g_allowed_cpus) != 0) {
        perror("sched_getaffinity");
        return 2;
    }

    load_embedded_image();
    if (g_header->thread_count == 0) {
        fprintf(stderr, "embedded trace has no allocator threads\n");
        return 1;
    }
    if (g_use_recorded_cpu && g_controller_cpu >= 0) {
        for (uint32_t index = 0; index < g_header->thread_count; ++index) {
            if (g_threads[index].recorded_cpu ==
                (uint32_t)g_controller_cpu) {
                fprintf(stderr,
                        "controller CPU %d is used by trace_tid=%" PRIu64 "\n",
                        g_controller_cpu, g_threads[index].trace_tid);
                return 2;
            }
        }
    }
    if ((g_header->flags & NATIVE_FLAG_HAS_UNKNOWN) != 0) {
        fprintf(stderr,
                "warning: unmatched frees are replayed as free(NULL)\n");
    }
    if (g_cpu_count != 0 && g_header->thread_count > g_cpu_count) {
        fprintf(
            stderr,
            "warning: %" PRIu32 " workers share %zu CPUs; "
            "use at least one CPU per worker for repeatability\n",
            g_header->thread_count, g_cpu_count);
    }
    pin_current_thread(g_controller_cpu, "controller", 0);

    g_slots = map_anonymous(
        (size_t)g_header->slot_count * sizeof(*g_slots));
    g_workers = map_anonymous(
        (size_t)g_header->thread_count * sizeof(*g_workers));

    uint64_t total_operations = 0;
    for (uint32_t index = 0; index < g_header->thread_count; ++index) {
        g_workers[index].description = &g_threads[index];
        total_operations += g_threads[index].op_count;
        int result = pthread_create(
            &g_workers[index].thread, NULL,
            worker_main, &g_workers[index]);
        if (result != 0) {
            fprintf(stderr,
                    "pthread_create trace_tid=%" PRIu64 ": %s\n",
                    g_threads[index].trace_tid, strerror(result));
            return 1;
        }
    }

    while (atomic_load_explicit(&g_ready, memory_order_acquire) !=
           g_header->thread_count) {
        sched_yield();
    }
    g_start_ns = monotonic_ns();
    atomic_store_explicit(&g_start, true, memory_order_release);

    uint64_t function_counts[FUNC_COUNT] = {0};
    uint64_t timing_buckets = 0;
    uint64_t late_events = 0;
    uint64_t total_lateness_ns = 0;
    uint64_t maximum_lateness_ns = 0;
    uint64_t dependency_yields = 0;
    uint64_t runtime_allocation_failures = 0;
    for (uint32_t index = 0; index < g_header->thread_count; ++index) {
        int result = pthread_join(g_workers[index].thread, NULL);
        if (result != 0) {
            fprintf(stderr,
                    "pthread_join trace_tid=%" PRIu64 ": %s\n",
                    g_threads[index].trace_tid, strerror(result));
            return 1;
        }
        for (uint32_t function = 0; function < FUNC_COUNT; ++function) {
            function_counts[function] +=
                g_workers[index].function_counts[function];
        }
        timing_buckets += g_workers[index].timing_buckets;
        late_events += g_workers[index].late_events;
        total_lateness_ns += g_workers[index].total_lateness_ns;
        if (g_workers[index].maximum_lateness_ns > maximum_lateness_ns) {
            maximum_lateness_ns =
                g_workers[index].maximum_lateness_ns;
        }
        dependency_yields += g_workers[index].dependency_yields;
        runtime_allocation_failures +=
            g_workers[index].runtime_allocation_failures;
    }
    uint64_t last_finish_ns = atomic_load_explicit(
        &g_last_finish_ns, memory_order_acquire);
    if (last_finish_ns < g_start_ns) {
        fprintf(stderr, "invalid replay finish timestamp\n");
        return 1;
    }
    uint64_t elapsed_ns = last_finish_ns - g_start_ns;
    double late_percent = timing_buckets != 0
        ? 100.0 * (double)late_events / (double)timing_buckets : 0.0;
    double mean_lateness_us = timing_buckets != 0
        ? (double)total_lateness_ns / (double)timing_buckets / 1000.0 : 0.0;

    if (g_json_output) {
        printf(
            "{\"replay_time_ms\":%.6f,\"operations\":%" PRIu64
            ",\"threads\":%" PRIu32 ",\"slots\":%" PRIu32
            ",\"timing_buckets\":%" PRIu64
            ",\"late_threshold_us\":%.3f"
            ",\"late_buckets\":%" PRIu64
            ",\"late_bucket_pct\":%.6f"
            ",\"mean_lateness_us\":%.6f"
            ",\"max_lateness_us\":%.6f"
            ",\"dependency_yields\":%" PRIu64
            ",\"runtime_allocation_failures\":%" PRIu64
            ",\"malloc\":%" PRIu64 ",\"free\":%" PRIu64
            ",\"calloc\":%" PRIu64 ",\"realloc\":%" PRIu64
            ",\"free_sized\":%" PRIu64
            ",\"posix_memalign\":%" PRIu64
            ",\"aligned_alloc\":%" PRIu64
            ",\"valloc\":%" PRIu64 ",\"memalign\":%" PRIu64
            ",\"pvalloc\":%" PRIu64 "}\n",
            (double)elapsed_ns / 1000000.0,
            total_operations, g_header->thread_count, g_header->slot_count,
            timing_buckets, (double)LATE_THRESHOLD_NS / 1000.0,
            late_events, late_percent, mean_lateness_us,
            (double)maximum_lateness_ns / 1000.0, dependency_yields,
            runtime_allocation_failures, function_counts[0],
            function_counts[1], function_counts[2], function_counts[3],
            function_counts[4], function_counts[5], function_counts[6],
            function_counts[7], function_counts[8], function_counts[9]);
        return runtime_allocation_failures == 0 ? 0 : 3;
    }

    printf(
        "native_replay operations=%" PRIu64 " threads=%" PRIu32
        " slots=%" PRIu32 " elapsed_ms=%.3f\n",
        total_operations, g_header->thread_count, g_header->slot_count,
        (double)elapsed_ns / 1000000.0);
    printf(
        "malloc=%" PRIu64 " free=%" PRIu64 " calloc=%" PRIu64
        " realloc=%" PRIu64 " free_sized=%" PRIu64
        " posix_memalign=%" PRIu64 " aligned_alloc=%" PRIu64
        " valloc=%" PRIu64 " memalign=%" PRIu64
        " pvalloc=%" PRIu64 "\n",
        function_counts[0], function_counts[1], function_counts[2],
        function_counts[3], function_counts[4], function_counts[5],
        function_counts[6], function_counts[7], function_counts[8],
        function_counts[9]);
    printf(
        "timing_buckets=%" PRIu64 " late_threshold_us=%.3f"
        " late_buckets=%" PRIu64
        " late_bucket_pct=%.3f mean_lateness_us=%.3f"
        " max_lateness_us=%.3f"
        " dependency_yields=%" PRIu64
        " runtime_allocation_failures=%" PRIu64 "\n",
        timing_buckets, (double)LATE_THRESHOLD_NS / 1000.0,
        late_events, late_percent, mean_lateness_us,
        (double)maximum_lateness_ns / 1000.0, dependency_yields,
        runtime_allocation_failures);
    return runtime_allocation_failures == 0 ? 0 : 3;
}
