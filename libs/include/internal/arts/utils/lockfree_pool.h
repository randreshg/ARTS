/* SPDX-License-Identifier: Apache-2.0
 *
 * arts_lockfree_pool_t — DWCAS tagged-head Treiber-stack object pool.
 *
 * Layout: 16-byte head = (arts_lf_link_t *ptr, uintptr_t tag).  Every push
 * and every pop increments the tag, so a pop whose CAS compares a head
 * that was popped and pushed back since its load (ABA) sees the tag differ
 * and retries.  Lifetime invariant: no node is freed while another thread
 * can pop it — pop/release transfer ownership only, and only destroy,
 * which no other thread may overlap, frees.  This is what makes a pop's read
 * of the top node's link safe (no use-after-free on `next`).
 *
 * Orders: a release CASes the head with release, publishing the node and
 * what its owner wrote into it; a pop loads and CASes the head with
 * acquire, taking them.  The node links are relaxed: the head's pair
 * carries them.
 *
 * Header-only inline — no separate .c file.  All callers compile with
 * -mcx16 on x86_64 (set globally in the top-level CMakeLists.txt).
 */

#ifndef ARTS_UTILS_LOCKFREE_POOL_H
#define ARTS_UTILS_LOCKFREE_POOL_H

#include "arts/utils/lockfree_lifo.h" /* arts_lf_link_t */
#include "arts/utils/stdatomic_compat.h"

#include <assert.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 16-byte DWCAS head (ptr + tag), 16-byte aligned as a 16-byte atomic
 *  requires (cmpxchg16b on x86-64; casp, or an ldaxp/stlxp loop, on
 *  AArch64).
 *
 *  Note: GCC C rejects `typedef struct alignas(16) {...}` (placement of
 *  the alignment specifier between the `struct` keyword and `{` is not
 *  accepted).  We use the GNU/C11 attribute on the typedef name instead. */
typedef struct {
  arts_lf_link_t *ptr;
  uintptr_t tag;
} __attribute__((aligned(16))) arts_lf_pool_head_t;
static_assert(sizeof(arts_lf_pool_head_t) == 16, "DWCAS layout");
static_assert(alignof(arts_lf_pool_head_t) == 16, "DWCAS alignment");

typedef struct {
  _Atomic(arts_lf_pool_head_t) head; /* orders: the Orders block above */
} arts_lockfree_pool_t;

/* ── Initialization / teardown ─────────────────────────────────────────── */

static inline void arts_lf_pool_init(arts_lockfree_pool_t *p) {
  arts_lf_pool_head_t empty = {NULL, 0};
  atomic_store_explicit(&p->head, empty, memory_order_relaxed);
}

/** Drain everything and free each node via `free_fn`.  Caller is
 *  responsible for ensuring no other thread touches the pool during destroy,
 *  and that `free_fn` is the matching free for whatever allocator produced
 *  the pool's nodes: a mismatched free is a correctness bug (double
 *  bookkeeping, or freeing memory the wrong allocator does not own). */
static inline void arts_lf_pool_destroy_with(arts_lockfree_pool_t *p,
                                             void (*free_fn)(void *)) {
  arts_lf_pool_head_t cur =
      atomic_load_explicit(&p->head, memory_order_acquire);
  arts_lf_link_t *node = cur.ptr;
  while (node) {
    arts_lf_link_t *next =
        atomic_load_explicit(&node->next, memory_order_relaxed);
    free_fn(node);
    node = next;
  }
  arts_lf_pool_head_t empty = {NULL, cur.tag};
  atomic_store_explicit(&p->head, empty, memory_order_relaxed);
}

/* ── Single-node release ───────────────────────────────────────────────── */

/** DWCAS push.  Tag is incremented by the writer (CAS install); reader
 *  side increments tag on pop too — both contribute to ABA defense. */
static inline void arts_lf_pool_release(arts_lockfree_pool_t *p, void *node) {
  arts_lf_link_t *n = (arts_lf_link_t *)node;
  arts_lf_pool_head_t cur =
      atomic_load_explicit(&p->head, memory_order_relaxed);
  for (;;) {
    atomic_store_explicit(&n->next, cur.ptr, memory_order_relaxed);
    arts_lf_pool_head_t newh = {n, cur.tag + 1};
    if (atomic_compare_exchange_weak_explicit(
            &p->head, &cur, newh, memory_order_release, memory_order_relaxed)) {
      return;
    }
    /* cur reloaded by CAS — retry */
  }
}

/* ── Single-node pop ───────────────────────────────────────────────────── */

/** Pop one recycled node, detached, or NULL if the pool is empty: the caller
 *  allocates on a miss.  The CAS's failure order stays acquire: a retry reads
 *  the link of the head it reloaded, so the reload must synchronize with the
 *  push that installed that head; a relaxed one could read an earlier value
 *  of the link (a previous pop's detach) and cut the pool short. */
static inline arts_lf_link_t *
arts_lf_pool_pop_or_null(arts_lockfree_pool_t *p) {
  arts_lf_pool_head_t cur =
      atomic_load_explicit(&p->head, memory_order_acquire);
  for (;;) {
    if (cur.ptr == NULL) {
      return NULL;
    }
    arts_lf_link_t *next =
        atomic_load_explicit(&cur.ptr->next, memory_order_relaxed);
    arts_lf_pool_head_t newh = {next, cur.tag + 1};
    if (atomic_compare_exchange_weak_explicit(
            &p->head, &cur, newh, memory_order_acquire, memory_order_acquire)) {
      atomic_store_explicit(&cur.ptr->next, NULL, memory_order_relaxed);
      return cur.ptr;
    }
    /* cur reloaded — retry from the new top. */
  }
}

#ifdef __cplusplus
}
#endif

#endif /* ARTS_UTILS_LOCKFREE_POOL_H */
