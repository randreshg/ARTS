/* SPDX-License-Identifier: Apache-2.0
 *
 * arts_mpsc_t — Vyukov intrusive multi-producer / single-consumer queue.
 *
 * Producers append with a single atomic_exchange on the head (wait-free);
 * the linking store that follows is what a concurrent consumer may briefly
 * observe as "inconsistent" (a producer has claimed the tail slot but not
 * yet linked it) — pop reports empty in that window and the next attempt
 * succeeds.  Exactly one consumer may run pop at a time: which thread that
 * is, is the owning structure's discipline, not the queue's.
 *
 * Node type: the queue rides the common intrusive link `arts_lf_link_t`
 * (a single atomic `next` cell, also used by arts_lf_stack_t).  Embed
 * it as the first member of any payload and recover the payload by casting
 * the popped node.  A node is on at most one list (queue, stack or pool) at
 * a time, since it has one link cell; once popped it may be pushed again onto
 * any list, this queue included — pop itself re-pushes the queue's stub.
 * Neither operation compares a node identity it read earlier, so reuse
 * exposes the queue to no ABA.
 *
 * Orders:
 *   head     producers exchange it acq_rel.  The release publishes the node's
 *            own NULL link; the acquire orders that previous node's NULL
 *            link before this producer's store into it, so the link cannot
 *            be overwritten back to NULL and the node lost.  The consumer
 *            reads it with acquire to tell an empty queue from a producer
 *            between its exchange and its link store.
 *   next     a producer's release store of prev->next publishes its node and
 *            what was written into it before the push; the consumer's
 *            acquire load of tail->next takes it.
 *   tail     the single consumer's own; plain.
 */

#ifndef ARTS_UTILS_MPSC_H
#define ARTS_UTILS_MPSC_H
#ifdef __cplusplus
extern "C" {
#endif

#include "arts/utils/lockfree_lifo.h" /* arts_lf_link_t */
#include "arts/utils/stdatomic_compat.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  _Atomic(arts_lf_link_t *) head; /* producers tail-append via xchg */
  arts_lf_link_t *tail;           /* single consumer reads/advances */
  arts_lf_link_t stub;            /* embedded sentinel */
} arts_mpsc_t;

static inline void arts_mpsc_init(arts_mpsc_t *q) {
  atomic_store_explicit(&q->stub.next, NULL, memory_order_relaxed);
  atomic_store_explicit(&q->head, &q->stub, memory_order_relaxed);
  q->tail = &q->stub;
}

/* Lock-free; any number of producers may run concurrently. */
static inline void arts_mpsc_push(arts_mpsc_t *q, arts_lf_link_t *n) {
  atomic_store_explicit(&n->next, NULL, memory_order_relaxed);
  arts_lf_link_t *prev =
      atomic_exchange_explicit(&q->head, n, memory_order_acq_rel);
  atomic_store_explicit(&prev->next, n, memory_order_release);
}

/* Single-consumer pop; the caller MUST be the queue's only consumer.  Returns
 * NULL when the queue is empty or transiently inconsistent (a producer is
 * mid-link). */
static inline arts_lf_link_t *arts_mpsc_pop(arts_mpsc_t *q) {
  arts_lf_link_t *tail = q->tail;
  arts_lf_link_t *next =
      atomic_load_explicit(&tail->next, memory_order_acquire);
  if (tail == &q->stub) {
    if (!next) {
      return NULL; /* empty */
    }
    q->tail = next;
    tail = next;
    next = atomic_load_explicit(&tail->next, memory_order_acquire);
  }
  if (next) {
    q->tail = next;
    return tail;
  }
  arts_lf_link_t *head = atomic_load_explicit(&q->head, memory_order_acquire);
  if (tail != head) {
    return NULL; /* producer mid-link — retry later */
  }
  /* Re-thread the stub to make the single remaining node poppable. */
  arts_mpsc_push(q, &q->stub);
  next = atomic_load_explicit(&tail->next, memory_order_acquire);
  if (next) {
    q->tail = next;
    return tail;
  }
  return NULL;
}

/*
 * arts_mpsc_drain_remaining — single-consumer cleanup.  Pops every node
 * currently linked and returns them as a NULL-terminated chain (the popped
 * node's `next` field re-threaded to the following popped node), which the
 * caller frees.  The embedded `stub` is never returned.
 *
 * Caller MUST be the sole consumer (a teardown path where no producer can
 * push and no other consumer runs).
 * Because pop() can transiently report empty while a producer is mid-link,
 * this is intended for a quiescent queue (object teardown): all producers
 * have stopped, so a single pass drains everything.
 */
static inline arts_lf_link_t *arts_mpsc_drain_remaining(arts_mpsc_t *q) {
  arts_lf_link_t *chain = NULL;
  arts_lf_link_t *tail = NULL;
  for (;;) {
    arts_lf_link_t *n = arts_mpsc_pop(q);
    if (!n) {
      break;
    }
    atomic_store_explicit(&n->next, NULL, memory_order_relaxed);
    if (tail) {
      atomic_store_explicit(&tail->next, n, memory_order_relaxed);
    } else {
      chain = n;
    }
    tail = n;
  }
  return chain;
}

#ifdef __cplusplus
}
#endif
#endif /* ARTS_UTILS_MPSC_H */
