/******************************************************************************
 * Copyright 2019 Battelle Memorial Institute
 * Licensed under the Apache License, Version 2.0
 ******************************************************************************/
#ifndef ARTS_CXL_LOCK_H
#define ARTS_CXL_LOCK_H
#ifdef __cplusplus
extern "C" {
#endif

#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <unistd.h>

#include "arts/cxl/wrapper.h"
#include "arts/utils/stdatomic_compat.h"

/* ── Cache-line padded value ────────────────────────────────────────────────
 */

typedef struct {
  _Atomic(int64_t) value;
  char pad[56];
} arts_cxl_cache_line_t;

/* The lock's words are shared by processes that map the store at one
 * address, which C11 orders only for lock-free, address-free atomics.  C names
 * int64_t's type exactly; C++, which has no _Generic, checks its size. */
#ifdef __cplusplus
static_assert(ATOMIC_LONG_LOCK_FREE == 2 && sizeof(long) == sizeof(int64_t),
              "the CXL lock words must be lock-free 64-bit atomics");
#else
static_assert(ATOMIC_LONG_LOCK_FREE == 2 &&
                  _Generic((int64_t)0, long: 1, default: 0),
              "the CXL lock words must be lock-free 64-bit atomics");
#endif

/* ── Peterson lock (2-process, CXL-safe) ────────────────────────────────────
 * flag[i] and turn carry Peterson's store->load pair between the two
 * processes of a tree node: every store and load of them is seq_cst, except
 * the holder's release of its flag, a release store that hands the critical
 * section to the next holder, whose seq_cst load of that flag acquires it.
 * owner is written and read only by the tree node's holder (relaxed).
 * Initialized before the arena table is published. */

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
    arts_cxl_peterson_lock_t *locks;
  } lock;
  uint64_t pad[((sizeof(uint64_t) * 2 + sizeof(arts_cxl_peterson_lock_t *) +
                 CACHELINE_SIZE - 1) /
                CACHELINE_SIZE) *
               (CACHELINE_SIZE / sizeof(uint64_t))];
} arts_cxl_tournament_lock_t;

/* ── Peterson lock operations ───────────────────────────────────────────────
 */

static inline void arts_cxl_peterson_lock_init(arts_cxl_peterson_lock_t *l) {
  atomic_store_explicit(&l->lock.flag[0].value, 0, memory_order_relaxed);
  atomic_store_explicit(&l->lock.flag[1].value, 0, memory_order_relaxed);
  atomic_store_explicit(&l->lock.turn.value, 0, memory_order_relaxed);
  atomic_store_explicit(&l->lock.owner.value, -1, memory_order_relaxed);
  FLUSH_FENCE_PRODUCER(l, sizeof(arts_cxl_peterson_lock_t));
}

static inline void arts_cxl_peterson_lock_acquire(arts_cxl_peterson_lock_t *l,
                                                  unsigned int id) {
  unsigned int delay = 1;
  unsigned int other = 1 - id;

  /* seq_cst: Peterson's store->load pair against the other process's
   * acquire of this node. */
  atomic_store_explicit(&l->lock.flag[id].value, 1, memory_order_seq_cst);
  FLUSH_FENCE_PRODUCER(&l->lock.flag[id], sizeof(arts_cxl_cache_line_t));

  atomic_store_explicit(&l->lock.turn.value, other, memory_order_seq_cst);
  FLUSH_FENCE_PRODUCER(&l->lock.turn, sizeof(arts_cxl_cache_line_t));

  /* Device order, not a C11 fence: the write-backs the producer flushes
   * above started must complete before this host's loads below, which a
   * store fence does not order.  The C11 order between the two processes
   * comes from the seq_cst accesses. */
  __asm__ __volatile__("mfence" ::: "memory");

  FLUSH_FENCE_CONSUMER(l, sizeof(arts_cxl_peterson_lock_t));

  while (
      atomic_load_explicit(&l->lock.flag[other].value, memory_order_seq_cst) &&
      atomic_load_explicit(&l->lock.turn.value, memory_order_seq_cst) ==
          other) {
    usleep(delay);
    if (delay < 1000000) {
      delay *= 2;
    }
    FLUSH_FENCE_CONSUMER(l, sizeof(arts_cxl_peterson_lock_t));
  }
  atomic_store_explicit(&l->lock.owner.value, id, memory_order_relaxed);
  FLUSH_FENCE_PRODUCER(&l->lock.owner, sizeof(arts_cxl_cache_line_t));
}

static inline void arts_cxl_peterson_lock_release(arts_cxl_peterson_lock_t *l) {
  int64_t id = atomic_load_explicit(&l->lock.owner.value, memory_order_relaxed);
  atomic_store_explicit(&l->lock.owner.value, -1, memory_order_relaxed);
  atomic_store_explicit(&l->lock.flag[id].value, 0, memory_order_release);
  FLUSH_FENCE_PRODUCER(l, sizeof(arts_cxl_peterson_lock_t));
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

static inline void
arts_cxl_tournament_lock_release(arts_cxl_tournament_lock_t *tl,
                                 pthread_mutex_t *local_lock, unsigned int id) {
  (void)id;
  unsigned int node_id = 0;
  for (unsigned int i = 0; i < tl->lock.k; i++) {
    unsigned int pid = (unsigned int)atomic_load_explicit(
        &tl->lock.locks[node_id].lock.owner.value, memory_order_relaxed);
    arts_cxl_peterson_lock_release(&tl->lock.locks[node_id]);
    node_id = (2 * node_id) + 1 + pid;
  }
  pthread_mutex_unlock(local_lock);
}

#ifdef __cplusplus
}
#endif
#endif /* ARTS_CXL_LOCK_H */
