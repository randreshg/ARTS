/* SPDX-License-Identifier: Apache-2.0
 *
 * EXCL protocol shared infrastructure.
 *
 * Defines: arts_db_home_init, arts_db_home_teardown, arts_db_home_seed_owed,
 *          arts_excl_home_teardown, arts_excl_set_readers_aside,
 *          arts_excl_take_readers_aside.
 *
 * This TU contains only the shared pieces that both the PURGE and RETAIN
 * retention variants need: the home-directory lifecycle, the DB-destroy
 * fan-out and the cache's two ro_aside steps.  Timing-specific arbiters
 * (excl_compute_next, cache_compute_next) and all acquire/release
 * handler/sender bodies live in the per-retention TU (purge.c or retain.c).
 * The home request FIFO is grantreq_queue.c.
 *
 * Compiled only for ARTS_VALIDITY=EXCL.
 */

#include "arts/coherence/excl/types.h"

#include "arts/utils/stdatomic_compat.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "arts/coherence/coherence.h"
#include "arts/coherence/cover_gate.h"
#include "arts/coherence/handlers.h"
#include "arts/coherence/directory.h"
#include "arts/gas/route_table.h"
#include "arts/ooo.h"
#include "arts/system/identity.h"
#include "arts/system/print.h"
#include "arts/system/schedfuzz.h"
#include "arts/transport/net.h"
#include "arts/utils/malloc.h"

/* ===== Home-directory lifecycle ========================================= */

void arts_db_home_init(struct arts_db_s *db, unsigned int rw_holder,
                       unsigned int nranks) {
  /* Seed the creator's RW hold: arts_db_create defaults to an RW acquire (OCR
   * contract), so the home arbiter starts with one writer (the creator rank)
   * holding the lock — its matching release drives the count back to 0.
   * NO_ACQUIRE resets this to the idle state in arts_db_create. */
#ifdef ARTS_RETENTION_PURGE
  /* PURGE tracks mode via the lock_state phase bit, not a separate rw_holder
   * field. */
  atomic_store_explicit(&db->lock_state, LOCK_MAKE_STATE(0u, 1u, 0u),
                        memory_order_relaxed);
#else /* ARTS_RETENTION_RETAIN */
  /* RETAIN: the home is a pure directory.  The creator is the first DATA owner
   * (cache-side: owner-bit + RW grant), but its RW hold/release are entirely
   * local (sticky — no REQUEST/RELEASE to home), so the home directory must NOT
   * count it.  Seed phase=IDLE, w=0, r=0, owner=creator: the lock is idle (no
   * home-tracked participant) and `owner` names the rank a future migrate/serve
   * FORWARD routes to.  This differs from PURGE, where the creator's RELEASE
   * drives a seeded w=1 back to 0. */
  atomic_store_explicit(&db->lock_state,
                        LOCK_MAKE(EXCL_PHASE_IDLE, rw_holder, 0u, 0u),
                        memory_order_relaxed);
#endif
  arts_home_grantreq_queue_init(&db->rw_waiters);
  arts_lf_stack_init(&db->ro_waiters);
  arts_rank_bitset_init(&db->cached_ranks, nranks);
  db->claim_rank = (unsigned int)-1;
#ifdef ARTS_RETENTION_RETAIN
  atomic_store_explicit(&db->cover_gate, 0u, memory_order_relaxed);
#endif
  /* A creator on another rank holds a cache of the block from the moment it
   * made it, so the destroy must reach that rank too: its cache would
   * otherwise outlive the block, keeping its storage and the rank's route
   * slot for the GUID. */
  if (rw_holder != arts_global_rank_id && rw_holder < nranks) {
    arts_rank_bitset_set(&db->cached_ranks, rw_holder);
  }
#ifdef ARTS_RETENTION_RETAIN
  arts_rank_bitset_init(&db->ro_retainers, nranks);
#endif
}

void arts_db_home_teardown(struct arts_db_s *db) {
  if (db == NULL) {
    return;
  }
  arts_home_grantreq_queue_destroy(&db->rw_waiters);
  /* Drain + free any rank-nodes still on ro_waiters (single-threaded at
   * teardown — the route slot is already absent, no concurrent push). */
  arts_lf_link_t *node = arts_lf_stack_drain(&db->ro_waiters);
  while (node != NULL) {
    arts_lf_link_t *nx =
        atomic_load_explicit(&node->next, memory_order_relaxed);
    arts_free(ARTS_CONTAINER_OF(node, struct arts_lock_ro_node_s, link));
    node = nx;
  }
  arts_rank_bitset_destroy(&db->cached_ranks);
#ifdef ARTS_RETENTION_RETAIN
  /* Retainers are legal at destroy — a retained grant has no live acquire — so
   * a non-empty roster here is a normal path, not an edge case. */
  arts_rank_bitset_destroy(&db->ro_retainers);
#endif
}

#ifdef ARTS_RETENTION_PURGE
void arts_db_home_seed_owed(struct arts_db_s *db, unsigned int creator,
                            uint8_t owed) {
  if (owed == 0u) {
    return;
  }
  /* The claim closes the write request alone. */
  assert(owed == ARTS_CREATE_OWED_RW);
  db->claim_rank = creator;
  uint64_t s = atomic_load_explicit(&db->lock_state, memory_order_relaxed);
  atomic_store_explicit(&db->lock_state,
                        LOCK_MAKE_STATE_FULL(EXCL_STATE_TEARDOWN(s),
                                             EXCL_STATE_BIT(s), 1u,
                                             EXCL_STATE_W(s), EXCL_STATE_R(s)),
                        memory_order_relaxed);
}
#else
void arts_db_home_seed_owed(struct arts_db_s *db, unsigned int creator,
                            uint8_t owed) {
  if (owed == 0u) {
    return;
  }
  /* The claim closes the write request alone. */
  assert(owed == ARTS_CREATE_OWED_RW);
  db->claim_rank = creator;
  atomic_store_explicit(&db->cover_gate, ARTS_COVER_GATE_RWO | 1u,
                        memory_order_relaxed);
}
#endif

/* Tear the home down: notify every rank that ever cached this DB, then return
 * the route slot.  The fan-out sits INSIDE the retire's single flight rather
 * than before it: duplicate destroys are produced by the protocol on purpose,
 * and only the caller whose retire actually detaches the object may put
 * CACHE_DESTROY on the wire. */
void arts_excl_home_teardown(struct arts_db_s *db, arts_guid_t db_guid) {
  /* Claim first: only the caller that actually detaches the object may put
   * CACHE_DESTROY on the wire.  Duplicate destroys are produced by the
   * protocol on purpose, and a second fan-out would tell every cache to drop a
   * DB twice.  Reading db below stays safe after the detach — the caller's
   * count on the object (its dispatch pin, or the hold a release returns),
   * which self_cb is borrowed under, keeps it across the call. */
  if (!arts_ooo_retire(db_guid, db->self_cb)) {
    return;
  }
  struct arts_rank_bitset_cursor_s c =
      arts_rank_bitset_cursor(&db->cached_ranks);
  unsigned int rank;
  while (arts_rank_bitset_next(&c, &rank)) {
    if (rank != arts_global_rank_id) {
      arts_send_db_cache_destroy(rank, db_guid);
    }
  }
  /* No rw_waiters drain here: the request queue is single-consumer (under
   * RETAIN the CONFIRM that pops the migrated requester, under PURGE the
   * committer of the transition that grants the front its write turn,
   * serialized by the lock phase machine).  Every rank that ever requested RW
   * is in cached_ranks (recorded at request time) and was just notified
   * above.  Draining here would be a second, unsynchronized consumer of a
   * single-consumer queue — a use-after-free against a concurrent pop. */
}

/* ===== The cache's ro_aside steps (excl/types.h) ======================= */

void arts_excl_set_readers_aside(struct arts_db_cache_s *cache, uint32_t n) {
  if (n == 0u) {
    return;
  }
  arts_sched_fuzz_point();
  uint32_t had =
      atomic_fetch_add_explicit(&cache->ro_aside, n, memory_order_acq_rel);
#ifdef ARTS_RETENTION_PURGE
  uint64_t limit = CACHE_CNT_MASK;
#else
  uint64_t limit = CACHE_OWNER_RC_MASK;
#endif
  if ((uint64_t)had + n > limit) {
    ARTS_ERROR("lock: guid %lu has more read acquires parked on one rank "
               "than its cache word can hold (aside=%u, set aside %u)",
               (unsigned long)cache->db_guid, had, n);
  }
#ifndef NDEBUG
  /* The transition that set these readers aside still holds the writers it
   * counted, none resumed yet, so its write grant stands, and the answer that
   * will take this count cannot have come; the word this thread wrote is the
   * latest it can read. */
  uint64_t now =
      atomic_load_explicit(&cache->cache_state, memory_order_relaxed);
  assert(CACHE_RW_ST(now) == CACHE_ST_GRANT &&
         CACHE_RO_ST(now) == CACHE_ST_REQ);
#endif
}

uint32_t arts_excl_take_readers_aside(struct arts_db_cache_s *cache) {
  uint32_t taken =
      atomic_exchange_explicit(&cache->ro_aside, 0u, memory_order_acq_rel);
  arts_sched_fuzz_point();
  return taken;
}
