#ifndef MINI_RPLY_FORMAT_H
#define MINI_RPLY_FORMAT_H

#include <stdint.h>

#define MINI_RPLY_ENTRY_WORDS 6u
#define MINI_RPLY_FUNCTION_COUNT 10u

#define MINI_RPLY_THREAD_CREATE 100u
#define MINI_RPLY_THREAD_START 101u
#define MINI_RPLY_THREAD_END 102u
#define MINI_RPLY_THREAD_JOIN 103u
#define MINI_RPLY_THREAD_DETACH 104u

typedef struct {
    uint64_t timestamp;
    uint64_t func_and_cpu;
    uint64_t thread_id;
    uint64_t address;
    uint64_t result;
    uint64_t size;
} MiniRplyEntry;

_Static_assert(
    sizeof(MiniRplyEntry) == 48,
    "MiniRplyEntry layout changed");

#endif
