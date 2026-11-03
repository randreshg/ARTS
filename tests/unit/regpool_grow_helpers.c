/* A grow that several threads find in flight, and what they do meanwhile.
 *
 * A grow reserves its slab's range and sets its placement, then opens the
 * range's population as chunks: every thread that finds the grow in flight
 * claims chunks and populates them, and the thread whose chunk ends the
 * round publishes the slab, or, when a chunk failed, gives the range up and
 * opens a round a step down.  Three legs, each in a fresh pool, checked by
 * counts:
 *
 *   stepped    (a) every thread that found the grow in flight populated part
 *              of the slab, every chunk was populated exactly once, one slab
 *              came of it, and the report counts the helpers and their
 *              chunks; (b) no thread slept while a chunk was unclaimed: a
 *              thread sleeps only before the population opens, or once
 *              every chunk is claimed;
 *   stepdown   one chunk of the first round fails to populate: that range is
 *              unmapped exactly once and no population call reaches it after
 *              the unmap, a round half the size is populated once per chunk,
 *              one slab of that size is published, and every thread's grow
 *              reports the slab;
 *   offnode    a kernel-placed slab, which carries no placement policy, is
 *              populated only by its grower and the node's own threads: a
 *              thread of another node that asks for the same node's grow
 *              populates none of it and sleeps through the round.
 *
 * The test holds the grower in its reservation (mprotect, wrapped at link
 * time) and holds population calls (madvise with MADV_POPULATE_WRITE,
 * wrapped); it counts the pool's sleeps (pthread_cond_wait, wrapped) and the
 * ranges given up (munmap, wrapped).  In the stepped leg every chunk's first
 * population call is held and released one at a time; after each release
 * the test waits until every thread is held in a population call, asleep in
 * the pool or returned, and records those counts with the number of chunks
 * begun, and (b) is judged over the records once the chunk count is known.
 * On a kernel without the advice the wrapper populates the range itself, so
 * the pool takes the same path on every host.
 *
 * The machine is declared (regpool_numa_sim.h): one node with room, or, in
 * the offnode leg, two nodes, both full by their free pages once the pool
 * has carved node 0, so the cascade's last step maps node 0's slab where the
 * kernel chooses on any host, a one-node one included.  A bounded wait only
 * limits how long the test waits for work that must finish; no assertion
 * depends on how fast anything runs.
 *
 * Whitebox unit test: no ARTS runtime, no ports, no config. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "arts/utils/stdatomic_compat.h"
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "arts/memory/regpool.h"
#include "regpool_numa_sim.h"

#ifndef MADV_POPULATE_WRITE
#define MADV_POPULATE_WRITE 23
#endif

#define MIB ((size_t)1 << 20)
#define SLAB_BYTES (32 * MIB)
#define NEXT_BYTES (64 * MIB) /* the node's next rung after the base slab */
#define ROOMY ((size_t)64 << 30)
#define NO_ROOM ((size_t)1 << 30) /* under the node headroom */
/* Machine availability that puts the stepdown leg's floor at the base slab,
 * below the 64 MiB rung. */
#define SMALL_MACHINE ((size_t)4 << 30)
#define EARLY 2 /* threads that arrive before the population opens */
#define LATE 3  /* threads that arrive once it is open */
#define THREADS (1 + EARLY + LATE)
#define MAX_CHUNKS 4096
#define MAX_STEPS (MAX_CHUNKS + 8)
#define MAX_RANGES 8
/* The stepdown leg's failing chunk of the first round. */
#define FAIL_CHUNK 5u
#define BLOCK MIB
#define MAX_BLOCKS 256
#define BOUND_MS 30000

static int g_failed;
#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      printf("FAIL regpool_grow_helpers: " __VA_ARGS__);                       \
      printf("\n");                                                            \
      g_failed = 1;                                                            \
    }                                                                          \
  } while (0)

/* Fatal: a thread is stuck or a precondition failed; exits without
 * joining. */
#define ABANDON(...)                                                           \
  do {                                                                         \
    printf("FAIL regpool_grow_helpers: " __VA_ARGS__);                         \
    printf("\n");                                                              \
    fflush(stdout);                                                            \
    _exit(1);                                                                  \
  } while (0)

typedef enum { LEG_STEPPED, LEG_STEPDOWN, LEG_OFFNODE } leg_t;
static _Atomic(int) g_leg;

/* The participant index of the calling thread, -1 for any other thread. */
static __thread int t_who = -1;

static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static bool sem_wait_ms(sem_t *s, unsigned ms) {
  uint64_t end = now_ms() + ms;
  while (sem_trywait(s) != 0) {
    if (now_ms() >= end)
      return false;
    usleep(1000);
  }
  return true;
}

static bool wait_for(bool (*cond)(void)) {
  uint64_t end = now_ms() + BOUND_MS;
  while (!cond()) {
    if (now_ms() >= end)
      return false;
    usleep(200);
  }
  return true;
}

/* ---- the ranges participants reserve ------------------------------------- */
/* In reservation order: base and length (written by the reserving thread
 * before it publishes the count), unmaps seen, and population calls per
 * chunk, after the unmap, and by the participant index. */
static _Atomic(uintptr_t) g_rbase[MAX_RANGES];
static _Atomic(size_t) g_rlen[MAX_RANGES];
static _Atomic(unsigned) g_rcount;
static _Atomic(unsigned) g_runmaps[MAX_RANGES];
static _Atomic(bool) g_runmapped[MAX_RANGES];
static _Atomic(unsigned) g_rafter[MAX_RANGES];
static _Atomic(unsigned) g_rseen[MAX_RANGES][64];
static _Atomic(unsigned) g_rby[MAX_RANGES][THREADS];
static pthread_mutex_t g_rlock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic(bool) g_failed_once;

static int range_of(uintptr_t p) {
  unsigned n = atomic_load_explicit(&g_rcount, memory_order_acquire);
  for (unsigned i = n; i-- > 0;) {
    uintptr_t b = atomic_load_explicit(&g_rbase[i], memory_order_relaxed);
    size_t l = atomic_load_explicit(&g_rlen[i], memory_order_relaxed);
    if (p >= b && p < b + l)
      return (int)i;
  }
  return -1;
}

/* ---- the grower's reservation, held -------------------------------------- */
static _Atomic(bool) g_reserve_armed;
static sem_t g_in_reserve, g_reserve_release;

int __real_mprotect(void *addr, size_t len, int prot);

int __wrap_mprotect(void *addr, size_t len, int prot) {
  if (t_who >= 0 && len >= SLAB_BYTES) {
    pthread_mutex_lock(&g_rlock);
    unsigned n = atomic_load_explicit(&g_rcount, memory_order_relaxed);
    if (n < MAX_RANGES) {
      atomic_store_explicit(&g_rbase[n], (uintptr_t)addr, memory_order_relaxed);
      atomic_store_explicit(&g_rlen[n], len, memory_order_relaxed);
      atomic_store_explicit(&g_rcount, n + 1, memory_order_release);
    }
    pthread_mutex_unlock(&g_rlock);
  }
  if (t_who == 0 && len == NEXT_BYTES &&
      atomic_exchange_explicit(&g_reserve_armed, false, memory_order_acq_rel)) {
    sem_post(&g_in_reserve);
    while (sem_wait(&g_reserve_release) != 0 && errno == EINTR)
      ;
  }
  return __real_mprotect(addr, len, prot);
}

int __real_munmap(void *addr, size_t len);

int __wrap_munmap(void *addr, size_t len) {
  int r = range_of((uintptr_t)addr);
  if (r >= 0 &&
      atomic_load_explicit(&g_rbase[r], memory_order_relaxed) ==
          (uintptr_t)addr &&
      atomic_load_explicit(&g_rlen[r], memory_order_relaxed) == len) {
    atomic_fetch_add_explicit(&g_runmaps[r], 1u, memory_order_relaxed);
    atomic_store_explicit(&g_runmapped[r], true, memory_order_release);
  }
  return __real_munmap(addr, len);
}

/* ---- the population calls ------------------------------------------------ */
static _Atomic(bool) g_pop_armed;
static sem_t g_pop_release;
/* Chunks begun and holds released: a held chunk has begun and not left. */
static _Atomic(unsigned) g_pop_begun, g_pop_left;
static _Atomic(unsigned) g_pop_by[THREADS];
static _Atomic(size_t) g_chunk_len;
/* Per chunk of the stepped leg's slab: the thread that began it (-1 none
 * yet), and how many threads began it.  A call for a chunk its own thread
 * began is that thread's retry of an interrupted population. */
static _Atomic(int) g_chunk_owner[MAX_CHUNKS];
static _Atomic(unsigned) g_chunk_seen[MAX_CHUNKS];
static _Atomic(unsigned) g_pop_stray;
/* Threads held in a population call, asleep in the pool or returned: one
 * count, so a thread moving between two of them is never seen in both.  A
 * thread leaves it before its hold counts as left. */
static _Atomic(unsigned) g_settled;

int __real_madvise(void *addr, size_t len, int advice);

static void pop_hold(void) {
  atomic_fetch_add_explicit(&g_pop_begun, 1u, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_settled, 1u, memory_order_acq_rel);
  while (sem_wait(&g_pop_release) != 0 && errno == EINTR)
    ;
  atomic_fetch_sub_explicit(&g_settled, 1u, memory_order_acq_rel);
  atomic_fetch_add_explicit(&g_pop_left, 1u, memory_order_acq_rel);
}

static void stepped_call(void *addr, size_t len) {
  size_t expect = 0;
  (void)atomic_compare_exchange_strong_explicit(
      &g_chunk_len, &expect, len, memory_order_relaxed, memory_order_relaxed);
  size_t chunk = atomic_load_explicit(&g_chunk_len, memory_order_relaxed);
  uintptr_t off =
      (uintptr_t)addr - atomic_load_explicit(&g_rbase[0], memory_order_relaxed);
  if (len != chunk || off % chunk != 0 || off >= NEXT_BYTES ||
      off / chunk >= MAX_CHUNKS) {
    atomic_fetch_add_explicit(&g_pop_stray, 1u, memory_order_relaxed);
    return;
  }
  size_t i = off / chunk;
  int none = -1;
  if (!atomic_compare_exchange_strong_explicit(&g_chunk_owner[i], &none, t_who,
                                               memory_order_relaxed,
                                               memory_order_relaxed) &&
      none == t_who)
    return;
  atomic_fetch_add_explicit(&g_chunk_seen[i], 1u, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_pop_by[t_who], 1u, memory_order_relaxed);
  pop_hold();
}

/* Records a call into a reserved range; true when it is to fail. */
static bool range_call(void *addr, size_t len) {
  int r = range_of((uintptr_t)addr);
  if (r < 0 || len != 2 * MIB) {
    atomic_fetch_add_explicit(&g_pop_stray, 1u, memory_order_relaxed);
    return false;
  }
  if (atomic_load_explicit(&g_runmapped[r], memory_order_acquire))
    atomic_fetch_add_explicit(&g_rafter[r], 1u, memory_order_relaxed);
  size_t i = ((uintptr_t)addr -
              atomic_load_explicit(&g_rbase[r], memory_order_relaxed)) /
             (2 * MIB);
  if (i < 64)
    atomic_fetch_add_explicit(&g_rseen[r][i], 1u, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_rby[r][t_who], 1u, memory_order_relaxed);
  return atomic_load_explicit(&g_leg, memory_order_relaxed) == LEG_STEPDOWN &&
         r == 0 && i == FAIL_CHUNK &&
         !atomic_exchange_explicit(&g_failed_once, true, memory_order_relaxed);
}

int __wrap_madvise(void *addr, size_t len, int advice) {
  if (advice != MADV_POPULATE_WRITE)
    return __real_madvise(addr, len, advice);
  if (t_who >= 0 && atomic_load_explicit(&g_pop_armed, memory_order_acquire)) {
    int leg = atomic_load_explicit(&g_leg, memory_order_relaxed);
    if (leg == LEG_STEPPED) {
      stepped_call(addr, len);
    } else if (range_call(addr, len)) {
      errno = ENOMEM;
      return -1;
    } else if (leg == LEG_OFFNODE && t_who == 0 &&
               atomic_load_explicit(&g_pop_begun, memory_order_relaxed) == 0) {
      pop_hold();
    }
  }
  int rc = __real_madvise(addr, len, advice);
  if (rc != 0 && errno == EINVAL) {
    long page = sysconf(_SC_PAGESIZE);
    for (size_t off = 0; off < len; off += (size_t)page)
      ((volatile unsigned char *)addr)[off] = 0;
    rc = 0;
  }
  return rc;
}

/* ---- sleeps in the pool -------------------------------------------------- */
static _Atomic(int) g_asleep;

int __real_pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);

int __wrap_pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
  bool mine = t_who >= 0;
  if (mine) {
    atomic_fetch_add_explicit(&g_asleep, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_settled, 1u, memory_order_acq_rel);
  }
  int rc = __real_pthread_cond_wait(c, m);
  if (mine) {
    atomic_fetch_sub_explicit(&g_settled, 1u, memory_order_acq_rel);
    atomic_fetch_sub_explicit(&g_asleep, 1, memory_order_relaxed);
  }
  return rc;
}

/* ---- the report ---------------------------------------------------------- */
typedef struct {
  unsigned grows, waits, sleeps, helpers, help_chunks, chunks, mapped;
  unsigned slabs, unplaced;
} counts_t;

static unsigned field(const char *line, const char *key) {
  const char *nl = strchr(line, '\n');
  const char *k = strstr(line, key);
  unsigned v;
  if (k == NULL || (nl != NULL && k > nl) ||
      sscanf(k + strlen(key), "%u", &v) != 1)
    ABANDON("the report lacks%s\n", key);
  return v;
}

static counts_t counts(void) {
  char *buf = NULL;
  size_t len = 0;
  FILE *f = open_memstream(&buf, &len);
  if (f == NULL)
    abort();
  arts_regpool_report(f);
  fclose(f);
  const char *line = strstr(buf, "[REGPOOL] node 0:");
  const char *total = strstr(buf, "[REGPOOL] total:");
  if (line == NULL || total == NULL)
    ABANDON("the report has no node 0 or total line:\n%s", buf);
  counts_t c = {field(line, " grows="),       field(line, " waits="),
                field(line, " sleeps="),      field(line, " helpers="),
                field(line, " help_chunks="), field(line, " chunks="),
                field(line, " mapped="),      field(total, " slabs="),
                field(total, " unplaced=")};
  free(buf);
  return c;
}

/* ---- participants -------------------------------------------------------- */
typedef struct {
  int who;
  int node;
  bool grew;
} part_t;

static _Atomic(unsigned) g_returned;

static void *participant(void *arg) {
  part_t *p = arg;
  t_who = p->who;
  regpool_sim_set_thread_node(p->node);
  p->grew = arts_regpool_grow(0);
  atomic_fetch_add_explicit(&g_returned, 1u, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_settled, 1u, memory_order_acq_rel);
  return NULL;
}

static unsigned held(void) {
  return atomic_load_explicit(&g_pop_begun, memory_order_acquire) -
         atomic_load_explicit(&g_pop_left, memory_order_acquire);
}

static int asleep(void) {
  return atomic_load_explicit(&g_asleep, memory_order_acquire);
}

static unsigned returned(void) {
  return atomic_load_explicit(&g_returned, memory_order_acquire);
}

/* Fresh state for a leg.  No participant of an earlier leg is alive. */
static void reset_state(leg_t leg) {
  atomic_store_explicit(&g_leg, leg, memory_order_relaxed);
  atomic_store_explicit(&g_rcount, 0, memory_order_relaxed);
  for (unsigned r = 0; r < MAX_RANGES; r++) {
    atomic_store_explicit(&g_runmaps[r], 0, memory_order_relaxed);
    atomic_store_explicit(&g_runmapped[r], false, memory_order_relaxed);
    atomic_store_explicit(&g_rafter[r], 0, memory_order_relaxed);
    for (unsigned i = 0; i < 64; i++)
      atomic_store_explicit(&g_rseen[r][i], 0, memory_order_relaxed);
    for (unsigned t = 0; t < THREADS; t++)
      atomic_store_explicit(&g_rby[r][t], 0, memory_order_relaxed);
  }
  atomic_store_explicit(&g_failed_once, false, memory_order_relaxed);
  atomic_store_explicit(&g_pop_begun, 0, memory_order_relaxed);
  atomic_store_explicit(&g_pop_left, 0, memory_order_relaxed);
  atomic_store_explicit(&g_pop_stray, 0, memory_order_relaxed);
  atomic_store_explicit(&g_returned, 0, memory_order_relaxed);
  atomic_store_explicit(&g_settled, 0, memory_order_relaxed);
  atomic_store_explicit(&g_asleep, 0, memory_order_relaxed);
  for (unsigned t = 0; t < THREADS; t++)
    atomic_store_explicit(&g_pop_by[t], 0, memory_order_relaxed);
  sem_init(&g_in_reserve, 0, 0);
  sem_init(&g_reserve_release, 0, 0);
  sem_init(&g_pop_release, 0, 0);
}

/* ---- leg 1: stepped ------------------------------------------------------ */
/* After the g_released-th hold is let go: every thread is held in a
 * population call, asleep in the pool or returned, and that hold has left.
 * A thread woken by the grow's end still counts as asleep until it holds
 * the pool's lock again, so the end is reached only once every thread has
 * returned; before the end nothing wakes a sleeper, and nothing moves until
 * the next release. */
static unsigned g_released;

static bool quiescent(void) {
  unsigned r = returned();
  return atomic_load_explicit(&g_pop_left, memory_order_acquire) ==
             g_released &&
         atomic_load_explicit(&g_settled, memory_order_acquire) == THREADS &&
         (r == 0 || r == THREADS);
}

static unsigned g_target;

static bool held_target(void) { return held() == g_target; }

typedef struct {
  unsigned begun, held, asleep, returned, sleeps;
} step_t;

static step_t g_steps[MAX_STEPS];

static void leg_stepped(void) {
  regpool_sim_declare(1, REGPOOL_SIM_FREE);
  regpool_sim_set_node_free(0, ROOMY);
  regpool_sim_set_thread_node(0);
  arts_regpool_set_topology(1, 0x1, NULL);
  if (!arts_regpool_init(NULL, NULL, SLAB_BYTES, 1))
    ABANDON("stepped: init failed");
  counts_t c0 = counts();

  reset_state(LEG_STEPPED);
  for (unsigned i = 0; i < MAX_CHUNKS; i++) {
    atomic_store_explicit(&g_chunk_owner[i], -1, memory_order_relaxed);
    atomic_store_explicit(&g_chunk_seen[i], 0, memory_order_relaxed);
  }
  atomic_store_explicit(&g_reserve_armed, true, memory_order_release);
  atomic_store_explicit(&g_pop_armed, true, memory_order_release);

  part_t parts[THREADS];
  pthread_t th[THREADS];
  for (int i = 0; i < THREADS; i++)
    parts[i] = (part_t){.who = i, .node = 0, .grew = false};

  /* The grower, held in its reservation: the grow is in flight and no
   * round of population is open yet. */
  pthread_create(&th[0], NULL, participant, &parts[0]);
  if (!sem_wait_ms(&g_in_reserve, BOUND_MS))
    ABANDON("stepped: the grower never reserved a %zu MiB slab",
            NEXT_BYTES / MIB);

  /* The early threads find the grow in flight with nothing to populate,
   * and sleep. */
  for (int i = 1; i <= EARLY; i++)
    pthread_create(&th[i], NULL, participant, &parts[i]);
  uint64_t end = now_ms() + BOUND_MS;
  while (asleep() != EARLY) {
    if (now_ms() >= end)
      ABANDON("stepped: %d of %d early threads asleep while the grower "
              "reserves",
              asleep(), EARLY);
    usleep(200);
  }
  counts_t c1 = counts();
  CHECK(c1.sleeps - c0.sleeps == EARLY && c1.waits - c0.waits == EARLY,
        "stepped: before the population opened: %u sleeps, %u waits; "
        "expected %d",
        c1.sleeps - c0.sleeps, c1.waits - c0.waits, EARLY);

  /* The population opens: the grower and the early threads each hold a
   * chunk, then the late threads join and each hold one more. */
  sem_post(&g_reserve_release);
  g_target = 1 + EARLY;
  if (!wait_for(held_target) || asleep() != 0)
    ABANDON("stepped: after the population opened %u threads populate and "
            "%d sleep; expected %d populating",
            held(), asleep(), 1 + EARLY);
  for (int i = 1 + EARLY; i < THREADS; i++)
    pthread_create(&th[i], NULL, participant, &parts[i]);
  g_target = THREADS;
  if (!wait_for(held_target) || asleep() != 0)
    ABANDON("stepped: after the late threads joined %u threads populate and "
            "%d sleep; expected %d populating",
            held(), asleep(), THREADS);

  /* One population call at a time, each followed by quiescence. */
  unsigned nsteps = 0;
  g_released = 0;
  for (;;) {
    if (nsteps == MAX_STEPS)
      ABANDON("stepped: more than %d population calls for one slab", MAX_STEPS);
    g_steps[nsteps++] =
        (step_t){atomic_load_explicit(&g_pop_begun, memory_order_acquire),
                 held(), (unsigned)asleep(), returned(), counts().sleeps};
    if (returned() == THREADS)
      break;
    sem_post(&g_pop_release);
    g_released++;
    if (!wait_for(quiescent))
      ABANDON("stepped: after %u population calls let go: %u populating, "
              "%d asleep, %u returned, of %d",
              g_released, held(), asleep(), returned(), THREADS);
  }
  for (int i = 0; i < THREADS; i++)
    pthread_join(th[i], NULL);
  atomic_store_explicit(&g_pop_armed, false, memory_order_release);
  counts_t c2 = counts();

  unsigned k = atomic_load_explicit(&g_pop_begun, memory_order_acquire);
  size_t chunk = atomic_load_explicit(&g_chunk_len, memory_order_relaxed);
  CHECK(chunk != 0 && k * chunk == NEXT_BYTES && k > THREADS,
        "stepped: the %zu MiB slab was populated in %u calls of %zu bytes; "
        "expected more chunks than the %d threads, covering it",
        NEXT_BYTES / MIB, k, chunk, THREADS);
  CHECK(atomic_load_explicit(&g_pop_stray, memory_order_relaxed) == 0,
        "stepped: %u population calls outside the slab's chunks",
        atomic_load_explicit(&g_pop_stray, memory_order_relaxed));
  for (unsigned i = 0; chunk != 0 && i < NEXT_BYTES / chunk; i++) {
    unsigned n = atomic_load_explicit(&g_chunk_seen[i], memory_order_relaxed);
    CHECK(n == 1, "stepped: chunk %u populated %u times", i, n);
  }

  /* (a) */
  for (int i = 0; i < THREADS; i++) {
    unsigned n = atomic_load_explicit(&g_pop_by[i], memory_order_relaxed);
    CHECK(n >= 1, "stepped: thread %d (%s) populated no chunk", i,
          i == 0       ? "the grower"
          : i <= EARLY ? "early"
                       : "late");
    CHECK(parts[i].grew, "stepped: thread %d's grow returned false", i);
  }
  unsigned by_grower = atomic_load_explicit(&g_pop_by[0], memory_order_relaxed);
  CHECK(c2.grows - c0.grows == 1, "stepped: %u slabs grown, expected one",
        c2.grows - c0.grows);
  CHECK(c2.helpers - c0.helpers == THREADS - 1,
        "stepped: the report counts %u helpers, expected %d",
        c2.helpers - c0.helpers, THREADS - 1);
  CHECK(c2.help_chunks - c0.help_chunks == k - by_grower &&
            c2.chunks - c0.chunks == k,
        "stepped: the report counts %u helped chunks of %u, expected %u of "
        "%u",
        c2.help_chunks - c0.help_chunks, c2.chunks - c0.chunks, k - by_grower,
        k);

  /* (b) Until every chunk is claimed, every thread populates: none sleeps
   * and the pool counts no sleep beyond the early threads'. */
  unsigned unclaimed_steps = 0;
  for (unsigned s = 0; s < nsteps; s++) {
    const step_t *st = &g_steps[s];
    if (st->begun >= k)
      continue;
    unclaimed_steps++;
    CHECK(st->held == THREADS && st->asleep == 0 && st->returned == 0 &&
              st->sleeps == c1.sleeps,
          "stepped: step %u (%u of %u chunks claimed): %u populating, %u "
          "asleep, %u returned, %u sleeps counted (%u before the population "
          "opened)",
          s, st->begun, k, st->held, st->asleep, st->returned, st->sleeps,
          c1.sleeps);
  }
  CHECK(unclaimed_steps == k - THREADS,
        "stepped: %u steps observed with chunks unclaimed, expected %u",
        unclaimed_steps, k - THREADS);
  CHECK(regpool_sim_consulted(),
        "stepped: the declared machine was not consulted");
  arts_regpool_cleanup();
}

/* ---- leg 2: stepdown ----------------------------------------------------- */
static void leg_stepdown(void) {
  regpool_sim_declare(1, REGPOOL_SIM_FREE);
  regpool_sim_set_node_free(0, ROOMY);
  regpool_sim_set_machine_available(SMALL_MACHINE);
  regpool_sim_set_thread_node(0);
  arts_regpool_set_topology(1, 0x1, NULL);
  if (!arts_regpool_init(NULL, NULL, SLAB_BYTES, 1))
    ABANDON("stepdown: init failed");
  counts_t c0 = counts();

  reset_state(LEG_STEPDOWN);
  atomic_store_explicit(&g_reserve_armed, true, memory_order_release);
  atomic_store_explicit(&g_pop_armed, true, memory_order_release);
  part_t parts[THREADS];
  pthread_t th[THREADS];
  for (int i = 0; i < THREADS; i++)
    parts[i] = (part_t){.who = i, .node = 0, .grew = false};
  pthread_create(&th[0], NULL, participant, &parts[0]);
  if (!sem_wait_ms(&g_in_reserve, BOUND_MS))
    ABANDON("stepdown: the grower never reserved a %zu MiB slab",
            NEXT_BYTES / MIB);
  for (int i = 1; i < THREADS; i++)
    pthread_create(&th[i], NULL, participant, &parts[i]);
  uint64_t end = now_ms() + BOUND_MS;
  while (asleep() != THREADS - 1) {
    if (now_ms() >= end)
      ABANDON("stepdown: %d of %d threads asleep while the grower reserves",
              asleep(), THREADS - 1);
    usleep(200);
  }
  sem_post(&g_reserve_release);
  for (int i = 0; i < THREADS; i++)
    pthread_join(th[i], NULL);
  atomic_store_explicit(&g_pop_armed, false, memory_order_release);
  counts_t c1 = counts();

  unsigned nr = atomic_load_explicit(&g_rcount, memory_order_acquire);
  size_t l0 = atomic_load_explicit(&g_rlen[0], memory_order_relaxed);
  size_t l1 = atomic_load_explicit(&g_rlen[1], memory_order_relaxed);
  CHECK(atomic_load_explicit(&g_failed_once, memory_order_relaxed),
        "stepdown: the failing chunk was never populated");
  CHECK(nr == 2 && l0 == NEXT_BYTES && l1 == NEXT_BYTES / 2,
        "stepdown: %u ranges reserved (%zu, %zu MiB); expected the %zu MiB "
        "round and then one of %zu MiB",
        nr, l0 >> 20, l1 >> 20, NEXT_BYTES / MIB, NEXT_BYTES / 2 / MIB);
  CHECK(atomic_load_explicit(&g_runmaps[0], memory_order_relaxed) == 1 &&
            atomic_load_explicit(&g_rafter[0], memory_order_relaxed) == 0,
        "stepdown: the failed range was unmapped %u times and took %u "
        "population calls after its unmap; expected once and none",
        atomic_load_explicit(&g_runmaps[0], memory_order_relaxed),
        atomic_load_explicit(&g_rafter[0], memory_order_relaxed));
  for (unsigned i = 0; i < NEXT_BYTES / (2 * MIB); i++) {
    unsigned n = atomic_load_explicit(&g_rseen[0][i], memory_order_relaxed);
    CHECK(n <= 1, "stepdown: chunk %u of the failed range populated %u times",
          i, n);
  }
  CHECK(nr < 2 ||
            atomic_load_explicit(&g_runmaps[1], memory_order_relaxed) == 0,
        "stepdown: the published range was unmapped");
  for (unsigned i = 0; nr >= 2 && i < l1 / (2 * MIB) && i < 64; i++) {
    unsigned n = atomic_load_explicit(&g_rseen[1][i], memory_order_relaxed);
    CHECK(n == 1, "stepdown: chunk %u of the second range populated %u times",
          i, n);
  }
  CHECK(atomic_load_explicit(&g_pop_stray, memory_order_relaxed) == 0,
        "stepdown: %u population calls outside the reserved ranges",
        atomic_load_explicit(&g_pop_stray, memory_order_relaxed));
  unsigned ntrue = 0;
  for (int i = 0; i < THREADS; i++)
    ntrue += parts[i].grew;
  CHECK(c1.grows == c0.grows + 1 && c1.slabs == c0.slabs + 1 &&
            c1.mapped == c0.mapped + NEXT_BYTES / 2 / MIB && ntrue == THREADS,
        "stepdown: grows %u -> %u, slabs %u -> %u, mapped %u -> %u MiB, %u "
        "of %d grows true; expected one %zu MiB slab and every grow true",
        c0.grows, c1.grows, c0.slabs, c1.slabs, c0.mapped, c1.mapped, ntrue,
        THREADS, NEXT_BYTES / 2 / MIB);
  arts_regpool_cleanup();
}

/* ---- leg 3: offnode ------------------------------------------------------ */
/* The node-0 thread allocates until its cascade's kernel-placed grow. */
static _Atomic(bool) g_alloc_done;
static void *allocs[MAX_BLOCKS];
static unsigned nallocs;

static void *node0_allocator(void *arg) {
  (void)arg;
  t_who = 0;
  regpool_sim_set_thread_node(0);
  counts_t c0 = counts();
  while (nallocs < MAX_BLOCKS && counts().grows == c0.grows)
    allocs[nallocs++] = arts_regpool_alloc_aligned(BLOCK, 64);
  atomic_store_explicit(&g_alloc_done, true, memory_order_release);
  return NULL;
}

static bool alloc_done(void) {
  return atomic_load_explicit(&g_alloc_done, memory_order_acquire);
}

static void leg_offnode(void) {
  regpool_sim_declare(2, REGPOOL_SIM_FREE);
  regpool_sim_set_node_free(0, ROOMY);
  regpool_sim_set_node_free(1, ROOMY);
  regpool_sim_set_thread_node(0);
  arts_regpool_set_topology(2, 0x3, NULL);
  if (!arts_regpool_init(NULL, NULL, SLAB_BYTES, 2))
    ABANDON("offnode: init failed");
  regpool_sim_set_node_free(0, NO_ROOM);
  regpool_sim_set_node_free(1, NO_ROOM);
  counts_t c0 = counts();

  reset_state(LEG_OFFNODE);
  atomic_store_explicit(&g_alloc_done, false, memory_order_relaxed);
  nallocs = 0;
  atomic_store_explicit(&g_reserve_armed, true, memory_order_release);
  atomic_store_explicit(&g_pop_armed, true, memory_order_release);
  pthread_t tx, ty;
  pthread_create(&tx, NULL, node0_allocator, NULL);
  if (!sem_wait_ms(&g_in_reserve, BOUND_MS))
    ABANDON("offnode: node 0's kernel-placed grow never reserved its slab");

  /* A node-1 thread asks for node 0's grow while it is in flight. */
  part_t y = {.who = 1, .node = 1, .grew = false};
  pthread_create(&ty, NULL, participant, &y);
  uint64_t end = now_ms() + BOUND_MS;
  while (asleep() != 1) {
    if (now_ms() >= end)
      ABANDON("offnode: the node-1 thread is not asleep on node 0's grow");
    usleep(200);
  }
  unsigned sleeps_before = counts().sleeps;
  sem_post(&g_reserve_release);
  /* The round opens: the grower holds its first chunk, and the node-1
   * thread, woken by the open, finds nothing it may populate and sleeps
   * again. */
  end = now_ms() + BOUND_MS;
  while (
      !(held() == 1 && asleep() == 1 && counts().sleeps == sleeps_before + 1)) {
    if (now_ms() >= end) {
      CHECK(false,
            "offnode: after the round opened the grower holds %u chunks, "
            "%d threads sleep and %u sleeps were counted since; expected the "
            "node-1 thread asleep again",
            held(), asleep(), counts().sleeps - sleeps_before);
      break;
    }
    usleep(200);
  }
  sem_post(&g_pop_release);
  if (!wait_for(alloc_done))
    ABANDON("offnode: node 0's allocations did not complete");
  pthread_join(tx, NULL);
  pthread_join(ty, NULL);
  atomic_store_explicit(&g_pop_armed, false, memory_order_release);
  counts_t c1 = counts();

  unsigned nr = atomic_load_explicit(&g_rcount, memory_order_acquire);
  unsigned by_x = 0, by_y = 0;
  for (unsigned r = 0; r < nr && r < MAX_RANGES; r++) {
    by_x += atomic_load_explicit(&g_rby[r][0], memory_order_relaxed);
    by_y += atomic_load_explicit(&g_rby[r][1], memory_order_relaxed);
  }
  CHECK(nr == 1 && c1.unplaced > c0.unplaced && c1.grows == c0.grows + 1,
        "offnode: %u ranges, kernel-placed %u -> %u MiB, grows %u -> %u; "
        "expected one kernel-placed slab",
        nr, c0.unplaced, c1.unplaced, c0.grows, c1.grows);
  CHECK(by_y == 0 && by_x == NEXT_BYTES / (2 * MIB),
        "offnode: the kernel-placed slab's chunks were populated %u times by "
        "its grower and %u times by the node-1 thread; expected all %zu by "
        "the grower",
        by_x, by_y, NEXT_BYTES / (2 * MIB));
  CHECK(y.grew, "offnode: the node-1 thread's grow returned false");
  for (unsigned i = 0; i < nallocs; i++)
    arts_regpool_free(allocs[i]);
  arts_regpool_cleanup();
}

int main(void) {
  leg_stepped();
  leg_stepdown();
  leg_offnode();
  if (g_failed)
    return 1;
  printf("PASS regpool_grow_helpers: %d threads populated a %zu MiB slab "
         "without sleeping while a chunk was unclaimed; a failed chunk's "
         "range was given up once and a round half its size published; a "
         "kernel-placed slab was populated by its grower alone\n",
         THREADS, NEXT_BYTES / MIB);
  return 0;
}
