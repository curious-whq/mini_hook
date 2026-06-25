#ifndef MINI_REPLAY_FORMAT_H
#define MINI_REPLAY_FORMAT_H

#include <stdatomic.h>
#include <stdint.h>

#define MINI_REPLAY_MAGIC "MNRPLY02"
#define MINI_REPLAY_VERSION 2
#define MINI_REPLAY_ENDIAN_LITTLE 1

#define MINI_REPLAY_INIT_EMPTY 0u
#define MINI_REPLAY_INIT_BUSY 1u
#define MINI_REPLAY_INIT_READY 2u

#define MINI_REPLAY_FLAG_OVERFLOW 1u

enum MiniReplayEventType {
    MINI_REPLAY_PROCESS_START = 1,
    MINI_REPLAY_MALLOC = 2,
    MINI_REPLAY_FREE = 3,
};

typedef struct {
    uint8_t magic[8];
    uint64_t capacity;
    _Atomic uint64_t next_index;
    uint16_t version;
    uint16_t header_size;
    uint16_t event_size;
    uint8_t pointer_size;
    uint8_t endian;
    uint32_t flags;
    _Atomic uint32_t init_state;
    _Atomic uint32_t runtime_flags;
    uint32_t reserved0;
    uint64_t reserved1[2];
} MiniReplayFileHeader;

typedef struct {
    _Atomic uint64_t sequence;
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
    sizeof(MiniReplayFileHeader) == 64,
    "MiniReplayFileHeader layout changed");
_Static_assert(
    sizeof(MiniReplayEvent) == 48,
    "MiniReplayEvent layout changed");

#endif
