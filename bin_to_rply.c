#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "replay_format.h"
#include "rply_format.h"

static bool parse_pid(const char *text, uint32_t *pid)
{
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value == 0 || value > UINT32_MAX) {
        return false;
    }
    *pid = (uint32_t)value;
    return true;
}

static bool header_is_valid(const MiniReplayFileHeader *header)
{
    return memcmp(
               header->magic, MINI_REPLAY_MAGIC,
               sizeof(header->magic)) == 0 &&
           header->version == MINI_REPLAY_VERSION &&
           header->header_size == sizeof(MiniReplayFileHeader) &&
           header->event_size == sizeof(MiniReplayEvent) &&
           header->pointer_size == sizeof(uint64_t) &&
           header->endian == MINI_REPLAY_ENDIAN_LITTLE &&
           atomic_load_explicit(
               &header->init_state, memory_order_relaxed) ==
               MINI_REPLAY_INIT_READY;
}

static bool convert_event(
    const MiniReplayEvent *event, MiniRplyEntry *entry)
{
    if (event->type >= MINI_RPLY_FUNCTION_COUNT &&
        (event->type < MINI_RPLY_THREAD_CREATE ||
         event->type > MINI_RPLY_THREAD_DETACH)) {
        return false;
    }

    entry->timestamp = event->timestamp_ns;
    entry->func_and_cpu = (uint64_t)event->type << 32;
    entry->thread_id = event->tid;
    entry->address = event->address;
    entry->result = event->result;
    entry->size = event->size;
    return true;
}

static int fail_conversion(
    FILE *input, FILE *output, const char *output_path,
    const char *message)
{
    fprintf(stderr, "%s\n", message);
    if (output != NULL) {
        fclose(output);
    }
    if (input != NULL) {
        fclose(input);
    }
    if (output_path != NULL) {
        remove(output_path);
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(
            stderr,
            "usage: %s <mini_replay.bin> <pid|auto> <output.rply>\n",
            argv[0]);
        return 2;
    }
    if (strcmp(argv[1], argv[3]) == 0) {
        fprintf(stderr, "input BIN and output RPLY must differ\n");
        return 2;
    }

    bool auto_pid = strcmp(argv[2], "auto") == 0;
    uint32_t target_pid = 0;
    if (!auto_pid && !parse_pid(argv[2], &target_pid)) {
        fprintf(stderr, "invalid pid: %s\n", argv[2]);
        return 2;
    }

    FILE *input = fopen(argv[1], "rb");
    if (input == NULL) {
        fprintf(stderr, "open %s: %s\n", argv[1], strerror(errno));
        return 1;
    }

    MiniReplayFileHeader header;
    if (fread(&header, sizeof(header), 1, input) != 1) {
        return fail_conversion(
            input, NULL, NULL, "missing or incomplete BIN header");
    }
    if (!header_is_valid(&header)) {
        return fail_conversion(
            input, NULL, NULL, "unsupported or incomplete BIN format");
    }

    uint64_t next_index = atomic_load_explicit(
        &header.next_index, memory_order_relaxed);
    uint32_t runtime_flags = atomic_load_explicit(
        &header.runtime_flags, memory_order_relaxed);
    if ((runtime_flags & MINI_REPLAY_FLAG_OVERFLOW) != 0) {
        return fail_conversion(
            input, NULL, NULL,
            "BIN overflow flag is set; refusing a truncated conversion");
    }
    if (next_index > header.capacity || next_index > UINT32_MAX) {
        return fail_conversion(
            input, NULL, NULL, "BIN event count exceeds its capacity");
    }

    FILE *output = fopen(argv[3], "wb+");
    if (output == NULL) {
        fprintf(stderr, "open %s: %s\n", argv[3], strerror(errno));
        fclose(input);
        return 1;
    }

    uint64_t index_words = 0;
    if (fwrite(&index_words, sizeof(index_words), 1, output) != 1) {
        return fail_conversion(
            input, output, argv[3], "failed to write RPLY header");
    }

    uint64_t converted = 0;
    uint64_t skipped_pid = 0;
    uint64_t skipped_metadata = 0;
    uint64_t skipped_incomplete = 0;
    uint64_t skipped_unknown = 0;

    for (uint64_t index = 0; index < next_index; ++index) {
        MiniReplayEvent event;
        if (fread(&event, sizeof(event), 1, input) != 1) {
            char message[128];
            snprintf(
                message, sizeof(message),
                "BIN event %" PRIu64 " is truncated", index);
            return fail_conversion(
                input, output, argv[3], message);
        }

        uint32_t sequence = atomic_load_explicit(
            &event.sequence, memory_order_relaxed);
        if (sequence != (uint32_t)(index + 1)) {
            fprintf(
                stderr,
                "warning: BIN event %" PRIu64
                " is incomplete: expected sequence %" PRIu64
                ", stored %" PRIu32 "; skipping\n",
                index, index + 1, sequence);
            ++skipped_incomplete;
            continue;
        }

        if (auto_pid && target_pid == 0 &&
            event.type == MINI_REPLAY_PROCESS_START) {
            target_pid = event.pid;
        }

        if (target_pid == 0 || event.pid != target_pid) {
            ++skipped_pid;
            continue;
        }
        if (event.type == MINI_REPLAY_PROCESS_START) {
            ++skipped_metadata;
            continue;
        }

        MiniRplyEntry entry;
        if (!convert_event(&event, &entry)) {
            fprintf(
                stderr,
                "warning: BIN event %" PRIu64
                " has unknown type %" PRIu16 "; skipping\n",
                index, event.type);
            ++skipped_unknown;
            continue;
        }
        if (fwrite(&entry, sizeof(entry), 1, output) != 1) {
            return fail_conversion(
                input, output, argv[3],
                "failed while writing RPLY entries");
        }
        ++converted;
    }

    if (converted == 0) {
        return fail_conversion(
            input, output, argv[3],
            target_pid == 0
                ? "no PROCESS_START event found for automatic pid selection"
                : "no valid replay events matched the requested pid");
    }
    if (converted > UINT64_MAX / MINI_RPLY_ENTRY_WORDS) {
        return fail_conversion(
            input, output, argv[3], "RPLY record count overflow");
    }
    index_words = converted * MINI_RPLY_ENTRY_WORDS;
    if (fseek(output, 0, SEEK_SET) != 0 ||
        fwrite(&index_words, sizeof(index_words), 1, output) != 1) {
        return fail_conversion(
            input, output, argv[3], "failed to finalize RPLY header");
    }

    if (fclose(output) != 0) {
        output = NULL;
        return fail_conversion(
            input, NULL, argv[3], "failed to close RPLY output");
    }
    output = NULL;
    if (fclose(input) != 0) {
        return fail_conversion(
            NULL, NULL, argv[3], "failed to close BIN input");
    }

    printf(
        "pid=%" PRIu32 " records=%" PRIu64
        " idx_words=%" PRIu64 " skipped_pid=%" PRIu64
        " skipped_metadata=%" PRIu64
        " skipped_incomplete=%" PRIu64
        " skipped_unknown=%" PRIu64 "\n",
        target_pid, converted, index_words, skipped_pid,
        skipped_metadata, skipped_incomplete, skipped_unknown);
    return 0;
}
