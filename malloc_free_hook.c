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
#include <time.h>
#include <unistd.h>

#define DEFAULT_OUTPUT_DIR                                                    \
    "/data/app/el2/100/base/com.ss.hm.ugc.aweme/haps/entry/files"
#define OUTPUT_DIR_CAPACITY 256
#define OUTPUT_PATH_CAPACITY 320
#define OUTPUT_BUFFER_CAPACITY 128

typedef void *(*malloc_fn)(size_t);
typedef void (*free_fn)(void *);

static malloc_fn real_malloc;
static free_fn real_free;

static _Atomic uint64_t malloc_count;
static _Atomic uint64_t free_count;
static _Atomic uint64_t writer_pid;
static atomic_bool writer_started;
static char output_dir[OUTPUT_DIR_CAPACITY];
static _Thread_local bool starting_writer;

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

static void copy_output_dir(const char *source)
{
    size_t i = 0;

    while (source[i] != '\0' && i + 1 < sizeof(output_dir)) {
        output_dir[i] = source[i];
        ++i;
    }
    output_dir[i] = '\0';
}

static void write_all(int fd, const char *buffer, size_t size)
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
        return;
    }
}

static void write_snapshot(pid_t pid)
{
    char path[OUTPUT_PATH_CAPACITY];
    char buffer[OUTPUT_BUFFER_CAPACITY];
    size_t offset = 0;
    size_t path_offset = append_text(path, 0, output_dir);

    if (path_offset != 0 && path[path_offset - 1] != '/') {
        path[path_offset++] = '/';
    }
    path_offset = append_u64(path, path_offset, (uint64_t)pid);
    path[path_offset] = '\0';

    offset = append_text(buffer, offset, "malloc=");
    offset = append_u64(
        buffer, offset,
        atomic_load_explicit(&malloc_count, memory_order_relaxed));
    buffer[offset++] = '\n';
    offset = append_text(buffer, offset, "free=");
    offset = append_u64(
        buffer, offset,
        atomic_load_explicit(&free_count, memory_order_relaxed));
    buffer[offset++] = '\n';

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return;
    }
    write_all(fd, buffer, offset);
    close(fd);
}

static void *writer_main(void *argument)
{
    const pid_t pid = (pid_t)(uintptr_t)argument;
    const struct timespec interval = {
        .tv_sec = 1,
        .tv_nsec = 0,
    };

    for (;;) {
        write_snapshot(pid);
        nanosleep(&interval, NULL);
    }

    return NULL;
}

static void start_writer(void)
{
    pthread_t thread;
    bool expected = false;
    const uint64_t current_pid = (uint64_t)getpid();
    uint64_t recorded_pid =
        atomic_load_explicit(&writer_pid, memory_order_acquire);

    if (recorded_pid != current_pid) {
        if (!atomic_compare_exchange_strong_explicit(
                &writer_pid, &recorded_pid, current_pid,
                memory_order_acq_rel, memory_order_acquire)) {
            return;
        }

        atomic_store_explicit(&malloc_count, 0, memory_order_relaxed);
        atomic_store_explicit(&free_count, 0, memory_order_relaxed);
        atomic_store_explicit(
            &writer_started, false, memory_order_release);
        write_snapshot((pid_t)current_pid);
    }

    if (starting_writer ||
        !atomic_compare_exchange_strong_explicit(
            &writer_started, &expected, true,
            memory_order_acq_rel, memory_order_relaxed)) {
        return;
    }

    starting_writer = true;
    int result = pthread_create(
        &thread, NULL, writer_main, (void *)(uintptr_t)current_pid);
    if (result == 0) {
        pthread_detach(thread);
    } else {
        atomic_store_explicit(
            &writer_started, false, memory_order_release);
    }
    starting_writer = false;
}

__attribute__((constructor)) static void initialize_hook(void)
{
    real_malloc = (malloc_fn)dlsym(RTLD_NEXT, "malloc");
    real_free = (free_fn)dlsym(RTLD_NEXT, "free");

    const char *configured_dir = getenv("MINI_HOOK_OUTPUT_DIR");
    copy_output_dir(
        configured_dir != NULL ? configured_dir : DEFAULT_OUTPUT_DIR);

    start_writer();
}

__attribute__((visibility("default"))) void *malloc(size_t size)
{
    atomic_fetch_add_explicit(&malloc_count, 1, memory_order_relaxed);
    void *result = real_malloc(size);
    start_writer();
    return result;
}

__attribute__((visibility("default"))) void free(void *ptr)
{
    atomic_fetch_add_explicit(&free_count, 1, memory_order_relaxed);
    real_free(ptr);
    start_writer();
}
