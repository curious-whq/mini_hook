#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rply_format.h"

static const char *function_name(uint32_t function)
{
    static const char *names[MINI_RPLY_FUNCTION_COUNT] = {
        "malloc",
        "free",
        "calloc",
        "realloc",
        "free_sized",
        "posix_memalign",
        "aligned_alloc",
        "valloc",
        "memalign",
        "pvalloc",
    };

    if (function < MINI_RPLY_FUNCTION_COUNT) {
        return names[function];
    }
    switch (function) {
        case MINI_RPLY_THREAD_CREATE:
            return "thread_create";
        case MINI_RPLY_THREAD_START:
            return "thread_start";
        case MINI_RPLY_THREAD_END:
            return "thread_end";
        case MINI_RPLY_THREAD_JOIN:
            return "thread_join";
        case MINI_RPLY_THREAD_DETACH:
            return "thread_detach";
        default:
            return "unknown";
    }
}

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3) {
        fprintf(
            stderr, "usage: %s <trace.rply> [output.txt]\n",
            argv[0]);
        return 2;
    }
    if (argc == 3 && strcmp(argv[1], argv[2]) == 0) {
        fprintf(stderr, "input RPLY and output TXT must differ\n");
        return 2;
    }

    FILE *input = fopen(argv[1], "rb");
    if (input == NULL) {
        fprintf(stderr, "open %s: %s\n", argv[1], strerror(errno));
        return 1;
    }

    FILE *output = stdout;
    if (argc == 3) {
        output = fopen(argv[2], "w");
        if (output == NULL) {
            fprintf(stderr, "open %s: %s\n", argv[2], strerror(errno));
            fclose(input);
            return 1;
        }
    }

    uint64_t index_words;
    if (fread(&index_words, sizeof(index_words), 1, input) != 1) {
        fprintf(stderr, "missing or incomplete RPLY header\n");
        if (output != stdout) {
            fclose(output);
        }
        fclose(input);
        return 1;
    }
    if (index_words % MINI_RPLY_ENTRY_WORDS != 0) {
        fprintf(
            stderr,
            "invalid RPLY idx: %" PRIu64
            " is not divisible by %u\n",
            index_words, MINI_RPLY_ENTRY_WORDS);
        if (output != stdout) {
            fclose(output);
        }
        fclose(input);
        return 1;
    }

    uint64_t record_count =
        index_words / MINI_RPLY_ENTRY_WORDS;
    fprintf(
        output,
        "idx_words=%" PRIu64 " records=%" PRIu64
        " entry_size=%zu\n",
        index_words, record_count, sizeof(MiniRplyEntry));

    uint64_t counts[MINI_RPLY_FUNCTION_COUNT] = {0};
    uint64_t thread_events = 0;
    uint64_t unknown = 0;
    for (uint64_t index = 0; index < record_count; ++index) {
        MiniRplyEntry entry;
        if (fread(&entry, sizeof(entry), 1, input) != 1) {
            fprintf(
                stderr, "RPLY record %" PRIu64 " is truncated\n",
                index);
            if (output != stdout) {
                fclose(output);
            }
            fclose(input);
            return 1;
        }

        uint32_t function = (uint32_t)(entry.func_and_cpu >> 32);
        uint32_t cpu = (uint32_t)entry.func_and_cpu;
        if (function < MINI_RPLY_FUNCTION_COUNT) {
            ++counts[function];
        } else if (
            function >= MINI_RPLY_THREAD_CREATE &&
            function <= MINI_RPLY_THREAD_DETACH) {
            ++thread_events;
        } else {
            ++unknown;
        }

        fprintf(
            output,
            "record=%" PRIu64 " function=%s function_id=%" PRIu32
            " cpu=%" PRIu32 " thread_id=%" PRIu64
            " time_ns=%" PRIu64 " address=0x%" PRIx64
            " result=0x%" PRIx64 " size=%" PRIu64 "\n",
            index, function_name(function), function, cpu,
            entry.thread_id, entry.timestamp, entry.address,
            entry.result, entry.size);
    }

    fprintf(output, "summary");
    for (uint32_t function = 0;
         function < MINI_RPLY_FUNCTION_COUNT; ++function) {
        fprintf(
            output, " %s=%" PRIu64,
            function_name(function), counts[function]);
    }
    fprintf(
        output, " thread_events=%" PRIu64
        " unknown=%" PRIu64 "\n",
        thread_events, unknown);

    if (output != stdout && fclose(output) != 0) {
        fprintf(stderr, "close %s: %s\n", argv[2], strerror(errno));
        fclose(input);
        return 1;
    }
    if (fclose(input) != 0) {
        fprintf(stderr, "close %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    return 0;
}
