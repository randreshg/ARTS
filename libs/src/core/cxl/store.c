/* SPDX-License-Identifier: Apache-2.0 */
#include "arts/cxl/store.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#include "arts.h"
#include "arts/counter/Preamble.h"
#include "arts/counter/papi.h"
#include "arts/cxl/deque.h"
#include "arts/runtime_state.h"
#include "arts/system/print.h"
#include "arts/utils/atomics.h"

/* One arena per device under round_robin, rotated per allocation, bounded by
 * the arena count the published deque carries; one arena, at index 0, under
 * static.  Arena creation currently ignores the device ids it is handed, so
 * an index selects an arena, not a device. */
static unsigned cxl_store_arena_index(void) {
  unsigned n = arts_node_info.cxl_db_dev_count;
  if (n > arts_node_info.cxl_deque->consts.db_arena_count) {
    n = arts_node_info.cxl_deque->consts.db_arena_count;
  }
  if (n > 1u) {
    return arts_atomic_fetch_add(&arts_node_info.cxl_db_rr_idx, 1U) % n;
  }
  return 0u;
}

/* One carve off the shared arena, the only step that takes the tournament
 * lock; counted as a refill whether it fills a pool or serves one oversized
 * slot.  NULL is fatal and names both sizes: the arena may still have room in
 * other ranks' pools, which nothing returns. */
static void *cxl_store_carve(size_t min_bytes, size_t want_bytes, unsigned idx,
                             size_t *got) {
  TIME_CXL_POOL_REFILL_START();
  void *p = arts_cxl_deque_db_carve_dev(arts_node_info.cxl_deque,
                                        &arts_node_info.cxl_local_lock,
                                        min_bytes, want_bytes, idx, got);
  TIME_CXL_POOL_REFILL_STOP();
  if (p == NULL) {
    ARTS_ERROR("cxl: the store cannot serve %zu bytes: the arena of "
               "%llu bytes is exhausted (ARTS_CXL_DB_ARENA_SIZE_BYTES; refilled "
               "%llu bytes per rank at a time, cxl_db_pool_bytes) - nothing is "
               "reclaimed within a run, and other ranks' pools keep what they "
               "have not handed out",
               min_bytes, (unsigned long long)ARTS_CXL_DB_ARENA_SIZE_BYTES,
               (unsigned long long)arts_node_info.cxl_db_pool_bytes);
  }
  INCREMENT_NUM_CXL_POOL_REFILL_BY(1);
  INCREMENT_BYTES_CXL_POOL_REFILL_BY(*got);
  return p;
}

/* Two levels.  A slot comes from this rank's pool for the arena, under the
 * pool's DRAM mutex alone; only when the pool cannot hold the slot does the
 * rank carve a fresh pool off the shared arena under the tournament lock.  The
 * old pool's tail (smaller than the slot) is abandoned, as a discarded slot
 * is.  A slot larger than a whole pool is carved on its own and leaves the
 * pool as it was.  Lock order: pool mutex, then cxl_local_lock (taken by the
 * tournament acquire); the CXL EDT deque takes cxl_local_lock alone, so the
 * order cannot invert. */
void *arts_cxl_store_alloc(size_t bytes) {
  size_t span = (bytes + ARTS_CXL_GRANULE - 1u) & ~(size_t)(ARTS_CXL_GRANULE - 1u);
  if (span == 0u) {
    span = ARTS_CXL_GRANULE;
  }
  unsigned idx = cxl_store_arena_index();
  size_t got = 0;
  void *p;
  if ((uint64_t)span > arts_node_info.cxl_db_pool_bytes) {
    p = cxl_store_carve(span, span, idx, &got);
  } else {
    arts_cxl_db_pool_t *pool = &arts_node_info.cxl_db_pool[idx];
    pthread_mutex_lock(&pool->lock);
    if ((size_t)(pool->end - pool->cur) < span) {
      char *base = (char *)cxl_store_carve(
          span, (size_t)arts_node_info.cxl_db_pool_bytes, idx, &got);
      pool->cur = base;
      pool->end = base + got;
    }
    p = pool->cur;
    pool->cur += span;
    pthread_mutex_unlock(&pool->lock);
  }
  if (((uintptr_t)p & (ARTS_CXL_GRANULE - 1u)) != 0u) {
    ARTS_ERROR("cxl: the arena returned %p, not a cache-line boundary", p);
  }
  return p;
}

bool arts_cxl_store_contains(const void *p) {
  return p != NULL && IS_CXL_PTR(p);
}

#ifdef ARTS_CXL_FLUSH_RECORDER
/* A slot is published by its stamp, seq + 1, stored after the rest of the
 * record, so a reader that sees the stamp sees the whole record. */
static struct {
  struct arts_cxl_flush_record_s rec;
  _Atomic uint64_t stamp;
} g_flush_ring[ARTS_CXL_FLUSH_RECORD_CAP];
static _Atomic uint64_t g_flush_next;

static void cxl_flush_record(const void *p, size_t bytes, bool producer) {
  uint64_t seq = atomic_fetch_add_explicit(&g_flush_next, 1u, memory_order_relaxed);
  if (seq >= ARTS_CXL_FLUSH_RECORD_CAP) {
    return;
  }
  g_flush_ring[seq].rec = (struct arts_cxl_flush_record_s){
      .seq = seq, .addr = (uintptr_t)p, .bytes = bytes, .producer = producer};
  atomic_store_explicit(&g_flush_ring[seq].stamp, seq + 1u, memory_order_release);
}

uint64_t arts_cxl_flush_record_count(void) {
  return atomic_load_explicit(&g_flush_next, memory_order_acquire);
}

bool arts_cxl_flush_record_get(uint64_t i, struct arts_cxl_flush_record_s *out) {
  if (i >= ARTS_CXL_FLUSH_RECORD_CAP ||
      atomic_load_explicit(&g_flush_ring[i].stamp, memory_order_acquire) != i + 1u) {
    return false;
  }
  *out = g_flush_ring[i].rec;
  return true;
}
#endif /* ARTS_CXL_FLUSH_RECORDER */

void arts_cxl_store_flush(const void *p, size_t bytes, bool producer) {
#ifdef ARTS_CXL_FLUSH_RECORDER
  cxl_flush_record(p, bytes, producer);
#endif
  /* The timers bracket the sweep and its fence, so a flush's time includes
   * the drain the fence waits on. */
  if (producer) {
    INCREMENT_NUM_CXL_FLUSH_PRODUCER_BY(1);
    INCREMENT_BYTES_CXL_FLUSH_PRODUCER_BY(bytes);
    TIME_CXL_FLUSH_PRODUCER_START();
    ARTS_PAPI_BEGIN(CXL_FLUSH_PRODUCER);
    FLUSH_FENCE_PRODUCER(p, bytes);
    __asm__ __volatile__("sfence" ::: "memory");
    ARTS_PAPI_END(CXL_FLUSH_PRODUCER);
    TIME_CXL_FLUSH_PRODUCER_STOP();
  } else {
    INCREMENT_NUM_CXL_FLUSH_CONSUMER_BY(1);
    INCREMENT_BYTES_CXL_FLUSH_CONSUMER_BY(bytes);
    TIME_CXL_FLUSH_CONSUMER_START();
    ARTS_PAPI_BEGIN(CXL_FLUSH_CONSUMER);
    FLUSH_FENCE_CONSUMER(p, bytes);
    __asm__ __volatile__("mfence" ::: "memory");
    ARTS_PAPI_END(CXL_FLUSH_CONSUMER);
    TIME_CXL_FLUSH_CONSUMER_STOP();
  }
}
