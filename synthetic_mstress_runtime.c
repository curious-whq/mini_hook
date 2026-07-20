/* Compact phase-based allocator workload runtime for rply_to_mstress.py. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <malloc.h>
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

#define PROFILE_MAGIC "MSRUN001"
#define TRAILER_MAGIC "MSEND001"
#define PROFILE_VERSION 2u
#define FUNCTION_COUNT 10u
#define INDEX_NONE UINT32_MAX

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
};

typedef struct {
    uint8_t magic[8];
    uint32_t version;
    uint32_t header_size;
    uint32_t thread_count;
    uint32_t phase_count;
    uint32_t function_count;
    uint32_t wave_count;
    uint64_t total_ops;
    uint64_t thread_offset;
    uint64_t phase_offset;
    uint64_t model_offset;
    uint64_t route_offset;
    uint64_t sample_offset;
    uint64_t image_size;
} ProfileHeader;

typedef struct {
    uint32_t first_phase;
    uint32_t phase_count;
    uint64_t operations;
} ProfileWave;

typedef struct {
    uint64_t trace_tid;
    uint64_t seed;
    uint32_t capacity;
    uint32_t recorded_cpu;
    uint64_t total_ops;
    uint64_t model_first;
} ProfileThread;

typedef struct {
    uint32_t first;
    uint32_t count;
} SampleRange;

typedef struct {
    uint64_t total_ops;
    SampleRange sizes[FUNCTION_COUNT];
} ProfilePhase;

typedef struct {
    uint64_t counts[FUNCTION_COUNT];
    uint64_t remote_free;
    uint64_t remote_publish;
    uint64_t realloc_null;
    uint64_t route_first;
    uint32_t route_count;
    uint32_t reserved;
} ThreadPhaseModel;

typedef struct {
    uint32_t target_thread;
    uint32_t reserved;
    uint64_t count;
} RemoteRoute;

typedef struct {
    uint8_t magic[8];
    uint64_t image_size;
    uint64_t version;
} ProfileTrailer;

_Static_assert(sizeof(ProfileHeader) == 88, "profile header layout");
_Static_assert(sizeof(ProfileWave) == 16, "profile wave layout");
_Static_assert(sizeof(ProfileThread) == 40, "profile thread layout");
_Static_assert(sizeof(ProfilePhase) == 88, "profile phase layout");
_Static_assert(sizeof(ThreadPhaseModel) == 120, "thread phase layout");
_Static_assert(sizeof(RemoteRoute) == 16, "route layout");
_Static_assert(sizeof(ProfileTrailer) == 24, "profile trailer layout");

typedef struct {
    void *pointer;
    uint64_t size;
    uint32_t owner;
    uint32_t next_plus_one;
} Object;

typedef enum {
    TOUCH_NONE,
    TOUCH_FIRST,
    TOUCH_PAGES,
    TOUCH_FULL,
} TouchMode;

typedef struct {
    pthread_t thread;
    uint32_t index;
    uint32_t object_first;
    uint32_t run_first_phase;
    uint32_t run_phase_count;
    const ProfileThread *profile;
    uint64_t random;
    uint32_t *local;
    uint32_t local_count;
    uint32_t next_descriptor_offset;
    uint32_t local_free_plus_one;
    _Atomic uint64_t recycled_head;
    _Atomic uint64_t inbound_head;
    uint64_t function_counts[FUNCTION_COUNT];
    uint64_t remote_frees;
    uint64_t deferred_remote_frees;
    uint64_t local_free_underflows;
    uint64_t allocation_failures;
    uint64_t finish_ns;
} Worker;

static const ProfileHeader *g_header;
static const ProfileWave *g_waves;
static const ProfileThread *g_profile_threads;
static const ProfilePhase *g_phases;
static const ThreadPhaseModel *g_models;
static const RemoteRoute *g_routes;
static const uint64_t *g_samples;
static uint64_t g_route_count;
static uint64_t g_sample_count;
static Worker *g_workers;
static Object *g_objects;
static _Atomic uint32_t g_ready;
static _Atomic uint32_t g_generated_done;
static uint32_t g_wave_worker_count;
static _Atomic bool g_start;
static _Atomic bool g_failed;
static _Atomic uint64_t g_last_finish_ns;
static uint64_t g_start_ns;
static uint64_t g_seed_override;
static bool g_json;
static TouchMode g_touch = TOUCH_FIRST;

extern void free_sized(void *, size_t) __attribute__((weak));

static void fail(const char *message)
{
    fprintf(stderr, "synthetic_mstress: %s\n", message);
    atomic_store_explicit(&g_failed, true, memory_order_release);
}

static uint64_t monotonic_ns(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        perror("clock_gettime");
        _Exit(1);
    }
    return (uint64_t)value.tv_sec * 1000000000ULL + (uint64_t)value.tv_nsec;
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
    void *result = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (result == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    return result;
}

static uint64_t random_next(Worker *worker)
{
    uint64_t value = (worker->random += UINT64_C(0x9e3779b97f4a7c15));
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static uint64_t make_head(uint32_t tag, uint32_t plus_one)
{
    return ((uint64_t)tag << 32) | plus_one;
}

static uint32_t stack_pop(_Atomic uint64_t *head)
{
    uint64_t old = atomic_load_explicit(head, memory_order_acquire);
    for (;;) {
        uint32_t plus_one = (uint32_t)old;
        if (plus_one == 0) {
            return INDEX_NONE;
        }
        uint32_t index = plus_one - 1;
        uint32_t next = g_objects[index].next_plus_one;
        uint64_t replacement = make_head((uint32_t)(old >> 32) + 1, next);
        if (atomic_compare_exchange_weak_explicit(
                head, &old, replacement,
                memory_order_acq_rel, memory_order_acquire)) {
            return index;
        }
    }
}

static void stack_push(_Atomic uint64_t *head, uint32_t index)
{
    uint64_t old = atomic_load_explicit(head, memory_order_acquire);
    for (;;) {
        g_objects[index].next_plus_one = (uint32_t)old;
        uint64_t replacement = make_head((uint32_t)(old >> 32) + 1, index + 1);
        if (atomic_compare_exchange_weak_explicit(
                head, &old, replacement,
                memory_order_release, memory_order_acquire)) {
            return;
        }
    }
}

static void touch_allocated(void *pointer, size_t size, uint64_t token)
{
    if (pointer == NULL || size == 0 || g_touch == TOUCH_NONE) {
        return;
    }
    volatile unsigned char *bytes = pointer;
    unsigned char value = (unsigned char)(token | 1u);
    if (g_touch == TOUCH_FIRST) {
        bytes[0] = value;
        return;
    }
    size_t stride = g_touch == TOUCH_PAGES ? 4096 : sizeof(uintptr_t);
    for (size_t offset = 0; offset < size;) {
        bytes[offset] = (unsigned char)(value ^ (unsigned char)offset);
        if (size - offset <= stride) {
            break;
        }
        offset += stride;
    }
    bytes[size - 1] = value;
}

static void touch_before_free(const Object *object)
{
    if (object->pointer == NULL || object->size == 0 || g_touch == TOUCH_NONE) {
        return;
    }
    volatile const unsigned char *bytes = object->pointer;
    volatile unsigned char sink = bytes[0];
    if (g_touch == TOUCH_PAGES || g_touch == TOUCH_FULL) {
        size_t stride = g_touch == TOUCH_PAGES ? 4096 : sizeof(uintptr_t);
        for (size_t offset = stride; offset < object->size;) {
            sink ^= bytes[offset];
            if (object->size - offset <= stride) {
                break;
            }
            offset += stride;
        }
        sink ^= bytes[object->size - 1];
    }
    (void)sink;
}

static void *run_new_allocation(uint32_t function, size_t size)
{
    void *pointer = NULL;
    switch (function) {
        case FUNC_MALLOC:
            return malloc(size);
        case FUNC_CALLOC:
            return calloc(1, size);
        case FUNC_POSIX_MEMALIGN:
            if (posix_memalign(&pointer, 64, size) != 0) {
                return NULL;
            }
            return pointer;
        case FUNC_ALIGNED_ALLOC: {
            if (size > SIZE_MAX - 63u) {
                return NULL;
            }
            size_t rounded = (size + 63u) & ~(size_t)63u;
            return aligned_alloc(64, rounded);
        }
        case FUNC_VALLOC:
            return valloc(size);
        case FUNC_MEMALIGN:
            return memalign(64, size);
        case FUNC_PVALLOC:
            return pvalloc(size);
        default:
            return malloc(size);
    }
}

static void recycle_object(Worker *freer, uint32_t object_index)
{
    Object *object = &g_objects[object_index];
    uint32_t owner = object->owner;
    object->pointer = NULL;
    object->size = 0;
    if (owner == freer->index) {
        object->next_plus_one = freer->local_free_plus_one;
        freer->local_free_plus_one = object_index + 1;
    } else {
        stack_push(&g_workers[owner].recycled_head, object_index);
    }
}

static void run_free_call(Worker *worker, uint32_t function, uint32_t object_index,
                          bool remote)
{
    Object *object = &g_objects[object_index];
    touch_before_free(object);
    if (function == FUNC_FREE_SIZED && free_sized != NULL) {
        free_sized(object->pointer, (size_t)object->size);
    } else {
        free(object->pointer);
    }
    ++worker->function_counts[function];
    if (remote && object->owner != worker->index) {
        ++worker->remote_frees;
    }
    recycle_object(worker, object_index);
}

static uint32_t local_remove(Worker *worker, uint32_t position)
{
    uint32_t result = worker->local[position];
    --worker->local_count;
    worker->local[position] = worker->local[worker->local_count];
    return result;
}

static uint64_t sample_size(Worker *worker, uint32_t phase, uint32_t function)
{
    SampleRange range = g_phases[phase].sizes[function];
    if (range.count == 0) {
        return 1;
    }
    return g_samples[range.first + random_next(worker) % range.count];
}

static uint32_t route_target(const ThreadPhaseModel *model, uint64_t ordinal)
{
    for (uint32_t index = 0; index < model->route_count; ++index) {
        const RemoteRoute *route = &g_routes[model->route_first + index];
        if (ordinal < route->count) {
            return route->target_thread;
        }
        ordinal -= route->count;
    }
    return INDEX_NONE;
}

static bool is_allocation_function(uint32_t function)
{
    return function != FUNC_FREE && function != FUNC_FREE_SIZED;
}

static uint64_t allocation_call_count(const ThreadPhaseModel *model)
{
    uint64_t result = 0;
    for (uint32_t function = 0; function < FUNCTION_COUNT; ++function) {
        if (is_allocation_function(function)) {
            result += model->counts[function];
        }
    }
    return result;
}

static uint32_t choose_function(Worker *worker, uint64_t remaining[FUNCTION_COUNT],
                                uint64_t total)
{
    uint64_t selected = random_next(worker) % total;
    for (uint32_t function = 0; function < FUNCTION_COUNT; ++function) {
        if (selected < remaining[function]) {
            --remaining[function];
            return function;
        }
        selected -= remaining[function];
    }
    return FUNC_MALLOC;
}

static void publish_object(uint32_t target, uint32_t object_index)
{
    stack_push(&g_workers[target].inbound_head, object_index);
}

static uint32_t take_descriptor(Worker *worker)
{
    if (worker->local_free_plus_one != 0) {
        uint32_t result = worker->local_free_plus_one - 1;
        worker->local_free_plus_one = g_objects[result].next_plus_one;
        return result;
    }
    uint32_t result = stack_pop(&worker->recycled_head);
    if (result != INDEX_NONE) {
        return result;
    }
    if (worker->next_descriptor_offset < worker->profile->capacity) {
        return worker->object_first + worker->next_descriptor_offset++;
    }
    fail("descriptor capacity exhausted; regenerate with a larger --capacity-factor");
    return INDEX_NONE;
}

static void run_alloc_call(Worker *worker, uint32_t phase, uint32_t function,
                           bool publish, uint32_t target, bool force_realloc_null)
{
    uint64_t sampled = sample_size(worker, phase, function);
    size_t size = sampled > SIZE_MAX ? SIZE_MAX : (size_t)sampled;
    uint32_t object_index = INDEX_NONE;
    Object *object = NULL;

    if (function == FUNC_REALLOC && !force_realloc_null &&
        worker->local_count != 0) {
        uint32_t position = (uint32_t)(random_next(worker) % worker->local_count);
        object_index = worker->local[position];
        object = &g_objects[object_index];
        void *old_pointer = object->pointer;
        void *result = realloc(old_pointer, size);
        ++worker->function_counts[function];
        if (result == NULL && size != 0) {
            ++worker->allocation_failures;
        } else {
            object->pointer = result;
            object->size = size;
            touch_allocated(result, size, random_next(worker));
        }
        if (publish) {
            (void)local_remove(worker, position);
            publish_object(target, object_index);
        } else if (result == NULL && size == 0) {
            (void)local_remove(worker, position);
            recycle_object(worker, object_index);
        }
        return;
    }

    object_index = take_descriptor(worker);
    if (object_index == INDEX_NONE) {
        return;
    }
    object = &g_objects[object_index];
    void *result;
    if (function == FUNC_REALLOC) {
        result = realloc(NULL, size);
    } else {
        result = run_new_allocation(function, size);
    }
    ++worker->function_counts[function];
    if (result == NULL && size != 0) {
        ++worker->allocation_failures;
    }
    object->pointer = result;
    object->size = size;
    object->owner = worker->index;
    touch_allocated(result, size, random_next(worker));
    if (publish) {
        publish_object(target, object_index);
    } else {
        worker->local[worker->local_count++] = object_index;
    }
}

static void run_local_free(Worker *worker, uint32_t function,
                           uint64_t local_debt[FUNCTION_COUNT])
{
    if (worker->local_count == 0) {
        ++local_debt[function];
        return;
    }
    uint32_t position = (uint32_t)(random_next(worker) % worker->local_count);
    run_free_call(worker, function, local_remove(worker, position), false);
}

static void drain_local_debt(Worker *worker, uint64_t debt[FUNCTION_COUNT])
{
    while (worker->local_count != 0 &&
           (debt[FUNC_FREE] != 0 || debt[FUNC_FREE_SIZED] != 0)) {
        uint32_t function = debt[FUNC_FREE] != 0 ? FUNC_FREE : FUNC_FREE_SIZED;
        --debt[function];
        run_local_free(worker, function, debt);
    }
}

static bool drain_one_remote(Worker *worker, uint64_t debt[FUNCTION_COUNT])
{
    uint32_t function;
    if (debt[FUNC_FREE] != 0) {
        function = FUNC_FREE;
    } else if (debt[FUNC_FREE_SIZED] != 0) {
        function = FUNC_FREE_SIZED;
    } else {
        return false;
    }
    uint32_t object_index = stack_pop(&worker->inbound_head);
    if (object_index == INDEX_NONE) {
        return false;
    }
    --debt[function];
    run_free_call(worker, function, object_index, true);
    return true;
}

static void run_remote_free(Worker *worker, uint32_t function,
                            uint64_t debt[FUNCTION_COUNT])
{
    uint32_t object_index = stack_pop(&worker->inbound_head);
    if (object_index == INDEX_NONE) {
        ++debt[function];
        ++worker->deferred_remote_frees;
        return;
    }
    run_free_call(worker, function, object_index, true);
}

static void finish_debts(Worker *worker, uint64_t local_debt[FUNCTION_COUNT],
                         uint64_t remote_debt[FUNCTION_COUNT])
{
    drain_local_debt(worker, local_debt);
    for (uint32_t function = FUNC_FREE; function <= FUNC_FREE_SIZED;
         function += FUNC_FREE_SIZED - FUNC_FREE) {
        while (local_debt[function] != 0) {
            free(NULL);
            ++worker->function_counts[function];
            ++worker->local_free_underflows;
            --local_debt[function];
        }
    }

    while (remote_debt[FUNC_FREE] != 0 || remote_debt[FUNC_FREE_SIZED] != 0) {
        if (drain_one_remote(worker, remote_debt)) {
            continue;
        }
        if (atomic_load_explicit(&g_generated_done, memory_order_acquire) ==
            g_wave_worker_count) {
            fail("remote-free route exhausted before all deferred frees were satisfied");
            break;
        }
        for (uint32_t spin = 0; spin < 1024; ++spin) {
            cpu_relax();
        }
        sched_yield();
    }
}

static void update_last_finish(uint64_t finish)
{
    uint64_t old = atomic_load_explicit(&g_last_finish_ns, memory_order_relaxed);
    while (old < finish && !atomic_compare_exchange_weak_explicit(
               &g_last_finish_ns, &old, finish,
               memory_order_relaxed, memory_order_relaxed)) {
    }
}

static void *worker_main(void *argument)
{
    Worker *worker = argument;
    atomic_fetch_add_explicit(&g_ready, 1, memory_order_release);
    while (!atomic_load_explicit(&g_start, memory_order_acquire)) {
        cpu_relax();
    }

    uint64_t local_debt[FUNCTION_COUNT] = {0};
    uint64_t remote_debt[FUNCTION_COUNT] = {0};
    uint32_t phase_end = worker->run_first_phase + worker->run_phase_count;
    for (uint32_t phase = worker->run_first_phase; phase < phase_end; ++phase) {
        const ThreadPhaseModel *model =
            &g_models[worker->profile->model_first + phase];
        uint64_t remaining[FUNCTION_COUNT];
        memcpy(remaining, model->counts, sizeof(remaining));
        uint64_t total = 0;
        for (uint32_t function = 0; function < FUNCTION_COUNT; ++function) {
            total += remaining[function];
        }
        uint64_t alloc_total = allocation_call_count(model);
        uint64_t free_total = model->counts[FUNC_FREE] +
                              model->counts[FUNC_FREE_SIZED];
        uint64_t publish_accumulator = 0;
        uint64_t remote_accumulator = 0;
        uint64_t realloc_null_accumulator = 0;
        uint64_t publish_ordinal = 0;

        while (total != 0 && !atomic_load_explicit(&g_failed, memory_order_acquire)) {
            uint32_t function = choose_function(worker, remaining, total--);
            if (is_allocation_function(function)) {
                bool publish = false;
                bool force_realloc_null = false;
                uint32_t target = INDEX_NONE;
                if (alloc_total != 0 && model->remote_publish != 0) {
                    publish_accumulator += model->remote_publish;
                    if (publish_accumulator >= alloc_total) {
                        publish_accumulator -= alloc_total;
                        publish = true;
                        target = route_target(model, publish_ordinal++);
                        if (target >= g_header->thread_count) {
                            fail("invalid remote-free route in profile");
                            break;
                        }
                    }
                }
                if (function == FUNC_REALLOC && model->realloc_null != 0) {
                    realloc_null_accumulator += model->realloc_null;
                    if (realloc_null_accumulator >= model->counts[FUNC_REALLOC]) {
                        realloc_null_accumulator -= model->counts[FUNC_REALLOC];
                        force_realloc_null = true;
                    }
                }
                run_alloc_call(worker, phase, function, publish, target,
                               force_realloc_null);
                drain_local_debt(worker, local_debt);
            } else {
                bool remote = false;
                if (free_total != 0 && model->remote_free != 0) {
                    remote_accumulator += model->remote_free;
                    if (remote_accumulator >= free_total) {
                        remote_accumulator -= free_total;
                        remote = true;
                    }
                }
                if (remote) {
                    run_remote_free(worker, function, remote_debt);
                } else {
                    run_local_free(worker, function, local_debt);
                }
            }
            while (drain_one_remote(worker, remote_debt)) {
            }
        }
    }

    atomic_fetch_add_explicit(&g_generated_done, 1, memory_order_release);
    if (!atomic_load_explicit(&g_failed, memory_order_acquire)) {
        finish_debts(worker, local_debt, remote_debt);
    }
    worker->finish_ns = monotonic_ns();
    update_last_finish(worker->finish_ns);
    return NULL;
}

static bool range_valid(uint64_t offset, uint64_t count, uint64_t element_size,
                        uint64_t image_size)
{
    return offset <= image_size && count <= (image_size - offset) / element_size;
}

static void load_profile(void)
{
    int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        perror("open /proc/self/exe");
        exit(1);
    }
    struct stat status;
    if (fstat(fd, &status) != 0 || status.st_size < (off_t)sizeof(ProfileTrailer)) {
        perror("fstat /proc/self/exe");
        exit(1);
    }
    size_t file_size = (size_t)status.st_size;
    void *mapping = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mapping == MAP_FAILED) {
        perror("mmap /proc/self/exe");
        exit(1);
    }
    const ProfileTrailer *trailer = (const ProfileTrailer *)
        ((const uint8_t *)mapping + file_size - sizeof(ProfileTrailer));
    if (memcmp(trailer->magic, TRAILER_MAGIC, 8) != 0 ||
        trailer->version != PROFILE_VERSION ||
        trailer->image_size > file_size - sizeof(ProfileTrailer)) {
        fail("missing or invalid appended profile trailer");
        exit(1);
    }
    const uint8_t *image = (const uint8_t *)trailer - trailer->image_size;
    g_header = (const ProfileHeader *)image;
    if (memcmp(g_header->magic, PROFILE_MAGIC, 8) != 0 ||
        g_header->version != PROFILE_VERSION ||
        g_header->header_size != sizeof(ProfileHeader) ||
        g_header->function_count != FUNCTION_COUNT ||
        g_header->image_size != trailer->image_size ||
        g_header->thread_count == 0 || g_header->phase_count == 0 ||
        g_header->wave_count == 0) {
        fail("invalid profile header");
        exit(1);
    }
    uint64_t model_count = (uint64_t)g_header->thread_count * g_header->phase_count;
    if (!range_valid(sizeof(ProfileHeader), g_header->wave_count,
                     sizeof(ProfileWave), g_header->image_size) ||
        !range_valid(g_header->thread_offset, g_header->thread_count,
                     sizeof(ProfileThread), g_header->image_size) ||
        !range_valid(g_header->phase_offset, g_header->phase_count,
                     sizeof(ProfilePhase), g_header->image_size) ||
        !range_valid(g_header->model_offset, model_count,
                     sizeof(ThreadPhaseModel), g_header->image_size) ||
        g_header->route_offset > g_header->sample_offset ||
        g_header->sample_offset > g_header->image_size) {
        fail("profile offsets are out of range");
        exit(1);
    }
    g_route_count = (g_header->sample_offset - g_header->route_offset) /
                    sizeof(RemoteRoute);
    g_sample_count = (g_header->image_size - g_header->sample_offset) / 8;
    g_waves = (const ProfileWave *)(image + sizeof(ProfileHeader));
    g_profile_threads = (const ProfileThread *)(image + g_header->thread_offset);
    g_phases = (const ProfilePhase *)(image + g_header->phase_offset);
    g_models = (const ThreadPhaseModel *)(image + g_header->model_offset);
    g_routes = (const RemoteRoute *)(image + g_header->route_offset);
    g_samples = (const uint64_t *)(image + g_header->sample_offset);

    uint32_t expected_phase = 0;
    uint64_t wave_operations = 0;
    for (uint32_t wave = 0; wave < g_header->wave_count; ++wave) {
        if (g_waves[wave].first_phase != expected_phase ||
            g_waves[wave].phase_count == 0 ||
            g_waves[wave].phase_count > g_header->phase_count - expected_phase) {
            fail("profile wave phase range is invalid");
            exit(1);
        }
        expected_phase += g_waves[wave].phase_count;
        wave_operations += g_waves[wave].operations;
    }
    if (expected_phase != g_header->phase_count ||
        wave_operations != g_header->total_ops) {
        fail("profile wave totals do not match header");
        exit(1);
    }

    for (uint32_t phase = 0; phase < g_header->phase_count; ++phase) {
        for (uint32_t function = 0; function < FUNCTION_COUNT; ++function) {
            SampleRange range = g_phases[phase].sizes[function];
            if ((uint64_t)range.first + range.count > g_sample_count) {
                fail("profile size sample range is invalid");
                exit(1);
            }
        }
    }
    for (uint64_t index = 0; index < model_count; ++index) {
        const ThreadPhaseModel *model = &g_models[index];
        if (model->route_first + model->route_count > g_route_count) {
            fail("profile route range is invalid");
            exit(1);
        }
    }
}

static void initialize_workers(void)
{
    g_workers = map_anonymous((size_t)g_header->thread_count * sizeof(Worker));
    uint64_t total_capacity = 0;
    for (uint32_t index = 0; index < g_header->thread_count; ++index) {
        total_capacity += g_profile_threads[index].capacity;
    }
    if (total_capacity >= UINT32_MAX || total_capacity > SIZE_MAX / sizeof(Object)) {
        fail("profile descriptor capacity is too large");
        exit(1);
    }
    g_objects = map_anonymous((size_t)total_capacity * sizeof(Object));
    uint32_t first = 0;
    for (uint32_t index = 0; index < g_header->thread_count; ++index) {
        Worker *worker = &g_workers[index];
        worker->index = index;
        worker->profile = &g_profile_threads[index];
        worker->random = worker->profile->seed ^ g_seed_override;
        worker->object_first = first;
        worker->local = map_anonymous((size_t)worker->profile->capacity * sizeof(uint32_t));
        uint32_t capacity = worker->profile->capacity;
        atomic_init(&worker->recycled_head, 0);
        atomic_init(&worker->inbound_head, 0);
        first += capacity;
    }
}

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [--json] [--seed N] [--touch none|first|pages|full]\n",
            program);
}

static void parse_options(int argc, char **argv)
{
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--json") == 0) {
            g_json = true;
        } else if (strcmp(argv[index], "--seed") == 0 && index + 1 < argc) {
            char *end = NULL;
            errno = 0;
            g_seed_override = strtoull(argv[++index], &end, 0);
            if (errno != 0 || end == argv[index] || *end != '\0') {
                fail("invalid --seed value");
                exit(2);
            }
        } else if (strcmp(argv[index], "--touch") == 0 && index + 1 < argc) {
            const char *mode = argv[++index];
            if (strcmp(mode, "none") == 0) g_touch = TOUCH_NONE;
            else if (strcmp(mode, "first") == 0) g_touch = TOUCH_FIRST;
            else if (strcmp(mode, "pages") == 0) g_touch = TOUCH_PAGES;
            else if (strcmp(mode, "full") == 0) g_touch = TOUCH_FULL;
            else {
                fail("invalid --touch mode");
                exit(2);
            }
        } else if (strcmp(argv[index], "--help") == 0) {
            usage(argv[0]);
            exit(0);
        } else {
            usage(argv[0]);
            exit(2);
        }
    }
}

int main(int argc, char **argv)
{
    parse_options(argc, argv);
    load_profile();
    initialize_workers();
    atomic_init(&g_ready, 0);
    atomic_init(&g_generated_done, 0);
    atomic_init(&g_start, false);
    atomic_init(&g_failed, false);
    atomic_init(&g_last_finish_ns, 0);

    g_start_ns = monotonic_ns();
    uint32_t maximum_wave_threads = 0;
    uint64_t worker_launches = 0;
    for (uint32_t wave_index = 0; wave_index < g_header->wave_count; ++wave_index) {
        const ProfileWave *wave = &g_waves[wave_index];
        g_wave_worker_count = 0;
        for (uint32_t index = 0; index < g_header->thread_count; ++index) {
            Worker *worker = &g_workers[index];
            bool active = false;
            for (uint32_t phase = wave->first_phase;
                 phase < wave->first_phase + wave->phase_count; ++phase) {
                const ThreadPhaseModel *model =
                    &g_models[worker->profile->model_first + phase];
                for (uint32_t function = 0; function < FUNCTION_COUNT; ++function) {
                    if (model->counts[function] != 0) {
                        active = true;
                        break;
                    }
                }
                if (active) break;
            }
            worker->run_first_phase = wave->first_phase;
            worker->run_phase_count = active ? wave->phase_count : 0;
            if (active) ++g_wave_worker_count;
        }
        if (g_wave_worker_count == 0) {
            fail("profile wave has no active workers");
            break;
        }
        if (g_wave_worker_count > maximum_wave_threads) {
            maximum_wave_threads = g_wave_worker_count;
        }
        worker_launches += g_wave_worker_count;
        atomic_store_explicit(&g_ready, 0, memory_order_relaxed);
        atomic_store_explicit(&g_generated_done, 0, memory_order_relaxed);
        atomic_store_explicit(&g_start, false, memory_order_relaxed);
        for (uint32_t index = 0; index < g_header->thread_count; ++index) {
            Worker *worker = &g_workers[index];
            if (worker->run_phase_count == 0) continue;
            int error = pthread_create(&worker->thread, NULL, worker_main, worker);
            if (error != 0) {
                fprintf(stderr, "pthread_create for trace tid=%" PRIu64 ": %s\n",
                        worker->profile->trace_tid, strerror(error));
                return 1;
            }
        }
        while (atomic_load_explicit(&g_ready, memory_order_acquire) !=
               g_wave_worker_count) {
            sched_yield();
        }
        atomic_store_explicit(&g_start, true, memory_order_release);
        for (uint32_t index = 0; index < g_header->thread_count; ++index) {
            Worker *worker = &g_workers[index];
            if (worker->run_phase_count == 0) continue;
            int error = pthread_join(worker->thread, NULL);
            if (error != 0) {
                fprintf(stderr, "pthread_join for trace tid=%" PRIu64 ": %s\n",
                        worker->profile->trace_tid, strerror(error));
                return 1;
            }
        }
        if (atomic_load_explicit(&g_failed, memory_order_acquire)) break;
    }

    uint64_t functions[FUNCTION_COUNT] = {0};
    uint64_t remote_frees = 0;
    uint64_t deferred = 0;
    uint64_t underflows = 0;
    uint64_t allocation_failures = 0;
    for (uint32_t index = 0; index < g_header->thread_count; ++index) {
        Worker *worker = &g_workers[index];
        for (uint32_t function = 0; function < FUNCTION_COUNT; ++function) {
            functions[function] += worker->function_counts[function];
        }
        remote_frees += worker->remote_frees;
        deferred += worker->deferred_remote_frees;
        underflows += worker->local_free_underflows;
        allocation_failures += worker->allocation_failures;
    }
    uint64_t actual_ops = 0;
    for (uint32_t function = 0; function < FUNCTION_COUNT; ++function) {
        actual_ops += functions[function];
    }
    uint64_t last = atomic_load_explicit(&g_last_finish_ns, memory_order_relaxed);
    double elapsed_ms = (last - g_start_ns) / 1000000.0;
    bool failed = atomic_load_explicit(&g_failed, memory_order_acquire);
    if (g_json) {
        printf("{\"elapsed_ms\":%.6f,\"modeled_operations\":%" PRIu64
               ",\"actual_operations\":%" PRIu64 ",\"threads\":%u"
               ",\"phases\":%u,\"waves\":%u,\"max_wave_threads\":%u"
               ",\"worker_launches\":%" PRIu64
               ",\"phase_barriers\":0,\"lifecycle_joins\":%u"
               ",\"malloc\":%" PRIu64 ",\"free\":%" PRIu64
               ",\"calloc\":%" PRIu64 ",\"realloc\":%" PRIu64
               ",\"free_sized\":%" PRIu64
               ",\"posix_memalign\":%" PRIu64
               ",\"aligned_alloc\":%" PRIu64
               ",\"valloc\":%" PRIu64 ",\"memalign\":%" PRIu64
               ",\"pvalloc\":%" PRIu64
               ",\"remote_frees\":%" PRIu64
               ",\"deferred_remote_frees\":%" PRIu64
               ",\"local_free_underflows\":%" PRIu64
               ",\"allocation_failures\":%" PRIu64
               ",\"failed\":%s}\n",
               elapsed_ms, g_header->total_ops, actual_ops,
               g_header->thread_count, g_header->phase_count,
               g_header->wave_count, maximum_wave_threads, worker_launches,
               g_header->wave_count > 0 ? g_header->wave_count - 1 : 0,
               functions[0], functions[1], functions[2], functions[3],
               functions[4], functions[5], functions[6], functions[7],
               functions[8], functions[9], remote_frees, deferred,
               underflows, allocation_failures, failed ? "true" : "false");
    } else {
        printf("elapsed_ms=%.6f operations=%" PRIu64 "/%" PRIu64
               " historical_threads=%u phases=%u waves=%u max_wave_threads=%u"
               " worker_launches=%" PRIu64 " phase_barriers=0\n",
               elapsed_ms, actual_ops, g_header->total_ops,
               g_header->thread_count, g_header->phase_count,
               g_header->wave_count, maximum_wave_threads, worker_launches);
        printf("malloc=%" PRIu64 " free=%" PRIu64 " calloc=%" PRIu64
               " realloc=%" PRIu64 " free_sized=%" PRIu64
               " remote_frees=%" PRIu64 " deferred=%" PRIu64
               " local_underflows=%" PRIu64 " allocation_failures=%" PRIu64
               "\n", functions[0], functions[1], functions[2], functions[3],
               functions[4], remote_frees, deferred, underflows,
               allocation_failures);
    }
    return failed ? 1 : 0;
}
