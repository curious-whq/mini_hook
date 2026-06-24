#ifndef MINI_REPLAY_FORMAT_H
#define MINI_REPLAY_FORMAT_H

#include <stdint.h>

#define MINI_REPLAY_MAGIC "MNRPLY01"
#define MINI_REPLAY_VERSION 1
#define MINI_REPLAY_ENDIAN_LITTLE 1

enum MiniReplayEventType {
    MINI_REPLAY_PROCESS_START = 1,
    MINI_REPLAY_MALLOC = 2,
    MINI_REPLAY_FREE = 3,
};

typedef struct {
    uint8_t magic[8];
    uint16_t version;
    uint16_t header_size;
    uint16_t event_size;
    uint8_t pointer_size;
    uint8_t endian;
    uint32_t flags;
    uint32_t reserved0;
    uint64_t reserved1;
} MiniReplayFileHeader;

typedef struct {
    uint64_t sequence;
    uint64_t timestamp_ns;
    uint64_t address;
    uint64_t size;
    uint32_t pid;
    uint32_t tid;
    uint16_t type;
    uint16_t flags;
    uint32_t reserved;
} MiniReplayEvent;

_Static_assert(
    sizeof(MiniReplayFileHeader) == 32,
    "MiniReplayFileHeader layout changed");
_Static_assert(
    sizeof(MiniReplayEvent) == 48,
    "MiniReplayEvent layout changed");

#endif
