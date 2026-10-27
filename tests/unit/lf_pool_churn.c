/* SPDX-License-Identifier: Apache-2.0
 *
 * arts_lockfree_pool_t's primitives racing one head (lockfree_pool.h):
 * release and pop_or_null, plus the empty pool's pop.
 *
 * The pool's lifetime invariant (no node is freed while another thread can
 * pop it) means a node is never freed by a pop — ownership only transfers.
 * So a fixed universe of UNIVERSE nodes is seeded, many threads churn it
 * through the primitives, and every node must be accounted for exactly once
 * throughout: each node carries an owner stamp a thread takes by CAS when the
 * pool hands it the node and gives back by CAS before releasing it, so a node
 * handed to two threads at once (a lost or duplicated push or pop) fails the
 * CAS.
 */

#include "arts/utils/lockfree_pool.h"

#include "arts/utils/stdatomic_compat.h"
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define UNIVERSE 512
#define THREADS 8
#define HAND_CAP 64
#define ITERS 200000

typedef struct {
  arts_lf_link_t link; /* first member */
  uint32_t id;
  _Atomic(int) owner; /* -1 = in pool, else owning thread index */
} node_t;

static arts_lockfree_pool_t g_pool;
static node_t *g_nodes[UNIVERSE];
static _Atomic(int) g_start;

static inline node_t *as_node(arts_lf_link_t *l) { return (node_t *)l; }

typedef struct {
  int tid;
  arts_lf_link_t *hand[HAND_CAP];
  int hand_n;
  uint64_t rng;
} ctx_t;

static inline uint64_t xs(uint64_t *s) {
  uint64_t x = *s;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *s = x;
  return x;
}

static void take(ctx_t *c, arts_lf_link_t *l) {
  node_t *n = as_node(l);
  int expect = -1;
  if (!atomic_compare_exchange_strong_explicit(&n->owner, &expect, c->tid,
                                               memory_order_acq_rel,
                                               memory_order_acquire)) {
    (void)fprintf(stderr,
                  "FAIL lf_pool_churn: node %u handed out while owned by %d\n",
                  n->id, expect);
    abort();
  }
  c->hand[c->hand_n++] = l;
}

static void give(ctx_t *c, arts_lf_link_t *l) {
  node_t *n = as_node(l);
  int expect = c->tid;
  if (!atomic_compare_exchange_strong_explicit(
          &n->owner, &expect, -1, memory_order_acq_rel, memory_order_acquire)) {
    (void)fprintf(stderr,
                  "FAIL lf_pool_churn: releasing node %u not owned by me "
                  "(owner=%d)\n",
                  n->id, expect);
    abort();
  }
  arts_lf_pool_release(&g_pool, l);
}

static void *worker(void *arg) {
  ctx_t *c = (ctx_t *)arg;
  while (atomic_load_explicit(&g_start, memory_order_acquire) == 0) {
  }
  for (int i = 0; i < ITERS; i++) {
    uint64_t r = xs(&c->rng);
    if ((r & 1u) == 0u) { /* pop */
      if (c->hand_n < HAND_CAP) {
        arts_lf_link_t *l = arts_lf_pool_pop_or_null(&g_pool);
        if (l != NULL) {
          if (atomic_load_explicit(&l->next, memory_order_relaxed) != NULL) {
            (void)fprintf(stderr,
                          "FAIL lf_pool_churn: popped node not detached\n");
            abort();
          }
          take(c, l);
        }
      }
    } else if (c->hand_n > 0) { /* release */
      give(c, c->hand[--c->hand_n]);
    }
  }
  return NULL;
}

/* ---- single-thread edge cases ---- */
static int edge_cases(void) {
  arts_lockfree_pool_t p;
  arts_lf_pool_init(&p);
  if (arts_lf_pool_pop_or_null(&p) != NULL) {
    (void)fprintf(stderr, "FAIL lf_pool_churn: empty pop returned a node\n");
    return 1;
  }
  /* One node in the pool: it comes back alone, detached, and the pool is
   * empty again. */
  node_t *one = (node_t *)calloc(1, sizeof(node_t));
  if (one == NULL) {
    return 1;
  }
  arts_lf_pool_release(&p, &one->link);
  arts_lf_link_t *c = arts_lf_pool_pop_or_null(&p);
  if (c != &one->link ||
      atomic_load_explicit(&c->next, memory_order_relaxed) != NULL ||
      arts_lf_pool_pop_or_null(&p) != NULL) {
    (void)fprintf(stderr, "FAIL lf_pool_churn: one-node pop c=%p\n", (void *)c);
    return 1;
  }
  arts_lf_pool_release(&p, c);
  arts_lf_pool_destroy_with(&p, free);
  return 0;
}

int main(void) {
  if (edge_cases() != 0) {
    return 1;
  }

  arts_lf_pool_init(&g_pool);
  atomic_init(&g_start, 0);
  for (int i = 0; i < UNIVERSE; i++) {
    g_nodes[i] = (node_t *)calloc(1, sizeof(node_t));
    if (!g_nodes[i]) {
      return 1;
    }
    g_nodes[i]->id = (uint32_t)i;
    atomic_init(&g_nodes[i]->owner, -1);
    arts_lf_pool_release(&g_pool, &g_nodes[i]->link);
  }

  pthread_t th[THREADS];
  ctx_t ctx[THREADS];
  for (int i = 0; i < THREADS; i++) {
    ctx[i].tid = i;
    ctx[i].hand_n = 0;
    ctx[i].rng = 0x1234567u + (uint64_t)i * 0x9E3779B9u;
    pthread_create(&th[i], NULL, worker, &ctx[i]);
  }
  atomic_store_explicit(&g_start, 1, memory_order_release);
  for (int i = 0; i < THREADS; i++) {
    pthread_join(th[i], NULL);
  }

  for (int i = 0; i < THREADS; i++) {
    while (ctx[i].hand_n > 0) {
      give(&ctx[i], ctx[i].hand[--ctx[i].hand_n]);
    }
  }

  /* Every universe node is back in the pool, and the pool holds exactly the
   * universe. */
  for (int i = 0; i < UNIVERSE; i++) {
    int o = atomic_load_explicit(&g_nodes[i]->owner, memory_order_relaxed);
    if (o != -1) {
      (void)fprintf(stderr, "FAIL lf_pool_churn: node %d still owned by %d\n",
                    i, o);
      return 1;
    }
  }
  unsigned in_pool = 0;
  while (arts_lf_pool_pop_or_null(&g_pool) != NULL) {
    in_pool++;
  }
  if (in_pool != UNIVERSE) {
    (void)fprintf(stderr, "FAIL lf_pool_churn: pool holds %u nodes, want %d\n",
                  in_pool, UNIVERSE);
    return 1;
  }
  for (int i = 0; i < UNIVERSE; i++) {
    free(g_nodes[i]);
  }
  printf("PASS lf_pool_churn: %d threads x %d iters, release / pop raced one "
         "head, %d-node universe conserved (no loss/dup), edge cases OK\n",
         THREADS, ITERS, UNIVERSE);
  return 0;
}
