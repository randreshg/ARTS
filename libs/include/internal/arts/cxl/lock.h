/******************************************************************************
 * Copyright 2019 Battelle Memorial Institute
 * Licensed under the Apache License, Version 2.0
 ******************************************************************************/
#ifndef ARTS_CXL_LOCK_H
#define ARTS_CXL_LOCK_H
#ifdef __cplusplus
extern "C" {
#endif

#include <inttypes.h>
#include <pthread.h>
#include <unistd.h>

#include "arts/cxl/shared_alloc.h"
#include "arts/cxl/wrapper.h"

/* ── Cache-line padded value ────────────────────────────────────────────────
 */

typedef struct {
  volatile int64_t value;
  char pad[56];
} arts_cxl_cache_line_t;

/* ── Peterson lock (2-process, CXL-safe) ────────────────────────────────────
 */

typedef union {
  struct {
    arts_cxl_cache_line_t flag[2];
    arts_cxl_cache_line_t turn;
    arts_cxl_cache_line_t owner;
  } lock;
  uint64_t pad[((sizeof(arts_cxl_cache_line_t) * 4 + CACHELINE_SIZE - 1) /
                CACHELINE_SIZE) *
               (CACHELINE_SIZE / sizeof(uint64_t))];
} arts_cxl_peterson_lock_t;

/* ── Tournament lock (N-process tree of Peterson locks) ─────────────────────
 */

typedef union {
  struct {
    uint64_t num_procs;
    uint64_t k; /* ceil(log2(num_procs)) */
    pthread_mutex_t local_lock;
    arts_cxl_peterson_lock_t *locks;
  } lock;
  uint64_t pad[((sizeof(uint64_t) * 2 + sizeof(pthread_mutex_t) +
                 sizeof(arts_cxl_peterson_lock_t *) + CACHELINE_SIZE - 1) /
                CACHELINE_SIZE) *
               (CACHELINE_SIZE / sizeof(uint64_t))];
} arts_cxl_tournament_lock_t;

/* ── Peterson lock operations ───────────────────────────────────────────────
 */

static inline void arts_cxl_peterson_lock_init(arts_cxl_peterson_lock_t *l) {
  l->lock.flag[0].value = 0;
  l->lock.flag[1].value = 0;
  l->lock.turn.value = 0;
  l->lock.owner.value = -1;
  FLUSH_FENCE_PRODUCER(l, sizeof(arts_cxl_peterson_lock_t));
}

static inline void arts_cxl_peterson_lock_acquire(arts_cxl_peterson_lock_t *l,
                                                  unsigned int id) {
  unsigned int delay = 1;
  unsigned int other = 1 - id;

  l->lock.flag[id].value = 1;
  FLUSH_FENCE_PRODUCER(&l->lock.flag[id], sizeof(arts_cxl_cache_line_t));

  COMPILER_DO_NOT_REORDER_WRITES();

  l->lock.turn.value = other;
  FLUSH_FENCE_PRODUCER(&l->lock.turn, sizeof(arts_cxl_cache_line_t));

  HW_MEMORY_FENCE();

  FLUSH_FENCE_CONSUMER(l, sizeof(arts_cxl_peterson_lock_t));

  while (l->lock.flag[other].value && l->lock.turn.value == other) {
    usleep(delay);
    if (delay < 1000000) {
      delay *= 2;
    }
    FLUSH_FENCE_CONSUMER(l, sizeof(arts_cxl_peterson_lock_t));
  }
  COMPILER_DO_NOT_REORDER_WRITES();
  l->lock.owner.value = id;
  FLUSH_FENCE_PRODUCER(&l->lock.owner, sizeof(arts_cxl_cache_line_t));
}

/* The releaser names its own side; it is never read back from `owner`.  On a
 * fabric with no hardware coherence a release that wrote two lines under one
 * flush let them land in address order -- `flag` (line 0) before `owner`
 * (line 3) -- so a waiter could enter on the cleared flag, publish itself as
 * owner, and then have the releaser's late `owner = -1` overwrite it.  Its own
 * release then read id -1 and cleared flag[-1], a line BEFORE the lock (for
 * locks[0], the device allocator's header), and walked the tree off its path:
 * a corruption that surfaced later as a segfault in GLOBAL_FREE at teardown.
 * Clearing the one line this side owns has no second line to race.  `owner`
 * is left naming the last acquirer, informational only. */
static inline void arts_cxl_peterson_lock_release(arts_cxl_peterson_lock_t *l,
                                                  unsigned int id) {
  COMPILER_DO_NOT_REORDER_WRITES();
  HW_MEMORY_FENCE(); /* critical-section writes drain before the flag clears */
  l->lock.flag[id].value = 0;
  FLUSH_FENCE_PRODUCER(&l->lock.flag[id], sizeof(arts_cxl_cache_line_t));
}

/* ── Tournament lock operations ─────────────────────────────────────────────
 */

static inline arts_cxl_tournament_lock_t *
arts_cxl_tournament_lock_new(unsigned int num_procs) {
  arts_cxl_tournament_lock_t *tl = (arts_cxl_tournament_lock_t *)GLOBAL_MALLOC(
      sizeof(arts_cxl_tournament_lock_t));
  unsigned int x = num_procs;
  uint64_t k = 0;
  while (x >>= 1) {
    k++;
  }
  if ((1U << k) < num_procs) {
    k++;
  }
  tl->lock.num_procs = (1U << k);
  tl->lock.k = k;
  pthread_mutex_init(&tl->lock.local_lock, NULL);

  unsigned int num_locks = tl->lock.num_procs - 1;
  tl->lock.locks = (arts_cxl_peterson_lock_t *)GLOBAL_MALLOC(
      sizeof(arts_cxl_peterson_lock_t) * num_locks);
  for (unsigned int i = 0; i < num_locks; i++) {
    arts_cxl_peterson_lock_init(&tl->lock.locks[i]);
  }

  FLUSH_FENCE_PRODUCER(tl, sizeof(arts_cxl_tournament_lock_t));
  FLUSH_FENCE_PRODUCER(tl->lock.locks,
                       sizeof(arts_cxl_peterson_lock_t) * num_locks);
  return tl;
}

static inline void
arts_cxl_tournament_lock_delete(arts_cxl_tournament_lock_t *tl) {
  pthread_mutex_destroy(&tl->lock.local_lock);
  GLOBAL_FREE(tl->lock.locks);
  GLOBAL_FREE(tl);
}

static inline void
arts_cxl_tournament_lock_acquire(arts_cxl_tournament_lock_t *tl,
                                 pthread_mutex_t *local_lock, unsigned int id) {
  pthread_mutex_lock(local_lock);
  unsigned int node_id = id + (tl->lock.num_procs - 1);
  for (unsigned int i = 0; i < tl->lock.k; i++) {
    unsigned int pid = (node_id + 1) % 2;
    node_id = (node_id - 1) / 2;
    arts_cxl_peterson_lock_acquire(&tl->lock.locks[node_id], pid);
  }
}

/* The path a rank climbs is a function of its id alone, so the release
 * recomputes it rather than following each node's `owner` down from the root:
 * `owner` is a shared line another rank may have overwritten (see
 * arts_cxl_peterson_lock_release), and a stale one sends the walk off the
 * path and the release onto locks this rank never took.  Released root first,
 * the reverse of acquisition, as before. */
static inline void
arts_cxl_tournament_lock_release(arts_cxl_tournament_lock_t *tl,
                                 pthread_mutex_t *local_lock, unsigned int id) {
  unsigned int nodes[64];
  unsigned int pids[64];
  unsigned int k = (unsigned int)tl->lock.k;
  unsigned int node_id = id + (tl->lock.num_procs - 1);
  for (unsigned int i = 0; i < k; i++) {
    pids[i] = (node_id + 1) % 2;
    node_id = (node_id - 1) / 2;
    nodes[i] = node_id;
  }
  for (unsigned int i = k; i-- > 0;) {
    arts_cxl_peterson_lock_release(&tl->lock.locks[nodes[i]], pids[i]);
  }
  pthread_mutex_unlock(local_lock);
}

#ifdef __cplusplus
}
#endif
#endif /* ARTS_CXL_LOCK_H */
