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
        case MINI_REPLAY_CALLOC:
            return "CALLOC";
        case MINI_REPLAY_REALLOC:
            return "REALLOC";
        case MINI_REPLAY_FREE_SIZED:
            return "FREE_SIZED";
        case MINI_REPLAY_POSIX_MEMALIGN:
            return "POSIX_MEMALIGN";
        case MINI_REPLAY_ALIGNED_ALLOC:
            return "ALIGNED_ALLOC";
        case MINI_REPLAY_VALLOC:
            return "VALLOC";
        case MINI_REPLAY_MEMALIGN:
            return "MEMALIGN";
        case MINI_REPLAY_PVALLOC:
            return "PVALLOC";
        case MINI_REPLAY_THREAD_CREATE:
            return "THREAD_CREATE";
        case MINI_REPLAY_THREAD_START:
            return "THREAD_START";
        case MINI_REPLAY_THREAD_END:
            return "THREAD_END";
        case MINI_REPLAY_THREAD_JOIN:
            return "THREAD_JOIN";
        case MINI_REPLAY_THREAD_DETACH:
            return "THREAD_DETACH";
        default:
            return "UNKNOWN";
    }
}

static void report_invalid_header(const MiniReplayFileHeader *header)
{
    uint32_t init_state = atomic_load_explicit(
        &header->init_state, memory_order_relaxed);

    fprintf(
        stderr,
        "unsupported or incomplete replay format\n"
        "actual:   magic=\"%.*s\" version=%u header_size=%u "
        "event_size=%u pointer_size=%u endian=%u init_state=%" PRIu32
        "\n"
        "expected: magic=\"%s\" version=%u header_size=%zu "
        "event_size=%zu pointer_size=%zu endian=%u init_state=%u\n",
        (int)sizeof(header->magic), (const char *)header->magic,
        header->version, header->header_size, header->event_size,
        header->pointer_size, header->endian, init_state,
        MINI_REPLAY_MAGIC, MINI_REPLAY_VERSION,
        sizeof(MiniReplayFileHeader), sizeof(MiniReplayEvent),
        sizeof(void *), MINI_REPLAY_ENDIAN_LITTLE,
        MINI_REPLAY_INIT_READY);

    if (memcmp(header->magic, "MNRPLY02", sizeof(header->magic)) == 0 ||
        header->version == 2) {
        fprintf(
            stderr,
            "diagnosis: this is a v2 BIN, but this dump tool expects v3; "
            "rebuild and redeploy the v3 hook, then collect a new BIN\n");
    } else if (init_state != MINI_REPLAY_INIT_READY) {
        fprintf(
            stderr,
            "diagnosis: the BIN header was not fully initialized "
            "(ready state is %u)\n",
            MINI_REPLAY_INIT_READY);
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
        report_invalid_header(&header);
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

        uint32_t sequence = atomic_load_explicit(
            &event.sequence, memory_order_relaxed);
        if (sequence != (uint32_t)(index + 1)) {
            fprintf(
                output,
                "event=%" PRIu64 " type=INCOMPLETE expected_seq=%" PRIu64
                " stored_seq=%" PRIu32 "\n",
                index, index + 1, sequence);
            ++incomplete;
            continue;
        }

        fprintf(
            output,
            "event=%" PRIu64 " type=%s pid=%" PRIu32
            " tid=%" PRIu32 " seq=%" PRIu32
            " time_ns=%" PRIu64 " address=0x%" PRIx64
            " result=0x%" PRIx64 " size=%" PRIu64
            " flags=0x%x\n",
            index, event_name(event.type), event.pid, event.tid,
            sequence, event.timestamp_ns, event.address, event.result,
            event.size, event.flags);
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
