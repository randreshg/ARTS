/* SPDX-License-Identifier: Apache-2.0
 *
 * INV protocol shared home-side infrastructure.
 *
 * Defines: arts_db_home_init, arts_db_home_teardown.
 *
 * arts_handler_db_destroy lives in engine.c: it fans out over
 * the wire, and this TU must stay linkable standalone (pure lifecycle)
 * for the whitebox unit tests.
 *
 * This TU carries the home-directory lifecycle shared by both write-policy
 * variants.  The arbiters live in arbiters.c, and the handler/sender bodies
 * live in the write-policy TU (wt.c or wb.c) plus the shared engine.c.  The
 * home request FIFO is grantreq_queue.c.
 *
 * Compiled only for ARTS_COHERENCE_PROTOCOL=INV.
 */

#include "arts/coherence/inv/types.h"

#include "arts/utils/stdatomic_compat.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "arts/coherence/coherence.h"
#include "arts/coherence/directory.h"
#include "arts/utils/malloc.h"

/* ===== Home-directory lifecycle ========================================= */

void arts_db_home_init(struct arts_db_s *db, unsigned int rw_holder,
                       unsigned int nranks) {
  /* Grant plane: the creator boots holding it (arts_db_create defaults to an
   * RW acquire under the OCR contract).  No count here: ownership lives on
   * the holder's writer_count, and a revoking home also names the holder
   * now; a home whose grants come back unasked names none. */
#ifndef ARTS_RELEASE_PURGE
  atomic_store_explicit(&db->rw_holder, rw_holder, memory_order_relaxed);
#endif
  arts_home_grantreq_queue_init(&db->pending_rw);
  atomic_store_explicit(&db->grant_baton, 0u, memory_order_relaxed);
#ifndef ARTS_RELEASE_PURGE
  atomic_store_explicit(&db->pending_install_owner, 0u, memory_order_relaxed);
#endif
#ifdef ARTS_RELEASE_PURGE
  atomic_store_explicit(&db->pending_return_from, ARTS_GRANT_NO_RETURNER,
                        memory_order_relaxed);
#endif

  /* Round plane: no round held, no ack outstanding. */
  atomic_store_explicit(&db->round_queue, (uintptr_t)0, memory_order_relaxed);
  atomic_store_explicit(&db->round_acks, 0u, memory_order_relaxed);
#ifdef ARTS_WRITE_POLICY_WT
  atomic_store_explicit(&db->hver, 1u, memory_order_relaxed);
#endif
  db->round_entries = NULL;
  arts_rank_bitset_init(&db->roster, nranks);
  assert(db->roster.nwords <= INV_ROUND_MAX_WORDS &&
         "a round keeps one target mask per roster word on its stack");
  arts_rank_bitset_init(&db->cached_ranks, nranks);
  arts_db_cover_home_init(db, nranks);
  /* A creator on another rank holds a cache of the block from the moment it
   * made it, so the destroy must reach that rank too: its cache would
   * otherwise outlive the block, keeping its storage and the rank's route
   * slot for the GUID. */
  if (rw_holder < nranks) {
    (void)arts_rank_bitset_set(&db->cached_ranks, rw_holder);
  }
}

void arts_db_home_teardown(struct arts_db_s *db) {
  if (db == NULL) {
    return;
  }
  arts_home_grantreq_queue_destroy(&db->pending_rw);
  /* Free the releases still queued and an armed round's batch.  Under the
   * destroy contract both are empty; outside it a round can still be armed
   * here (its acks lost to the destroy), while a held, unarmed one cannot:
   * its running holder pins the descriptor.  Relaxed: the descriptor's
   * last-reference decrement orders this after every pusher, each of which
   * pushed under a reference it released after. */
  struct arts_db_inv_pub_s *e =
      (struct arts_db_inv_pub_s *)(atomic_exchange_explicit(
                                       &db->round_queue, (uintptr_t)0,
                                       memory_order_relaxed) &
                                   ~INV_ROUND_OPEN);
  while (e != NULL) {
    struct arts_db_inv_pub_s *nx = e->next;
    arts_free(e);
    e = nx;
  }
  for (e = db->round_entries; e != NULL;) {
    struct arts_db_inv_pub_s *nx = e->next;
    arts_free(e);
    e = nx;
  }
  db->round_entries = NULL;
  arts_rank_bitset_destroy(&db->roster);
  arts_rank_bitset_destroy(&db->cached_ranks);
  arts_db_cover_home_teardown(db);
}
