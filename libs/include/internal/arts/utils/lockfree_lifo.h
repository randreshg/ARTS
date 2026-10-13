/* SPDX-License-Identifier: Apache-2.0
 *
 * 8-byte head CAS Treiber LIFO (header-only, inline).
 *
 * ABA-freedom comes from the caller upholding the "no re-entry into the same
 * stack" invariant documented below, not from a packed tag counter — which is
 * why the head is a plain pointer and the whole structure is inlineable.
 */

#ifndef ARTS_UTILS_LOCKFREE_LIFO_H
#define ARTS_UTILS_LOCKFREE_LIFO_H

#include <stddef.h>

/* runtime_types.h pulls this header transitively into .cu translation
 * units (via arts_event_s).  C11 _Atomic isn't available in C++/nvcc, so
 * fall back to plain pointers for layout-only visibility there.  The
 * inline producer/consumer helpers below only compile in C. */
#ifdef __cplusplus

/** Common intrusive link node.  Embed as the first member of any
 *  caller struct that participates in lock-free stacks/pools. */
typedef struct arts_lf_link_s {
  struct arts_lf_link_s *next;
} arts_lf_link_t;

/** 8-byte head CAS Treiber stack.
 *
 * INVARIANT: a node may belong to at most one arts_lf_stack_t at a
 * time.  After pop/drain, the node must EITHER be freed OR pushed onto a
 * DIFFERENT stack (e.g., an arts_lockfree_pool_t).  Pushing the
 * same node back onto the same stack invalidates the ABA-free guarantee. */
typedef struct {
  arts_lf_link_t *head;
} arts_lf_stack_t;

#else /* C path with _Atomic + inline helpers */

#include <stdatomic.h>
#include <stdbool.h>

/** Common intrusive link node.  Embed as the first member of any
 *  caller struct that participates in lock-free stacks/pools. */
typedef struct arts_lf_link_s {
  _Atomic(struct arts_lf_link_s *) next;
} arts_lf_link_t;

/** 8-byte head CAS Treiber stack.
 *
 * INVARIANT: a node may belong to at most one arts_lf_stack_t at a
 * time.  After pop/drain, the node must EITHER be freed OR pushed onto a
 * DIFFERENT stack (e.g., an arts_lockfree_pool_t).  Pushing the
 * same node back onto the same stack invalidates the ABA-free guarantee. */
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
 *  proceeds re-synchronizes on its own acquire. */
static inline bool arts_lf_stack_empty(arts_lf_stack_t *s) {
  return atomic_load_explicit(&s->head, memory_order_relaxed) == NULL;
}

static inline void arts_lf_stack_push(arts_lf_stack_t *s,
                                      arts_lf_link_t *node) {
  arts_lf_link_t *old = atomic_load_explicit(&s->head, memory_order_relaxed);
  do {
    atomic_store_explicit(&node->next, old, memory_order_relaxed);
  } while (!atomic_compare_exchange_weak_explicit(
      &s->head, &old, node, memory_order_release, memory_order_relaxed));
}

/** Atomically detach the entire chain.  Returns LIFO order (top of stack
 *  first). */
static inline arts_lf_link_t *arts_lf_stack_drain(arts_lf_stack_t *s) {
  return atomic_exchange_explicit(&s->head, NULL, memory_order_acquire);
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

#endif /* __cplusplus */

/* Both definitions are one pointer-width cell at offset 0, aligned as a
 * pointer, so a structure that embeds them has one layout in C and in C++. */
#ifdef __cplusplus
#define ARTS_LF_LAYOUT_ASSERT(c, m) static_assert((c), m)
#define ARTS_LF_ALIGNOF(t) alignof(t)
#else
#define ARTS_LF_LAYOUT_ASSERT(c, m) _Static_assert((c), m)
#define ARTS_LF_ALIGNOF(t) _Alignof(t)
#endif
ARTS_LF_LAYOUT_ASSERT(sizeof(arts_lf_link_t) == sizeof(void *),
                      "arts_lf_link_t must be one pointer wide");
ARTS_LF_LAYOUT_ASSERT(offsetof(arts_lf_link_t, next) == 0,
                      "arts_lf_link_t.next must be at offset 0");
ARTS_LF_LAYOUT_ASSERT(ARTS_LF_ALIGNOF(arts_lf_link_t) ==
                          ARTS_LF_ALIGNOF(void *),
                      "arts_lf_link_t must be aligned as a pointer");
ARTS_LF_LAYOUT_ASSERT(sizeof(arts_lf_stack_t) == sizeof(void *),
                      "arts_lf_stack_t must be one pointer wide");
ARTS_LF_LAYOUT_ASSERT(offsetof(arts_lf_stack_t, head) == 0,
                      "arts_lf_stack_t.head must be at offset 0");
ARTS_LF_LAYOUT_ASSERT(ARTS_LF_ALIGNOF(arts_lf_stack_t) ==
                          ARTS_LF_ALIGNOF(void *),
                      "arts_lf_stack_t must be aligned as a pointer");
#undef ARTS_LF_LAYOUT_ASSERT
#undef ARTS_LF_ALIGNOF

#endif /* ARTS_UTILS_LOCKFREE_LIFO_H */
