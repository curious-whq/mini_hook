/* replay.c -- application-like allocator trace replay with thread lifetimes
 *
 * Unified model:
 *   1. One thread instance in the trace becomes one real replay pthread.
 *   2. THREAD_START/THREAD_END events define application-like lifetimes.
 *      Threads are created dynamically as the replay clock reaches their
 *      start timestamp, and they leave only after their end timestamp.
 *   3. Matched free is never dropped: free waits for the allocation/realloc
 *      generation it depends on.
 *   4. Real realloc is supported with per-slot generations, so a free after a
 *      realloc cannot accidentally free the pre-realloc pointer.
 *   5. Unknown free is never passed to free(trace_addr). Default policy is skip
 *      and count; optional synthetic mode frees preallocated synthetic objects.
 *   6. A coarse epoch clock is always used. This is the single application-like
 *      mode; there is no benchmark-specific mode selection.
 *
 * Expected trace layout is compatible with the previous hook format:
 *   [ atomic idx in 8 bytes ][ REPLAY_ENTRY array ... ]
 * where idx is measured in uint64_t words.
 *
 * Build:
 *   gcc -O3 -DNDEBUG -std=gnu11 -pthread replay.c -o replay
 *
 * Examples:
 *   ./replay -S trace.rply
 *   ./replay -S --epoch-size 1000000 trace.rply
 *   ./replay -j --progress --latency-sample-rate 1000 trace.rply
 *   ./replay -S --unknown-policy synthetic --synthetic-size 64 trace.rply
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define NUM_FUNCS 10
#define MAX_THREADS 65536
#define TIDCAP (1u << 20)
#define ENTRY_WORDS (sizeof(REPLAY_ENTRY) / sizeof(uint64_t))
#define DEFAULT_MAP_LIVE (4u << 20)
#define DEFAULT_LAT_SAMPLES 1000000u

#if defined(__x86_64__) || defined(__i386__)
#define CPU_RELAX() __builtin_ia32_pause()
#else
#define CPU_RELAX() __asm__ __volatile__("" ::: "memory")
#endif

/* Function IDs must match the hook. */
enum FuncIndex {
  MALLOC_I,
  FREE_I,
  CALLOC_I,
  REALLOC_I,
  FREE_SIZED_I,
  POSIX_MEMALIGN_I,
  ALIGNED_ALLOC_I,
  VALLOC_I,
  MEMALIGN_I,
  PVALLOC_I,

  /* Thread lifecycle events emitted by the new hook into the same .rply. */
  REPLAY_THREAD_CREATE_I = 100,
  REPLAY_THREAD_START_I  = 101,
  REPLAY_THREAD_END_I    = 102,
  REPLAY_THREAD_JOIN_I   = 103,
  REPLAY_THREAD_DETACH_I = 104
};

static const char *func_names[NUM_FUNCS] = {
    "malloc", "free", "calloc", "realloc", "free_sized",
    "posix_memalign", "aligned_alloc", "valloc", "memalign", "pvalloc"};

typedef struct replay_entry {
  uint64_t timestamp;
  uint64_t func_and_cpu;
  uint64_t thread_id;
  uint64_t address; /* old pointer for free/realloc */
  uint64_t result;  /* returned pointer for alloc/realloc */
  uint64_t size;    /* total bytes, as recorded by the hook */
} REPLAY_ENTRY;

typedef struct hook_log {
  atomic_uint_fast64_t idx;
  uint64_t log[1];
} HOOK_LOG;

/* ---------------- mmap helpers: keep harness metadata away from allocator ---- */
static void *xmap(size_t n) {
  if (n == 0) n = 1;
  void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                 -1, 0);
  if (p == MAP_FAILED) {
    perror("mmap");
    exit(1);
  }
  return p;
}

static void xunmap(void *p, size_t n) {
  if (p && n) munmap(p, n);
}

static inline uint64_t mix64(uint64_t k) {
  k ^= k >> 33;
  k *= 0xff51afd7ed558ccdULL;
  k ^= k >> 33;
  k *= 0xc4ceb9fe1a85ec53ULL;
  k ^= k >> 33;
  return k;
}

static inline uint64_t max_u64(uint64_t a, uint64_t b) { return a > b ? a : b; }

static inline uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static inline uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

static long read_vmhwm_kb(void) {
  FILE *f = fopen("/proc/self/status", "r");
  if (!f) return -1;
  char line[256];
  long kb = -1;
  while (fgets(line, sizeof line, f)) {
    if (sscanf(line, "VmHWM: %ld kB", &kb) == 1) break;
  }
  fclose(f);
  return kb;
}

static uint64_t parse_u64(const char *s, const char *name) {
  errno = 0;
  char *end = NULL;
  unsigned long long v = strtoull(s, &end, 0);
  if (errno || !end || *end != '\0') {
    fprintf(stderr, "invalid %s: %s\n", name, s);
    exit(2);
  }
  return (uint64_t)v;
}

static size_t round_up(size_t x, size_t a) {
  if (a == 0) return x;
  return (x + a - 1) & ~(a - 1);
}

/* ---------------- operation stream ----------------------------------------- */
enum OpKind {
  OP_ALLOC = 0,
  OP_FREE = 1,
  OP_REALLOC = 2,
  OP_REALLOC_NULL = 3,
  OP_UNKNOWN_FREE = 4
};

enum WorkerState {
  WORKER_NOT_STARTED = 0,
  WORKER_EPOCH_WAIT = 1,
  WORKER_EXECUTING = 2,
  WORKER_SLOT_WAIT = 3,
  WORKER_FINISHED = 4
};

typedef struct op {
  uint64_t ts;
  uint64_t size;
  uintptr_t aux;
  uint32_t slot;
  uint32_t wait_gen;
  uint32_t out_gen;
  uint16_t func;
  uint16_t kind;
  uint32_t access_desc_idx;
} op_t;

typedef struct opvec {
  op_t *d;
  size_t len, cap, cap_bytes;
} opvec_t;

static void opvec_push(opvec_t *v, op_t o) {
  if (v->len == v->cap) {
    size_t ncap = v->cap ? v->cap * 2 : 1024;
    size_t nb = ncap * sizeof(op_t);
    op_t *nd = (op_t *)xmap(nb);
    if (v->len) memcpy(nd, v->d, v->len * sizeof(op_t));
    if (v->d) xunmap(v->d, v->cap_bytes);
    v->d = nd;
    v->cap = ncap;
    v->cap_bytes = nb;
  }
  v->d[v->len++] = o;
}

/* ---------------- slot state with generations ------------------------------ */
typedef struct slot_state {
  atomic_uint_fast64_t gen;
  atomic_uintptr_t ptr;
} slot_state_t;

static slot_state_t *g_slots = NULL;
static uint32_t g_total_slots = 0;

/* ---------------- latency sampling ----------------------------------------- */
typedef struct func_latency {
  uint64_t count;
  uint64_t total_ns;
  uint64_t min_ns;
  uint64_t max_ns;
  uint64_t *samples;
  uint64_t sample_count;
  uint64_t sample_cap;
} func_latency_t;

static void lat_init(func_latency_t *l, uint64_t cap) {
  memset(l, 0, sizeof(*l));
  l->sample_cap = cap;
  if (cap) l->samples = (uint64_t *)xmap(cap * sizeof(uint64_t));
}

static inline void lat_record(func_latency_t *l, uint64_t ns) {
  l->count++;
  l->total_ns += ns;
  if (l->min_ns == 0 || ns < l->min_ns) l->min_ns = ns;
  if (ns > l->max_ns) l->max_ns = ns;
  if (l->samples && l->sample_count < l->sample_cap) {
    l->samples[l->sample_count++] = ns;
  }
}

static int cmp_u64(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a;
  uint64_t y = *(const uint64_t *)b;
  return (x > y) - (x < y);
}

static uint64_t pct(uint64_t *a, uint64_t n, double p) {
  if (!n) return 0;
  uint64_t idx = (uint64_t)((p / 100.0) * (double)(n - 1));
  return a[idx];
}

/* ---------------- workers -------------------------------------------------- */
typedef struct worker {
  pthread_t th;
  int idx;
  uint64_t tid_trace;

  /* Application-like thread lifetime.  If the trace has no lifecycle events,
   * these are inferred from the first/last allocator op for backward
   * compatibility with old traces.
   */
  uint64_t start_ts;
  uint64_t end_ts;
  uint64_t first_op_ts;
  uint64_t last_op_ts;
  int has_start;
  int has_end;
  int launched;
  int joined;
  int recorded_cpu;
  atomic_int finished;
  atomic_int state;
  atomic_uint_fast64_t current_op;
  atomic_uint_fast64_t current_size;
  atomic_uint current_slot;
  atomic_uint current_wait_gen;
  atomic_uint current_kind;

  opvec_t ops;
  func_latency_t lat[NUM_FUNCS];
  uint64_t executed;
  uint64_t waits;
  uint64_t wait_yields;
  uint64_t unknown_executed;
  uint64_t failed_synth_allocs;
} worker_t;

static worker_t *g_ws = NULL;
static int g_nthreads = 0;
static int64_t *g_tk = NULL;
static int *g_tv = NULL;

static int worker_register(uint64_t tid) {
  uint64_t h = mix64(tid) & (TIDCAP - 1);
  while (g_tk[h] != -1 && (uint64_t)g_tk[h] != tid + 1) {
    h = (h + 1) & (TIDCAP - 1);
  }
  if (g_tk[h] == -1) {
    if (g_nthreads >= MAX_THREADS) {
      fprintf(stderr, "too many trace tids, max=%d\n", MAX_THREADS);
      exit(1);
    }
    int wi = g_nthreads++;
    g_ws[wi].idx = wi;
    g_ws[wi].tid_trace = tid;
    g_ws[wi].start_ts = UINT64_MAX;
    g_ws[wi].end_ts = 0;
    g_ws[wi].first_op_ts = UINT64_MAX;
    g_ws[wi].last_op_ts = 0;
    atomic_init(&g_ws[wi].finished, 0);
    g_ws[wi].recorded_cpu = -1;
    g_tk[h] = (int64_t)(tid + 1);
    g_tv[h] = wi;
    return wi;
  }
  return g_tv[h];
}


static inline void worker_note_op(int wi, uint64_t ts) {
  if (ts < g_ws[wi].first_op_ts) g_ws[wi].first_op_ts = ts;
  if (ts > g_ws[wi].last_op_ts) g_ws[wi].last_op_ts = ts;
}

/* ---------------- live object map: trace_addr -> slot/gen/owner/size/last_ts */
typedef struct omap {
  uint64_t *keys; /* 0 = empty */
  uint32_t *slot;
  uint32_t *gen;
  uint32_t *owner;
  uint64_t *size;
  uint64_t *last_ts;
  size_t cap, mask, count;
  size_t bk, b32, b64;
} omap_t;

static void omap_alloc(omap_t *m, size_t cap) {
  if (cap < 1024) cap = 1024;
  m->cap = cap;
  m->mask = cap - 1;
  m->count = 0;
  m->bk = cap * sizeof(uint64_t);
  m->b32 = cap * sizeof(uint32_t);
  m->b64 = cap * sizeof(uint64_t);
  m->keys = (uint64_t *)xmap(m->bk);
  m->slot = (uint32_t *)xmap(m->b32);
  m->gen = (uint32_t *)xmap(m->b32);
  m->owner = (uint32_t *)xmap(m->b32);
  m->size = (uint64_t *)xmap(m->b64);
  m->last_ts = (uint64_t *)xmap(m->b64);
}

static void omap_free(omap_t *m) {
  xunmap(m->keys, m->bk);
  xunmap(m->slot, m->b32);
  xunmap(m->gen, m->b32);
  xunmap(m->owner, m->b32);
  xunmap(m->size, m->b64);
  xunmap(m->last_ts, m->b64);
  memset(m, 0, sizeof(*m));
}

static void omap_init(omap_t *m, size_t max_live) {
  size_t cap = 1;
  while (cap < max_live * 2) cap <<= 1;
  omap_alloc(m, cap);
}

static void omap_put_raw(omap_t *m, uint64_t a, uint32_t slot, uint32_t gen,
                         uint32_t owner, uint64_t size, uint64_t last_ts) {
  uint64_t h = mix64(a) & m->mask;
  while (m->keys[h] != 0) {
    if (m->keys[h] == a) {
      m->slot[h] = slot;
      m->gen[h] = gen;
      m->owner[h] = owner;
      m->size[h] = size;
      m->last_ts[h] = last_ts;
      return;
    }
    h = (h + 1) & m->mask;
  }
  m->keys[h] = a;
  m->slot[h] = slot;
  m->gen[h] = gen;
  m->owner[h] = owner;
  m->size[h] = size;
  m->last_ts[h] = last_ts;
  m->count++;
}

static void omap_resize(omap_t *m, size_t ncap) {
  omap_t nm;
  omap_alloc(&nm, ncap);
  for (size_t i = 0; i < m->cap; i++) {
    if (m->keys[i]) {
      omap_put_raw(&nm, m->keys[i], m->slot[i], m->gen[i], m->owner[i],
                   m->size[i], m->last_ts[i]);
    }
  }
  omap_free(m);
  *m = nm;
}

static void omap_put(omap_t *m, uint64_t a, uint32_t slot, uint32_t gen,
                     uint32_t owner, uint64_t size, uint64_t last_ts) {
  if ((m->count + 1) * 10 >= m->cap * 7) omap_resize(m, m->cap * 2);
  omap_put_raw(m, a, slot, gen, owner, size, last_ts);
}

static int omap_take(omap_t *m, uint64_t a, uint32_t *slot, uint32_t *gen,
                     uint32_t *owner, uint64_t *size, uint64_t *last_ts) {
  uint64_t h = mix64(a) & m->mask;
  while (m->keys[h] != 0) {
    if (m->keys[h] == a) {
      *slot = m->slot[h];
      *gen = m->gen[h];
      *owner = m->owner[h];
      *size = m->size[h];
      *last_ts = m->last_ts[h];
      m->keys[h] = 0;
      m->count--;
      uint64_t j = (h + 1) & m->mask;
      while (m->keys[j] != 0) {
        uint64_t kk = m->keys[j];
        uint32_t ss = m->slot[j], gg = m->gen[j], oo = m->owner[j];
        uint64_t zz = m->size[j], tt = m->last_ts[j];
        m->keys[j] = 0;
        m->count--;
        omap_put_raw(m, kk, ss, gg, oo, zz, tt);
        j = (j + 1) & m->mask;
      }
      return 1;
    }
    h = (h + 1) & m->mask;
  }
  return 0;
}

/* ---------------- global options and stats -------------------------------- */
enum UnknownPolicy { UNKNOWN_SKIP = 0, UNKNOWN_ERROR = 1, UNKNOWN_SYNTHETIC = 2 };
enum TouchMode { TOUCH_NONE = 0, TOUCH_ALLOC = 1, TOUCH_FREE = 2, TOUCH_BOTH = 3 };

#define ACCESS_DESC_NONE UINT32_MAX

#pragma pack(push, 1)
typedef struct block_access_desc {
    uint32_t slot;
    uint32_t _pad0;
    uint64_t size;
    uint64_t alloc_ts;
    uint64_t free_ts;
    uint16_t dirty_page_ratio_q12;
    uint16_t peak_dirty_ratio_q12;
    uint32_t memcpy_count;
    uint32_t memset_count;
    uint32_t total_bulk_bytes_lo;
    uint32_t _pad1;
    uint16_t hotspot_offsets[4];
    uint8_t  hotspot_weights[4];
    uint32_t estimate_load_count;
    uint32_t estimate_store_count;
    uint32_t _pad2;
} block_access_desc_t;
#pragma pack(pop)

#define ACCESS_PROFILE_MAGIC 0x4D414350U
#define ACCESS_PROFILE_VERSION 1

typedef struct access_profile_header {
    uint32_t magic;
    uint32_t version;
    uint32_t num_descriptors;
    uint32_t reserved;
} access_profile_header_t;

static enum UnknownPolicy g_unknown_policy = UNKNOWN_SKIP;
static enum TouchMode g_touch_mode = TOUCH_NONE;
static uint64_t g_synthetic_size = 64;
static uint64_t g_latency_sample_rate = 0;
static uint64_t g_latency_sample_cap = DEFAULT_LAT_SAMPLES;
static uint64_t g_spin_before_yield = 1024;
static uint64_t g_dependency_timeout_ms = 30000;
static uint64_t g_stall_timeout_ms = 30000;
static uint64_t g_epoch_size = 0; /* raw trace timestamp units; 0 = free-run */
static int g_show_progress = 0;
static int g_show_stats = 0;
static int g_json_output = 0;

static uint64_t g_total_ops = 0;
static uint64_t g_total_records = 0;
static uint64_t g_total_allocs = 0;
static uint64_t g_total_frees = 0;
static uint64_t g_total_reallocs = 0;
static uint64_t g_failed_allocs = 0;
static uint64_t g_failed_reallocs = 0;
static uint64_t g_unknown_frees = 0;
static uint64_t g_unknown_reallocs = 0;
static uint64_t g_remote_frees = 0;
static uint64_t g_local_frees = 0;
static uint64_t g_remote_reallocs = 0;
static uint64_t g_local_reallocs = 0;
static uint64_t g_live_left_at_end = 0;
static uint64_t g_min_ts = UINT64_MAX;
static uint64_t g_max_ts = 0;
static uint64_t g_num_epochs = 0;

static uint64_t g_thread_create_events = 0;
static uint64_t g_thread_start_events = 0;
static uint64_t g_thread_end_events = 0;
static uint64_t g_thread_join_events = 0;
static uint64_t g_lifecycle_inferred_threads = 0;

static atomic_uint_fast64_t g_completed = 0;
static atomic_int g_start = 0;
static atomic_int g_done = 0;

static block_access_desc_t *g_access_descs = NULL;
static uint32_t g_access_desc_count = 0;
static float g_access_fidelity = 0.1f;
static uint64_t g_access_sim_ops = 0;

static int g_free_run = 0;
static int g_cpu_affinity = 0;
static double g_timing_scale = 0.0;

/* Dynamic application-like epoch scheduler. */
static uint64_t g_auto_epoch_target = 1000;
static pthread_mutex_t g_epoch_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_epoch_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_epoch_id = 0;
static uint64_t g_epoch_end = 0;
static uint64_t g_epoch_target = 0;
static uint64_t g_epoch_arrived = 0;

static void create_thread_or_exit(
    pthread_t *thread, void *(*start_routine)(void *), void *arg,
    const char *role, uint64_t trace_tid) {
  int rc = pthread_create(thread, NULL, start_routine, arg);
  if (rc != 0) {
    fprintf(stderr,
            "pthread_create failed: role=%s trace_tid=%lu error=%s (%d)\n",
            role, trace_tid, strerror(rc), rc);
    exit(1);
  }
}

static void join_thread_or_exit(
    pthread_t thread, const char *role, uint64_t trace_tid) {
  int rc = pthread_join(thread, NULL);
  if (rc != 0) {
    fprintf(stderr,
            "pthread_join failed: role=%s trace_tid=%lu error=%s (%d)\n",
            role, trace_tid, strerror(rc), rc);
    exit(1);
  }
}

static const char *worker_state_name(int state) {
  switch (state) {
  case WORKER_NOT_STARTED: return "not_started";
  case WORKER_EPOCH_WAIT: return "epoch_wait";
  case WORKER_EXECUTING: return "executing";
  case WORKER_SLOT_WAIT: return "slot_wait";
  case WORKER_FINISHED: return "finished";
  default: return "unknown";
  }
}

static const char *op_kind_name(uint32_t kind) {
  switch (kind) {
  case OP_ALLOC: return "alloc";
  case OP_FREE: return "free";
  case OP_REALLOC: return "realloc";
  case OP_REALLOC_NULL: return "realloc_null";
  case OP_UNKNOWN_FREE: return "unknown_free";
  default: return "unknown";
  }
}

static void dump_stalled_workers(uint64_t completed) {
  int epoch_lock_rc = pthread_mutex_trylock(&g_epoch_mu);
  if (epoch_lock_rc == 0) {
    fprintf(stderr,
            "\nreplay stalled: no completed operation for %lu ms "
            "(completed=%lu/%lu epoch=%lu arrived=%lu target=%lu)\n",
            g_stall_timeout_ms, completed, g_total_ops, g_epoch_id,
            g_epoch_arrived, g_epoch_target);
    pthread_mutex_unlock(&g_epoch_mu);
  } else {
    fprintf(stderr,
            "\nreplay stalled: no completed operation for %lu ms "
            "(completed=%lu/%lu epoch_mutex=%s)\n",
            g_stall_timeout_ms, completed, g_total_ops,
            strerror(epoch_lock_rc));
  }
  for (int i = 0; i < g_nthreads; i++) {
    worker_t *w = &g_ws[i];
    if (!w->launched ||
        atomic_load_explicit(&w->finished, memory_order_acquire))
      continue;
    int state = atomic_load_explicit(&w->state, memory_order_acquire);
    uint64_t op_index = atomic_load_explicit(
        &w->current_op, memory_order_relaxed);
    uint32_t kind = atomic_load_explicit(
        &w->current_kind, memory_order_relaxed);
    uint32_t slot = atomic_load_explicit(
        &w->current_slot, memory_order_relaxed);
    uint32_t wait_gen = atomic_load_explicit(
        &w->current_wait_gen, memory_order_relaxed);
    uint64_t size = atomic_load_explicit(
        &w->current_size, memory_order_relaxed);
    fprintf(stderr,
            "  trace_tid=%lu state=%s op=%lu/%zu kind=%s "
            "slot=%u wait_gen=%u current_gen=%lu size=%lu\n",
            w->tid_trace, worker_state_name(state), op_index,
            w->ops.len, op_kind_name(kind), slot, wait_gen,
            slot < g_total_slots
                ? atomic_load_explicit(
                      &g_slots[slot].gen, memory_order_relaxed)
                : 0,
            size);
  }
  fflush(stderr);
}

static void *stall_watchdog_thread(void *arg) {
  (void)arg;
  while (!atomic_load_explicit(&g_start, memory_order_acquire) &&
         !atomic_load_explicit(&g_done, memory_order_acquire))
    usleep(1000);

  uint64_t last_completed = atomic_load_explicit(
      &g_completed, memory_order_relaxed);
  uint64_t last_progress_ms = now_ms();
  while (!atomic_load_explicit(&g_done, memory_order_acquire)) {
    usleep(200000);
    uint64_t completed = atomic_load_explicit(
        &g_completed, memory_order_relaxed);
    if (completed != last_completed) {
      last_completed = completed;
      last_progress_ms = now_ms();
      continue;
    }
    if (g_stall_timeout_ms != 0 &&
        now_ms() - last_progress_ms >= g_stall_timeout_ms) {
      dump_stalled_workers(completed);
      _Exit(5);
    }
  }
  return NULL;
}

/* ---------------- allocator wrappers -------------------------------------- */
static void *portable_memalign(size_t alignment, size_t size) {
  if (alignment < sizeof(void *)) alignment = sizeof(void *);
  void *p = NULL;
#if defined(__GLIBC__) || defined(__ANDROID__) || defined(__linux__)
  p = memalign(alignment, size);
#else
  if (posix_memalign(&p, alignment, size) != 0) p = NULL;
#endif
  return p;
}

static void *portable_valloc(size_t size) {
  long ps = sysconf(_SC_PAGESIZE);
  size_t page = ps > 0 ? (size_t)ps : 4096u;
  return portable_memalign(page, size);
}

static void *portable_pvalloc(size_t size) {
  long ps = sysconf(_SC_PAGESIZE);
  size_t page = ps > 0 ? (size_t)ps : 4096u;
  return portable_memalign(page, round_up(size, page));
}

static void *do_alloc_func(uint16_t func, uint64_t size64) {
  size_t size = (size_t)size64;
  switch (func) {
  case MALLOC_I:
    return malloc(size);
  case CALLOC_I:
    return calloc(1, size);
  case POSIX_MEMALIGN_I: {
    void *p = NULL;
    if (posix_memalign(&p, 64, size) != 0) p = NULL;
    return p;
  }
  case ALIGNED_ALLOC_I: {
    size_t s = round_up(size ? size : 1, 64);
#if __STDC_VERSION__ >= 201112L || defined(__GLIBC__) || defined(__linux__)
    return aligned_alloc(64, s);
#else
    return portable_memalign(64, s);
#endif
  }
  case VALLOC_I:
    return portable_valloc(size);
  case MEMALIGN_I:
    return portable_memalign(64, size);
  case PVALLOC_I:
    return portable_pvalloc(size);
  default:
    return malloc(size);
  }
}

static inline void touch_write_block(void *p, uint64_t size) {
  if (!p || size == 0) return;
  volatile unsigned char *c = (volatile unsigned char *)p;
  for (uint64_t off = 0; off < size; off += 64) c[off] = (unsigned char)off;
  c[size - 1] = 0xAB;
}

static inline void touch_read_block(void *p, uint64_t size) {
  if (!p || size == 0) return;
  volatile unsigned char *c = (volatile unsigned char *)p;
  unsigned char sink = 0;
  for (uint64_t off = 0; off < size; off += 64) sink ^= c[off];
  sink ^= c[size - 1];
  (void)sink;
}

/* ---------------- access profile loading ----------------------------------- */
static void load_access_profile(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    perror("open access profile");
    return;
  }

  access_profile_header_t hdr;
  if (read(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
    fprintf(stderr, "access profile: failed to read header\n");
    close(fd);
    return;
  }

  if (hdr.magic != ACCESS_PROFILE_MAGIC) {
    fprintf(stderr, "access profile: bad magic 0x%x\n", hdr.magic);
    close(fd);
    return;
  }

  if (hdr.version != ACCESS_PROFILE_VERSION) {
    fprintf(stderr, "access profile: unsupported version %u\n", hdr.version);
    close(fd);
    return;
  }

  g_access_desc_count = hdr.num_descriptors;
  if (g_access_desc_count == 0) {
    close(fd);
    return;
  }

  size_t bytes = (size_t)g_access_desc_count * sizeof(block_access_desc_t);
  g_access_descs = (block_access_desc_t *)xmap(bytes);
  size_t loaded = 0;
  while (loaded < bytes) {
    ssize_t r = read(fd, (char *)g_access_descs + loaded, bytes - loaded);
    if (r <= 0) break;
    loaded += (size_t)r;
  }
  if (loaded != bytes) {
    fprintf(stderr, "access profile: failed to read descriptors (%zu/%zu)\n",
            loaded, bytes);
    xunmap(g_access_descs, bytes);
    g_access_descs = NULL;
    g_access_desc_count = 0;
  }

  close(fd);
  fprintf(stderr, "access profile: loaded %u descriptors\n", g_access_desc_count);
}

static inline uint64_t select_access_offset(uint64_t size,
                                             block_access_desc_t *desc) {
  uint8_t r = (uint8_t)(now_ns() & 0xFF);
  uint8_t cum = 0;
  for (int i = 0; i < 4; i++) {
    cum += desc->hotspot_weights[i];
    if (r < cum && (uint64_t)desc->hotspot_offsets[i] * 64 < size) {
      return (uint64_t)desc->hotspot_offsets[i] * 64;
    }
  }
  uint64_t num_lines = size / 64;
  if (num_lines == 0) return 0;
  return ((now_ns() % num_lines) * 64);
}

static inline void execute_access_for_block(void *ptr, uint64_t size,
                                             block_access_desc_t *desc,
                                             int is_alloc) {
  if (!desc || !ptr || size == 0) return;

  if (is_alloc) {
    float dirty_ratio = desc->peak_dirty_ratio_q12 / 4096.0f;
    uint64_t pages = (size + 4095) / 4096;
    uint32_t pages_to_touch =
        (uint32_t)(dirty_ratio * (float)pages * g_access_fidelity);
    if (pages_to_touch > pages) pages_to_touch = (uint32_t)pages;

    volatile unsigned char *c = (volatile unsigned char *)ptr;
    for (uint32_t i = 0; i < pages_to_touch; i++) {
      uint64_t off = select_access_offset(size, desc);
      if (off < size) c[off] = (unsigned char)off;
    }

    uint32_t stores =
        (uint32_t)((float)desc->estimate_store_count * g_access_fidelity);
    for (uint32_t i = 0; i < stores && i < pages_to_touch * 8; i++) {
      uint64_t off = select_access_offset(size, desc);
      if (off < size) c[off] = (unsigned char)off;
    }
  } else {
    uint32_t loads =
        (uint32_t)((float)desc->estimate_load_count * g_access_fidelity);
    uint32_t reads = loads / 10;
    volatile unsigned char *c = (volatile unsigned char *)ptr;
    unsigned char sink = 0;
    for (uint32_t i = 0; i < reads; i++) {
      uint64_t off = select_access_offset(size, desc);
      if (off < size) sink ^= c[off];
    }
    (void)sink;
  }

  g_access_sim_ops++;
}

/* ---------------- execution ------------------------------------------------ */
static void apply_cpu_affinity(int cpu) {
  if (cpu < 0) return;
  int ncpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
  if (ncpus <= 0) ncpus = 1;
  cpu_set_t cs;
  CPU_ZERO(&cs);
  CPU_SET((unsigned)(cpu % ncpus), &cs);
  pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
}

static uintptr_t wait_slot_gen(worker_t *w, uint32_t slot, uint32_t gen) {
  uint64_t spins = 0;
  uint64_t wait_start_ms = 0;
  int waited = 0;
  for (;;) {
    uint64_t g = atomic_load_explicit(&g_slots[slot].gen, memory_order_acquire);
    if (g >= gen) {
      if (waited) w->waits++;
      atomic_store_explicit(
          &w->state, WORKER_EXECUTING, memory_order_release);
      return atomic_load_explicit(&g_slots[slot].ptr, memory_order_acquire);
    }
    waited = 1;
    atomic_store_explicit(
        &w->state, WORKER_SLOT_WAIT, memory_order_release);
    if (wait_start_ms == 0) wait_start_ms = now_ms();
    if (spins++ < g_spin_before_yield) {
      CPU_RELAX();
    } else {
      spins = 0;
      w->wait_yields++;
      if (g_dependency_timeout_ms != 0 &&
          now_ms() - wait_start_ms >= g_dependency_timeout_ms) {
        fprintf(stderr,
                "dependency wait timed out: trace_tid=%lu slot=%u "
                "expected_gen=%u current_gen=%lu timeout_ms=%lu\n",
                w->tid_trace, slot, gen, g, g_dependency_timeout_ms);
        exit(4);
      }
      sched_yield();
    }
  }
}

static void publish_slot(uint32_t slot, uint32_t gen, void *p) {
  atomic_store_explicit(&g_slots[slot].ptr, (uintptr_t)p, memory_order_release);
  atomic_store_explicit(&g_slots[slot].gen, gen, memory_order_release);
}

static void execute_op_inner(worker_t *w, op_t *o) {
  switch (o->kind) {
  case OP_ALLOC: {
    void *p = do_alloc_func(o->func, o->size);
    if ((g_touch_mode & TOUCH_ALLOC) && p) touch_write_block(p, o->size);
    if (o->access_desc_idx != ACCESS_DESC_NONE && p) {
      block_access_desc_t *d = &g_access_descs[o->access_desc_idx];
      execute_access_for_block(p, o->size, d, 1);
    }
    publish_slot(o->slot, o->out_gen, p);
    break;
  }
  case OP_REALLOC_NULL: {
    void *p = realloc(NULL, (size_t)o->size);
    if ((g_touch_mode & TOUCH_ALLOC) && p) touch_write_block(p, o->size);
    if (o->access_desc_idx != ACCESS_DESC_NONE && p) {
      block_access_desc_t *d = &g_access_descs[o->access_desc_idx];
      execute_access_for_block(p, o->size, d, 1);
    }
    publish_slot(o->slot, o->out_gen, p);
    break;
  }
  case OP_REALLOC: {
    void *oldp = (void *)wait_slot_gen(w, o->slot, o->wait_gen);
    void *newp;
    if (o->size == 0) {
      /* realloc(p, 0) is implementation-sensitive. We still exercise realloc;
       * if it returns a unique zero-size pointer, immediately release it so the
       * slot is terminal and no later op can use it.
       */
      newp = realloc(oldp, 0);
      if (newp) free(newp);
      newp = NULL;
    } else {
      newp = realloc(oldp, (size_t)o->size);
      if (!newp) {
        /* Runtime allocator failed although the trace realloc succeeded.
         * Keep old pointer alive and publish the next generation with oldp so
         * later frees do not spin forever. Count is not currently separated
         * from preprocessing failed_reallocs.
         */
        newp = oldp;
      }
      if ((g_touch_mode & TOUCH_ALLOC) && newp) touch_write_block(newp, o->size);
      if (o->access_desc_idx != ACCESS_DESC_NONE && newp) {
        block_access_desc_t *d = &g_access_descs[o->access_desc_idx];
        execute_access_for_block(newp, o->size, d, 1);
      }
    }
    publish_slot(o->slot, o->out_gen, newp);
    break;
  }
  case OP_FREE: {
    void *p = (void *)wait_slot_gen(w, o->slot, o->wait_gen);
    if (o->access_desc_idx != ACCESS_DESC_NONE && p) {
      block_access_desc_t *d = &g_access_descs[o->access_desc_idx];
      execute_access_for_block(p, o->size, d, 0);
    }
    if ((g_touch_mode & TOUCH_FREE) && p) touch_read_block(p, o->size);
    free(p);
    break;
  }
  case OP_UNKNOWN_FREE: {
    w->unknown_executed++;
    if (g_unknown_policy == UNKNOWN_SYNTHETIC) {
      void *p = (void *)o->aux;
      if (p) {
        if ((g_touch_mode & TOUCH_FREE)) touch_read_block(p, o->size);
        free(p);
      }
    }
    break;
  }
  default:
    break;
  }
}

static uint16_t op_latency_func(const op_t *o) {
  switch (o->kind) {
  case OP_FREE:
  case OP_UNKNOWN_FREE:
    return FREE_I;
  case OP_REALLOC:
  case OP_REALLOC_NULL:
    return REALLOC_I;
  case OP_ALLOC:
  default:
    return o->func < NUM_FUNCS ? o->func : MALLOC_I;
  }
}

static void execute_op(worker_t *w, op_t *o, uint64_t local_index) {
  atomic_store_explicit(
      &w->current_op, local_index, memory_order_relaxed);
  atomic_store_explicit(
      &w->current_size, o->size, memory_order_relaxed);
  atomic_store_explicit(
      &w->current_slot, o->slot, memory_order_relaxed);
  atomic_store_explicit(
      &w->current_wait_gen, o->wait_gen, memory_order_relaxed);
  atomic_store_explicit(
      &w->current_kind, o->kind, memory_order_relaxed);
  atomic_store_explicit(
      &w->state, WORKER_EXECUTING, memory_order_release);
  if (g_latency_sample_rate && (local_index % g_latency_sample_rate == 0)) {
    uint16_t f = op_latency_func(o);
    uint64_t t0 = now_ns();
    execute_op_inner(w, o);
    uint64_t t1 = now_ns();
    lat_record(&w->lat[f], t1 - t0);
  } else {
    execute_op_inner(w, o);
  }
  atomic_store_explicit(
      &w->state, WORKER_EXECUTING, memory_order_release);
  w->executed++;
  atomic_fetch_add_explicit(&g_completed, 1, memory_order_relaxed);
}

static void prepare_synthetic_unknowns(worker_t *w) {
  if (g_unknown_policy != UNKNOWN_SYNTHETIC) return;
  for (size_t i = 0; i < w->ops.len; i++) {
    op_t *o = &w->ops.d[i];
    if (o->kind == OP_UNKNOWN_FREE) {
      uint64_t sz = o->size ? o->size : g_synthetic_size;
      o->size = sz;
      void *p = malloc((size_t)sz);
      if (!p) w->failed_synth_allocs++;
      o->aux = (uintptr_t)p;
    }
  }
}

static void *worker_main(void *arg) {
  worker_t *w = (worker_t *)arg;

  if (g_cpu_affinity) apply_cpu_affinity(w->recorded_cpu);

  for (int i = 0; i < NUM_FUNCS; i++) {
    lat_init(&w->lat[i], g_latency_sample_rate ? g_latency_sample_cap : 0);
  }

  prepare_synthetic_unknowns(w);

  while (!atomic_load_explicit(&g_start, memory_order_acquire)) CPU_RELAX();

  uint64_t local_idx = 0;
  uint64_t seen_epoch = 0;
  size_t pos = 0;

  for (;;) {
    atomic_store_explicit(
        &w->state, WORKER_EPOCH_WAIT, memory_order_release);
    pthread_mutex_lock(&g_epoch_mu);
    while (seen_epoch == g_epoch_id && !atomic_load_explicit(&g_done, memory_order_acquire)) {
      pthread_cond_wait(&g_epoch_cv, &g_epoch_mu);
    }
    seen_epoch = g_epoch_id;
    uint64_t ep_end = g_epoch_end;
    pthread_mutex_unlock(&g_epoch_mu);

    while (pos < w->ops.len && w->ops.d[pos].ts < ep_end) {
      execute_op(w, &w->ops.d[pos], local_idx++);
      pos++;
    }

    int should_finish = (pos >= w->ops.len && ep_end >= w->end_ts);

    pthread_mutex_lock(&g_epoch_mu);
    if (should_finish) {
      atomic_store_explicit(
          &w->state, WORKER_FINISHED, memory_order_release);
      atomic_store_explicit(&w->finished, 1, memory_order_release);
    }
    g_epoch_arrived++;
    pthread_cond_broadcast(&g_epoch_cv);
    pthread_mutex_unlock(&g_epoch_mu);

    if (should_finish) break;
  }

  return NULL;
}

static void *worker_main_freerun(void *arg) {
  worker_t *w = (worker_t *)arg;

  if (g_cpu_affinity) apply_cpu_affinity(w->recorded_cpu);

  for (int i = 0; i < NUM_FUNCS; i++)
    lat_init(&w->lat[i], g_latency_sample_rate ? g_latency_sample_cap : 0);

  prepare_synthetic_unknowns(w);

  while (!atomic_load_explicit(&g_start, memory_order_acquire)) CPU_RELAX();

  for (size_t i = 0; i < w->ops.len; i++) {
    execute_op(w, &w->ops.d[i], i);
    if (g_timing_scale > 0.0 && i + 1 < w->ops.len) {
      uint64_t gap = w->ops.d[i + 1].ts > w->ops.d[i].ts
                         ? w->ops.d[i + 1].ts - w->ops.d[i].ts : 0;
      /* Trace timestamps are in ~ms; convert gap to ns and cap at 10 ms. */
      uint64_t sleep_ns = (uint64_t)((double)gap * g_timing_scale * 1000000.0);
      if (sleep_ns > 10000000ULL) sleep_ns = 10000000ULL;
      if (sleep_ns > 0) {
        struct timespec ts = {0, (long)sleep_ns};
        nanosleep(&ts, NULL);
      }
    }
  }

  atomic_store_explicit(
      &w->state, WORKER_FINISHED, memory_order_release);
  atomic_store_explicit(&w->finished, 1, memory_order_release);
  return NULL;
}

static void *progress_thread(void *arg) {
  (void)arg;
  uint64_t start = now_ms();
  const int width = 40;
  while (!atomic_load_explicit(&g_done, memory_order_acquire)) {
    uint64_t done = atomic_load_explicit(&g_completed, memory_order_relaxed);
    uint64_t denom = g_total_ops ? g_total_ops : 1;
    double frac = (double)done / (double)denom;
    if (frac > 1.0) frac = 1.0;
    int filled = (int)(frac * width);
    uint64_t el = now_ms() - start;
    double rate = el ? done / (el / 1000.0) : 0.0;
    double eta = rate > 0 ? (denom - done) / rate : 0.0;
    fprintf(stderr, "\r[");
    for (int i = 0; i < width; i++) fputc(i < filled ? '#' : '-', stderr);
    fprintf(stderr, "] %5.1f%%  %lu/%lu  %.0f ops/s  ETA %.0fs   ",
            frac * 100.0, (unsigned long)done, (unsigned long)denom, rate, eta);
    fflush(stderr);
    usleep(200000);
  }
  fprintf(stderr, "\r[");
  for (int i = 0; i < width; i++) fputc('#', stderr);
  fprintf(stderr, "] 100.0%%  %lu/%lu  done            \n",
          (unsigned long)g_total_ops, (unsigned long)g_total_ops);
  return NULL;
}

/* ---------------- output --------------------------------------------------- */
static void merge_latency(func_latency_t out[NUM_FUNCS]) {
  for (int i = 0; i < NUM_FUNCS; i++) lat_init(&out[i], g_latency_sample_rate ? g_latency_sample_cap : 0);

  for (int t = 0; t < g_nthreads; t++) {
    for (int f = 0; f < NUM_FUNCS; f++) {
      func_latency_t *src = &g_ws[t].lat[f];
      func_latency_t *dst = &out[f];
      if (!src->count) continue;
      dst->count += src->count;
      dst->total_ns += src->total_ns;
      if (dst->min_ns == 0 || src->min_ns < dst->min_ns) dst->min_ns = src->min_ns;
      if (src->max_ns > dst->max_ns) dst->max_ns = src->max_ns;
      for (uint64_t k = 0; k < src->sample_count && dst->sample_count < dst->sample_cap; k++) {
        dst->samples[dst->sample_count++] = src->samples[k];
      }
    }
  }
  for (int f = 0; f < NUM_FUNCS; f++) {
    if (out[f].sample_count > 1) qsort(out[f].samples, out[f].sample_count, sizeof(uint64_t), cmp_u64);
  }
}

static uint64_t sum_worker_field_waits(void) {
  uint64_t s = 0;
  for (int t = 0; t < g_nthreads; t++) s += g_ws[t].waits;
  return s;
}

static uint64_t sum_worker_field_yields(void) {
  uint64_t s = 0;
  for (int t = 0; t < g_nthreads; t++) s += g_ws[t].wait_yields;
  return s;
}

static uint64_t sum_worker_failed_synth(void) {
  uint64_t s = 0;
  for (int t = 0; t < g_nthreads; t++) s += g_ws[t].failed_synth_allocs;
  return s;
}

static void print_json(uint64_t replay_ms, long vmhwm_kb) {
  func_latency_t lat[NUM_FUNCS];
  merge_latency(lat);
  printf("{\n");
  printf("  \"total_records\": %lu,\n", g_total_records);
  printf("  \"total_ops\": %lu,\n", g_total_ops);
  printf("  \"workers\": %d,\n", g_nthreads);
  printf("  \"slots\": %u,\n", g_total_slots);
  printf("  \"replay_time_ms\": %lu,\n", replay_ms);
  printf("  \"peak_memory_mb\": %.2f,\n", vmhwm_kb >= 0 ? vmhwm_kb / 1024.0 : -1.0);
  printf("  \"epoch_size\": %lu,\n", g_epoch_size);
  printf("  \"epochs\": %lu,\n", g_num_epochs);
  printf("  \"allocs\": %lu,\n", g_total_allocs);
  printf("  \"frees\": %lu,\n", g_total_frees);
  printf("  \"reallocs\": %lu,\n", g_total_reallocs);
  printf("  \"failed_allocs\": %lu,\n", g_failed_allocs);
  printf("  \"failed_reallocs\": %lu,\n", g_failed_reallocs);
  printf("  \"unknown_frees\": %lu,\n", g_unknown_frees);
  printf("  \"unknown_reallocs\": %lu,\n", g_unknown_reallocs);
  printf("  \"local_frees\": %lu,\n", g_local_frees);
  printf("  \"remote_frees\": %lu,\n", g_remote_frees);
  printf("  \"local_reallocs\": %lu,\n", g_local_reallocs);
  printf("  \"remote_reallocs\": %lu,\n", g_remote_reallocs);
  printf("  \"live_left_at_end\": %lu,\n", g_live_left_at_end);
  printf("  \"waits\": %lu,\n", sum_worker_field_waits());
  printf("  \"wait_yields\": %lu,\n", sum_worker_field_yields());
  printf("  \"failed_synthetic_allocs\": %lu", sum_worker_failed_synth());
  if (g_latency_sample_rate) {
    printf(",\n  \"latency_sample_rate\": %lu,\n", g_latency_sample_rate);
    printf("  \"functions\": {\n");
    int first = 1;
    for (int f = 0; f < NUM_FUNCS; f++) {
      if (!lat[f].count) continue;
      if (!first) printf(",\n");
      first = 0;
      printf("    \"%s\": {\"samples\": %lu, \"avg_ns\": %lu, \"p50_ns\": %lu, \"p99_ns\": %lu, \"max_ns\": %lu}",
             func_names[f], lat[f].count,
             lat[f].count ? lat[f].total_ns / lat[f].count : 0,
             pct(lat[f].samples, lat[f].sample_count, 50),
             pct(lat[f].samples, lat[f].sample_count, 99), lat[f].max_ns);
    }
    printf("\n  }\n");
  } else {
    printf("\n");
  }
  printf("}\n");
}

static void print_stats(uint64_t replay_ms, long vmhwm_kb) {
  fprintf(stderr, "\n=== replay.c dependency-safe allocator replay ===\n");
  fprintf(stderr, "records=%lu ops=%lu thread_instances=%d slots=%u replay=%lu ms peak=%.2f MB\n",
          g_total_records, g_total_ops, g_nthreads, g_total_slots, replay_ms,
          vmhwm_kb >= 0 ? vmhwm_kb / 1024.0 : -1.0);
  fprintf(stderr, "epoch_size=%lu epochs=%lu touch=%d latency_sample_rate=%lu\n",
          g_epoch_size, g_num_epochs, g_touch_mode, g_latency_sample_rate);
  fprintf(stderr, "thread_events: create=%lu start=%lu end=%lu join=%lu inferred=%lu\n",
          g_thread_create_events, g_thread_start_events, g_thread_end_events,
          g_thread_join_events, g_lifecycle_inferred_threads);
  fprintf(stderr, "allocs=%lu frees=%lu reallocs=%lu failed_allocs=%lu failed_reallocs=%lu\n",
          g_total_allocs, g_total_frees, g_total_reallocs, g_failed_allocs,
          g_failed_reallocs);
  fprintf(stderr, "unknown_free=%lu unknown_realloc=%lu live_left_at_end=%lu\n",
          g_unknown_frees, g_unknown_reallocs, g_live_left_at_end);
  fprintf(stderr, "local_free=%lu remote_free=%lu remote_free%%=%.2f\n",
          g_local_frees, g_remote_frees,
          (g_local_frees + g_remote_frees)
              ? 100.0 * (double)g_remote_frees / (double)(g_local_frees + g_remote_frees)
              : 0.0);
  fprintf(stderr, "local_realloc=%lu remote_realloc=%lu wait_yields=%lu failed_synthetic_allocs=%lu\n",
          g_local_reallocs, g_remote_reallocs, sum_worker_field_yields(),
          sum_worker_failed_synth());
  if (g_access_desc_count > 0) {
    fprintf(stderr, "access_profile: descs=%u fidelity=%.2f sim_ops=%lu\n",
            g_access_desc_count, g_access_fidelity, g_access_sim_ops);
  }

  if (g_latency_sample_rate) {
    func_latency_t lat[NUM_FUNCS];
    merge_latency(lat);
    fprintf(stderr, "\n%-18s %12s %12s %12s %12s %12s\n", "func", "samples", "avg", "p50", "p99", "max");
    for (int f = 0; f < NUM_FUNCS; f++) {
      if (!lat[f].count) continue;
      fprintf(stderr, "%-18s %12lu %12lu %12lu %12lu %12lu\n",
              func_names[f], lat[f].count,
              lat[f].count ? lat[f].total_ns / lat[f].count : 0,
              pct(lat[f].samples, lat[f].sample_count, 50),
              pct(lat[f].samples, lat[f].sample_count, 99), lat[f].max_ns);
    }
  }
}

/* ---------------- CLI and preprocessing ----------------------------------- */

static int cmp_worker_start(const void *a, const void *b) {
  int ia = *(const int *)a;
  int ib = *(const int *)b;
  if (g_ws[ia].start_ts < g_ws[ib].start_ts) return -1;
  if (g_ws[ia].start_ts > g_ws[ib].start_ts) return 1;
  return ia - ib;
}

static int worker_has_ops_or_lifecycle(const worker_t *w) {
  return w->ops.len > 0 || w->has_start || w->has_end;
}

static void finalize_worker_lifetimes(void) {
  for (int i = 0; i < g_nthreads; i++) {
    worker_t *w = &g_ws[i];
    if (!worker_has_ops_or_lifecycle(w)) continue;
    if (!w->has_start) {
      g_lifecycle_inferred_threads++;
      w->start_ts = (w->first_op_ts != UINT64_MAX) ? w->first_op_ts : g_min_ts;
    }
    if (!w->has_end) {
      w->end_ts = (w->last_op_ts != 0) ? w->last_op_ts : g_max_ts;
    }
    if (w->start_ts == UINT64_MAX) w->start_ts = g_min_ts;
    if (w->end_ts < w->start_ts) w->end_ts = w->start_ts;
  }
}

static uint64_t compute_auto_epoch_size(void) {
  uint64_t span = (g_max_ts >= g_min_ts) ? (g_max_ts - g_min_ts + 1) : 1;
  uint64_t target = g_auto_epoch_target ? g_auto_epoch_target : 1000;
  uint64_t ep = (span + target - 1) / target;
  return ep ? ep : 1;
}

static void run_application_like_replay(uint64_t *replay_ms_out) {
  int *order = (int *)xmap((size_t)g_nthreads * sizeof(int));
  int norder = 0;
  for (int i = 0; i < g_nthreads; i++) {
    if (worker_has_ops_or_lifecycle(&g_ws[i])) order[norder++] = i;
  }
  qsort(order, (size_t)norder, sizeof(int), cmp_worker_start);

  uint64_t epoch_size = g_epoch_size ? g_epoch_size : compute_auto_epoch_size();
  g_epoch_size = epoch_size;
  uint64_t span = (g_max_ts >= g_min_ts) ? (g_max_ts - g_min_ts + 1) : 1;
  g_num_epochs = (span + g_epoch_size - 1) / g_epoch_size;
  if (g_num_epochs == 0) g_num_epochs = 1;

  atomic_store(&g_completed, 0);
  atomic_store(&g_done, 0);
  atomic_store(&g_start, 0);

  pthread_t prog_thr;
  pthread_t watchdog_thr;
  int watchdog_started = 0;
  if (g_show_progress)
    create_thread_or_exit(
        &prog_thr, progress_thread, NULL, "progress", 0);
  if (g_stall_timeout_ms != 0) {
    create_thread_or_exit(
        &watchdog_thr, stall_watchdog_thread, NULL, "watchdog", 0);
    watchdog_started = 1;
  }

  uint64_t start_ms = now_ms();
  atomic_store_explicit(&g_start, 1, memory_order_release);

  int next = 0;
  int active = 0;
  for (uint64_t ep = 0; ep < g_num_epochs; ep++) {
    uint64_t ep_end = g_min_ts + (ep + 1) * g_epoch_size;

    while (next < norder && g_ws[order[next]].start_ts < ep_end) {
      worker_t *w = &g_ws[order[next]];
      if (!w->launched) {
        create_thread_or_exit(
            &w->th, worker_main, w, "epoch-worker", w->tid_trace);
        w->launched = 1;
        active++;
      }
      next++;
    }

    if (active == 0) continue;

    pthread_mutex_lock(&g_epoch_mu);
    g_epoch_end = ep_end;
    g_epoch_arrived = 0;
    g_epoch_target = (uint64_t)active;
    g_epoch_id++;
    pthread_cond_broadcast(&g_epoch_cv);
    while (g_epoch_arrived < g_epoch_target) {
      pthread_cond_wait(&g_epoch_cv, &g_epoch_mu);
    }
    pthread_mutex_unlock(&g_epoch_mu);

    for (int k = 0; k < next; k++) {
      worker_t *w = &g_ws[order[k]];
      if (w->launched && !w->joined && atomic_load_explicit(&w->finished, memory_order_acquire)) {
        join_thread_or_exit(w->th, "epoch-worker", w->tid_trace);
        w->joined = 1;
        active--;
      }
    }
  }

  /* Drain any not-yet-launched zero-duration or late workers and then run one
   * final epoch covering the entire trace. This mainly protects old traces or
   * traces whose lifecycle events were truncated. */
  while (next < norder) {
    worker_t *w = &g_ws[order[next++]];
    if (!w->launched) {
      create_thread_or_exit(
          &w->th, worker_main, w, "epoch-worker", w->tid_trace);
      w->launched = 1;
      active++;
    }
  }
  while (active > 0) {
    pthread_mutex_lock(&g_epoch_mu);
    g_epoch_end = UINT64_MAX;
    g_epoch_arrived = 0;
    g_epoch_target = (uint64_t)active;
    g_epoch_id++;
    pthread_cond_broadcast(&g_epoch_cv);
    while (g_epoch_arrived < g_epoch_target) {
      pthread_cond_wait(&g_epoch_cv, &g_epoch_mu);
    }
    pthread_mutex_unlock(&g_epoch_mu);

    for (int k = 0; k < norder; k++) {
      worker_t *w = &g_ws[order[k]];
      if (w->launched && !w->joined && atomic_load_explicit(&w->finished, memory_order_acquire)) {
        join_thread_or_exit(w->th, "epoch-worker", w->tid_trace);
        w->joined = 1;
        active--;
      }
    }
  }

  uint64_t replay_ms = now_ms() - start_ms;
  *replay_ms_out = replay_ms;

  atomic_store_explicit(&g_done, 1, memory_order_release);
  pthread_mutex_lock(&g_epoch_mu);
  pthread_cond_broadcast(&g_epoch_cv);
  pthread_mutex_unlock(&g_epoch_mu);
  if (g_show_progress)
    join_thread_or_exit(prog_thr, "progress", 0);
  if (watchdog_started)
    join_thread_or_exit(watchdog_thr, "watchdog", 0);

  xunmap(order, (size_t)g_nthreads * sizeof(int));
}

static void run_free_run_replay(uint64_t *replay_ms_out) {
  atomic_store(&g_completed, 0);
  atomic_store(&g_done, 0);
  atomic_store(&g_start, 0);

  pthread_t prog_thr;
  pthread_t watchdog_thr;
  int watchdog_started = 0;
  if (g_show_progress)
    create_thread_or_exit(
        &prog_thr, progress_thread, NULL, "progress", 0);
  if (g_stall_timeout_ms != 0) {
    create_thread_or_exit(
        &watchdog_thr, stall_watchdog_thread, NULL, "watchdog", 0);
    watchdog_started = 1;
  }

  for (int i = 0; i < g_nthreads; i++) {
    if (!worker_has_ops_or_lifecycle(&g_ws[i])) continue;
    create_thread_or_exit(
        &g_ws[i].th, worker_main_freerun, &g_ws[i],
        "free-run-worker", g_ws[i].tid_trace);
    g_ws[i].launched = 1;
  }

  uint64_t start_ms = now_ms();
  atomic_store_explicit(&g_start, 1, memory_order_release);

  for (int i = 0; i < g_nthreads; i++) {
    if (g_ws[i].launched)
      join_thread_or_exit(
          g_ws[i].th, "free-run-worker", g_ws[i].tid_trace);
  }

  *replay_ms_out = now_ms() - start_ms;
  atomic_store_explicit(&g_done, 1, memory_order_release);
  if (g_show_progress)
    join_thread_or_exit(prog_thr, "progress", 0);
  if (watchdog_started)
    join_thread_or_exit(watchdog_thr, "watchdog", 0);
}

static void usage(const char *prog) {
  fprintf(stderr,
          "Usage: %s [options] <trace.rply>\n"
          "\n"
          "Core options:\n"
          "  -S, --stats                    print human-readable stats\n"
          "  -j, --json                     print JSON stats\n"
          "  -p, --progress                 enable progress bar; adds per-op atomic overhead\n"
          "      --epoch-size N             replay in raw timestamp windows; 0 = free-run\n"
          "      --map-live N               initial live-object map estimate [default %u]\n"
          "\n"
          "Correctness / modeling:\n"
          "      --unknown-policy skip      default: skip unknown free and count it\n"
          "      --unknown-policy error     exit if unknown free/realloc exists\n"
          "      --unknown-policy synthetic free preallocated synthetic objects\n"
          "      --synthetic-size N         size for synthetic unknown-free objects [64]\n"
          "      --touch none|alloc|free|both  default none\n"
          "\n"
          "Access profile (memory access simulation):\n"
          "      --access-profile <file>  load .macc_profile binary (from profile_access.py)\n"
          "      --access-fidelity F      simulation fraction 0.0-1.0 [0.1]\n"
          "\n"
          "Measurement/debug:\n"
          "      --latency-sample-rate N    sample one op every N ops; 0 disables\n"
          "      --latency-sample-cap N     max samples per function after merge\n"
          "      --spin-before-yield N      free/realloc wait spin threshold [1024]\n"
          "      --dependency-timeout-ms N  abort stalled slot dependency [30000]; 0 disables\n"
          "      --stall-timeout-ms N       abort when no operation completes [30000]; 0 disables\n"
          "  -h, --help                     show this help\n"
          "\n"
          "Concurrency / timing:\n"
          "      --free-run                 launch all threads at once, no epoch barrier\n"
          "      --cpu-affinity             pin each worker to its recorded CPU id\n"
          "      --timing-scale F           sleep F×recorded_gap (ms) between ops; best with --free-run\n",
          prog, DEFAULT_MAP_LIVE);
}

static void parse_unknown_policy(const char *s) {
  if (strcmp(s, "skip") == 0) g_unknown_policy = UNKNOWN_SKIP;
  else if (strcmp(s, "error") == 0 || strcmp(s, "strict") == 0) g_unknown_policy = UNKNOWN_ERROR;
  else if (strcmp(s, "synthetic") == 0) g_unknown_policy = UNKNOWN_SYNTHETIC;
  else {
    fprintf(stderr, "unknown --unknown-policy: %s\n", s);
    exit(2);
  }
}

static void parse_touch_mode(const char *s) {
  if (strcmp(s, "none") == 0) g_touch_mode = TOUCH_NONE;
  else if (strcmp(s, "alloc") == 0) g_touch_mode = TOUCH_ALLOC;
  else if (strcmp(s, "free") == 0) g_touch_mode = TOUCH_FREE;
  else if (strcmp(s, "both") == 0) g_touch_mode = TOUCH_BOTH;
  else {
    fprintf(stderr, "unknown --touch: %s\n", s);
    exit(2);
  }
}

int main(int argc, char **argv) {
  size_t map_live = DEFAULT_MAP_LIVE;

  enum {
    OPT_EPOCH = 1000,
    OPT_UNKNOWN,
    OPT_SYNTH_SIZE,
    OPT_TOUCH,
    OPT_LAT_RATE,
    OPT_LAT_CAP,
    OPT_SPIN,
    OPT_DEP_TIMEOUT,
    OPT_STALL_TIMEOUT,
    OPT_MAP_LIVE,
    OPT_AUTO_EPOCH_TARGET,
    OPT_ACCESS_PROFILE,
    OPT_ACCESS_FIDELITY,
    OPT_FREE_RUN,
    OPT_CPU_AFFINITY,
    OPT_TIMING_SCALE
  };

  static struct option long_opts[] = {
      {"stats", no_argument, 0, 'S'},
      {"json", no_argument, 0, 'j'},
      {"progress", no_argument, 0, 'p'},
      {"help", no_argument, 0, 'h'},
      {"epoch-size", required_argument, 0, OPT_EPOCH},
      {"unknown-policy", required_argument, 0, OPT_UNKNOWN},
      {"synthetic-size", required_argument, 0, OPT_SYNTH_SIZE},
      {"touch", required_argument, 0, OPT_TOUCH},
      {"latency-sample-rate", required_argument, 0, OPT_LAT_RATE},
      {"latency-sample-cap", required_argument, 0, OPT_LAT_CAP},
      {"spin-before-yield", required_argument, 0, OPT_SPIN},
      {"dependency-timeout-ms", required_argument, 0, OPT_DEP_TIMEOUT},
      {"stall-timeout-ms", required_argument, 0, OPT_STALL_TIMEOUT},
      {"map-live", required_argument, 0, OPT_MAP_LIVE},
      {"auto-epoch-target", required_argument, 0, OPT_AUTO_EPOCH_TARGET},
      {"access-profile", required_argument, 0, OPT_ACCESS_PROFILE},
      {"access-fidelity", required_argument, 0, OPT_ACCESS_FIDELITY},
      {"free-run", no_argument, 0, OPT_FREE_RUN},
      {"cpu-affinity", no_argument, 0, OPT_CPU_AFFINITY},
      {"timing-scale", required_argument, 0, OPT_TIMING_SCALE},
      {0, 0, 0, 0}};

  int c;
  while ((c = getopt_long(argc, argv, "Sjph", long_opts, NULL)) != -1) {
    switch (c) {
    case 'S': g_show_stats = 1; break;
    case 'j': g_json_output = 1; break;
    case 'p': g_show_progress = 1; break;
    case 'h': usage(argv[0]); return 0;
    case OPT_EPOCH: g_epoch_size = parse_u64(optarg, "epoch-size"); break;
    case OPT_UNKNOWN: parse_unknown_policy(optarg); break;
    case OPT_SYNTH_SIZE: g_synthetic_size = parse_u64(optarg, "synthetic-size"); break;
    case OPT_TOUCH: parse_touch_mode(optarg); break;
    case OPT_LAT_RATE: g_latency_sample_rate = parse_u64(optarg, "latency-sample-rate"); break;
    case OPT_LAT_CAP: g_latency_sample_cap = parse_u64(optarg, "latency-sample-cap"); break;
    case OPT_SPIN: g_spin_before_yield = parse_u64(optarg, "spin-before-yield"); break;
    case OPT_DEP_TIMEOUT: g_dependency_timeout_ms = parse_u64(optarg, "dependency-timeout-ms"); break;
    case OPT_STALL_TIMEOUT: g_stall_timeout_ms = parse_u64(optarg, "stall-timeout-ms"); break;
    case OPT_MAP_LIVE: map_live = (size_t)parse_u64(optarg, "map-live"); break;
    case OPT_AUTO_EPOCH_TARGET: g_auto_epoch_target = parse_u64(optarg, "auto-epoch-target"); break;
    case OPT_ACCESS_PROFILE: load_access_profile(optarg); break;
    case OPT_ACCESS_FIDELITY: g_access_fidelity = (float)strtod(optarg, NULL); break;
    case OPT_FREE_RUN: g_free_run = 1; break;
    case OPT_CPU_AFFINITY: g_cpu_affinity = 1; break;
    case OPT_TIMING_SCALE: g_timing_scale = strtod(optarg, NULL); break;
    default: usage(argv[0]); return 2;
    }
  }

  if (optind >= argc) {
    usage(argv[0]);
    return 2;
  }

  const char *trace_path = argv[optind];

  int fd = open(trace_path, O_RDONLY);
  if (fd < 0) {
    perror("open trace");
    return 1;
  }

  uint64_t idx_val = 0;
  if (pread(fd, &idx_val, sizeof(idx_val), 0) != (ssize_t)sizeof(idx_val)) {
    perror("pread idx");
    close(fd);
    return 1;
  }

  if (idx_val % ENTRY_WORDS != 0) {
    fprintf(stderr,
            "invalid RPLY header: idx=%lu is not divisible by "
            "entry_words=%zu\n",
            idx_val, (size_t)ENTRY_WORDS);
    close(fd);
    return 1;
  }

  g_total_records = idx_val / ENTRY_WORDS;
  size_t log_off = offsetof(HOOK_LOG, log);
  if (g_total_records > (SIZE_MAX - log_off) / sizeof(REPLAY_ENTRY)) {
    fprintf(stderr,
            "RPLY is too large for this process: records=%lu\n",
            g_total_records);
    close(fd);
    return 1;
  }
  size_t map_len =
      log_off + (size_t)g_total_records * sizeof(REPLAY_ENTRY);

  struct stat trace_stat;
  if (fstat(fd, &trace_stat) != 0) {
    perror("fstat trace");
    close(fd);
    return 1;
  }
  if (trace_stat.st_size < 0 ||
      (uint64_t)trace_stat.st_size < (uint64_t)map_len) {
    fprintf(stderr,
            "truncated RPLY: header declares %lu records "
            "(need %zu bytes), file has %lld bytes\n",
            g_total_records, map_len, (long long)trace_stat.st_size);
    close(fd);
    return 1;
  }

  char *map_base = (char *)mmap(NULL, map_len, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (map_base == MAP_FAILED) {
    perror("mmap trace");
    return 1;
  }
  madvise(map_base, map_len, MADV_SEQUENTIAL);
  REPLAY_ENTRY *entries = (REPLAY_ENTRY *)(map_base + log_off);

  g_tk = (int64_t *)xmap(TIDCAP * sizeof(int64_t));
  g_tv = (int *)xmap(TIDCAP * sizeof(int));
  for (size_t i = 0; i < TIDCAP; i++) g_tk[i] = -1;

  g_ws = (worker_t *)xmap(MAX_THREADS * sizeof(worker_t));
  memset(g_ws, 0, MAX_THREADS * sizeof(worker_t));

  omap_t live;
  omap_init(&live, map_live);

  uint64_t next_slot = 0;

  for (size_t i = 0; i < g_total_records; i++) {
    const REPLAY_ENTRY *e = &entries[i];
    uint16_t func = (uint16_t)(e->func_and_cpu >> 32);
    if (e->timestamp < g_min_ts) g_min_ts = e->timestamp;
    if (e->timestamp > g_max_ts) g_max_ts = e->timestamp;

    if (func >= REPLAY_THREAD_CREATE_I) {
      int wi = worker_register(e->thread_id);
      switch (func) {
      case REPLAY_THREAD_CREATE_I:
        g_thread_create_events++;
        break;
      case REPLAY_THREAD_START_I:
        g_thread_start_events++;
        g_ws[wi].has_start = 1;
        if (e->timestamp < g_ws[wi].start_ts) g_ws[wi].start_ts = e->timestamp;
        break;
      case REPLAY_THREAD_END_I:
        g_thread_end_events++;
        g_ws[wi].has_end = 1;
        if (e->timestamp > g_ws[wi].end_ts) g_ws[wi].end_ts = e->timestamp;
        break;
      case REPLAY_THREAD_JOIN_I:
        g_thread_join_events++;
        break;
      default:
        break;
      }
      continue;
    }

    if (func >= NUM_FUNCS) {
      continue;
    }

    bool is_free = (func == FREE_I || func == FREE_SIZED_I);
    bool is_realloc = (func == REALLOC_I);
    bool is_alloc_like = !is_free && !is_realloc;

    if (is_alloc_like) {
      g_total_allocs++;
      if (e->result == 0) {
        g_failed_allocs++;
        continue;
      }
      if (next_slot > UINT32_MAX) {
        fprintf(stderr, "slot overflow\n");
        return 1;
      }
      int wi = worker_register(e->thread_id);
      worker_note_op(wi, e->timestamp);
      if (g_ws[wi].recorded_cpu < 0)
        g_ws[wi].recorded_cpu = (int)(e->func_and_cpu & 0xFFFFFFFF);
      uint32_t slot = (uint32_t)next_slot++;
      op_t op = {.ts = e->timestamp,
                 .size = e->size,
                 .aux = 0,
                 .slot = slot,
                 .wait_gen = 0,
                 .out_gen = 1,
                 .func = func,
                 .kind = OP_ALLOC,
                 .access_desc_idx = (slot < g_access_desc_count) ? slot : ACCESS_DESC_NONE};
      opvec_push(&g_ws[wi].ops, op);
      omap_put(&live, e->result, slot, 1, (uint32_t)wi, e->size, op.ts);
      continue;
    }

    if (is_free) {
      g_total_frees++;
      if (e->address == 0) continue;
      uint32_t slot, gen, owner;
      uint64_t size, last_ts;
      int fw = worker_register(e->thread_id);
      worker_note_op(fw, e->timestamp);
      if (g_ws[fw].recorded_cpu < 0)
        g_ws[fw].recorded_cpu = (int)(e->func_and_cpu & 0xFFFFFFFF);
      if (!omap_take(&live, e->address, &slot, &gen, &owner, &size, &last_ts)) {
        g_unknown_frees++;
        op_t op = {.ts = e->timestamp,
                   .size = g_synthetic_size,
                   .aux = 0,
                   .slot = 0,
                   .wait_gen = 0,
                   .out_gen = 0,
                   .func = FREE_I,
                   .kind = OP_UNKNOWN_FREE,
                   .access_desc_idx = ACCESS_DESC_NONE};
        opvec_push(&g_ws[fw].ops, op);
        continue;
      }
      if ((uint32_t)fw == owner) g_local_frees++; else g_remote_frees++;
      op_t op = {.ts = max_u64(e->timestamp, last_ts),
                 .size = size,
                 .aux = 0,
                 .slot = slot,
                 .wait_gen = gen,
                 .out_gen = 0,
                 .func = FREE_I,
                 .kind = OP_FREE,
                 .access_desc_idx = (slot < g_access_desc_count) ? slot : ACCESS_DESC_NONE};
      opvec_push(&g_ws[fw].ops, op);
      continue;
    }

    if (is_realloc) {
      g_total_reallocs++;
      int rw = worker_register(e->thread_id);
      worker_note_op(rw, e->timestamp);
      if (g_ws[rw].recorded_cpu < 0)
        g_ws[rw].recorded_cpu = (int)(e->func_and_cpu & 0xFFFFFFFF);
      if (e->address == 0) {
        if (e->result == 0) {
          g_failed_reallocs++;
          continue;
        }
        if (next_slot > UINT32_MAX) {
          fprintf(stderr, "slot overflow\n");
          return 1;
        }
        uint32_t slot = (uint32_t)next_slot++;
        op_t op = {.ts = e->timestamp,
                   .size = e->size,
                   .aux = 0,
                   .slot = slot,
                   .wait_gen = 0,
                   .out_gen = 1,
                   .func = REALLOC_I,
                   .kind = OP_REALLOC_NULL,
                   .access_desc_idx = (slot < g_access_desc_count) ? slot : ACCESS_DESC_NONE};
        opvec_push(&g_ws[rw].ops, op);
        omap_put(&live, e->result, slot, 1, (uint32_t)rw, e->size, op.ts);
        continue;
      }

      uint32_t slot, gen, owner;
      uint64_t old_size, last_ts;
      if (!omap_take(&live, e->address, &slot, &gen, &owner, &old_size, &last_ts)) {
        g_unknown_reallocs++;
        if (e->result != 0) {
          if (next_slot > UINT32_MAX) {
            fprintf(stderr, "slot overflow\n");
            return 1;
          }
          uint32_t nslot = (uint32_t)next_slot++;
          op_t op = {.ts = e->timestamp,
                     .size = e->size,
                     .aux = 0,
                     .slot = nslot,
                     .wait_gen = 0,
                     .out_gen = 1,
                     .func = MALLOC_I,
                     .kind = OP_ALLOC,
                     .access_desc_idx = (nslot < g_access_desc_count) ? nslot : ACCESS_DESC_NONE};
          opvec_push(&g_ws[rw].ops, op);
          omap_put(&live, e->result, nslot, 1, (uint32_t)rw, e->size, op.ts);
        }
        continue;
      }

      if ((uint32_t)rw == owner) g_local_reallocs++; else g_remote_reallocs++;

      if (e->result == 0 && e->size != 0) {
        g_failed_reallocs++;
        omap_put(&live, e->address, slot, gen, owner, old_size, last_ts);
        continue;
      }

      uint32_t new_gen = gen + 1;
      op_t op = {.ts = max_u64(e->timestamp, last_ts),
                 .size = e->size,
                 .aux = 0,
                 .slot = slot,
                 .wait_gen = gen,
                 .out_gen = new_gen,
                 .func = REALLOC_I,
                 .kind = OP_REALLOC,
                 .access_desc_idx = (slot < g_access_desc_count) ? slot : ACCESS_DESC_NONE};
      opvec_push(&g_ws[rw].ops, op);

      if (e->result != 0) {
        omap_put(&live, e->result, slot, new_gen, (uint32_t)rw, e->size, op.ts);
      }
      continue;
    }
  }

  g_live_left_at_end = live.count;
  omap_free(&live);

  if (g_unknown_policy == UNKNOWN_ERROR && (g_unknown_frees || g_unknown_reallocs)) {
    fprintf(stderr, "strict/error mode: unknown_frees=%lu unknown_reallocs=%lu\n",
            g_unknown_frees, g_unknown_reallocs);
    return 3;
  }

  g_total_slots = (uint32_t)next_slot;
  g_slots = (slot_state_t *)xmap((size_t)g_total_slots * sizeof(slot_state_t));
  /* mmap zeroes atomics: gen=0, ptr=NULL */

  g_total_ops = 0;
  for (int t = 0; t < g_nthreads; t++) g_total_ops += g_ws[t].ops.len;

  if (g_min_ts == UINT64_MAX) g_min_ts = 0;
  finalize_worker_lifetimes();

  uint64_t replay_ms = 0;
  if (g_free_run)
    run_free_run_replay(&replay_ms);
  else
    run_application_like_replay(&replay_ms);

  long vmhwm_kb = read_vmhwm_kb();
  if (g_json_output) print_json(replay_ms, vmhwm_kb);
  if (g_show_stats || (!g_json_output && !g_show_stats)) print_stats(replay_ms, vmhwm_kb);

  munmap(map_base, map_len);
  return 0;
}
