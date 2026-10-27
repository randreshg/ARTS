/* SPDX-License-Identifier: Apache-2.0
 *
 * EXCL protocol shared home-side infrastructure.
 *
 * Defines: arts_home_grantreq_queue_{init,push,pop,peek,empty,pending,destroy},
 *          arts_db_home_init, arts_db_home_teardown,
 *          arts_excl_home_teardown.
 *
 * This TU contains only the shared pieces that both the PURGE and RETAIN release-policy
 * variants need: the home ownership-request FIFO, the home-directory
 * lifecycle, and the DB-destroy fan-out.  Timing-
 * specific arbiters (excl_compute_next, cache_compute_next) and all
 * acquire/release handler/sender bodies live in the per-release-policy TU (purge.c
 * or retain.c).
 *
 * The grantreq queue is this arm's own copy of the home's Vyukov MPSC request
 * FIFO, over the struct excl/types.h defines; its push also counts the requests
 * that could not be granted on arrival.
 *
 * Compiled only for ARTS_COHERENCE_PROTOCOL=EXCL.
 */

/* excl/types.h must precede directory.h: it defines arts_home_grantreq_node_s and
 * arts_home_grantreq_queue_s for the EXCL build (directory.h declares functions
 * that take these types by pointer, and coherence.h embeds them in
 * arts_db_s). */
#include "arts/coherence/excl/types.h"

#include "arts/utils/stdatomic_compat.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "arts/coherence/coherence.h"
#include "arts/coherence/handlers.h"
#include "arts/coherence/directory.h"
#include "arts/gas/route_table.h"
#include "arts/ooo.h"
#include "arts/system/identity.h"
#include "arts/transport/net.h"
#include "arts/utils/malloc.h"

/* ===== Home OWNERSHIP-REQUEST FIFO (Vyukov MPSC) =======================
 *
 * Standard Vyukov MPSC FIFO storing an unsigned int requester rank.  The
 * embedded stub sentinel in arts_home_grantreq_queue_s is the permanent queue
 * sentinel; it is never malloc'd or free'd separately.
 */

void arts_home_grantreq_queue_init(struct arts_home_grantreq_queue_s *q) {
  atomic_store_explicit(&q->stub.next, (struct arts_home_grantreq_node_s *)NULL,
                        memory_order_relaxed);
  q->stub.rank = 0;
  atomic_store_explicit(&q->tail, &q->stub, memory_order_relaxed);
  atomic_store_explicit(&q->head, &q->stub, memory_order_relaxed);
}

void arts_home_grantreq_queue_push(struct arts_home_grantreq_queue_s *q,
                                  unsigned int rank,
                                  const struct arts_rdzv_landing_s *rdzv,
                                  uint64_t have_version) {
  struct arts_home_grantreq_node_s *n =
      (struct arts_home_grantreq_node_s *)arts_malloc(sizeof(*n));
  n->rank = rank;
  n->have_version = have_version;
  if (rdzv != NULL) {
    n->rdzv = *rdzv;
  } else {
    n->rdzv = (struct arts_rdzv_landing_s){0, 0, 0, 0};
  }
  atomic_store_explicit(&n->next, (struct arts_home_grantreq_node_s *)NULL,
                        memory_order_relaxed);
  struct arts_home_grantreq_node_s *prev =
      atomic_exchange_explicit(&q->tail, n, memory_order_acq_rel);
  atomic_store_explicit(&prev->next, n, memory_order_release);
}

bool arts_home_grantreq_queue_pop(struct arts_home_grantreq_queue_s *q,
                                 unsigned int *out_rank,
                                 struct arts_rdzv_landing_s *out_rdzv,
                                 uint64_t *out_have) {
  for (;;) {
    struct arts_home_grantreq_node_s *head =
        atomic_load_explicit(&q->head, memory_order_acquire);
    struct arts_home_grantreq_node_s *next =
        atomic_load_explicit(&head->next, memory_order_acquire);
    if (next == NULL) {
      struct arts_home_grantreq_node_s *tail =
          atomic_load_explicit(&q->tail, memory_order_acquire);
      if (head == tail) {
        return false;
      }
      continue;
    }
    *out_rank = next->rank;
    if (out_rdzv != NULL) {
      *out_rdzv = next->rdzv;
    }
    if (out_have != NULL) {
      *out_have = next->have_version;
    }
    atomic_store_explicit(&q->head, next, memory_order_release);
    if (head != &q->stub) {
      arts_free(head);
    }
    return true;
  }
}

bool arts_home_grantreq_queue_peek(const struct arts_home_grantreq_queue_s *q,
                                  unsigned int *out_rank,
                                  struct arts_rdzv_landing_s *out_rdzv,
                                  uint64_t *out_have) {
  /* Same mid-link disambiguation as pop: a null forward link means empty only
   * when head == tail; otherwise a producer is between its tail exchange and
   * its link store and the front exists but is not yet reachable.  Returning
   * "no front" there would contradict the deref-free pending probe, which
   * counts that producer, and leave a caller alternating between the two.
   * The queue is not mutated (no head advance, no node free); single
   * consumer (the baton holder). */
  for (;;) {
    struct arts_home_grantreq_node_s *head =
        atomic_load_explicit(&q->head, memory_order_acquire);
    struct arts_home_grantreq_node_s *next =
        atomic_load_explicit(&head->next, memory_order_acquire);
    if (next == NULL) {
      struct arts_home_grantreq_node_s *tail =
          atomic_load_explicit(&q->tail, memory_order_acquire);
      if (head == tail) {
        return false; /* truly empty */
      }
      continue; /* producer mid-link: tight retry (single consumer, ns) */
    }
    *out_rank = next->rank;
    if (out_rdzv != NULL) {
      *out_rdzv = next->rdzv;
    }
    if (out_have != NULL) {
      *out_have = next->have_version;
    }
    return true;
  }
}

bool arts_home_grantreq_queue_empty(const struct arts_home_grantreq_queue_s *q) {
  struct arts_home_grantreq_node_s *head =
      atomic_load_explicit(&q->head, memory_order_acquire);
  struct arts_home_grantreq_node_s *next =
      atomic_load_explicit(&head->next, memory_order_acquire);
  if (next != NULL) {
    return false;
  }
  struct arts_home_grantreq_node_s *tail =
      atomic_load_explicit(&q->tail, memory_order_acquire);
  return head == tail;
}

bool arts_home_grantreq_queue_pending(
    const struct arts_home_grantreq_queue_s *q) {
  /* Deref-free: a push linearizes on the tail exchange, so head != tail
   * already names a queued requester even while its forward link is still in
   * flight.  Reading head->next instead would follow a pointer a concurrent
   * pop is free to have released, which is why the baton-release re-checks
   * use this and not the dereferencing test above.  Both loads may name freed
   * nodes; that is harmless because baton RELEASES order them — the last
   * release's probe can have no pop between its two loads, since any such pop
   * belongs to a later holder who must release again. */
  struct arts_home_grantreq_node_s *head =
      atomic_load_explicit(&q->head, memory_order_acquire);
  struct arts_home_grantreq_node_s *tail =
      atomic_load_explicit(&q->tail, memory_order_acquire);
  return head != tail;
}

void arts_home_grantreq_queue_destroy(struct arts_home_grantreq_queue_s *q) {
  struct arts_home_grantreq_node_s *cur =
      atomic_load_explicit(&q->head, memory_order_relaxed);
  while (cur != NULL) {
    struct arts_home_grantreq_node_s *nxt =
        atomic_load_explicit(&cur->next, memory_order_relaxed);
    if (cur != &q->stub) {
      arts_free(cur);
    }
    cur = nxt;
  }
  atomic_store_explicit(&q->tail, (struct arts_home_grantreq_node_s *)NULL,
                        memory_order_relaxed);
  atomic_store_explicit(&q->head, (struct arts_home_grantreq_node_s *)NULL,
                        memory_order_relaxed);
}

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
  atomic_store_explicit(&db->cover_gate, (rwo << 30) | (roo << 29) | n,
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
  unsigned int self = arts_global_rank_id;
  /* Inline the bitset word-walk (arts_rank_bitset_for_each requires a
   * callback; C has no local functions, so we open-code the loop). */
  const struct arts_rank_bitset_s *bs = &db->cached_ranks;
  for (unsigned int w = 0; w < bs->nwords; w++) {
    uint64_t snap = atomic_load_explicit(&bs->words[w], memory_order_acquire);
    while (snap) {
      unsigned int b = (unsigned int)__builtin_ctzll(snap);
      unsigned int rank = w * 64 + b;
      if (rank != self) {
        arts_send_db_cache_destroy(rank, db_guid);
      }
      snap &= snap - 1;
    }
  }
  /* No rw_waiters drain here: the request queue is single-consumer (the CONFIRM
   * that pops the migrated requester is its only consumer, serialized by the
   * lock phase machine).  Every rank that ever requested RW is in cached_ranks
   * (recorded at request time) and was just notified above.  Draining here
   * would be a second, unsynchronized consumer of a single-consumer queue — a
   * use-after-free against a concurrent CONFIRM. */
}

