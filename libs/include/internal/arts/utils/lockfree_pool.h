/* SPDX-License-Identifier: Apache-2.0
 *
 * arts_lockfree_pool_t — DWCAS tagged-head Treiber-stack object pool.
 *
 * Layout: 16-byte head = (arts_lf_link_t *ptr, uintptr_t tag).  Each push
 * increments the tag, so even if a popped node is re-pushed (ABA), the
 * DWCAS observes the tag mismatch and retries.  Lifetime invariant: no node
 * is freed while another thread can pop it — fetch/release transfer
 * ownership only, and only destroy, which no other thread may overlap, frees.
 * This makes the batch_fetch chain-walk safe (no use-after-free on `next`).
 *
 * Header-only inline — no separate .c file.  All callers compile with
 * -mcx16 on x86_64 (set globally in the top-level CMakeLists.txt).
 */

#ifndef ARTS_UTILS_LOCKFREE_POOL_H
#define ARTS_UTILS_LOCKFREE_POOL_H

#include "arts/utils/lockfree_lifo.h" /* arts_lf_link_t */

#include <stddef.h>
#include <stdint.h>
#ifndef __cplusplus
#include <stdalign.h>
#include <stdatomic.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** 16-byte DWCAS head (ptr + tag).  Must be 16-byte aligned for cmpxchg16b
 *  / ldaxp+stxp.  GCC and Clang issue lock-free 128-bit atomics for this
 *  layout when -mcx16 is set.
 *
 *  Note: GCC C rejects `typedef struct alignas(16) {...}` (placement of
 *  the alignment specifier between the `struct` keyword and `{` is not
 *  accepted).  We use the GNU/C11 attribute on the typedef name instead. */
typedef struct {
  arts_lf_link_t *ptr;
  uintptr_t tag;
} __attribute__((aligned(16))) arts_lf_pool_head_t;
#ifdef __cplusplus
static_assert(sizeof(arts_lf_pool_head_t) == 16, "DWCAS layout");
static_assert(alignof(arts_lf_pool_head_t) == 16, "DWCAS alignment");
#else
_Static_assert(sizeof(arts_lf_pool_head_t) == 16, "DWCAS layout");
_Static_assert(alignof(arts_lf_pool_head_t) == 16, "DWCAS alignment");
#endif

typedef struct {
#ifdef __cplusplus
  arts_lf_pool_head_t head; /* layout view — the real field is _Atomic in C */
#else
  _Atomic(arts_lf_pool_head_t) head;
#endif
} arts_lockfree_pool_t;

#ifndef __cplusplus
/* ── Initialization / teardown ─────────────────────────────────────────── */

static inline void arts_lf_pool_init(arts_lockfree_pool_t *p) {
  arts_lf_pool_head_t empty = {.ptr = NULL, .tag = 0};
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
  arts_lf_pool_head_t empty = {.ptr = NULL, .tag = cur.tag};
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
    arts_lf_pool_head_t newh = {.ptr = n, .tag = cur.tag + 1};
    if (atomic_compare_exchange_weak_explicit(
            &p->head, &cur, newh, memory_order_release, memory_order_relaxed)) {
      return;
    }
    /* cur reloaded by CAS — retry */
  }
}

/* ── Batch primitives ───────────────────────────────────────────────────── */

/** Try to detach up to `want` nodes from the top of the pool.  Walks the
 *  chain to determine the tail (or end), then DWCAS the new head.  On
 *  success, returns chain head and writes the actual count to *got.
 *  Returns NULL when the pool is empty (sets *got = 0).
 *
 *  Caller becomes the owner of the returned chain — must either re-push
 *  it, install it elsewhere, or free each node once no other thread can pop
 *  from the pool. */
static inline arts_lf_link_t *arts_lf_pool_batch_fetch(arts_lockfree_pool_t *p,
                                                       uint32_t want,
                                                       uint32_t *got) {
  if (got) {
    *got = 0;
  }
  if (want == 0) {
    return NULL;
  }
  arts_lf_pool_head_t cur =
      atomic_load_explicit(&p->head, memory_order_acquire);
  for (;;) {
    if (!cur.ptr) {
      if (got) {
        *got = 0;
      }
      return NULL;
    }
    /* Walk up to `want` nodes: safe because no node is freed while another
     * thread can pop it. */
    arts_lf_link_t *tail = cur.ptr;
    uint32_t taken = 1;
    arts_lf_link_t *next_after =
        atomic_load_explicit(&tail->next, memory_order_relaxed);
    while (taken < want && next_after) {
      tail = next_after;
      next_after = atomic_load_explicit(&tail->next, memory_order_relaxed);
      taken++;
    }
    arts_lf_pool_head_t newh = {.ptr = next_after, .tag = cur.tag + 1};
    if (atomic_compare_exchange_weak_explicit(
            &p->head, &cur, newh, memory_order_acquire, memory_order_acquire)) {
      /* Detach the chain (so caller doesn't accidentally walk into the
       * remaining pool nodes). */
      atomic_store_explicit(&tail->next, NULL, memory_order_relaxed);
      if (got) {
        *got = taken;
      }
      return cur.ptr;
    }
    /* cur reloaded — retry walk from new top. */
  }
}

/** Pop exactly one recycled node, or NULL if the pool is empty: the caller
 *  allocates on a miss.  Built on batch_fetch(1) so it shares the DWCAS
 *  ABA-safe pop. */
static inline arts_lf_link_t *
arts_lf_pool_pop_or_null(arts_lockfree_pool_t *p) {
  uint32_t got = 0;
  return arts_lf_pool_batch_fetch(p, 1, &got);
}

#endif /* !__cplusplus */

#ifdef __cplusplus
}
#endif

#endif /* ARTS_UTILS_LOCKFREE_POOL_H */
