/* SPDX-License-Identifier: Apache-2.0
 *
 * INV protocol shared home-side infrastructure.
 *
 * Defines: arts_home_grantreq_queue_{init,push,pop,peek,empty,pending,destroy},
 *          arts_db_home_init, arts_db_home_teardown.
 *
 * arts_handler_db_destroy lives in engine.c: it fans out over
 * the wire, and this TU must stay linkable standalone (pure queue/lifecycle)
 * for the whitebox unit tests.
 *
 * This TU carries the pieces shared by both write-policy variants: the home
 * request FIFO and the home-directory lifecycle.  The
 * arbiters live in arbiters.c, and the handler/sender bodies live in the
 * write-policy TU (wt.c or wb.c) plus the shared engine.c.
 *
 * The grantreq queue is this arm's own copy of the home's Vyukov MPSC request
 * FIFO, over the struct inv/types.h defines.
 *
 * Compiled only for ARTS_COHERENCE_PROTOCOL=INV.
 */

/* inv/types.h must precede directory.h: it defines arts_home_grantreq_node_s and
 * arts_home_grantreq_queue_s for the INV build. */
#include "arts/coherence/inv/types.h"

#include "arts/utils/stdatomic_compat.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "arts/coherence/coherence.h"
#include "arts/coherence/directory.h"
#include "arts/utils/malloc.h"

/* ===== Home REQUEST FIFO (Vyukov MPSC) ================================== */

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
  /* Grant plane: the creator boots holding it (arts_db_create defaults to an
   * RW acquire under the OCR contract).  No count here: ownership lives on
   * the holder's writer_count, and a revoking home also names the holder
   * now; a home whose grants come back unasked names none. */
#ifndef ARTS_RELEASE_PURGE
  atomic_store_explicit(&db->rw_holder, rw_holder, memory_order_relaxed);
#endif
  arts_home_grantreq_queue_init(&db->pending_rw);
  atomic_store_explicit(&db->invalidate_in_flight, 0u, memory_order_relaxed);
#ifndef ARTS_RELEASE_PURGE
  atomic_store_explicit(&db->pending_install_owner, 0u, memory_order_relaxed);
#endif
#ifdef ARTS_RELEASE_PURGE
  atomic_store_explicit(&db->pending_return_from, ARTS_GRANT_NO_RETURNER,
                        memory_order_relaxed);
#endif

  /* Round plane: no round open, no acks outstanding. */
  atomic_store_explicit(&db->dir_state, INV_DIR_MAKE(0u, 0u),
                        memory_order_relaxed);
  atomic_store_explicit(&db->hver, 1u, memory_order_relaxed);
  arts_lf_stack_init(&db->pub_queue);
  db->round_entries = NULL;
  arts_rank_bitset_init(&db->roster, nranks);
  arts_rank_bitset_init(&db->cached_ranks, nranks);
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
  /* Drain + free anything still queued.  Single-threaded at teardown: the
   * route slot is already absent, so no concurrent push can land. */
  arts_lf_link_t *node = arts_lf_stack_drain(&db->pub_queue);
  while (node != NULL) {
    arts_lf_link_t *nx = atomic_load_explicit(&node->next, memory_order_relaxed);
    arts_free(ARTS_CONTAINER_OF(node, struct arts_db_inv_pub_s, link));
    node = nx;
  }
  arts_rank_bitset_destroy(&db->roster);
  arts_rank_bitset_destroy(&db->cached_ranks);
}
