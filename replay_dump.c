#include <errno.h>
#include <inttypes.h>
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
    if (argc != 2) {
        fprintf(stderr, "usage: %s <mini_replay.bin>\n", argv[0]);
        return 2;
    }

    FILE *file = fopen(argv[1], "rb");
    if (file == NULL) {
        fprintf(stderr, "open %s: %s\n", argv[1], strerror(errno));
        return 1;
    }

    MiniReplayFileHeader header;
    if (fread(&header, sizeof(header), 1, file) != 1) {
        fprintf(stderr, "missing or incomplete replay header\n");
        fclose(file);
        return 1;
    }

    if (memcmp(header.magic, MINI_REPLAY_MAGIC, sizeof(header.magic)) != 0 ||
        header.version != MINI_REPLAY_VERSION ||
        header.header_size != sizeof(MiniReplayFileHeader) ||
        header.event_size != sizeof(MiniReplayEvent)) {
        fprintf(stderr, "unsupported replay format\n");
        fclose(file);
        return 1;
    }

    printf(
        "version=%u header_size=%u event_size=%u pointer_size=%u endian=%u\n",
        header.version, header.header_size, header.event_size,
        header.pointer_size, header.endian);

    MiniReplayEvent event;
    uint64_t index = 0;
    while (fread(&event, sizeof(event), 1, file) == 1) {
        printf(
            "event=%" PRIu64 " type=%s pid=%" PRIu32
            " tid=%" PRIu32 " seq=%" PRIu64
            " time_ns=%" PRIu64 " address=0x%" PRIx64
            " size=%" PRIu64 "\n",
            index, event_name(event.type), event.pid, event.tid,
            event.sequence, event.timestamp_ns, event.address, event.size);
        ++index;
    }

    if (!feof(file)) {
        fprintf(stderr, "failed while reading replay events\n");
        fclose(file);
        return 1;
    }

    long final_offset = ftell(file);
    if (final_offset < 0 ||
        ((uint64_t)final_offset - sizeof(MiniReplayFileHeader)) %
                sizeof(MiniReplayEvent) !=
            0) {
        fprintf(stderr, "trailing partial replay event\n");
        fclose(file);
        return 1;
    }

    fclose(file);
    return 0;
}
