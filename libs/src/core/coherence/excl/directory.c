/* SPDX-License-Identifier: Apache-2.0
 *
 * EXCL protocol shared home-side infrastructure.
 *
 * Defines: arts_db_home_init, arts_db_home_teardown, arts_db_home_seed_owed,
 *          arts_excl_home_teardown.
 *
 * This TU contains only the shared pieces that both the PURGE and RETAIN
 * release-policy variants need: the home-directory lifecycle and the
 * DB-destroy fan-out.  Timing-specific arbiters (excl_compute_next,
 * cache_compute_next) and all acquire/release handler/sender bodies live in
 * the per-release-policy TU (purge.c or retain.c).  The home request FIFO is
 * grantreq_queue.c.
 *
 * Compiled only for ARTS_COHERENCE_PROTOCOL=EXCL.
 */

#include "arts/coherence/excl/types.h"

#include "arts/utils/stdatomic_compat.h"
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
#include "arts/transport/net.h"
#include "arts/utils/malloc.h"

/* ===== Home-directory lifecycle ========================================= */

void arts_db_home_init(struct arts_db_s *db, unsigned int rw_holder,
                       unsigned int nranks) {
  /* Seed the creator's RW hold: arts_db_create defaults to an RW acquire (OCR
   * contract), so the home arbiter starts with one writer (the creator rank)
   * holding the lock — its matching release drives the count back to 0.
   * NO_ACQUIRE resets this to the idle state in arts_db_create. */
#ifdef ARTS_RELEASE_PURGE
  /* PURGE tracks mode via the lock_state phase bit, not a separate rw_holder
   * field. */
  atomic_store_explicit(&db->lock_state, LOCK_MAKE_STATE(0u, 1u, 0u),
                        memory_order_relaxed);
#else /* ARTS_RELEASE_RETAIN */
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
  arts_rank_bitset_init(&db->ro_parity, nranks);
  arts_rank_bitset_init(&db->ro_queued, nranks);
  db->claim_rank = (unsigned int)-1;
#ifdef ARTS_RELEASE_RETAIN
  atomic_store_explicit(&db->cover_gate, 0u, memory_order_relaxed);
#endif
  /* A creator on another rank holds a cache of the block from the moment it
   * made it, so the destroy must reach that rank too: its cache would
   * otherwise outlive the block, keeping its storage and the rank's route
   * slot for the GUID. */
  if (rw_holder != arts_global_rank_id && rw_holder < nranks) {
    arts_rank_bitset_set(&db->cached_ranks, rw_holder);
  }
#ifdef ARTS_RELEASE_RETAIN
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
  arts_rank_bitset_destroy(&db->ro_parity);
  arts_rank_bitset_destroy(&db->ro_queued);
#ifdef ARTS_RELEASE_RETAIN
  /* Retainers are legal at destroy — a retained grant has no live acquire — so
   * a non-empty roster here is a normal path, not an edge case. */
  arts_rank_bitset_destroy(&db->ro_retainers);
#endif
}

void arts_db_home_seed_owed(struct arts_db_s *db, unsigned int creator,
                            uint8_t owed) {
  if (owed == 0u) {
    return;
  }
  db->claim_rank = creator;
  uint32_t n = 0u;
  uint32_t rwo = (owed & ARTS_CREATE_OWED_RW) ? 1u : 0u;
  uint32_t roo = (owed & ARTS_CREATE_OWED_RO) ? 1u : 0u;
  if (roo) {
    /* As if the pre-claim read request had arrived: the creator's next read
     * request (the opposite parity) pairs as any. */
    (void)arts_rank_bitset_put(&db->ro_parity, creator,
                               (owed & ARTS_CREATE_OWED_RO_PAR) != 0u);
  }
  n = rwo + roo;
#ifdef ARTS_RELEASE_PURGE
  uint64_t s = atomic_load_explicit(&db->lock_state, memory_order_relaxed);
  atomic_store_explicit(
      &db->lock_state,
      LOCK_MAKE_STATE_FULL(EXCL_STATE_TEARDOWN(s), EXCL_STATE_BIT(s), rwo, roo,
                           n, EXCL_STATE_W(s), EXCL_STATE_R(s)),
      memory_order_relaxed);
#else
  atomic_store_explicit(&db->cover_gate,
                        (rwo != 0u ? ARTS_COVER_GATE_RWO : 0u) |
                            (roo != 0u ? ARTS_COVER_GATE_ROO : 0u) | n,
                        memory_order_relaxed);
#endif
}

/* Tear the home down: notify every rank that ever cached this DB, then return
 * the route slot.  The fan-out sits INSIDE the retire's single flight rather
 * than before it: duplicate destroys are produced by the protocol on purpose,
 * and only the caller whose retire actually detaches the object may put
 * CACHE_DESTROY on the wire. */
void arts_excl_home_teardown(struct arts_db_s *db, arts_guid_t db_guid) {
  /* Claim first: only the caller that actually detaches the object may put
   * CACHE_DESTROY on the wire.  Duplicate destroys are produced by the
   * protocol on purpose, and a second fan-out would tell every cache to drop a
   * DB twice.  Reading db below stays safe after the detach — the dispatch
   * that reached this handler pins the object across the call. */
  if (!arts_ooo_retire_item(db_guid, db)) {
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

