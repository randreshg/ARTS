/* SPDX-License-Identifier: Apache-2.0
 *
 * The RETAIN release policy's half of the migrating grant.
 *
 * An unasked holder keeps the grant forever.  Every hand-over is therefore
 * demand-driven and home-initiated: a queued request opens a revocation round,
 * the round's INVALIDATE names the next owner, the holder ships owner->owner,
 * and at the home's CONFIRM the baton holder either revokes again for the
 * next queued requester or puts the baton down.  Nothing a holder does at
 * its own idle edge is visible to the home — its release only decrements, and
 * ships only where a round has already named a target.
 *
 * Both write policies link this TU.  A round starts, advances and ends in one
 * function: under write-back the CONFIRM first tries to name the next target
 * on its CONFIRM_ACK, and comes here only when it found none.
 */
#include <stdbool.h>
#include <stdint.h>

#include "arts/coherence/coherence.h"
#include "arts/coherence/directory.h"
#include "arts/coherence/handlers.h"
#include "arts/db.h"
#include "arts/runtime_state.h"
#include "arts/system/threads.h"
#include "arts/runtime_types.h"
#include "arts/system/schedfuzz.h"
#include "arts/counter/Preamble.h"

/* ===== Home: a request arrived ========================================= */

void arts_db_grant_request_arrived(struct arts_db_cache_s *cache,
                                   struct arts_db_s *db,
                                   unsigned int requester) {
  (void)requester;
  /* Same-word pairing with the holder's put-down (arts_db_grant_advance):
   * this claim follows the caller's push, so a holder whose put-down meets it
   * looks at the FIFO again, and a claim that reads 0 serves itself. */
  if (atomic_fetch_add_explicit(&db->grant_baton, 1u, memory_order_acq_rel) ==
      0u) {
    arts_db_grant_advance(cache, db, false, 1u);
  }
}

/* ===== Home: the baton holder serves or puts the baton down ============= */

void arts_db_grant_advance(struct arts_db_cache_s *cache, struct arts_db_s *db,
                           bool looked, uint64_t seen) {
  unsigned int next_owner;
  struct arts_rdzv_landing_s next_rdzv;
  uint64_t next_have = ARTS_GRANT_VERSION_NONE;
  bool got = !looked &&
             arts_home_grantreq_queue_pop_linked(&db->pending_rw, &next_owner,
                                                 &next_rdzv, &next_have);
  while (!got) {
    arts_sched_fuzz_point(); /* widen the look<->put-down window */
    /* Same-word pairing with every claim (arts_db_grant_request_arrived):
     * a claim made since `seen` makes this CAS fail, and its acquire failure
     * order takes that claimant's push for the next look. */
    if (atomic_compare_exchange_strong_explicit(&db->grant_baton, &seen, 0u,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
      return;
    }
    INCREMENT_NUM_GRANT_BATON_RECLAIM_BY(1);
    got = arts_home_grantreq_queue_pop_linked(&db->pending_rw, &next_owner,
                                              &next_rdzv, &next_have);
  }
  atomic_store_explicit(&db->pending_install_owner, next_owner,
                        memory_order_relaxed);
  /* The INVALIDATE target is rw_holder, which the home names only after that
   * rank's install (the creator at DB_CREATE, a new owner at its CONFIRM), so
   * the target's cache is installed when the INVALIDATE arrives and it is
   * never deferred. */
  arts_send_db_grant_invalidate(
      atomic_load_explicit(&db->rw_holder, memory_order_acquire),
      cache->db_guid, next_owner, &next_rdzv, next_have);
}

/* A grant is never handed back unasked here, so there is no obligation to
 * arm, to carry, or to settle: every release takes the ordinary
 * count-dropping edge, and a home-bound publish carries payload alone. */
uint64_t arts_db_grant_release_claim(struct arts_db_cache_s *cache,
                                     bool will_publish) {
  (void)cache;
  (void)will_publish;
  return 0u;
}

bool arts_db_grant_return_claim_leg(struct arts_db_cache_s *cache) {
  (void)cache;
  return false;
}

void arts_db_grant_release_settle(struct arts_db_cache_s *cache,
                                  uint64_t token) {
  (void)cache;
  (void)token;
}

/* The shared publish paths ask the home to accept a hand-back that rode a
 * release.  No release here carries one, so the flag they unwrap is never
 * set and this is the arm's answer to a question it never gets asked. */
void arts_db_grant_return_arrived(
    struct arts_db_s *db, unsigned int returner,
    const struct arts_db_grant_return_reply_s *reply) {
  (void)db;
  (void)returner;
  (void)reply;
}

/* ===== Holder: the word's possession-setting and count-dropping edges ==== */

void arts_db_grant_install(struct arts_db_cache_s *cache) {
  /* Sentinel(+1) + drain guard(+1), single op (0->2).  Release: the
   * installed buffer and the learned size go with the right. */
  atomic_fetch_add_explicit(&cache->writer_count, 2u, memory_order_acq_rel);
}

bool arts_db_grant_release_skip(struct arts_db_cache_s *cache) {
  /* An all-zero word is the only state with nothing to drop: possession is a
   * hold of its own here, so any positive word owes a decrement. */
  return atomic_load_explicit(&cache->writer_count, memory_order_acquire) == 0;
}

void arts_db_grant_release_commit(struct arts_db_cache_s *cache) {
  /* writer_count is non-negative (post-install flip + install guard absorb any
   * INVALIDATE that lands during install).  A true 1->0 release reads 0 and
   * ships the transfer; the (int) cast is defensive. */
  int rest = (int)(atomic_fetch_sub_explicit(&cache->writer_count, 1u,
                                             memory_order_acq_rel) -
                   1u);
  if (rest == 0 && cache->incoming_new_owner != ARTS_NO_PENDING_OWNER) {
    /* An INVALIDATE published a transfer target while writers were live; this
     * (last) releaser is the unique actor that ships the owner→owner transfer.
     * Identical for home and non-home owners. */
    arts_db_grant_ship_pending(cache);
  }
}

unsigned int arts_db_grant_commit_drain(struct arts_db_cache_s *cache) {
  /* The commit's own hold stays on the word for the whole walk here: a
   * revocation can arrive at any moment and decrement it, and the hold is what
   * stops that decrement reaching the edge while waiters are still being
   * counted in.  Each waiter takes its hold as it is woken. */
  arts_db_drain_pending_rw_after_grant(cache);
  return 0u;
}

void arts_db_grant_commit_finish(struct arts_db_cache_s *cache,
                                 unsigned int drained) {
  (void)drained;
  /* Drop the commit's hold.  If a next-round INVALIDATE already withdrew the
   * sentinel and no local writer remains, we are the unique actor that ships
   * the next transfer. */
  if (atomic_fetch_sub_explicit(&cache->writer_count, 1u,
                                memory_order_acq_rel) == 1u &&
      cache->incoming_new_owner != ARTS_NO_PENDING_OWNER) {
    arts_db_grant_ship_pending(cache);
  }
}
