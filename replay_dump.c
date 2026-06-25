#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "replay_format.h"

static const char *event_name(uint16_t type)
{
    switch (type) {
        case MINI_REPLAY_PROCESS_START:
            return "PROCESS_START";
        case MINI_REPLAY_MALLOC:
            return "MALLOC";
        case MINI_REPLAY_FREE:
            return "FREE";
        default:
            return "UNKNOWN";
    }
}

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3) {
        fprintf(
            stderr,
            "usage: %s <mini_replay.bin> [replay.txt]\n",
            argv[0]);
        return 2;
    }

    FILE *file = fopen(argv[1], "rb");
    if (file == NULL) {
        fprintf(stderr, "open %s: %s\n", argv[1], strerror(errno));
        return 1;
    }

    FILE *output = stdout;
    if (argc == 3) {
        output = fopen(argv[2], "w");
        if (output == NULL) {
            fprintf(stderr, "open %s: %s\n", argv[2], strerror(errno));
            fclose(file);
            return 1;
        }
    }

    MiniReplayFileHeader header;
    if (fread(&header, sizeof(header), 1, file) != 1) {
        fprintf(stderr, "missing or incomplete replay header\n");
        if (output != stdout) {
            fclose(output);
        }
        fclose(file);
        return 1;
    }

    if (memcmp(header.magic, MINI_REPLAY_MAGIC, sizeof(header.magic)) != 0 ||
        header.version != MINI_REPLAY_VERSION ||
        header.header_size != sizeof(MiniReplayFileHeader) ||
        header.event_size != sizeof(MiniReplayEvent) ||
        atomic_load_explicit(
            &header.init_state, memory_order_relaxed) !=
            MINI_REPLAY_INIT_READY) {
        fprintf(stderr, "unsupported or incomplete replay format\n");
        if (output != stdout) {
            fclose(output);
        }
        fclose(file);
        return 1;
    }

    uint64_t next_index = atomic_load_explicit(
        &header.next_index, memory_order_relaxed);
    uint32_t runtime_flags = atomic_load_explicit(
        &header.runtime_flags, memory_order_relaxed);
    uint64_t readable_events =
        next_index < header.capacity ? next_index : header.capacity;

    fprintf(
        output,
        "version=%u header_size=%u event_size=%u pointer_size=%u "
        "endian=%u capacity=%" PRIu64 " next_index=%" PRIu64
        " runtime_flags=0x%x\n",
        header.version, header.header_size, header.event_size,
        header.pointer_size, header.endian, header.capacity,
        next_index, runtime_flags);

    uint64_t committed = 0;
    uint64_t incomplete = 0;
    for (uint64_t index = 0; index < readable_events; ++index) {
        MiniReplayEvent event;
        if (fread(&event, sizeof(event), 1, file) != 1) {
            fprintf(stderr, "event %" PRIu64 " is truncated\n", index);
            if (output != stdout) {
                fclose(output);
            }
            fclose(file);
            return 1;
        }

        uint64_t sequence = atomic_load_explicit(
            &event.sequence, memory_order_relaxed);
        if (sequence != index + 1) {
            fprintf(
                output,
                "event=%" PRIu64 " type=INCOMPLETE expected_seq=%" PRIu64
                " stored_seq=%" PRIu64 "\n",
                index, index + 1, sequence);
            ++incomplete;
            continue;
        }

        fprintf(
            output,
            "event=%" PRIu64 " type=%s pid=%" PRIu32
            " tid=%" PRIu32 " seq=%" PRIu64
            " time_ns=%" PRIu64 " address=0x%" PRIx64
            " size=%" PRIu64 " flags=0x%x\n",
            index, event_name(event.type), event.pid, event.tid,
            sequence, event.timestamp_ns, event.address, event.size,
            event.flags);
        ++committed;
    }

    fprintf(
        output,
        "summary committed=%" PRIu64 " incomplete=%" PRIu64
        " overflow=%s\n",
        committed, incomplete,
        (runtime_flags & MINI_REPLAY_FLAG_OVERFLOW) != 0 ? "yes" : "no");

    if (output != stdout && fclose(output) != 0) {
        fprintf(stderr, "close %s: %s\n", argv[2], strerror(errno));
        fclose(file);
        return 1;
    }
    fclose(file);
    return (runtime_flags & MINI_REPLAY_FLAG_OVERFLOW) != 0 ? 1 : 0;
}
