/* SPDX-License-Identifier: Apache-2.0
 *
 * The home's ownership-request FIFO (Vyukov MPSC), shared by every arm that
 * keeps one.  Declarations, orders and the progress guarantee:
 * coherence/directory.h.
 *
 * Defines: arts_home_grantreq_queue_{init,push,pop,peek,empty,pending,destroy}.
 *
 * Compiled for ARTS_COHERENCE_PROTOCOL = VAL, INV and EXCL.
 */

#include "arts/coherence/directory.h"

#include "arts/utils/stdatomic_compat.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arts/coherence/coherence.h"
#include "arts/utils/malloc.h"

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
  /* Same mid-link disambiguation as pop: returning "no front" while head !=
   * tail would contradict the dereference-free pending probe, which counts
   * that producer. */
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
  /* Both loads may name freed nodes; that is harmless because the serving
   * turn's releases order them: the last release's probe can have no pop
   * between its two loads, since any such pop belongs to a later holder who
   * must release again. */
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
