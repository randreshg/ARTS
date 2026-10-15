/* SPDX-License-Identifier: Apache-2.0
 *
 * Treiber LIFO with an 8-byte CAS head (header-only, inline).
 *
 * Push needs only head equality and links behind whatever the head is, and
 * drain takes the whole chain in one exchange, so neither is exposed to ABA
 * however nodes are reused, and the head needs no tag.  Two rules keep it so:
 * a node is on at most one list at a time (it has one link cell), and the
 * stack has no single-node pop, whose CAS would compare a head that may have
 * been popped, reused and pushed back in between.
 */

#ifndef ARTS_UTILS_LOCKFREE_LIFO_H
#define ARTS_UTILS_LOCKFREE_LIFO_H

#include "arts/utils/stdatomic_compat.h"

#include <assert.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>

/** Common intrusive link node.  Embed as the first member of any
 *  caller struct that participates in lock-free stacks/pools/queues.
 *
 *  `next` belongs to whichever list holds the node.  On a stack or a pool it
 *  is written by the node's owner before the RMW that publishes the node and
 *  read by the owner of the chain an RMW took out, so that RMW pair carries
 *  it and its own accesses are relaxed; the MPSC queue publishes through it
 *  (see mpsc.h). */
typedef struct arts_lf_link_s {
  _Atomic(struct arts_lf_link_s *) next;
} arts_lf_link_t;

/** 8-byte head CAS Treiber stack.
 *
 *  `head`: pushers CAS a node in, drainers exchange the whole chain out.
 *  Both RMWs are acq_rel, and every write of `head` after init is one of
 *  them, so of a push and a drain the later in head's modification order
 *  reads from the earlier or from an RMW after it, and synchronizes with the
 *  earlier: a drain sees each node pushed since the last drain, with what
 *  its pusher wrote before the push, and a push after a drain sees what the
 *  drainer wrote before the drain.  So a caller that pushes and then loads
 *  another word, against one that stores that word and then drains the same
 *  stack, needs no fence.  The emptiness probe is a relaxed load that
 *  carries only its value. */
typedef struct {
  _Atomic(arts_lf_link_t *) head;
} arts_lf_stack_t;

static inline void arts_lf_stack_init(arts_lf_stack_t *s) {
  atomic_store_explicit(&s->head, NULL, memory_order_relaxed);
}

/** Read-only emptiness probe.  Never writes the head line, so any number of
 *  concurrent pollers keep it in shared cache state — the scalable gate for
 *  poll-before-consume loops (a failed CAS would still take the line
 *  exclusive and ping-pong it between pollers).  Relaxed: a poller that
 *  observes a stale NULL simply retries on its next poll; the consumer that
 *  proceeds re-synchronizes on its own drain. */
static inline bool arts_lf_stack_empty(arts_lf_stack_t *s) {
  return atomic_load_explicit(&s->head, memory_order_relaxed) == NULL;
}

/* A failed CAS only re-reads the head to relink behind it; it consumes
 * nothing, so its order is relaxed. */
static inline void arts_lf_stack_push(arts_lf_stack_t *s,
                                      arts_lf_link_t *node) {
  arts_lf_link_t *old = atomic_load_explicit(&s->head, memory_order_relaxed);
  do {
    atomic_store_explicit(&node->next, old, memory_order_relaxed);
  } while (!atomic_compare_exchange_weak_explicit(
      &s->head, &old, node, memory_order_acq_rel, memory_order_relaxed));
}

/** Atomically detach the entire chain.  Returns LIFO order (top of stack
 *  first). */
static inline arts_lf_link_t *arts_lf_stack_drain(arts_lf_stack_t *s) {
  return atomic_exchange_explicit(&s->head, NULL, memory_order_acq_rel);
}

/** Drain + reverse — returns FIFO order (oldest push first). */
static inline arts_lf_link_t *arts_lf_stack_reverse_drain(arts_lf_stack_t *s) {
  arts_lf_link_t *lifo = arts_lf_stack_drain(s);
  arts_lf_link_t *fifo = NULL;
  while (lifo) {
    arts_lf_link_t *next =
        atomic_load_explicit(&lifo->next, memory_order_relaxed);
    atomic_store_explicit(&lifo->next, fifo, memory_order_relaxed);
    fifo = lifo;
    lifo = next;
  }
  return fifo;
}

/* One pointer-width cell at offset 0, aligned as a pointer, so a structure
 * that embeds either has one layout in C and in C++. */
static_assert(sizeof(arts_lf_link_t) == sizeof(void *),
              "arts_lf_link_t must be one pointer wide");
static_assert(offsetof(arts_lf_link_t, next) == 0,
              "arts_lf_link_t.next must be at offset 0");
static_assert(alignof(arts_lf_link_t) == alignof(void *),
              "arts_lf_link_t must be aligned as a pointer");
static_assert(sizeof(arts_lf_stack_t) == sizeof(void *),
              "arts_lf_stack_t must be one pointer wide");
static_assert(offsetof(arts_lf_stack_t, head) == 0,
              "arts_lf_stack_t.head must be at offset 0");
static_assert(alignof(arts_lf_stack_t) == alignof(void *),
              "arts_lf_stack_t must be aligned as a pointer");

#endif /* ARTS_UTILS_LOCKFREE_LIFO_H */
