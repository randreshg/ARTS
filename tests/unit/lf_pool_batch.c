/* SPDX-License-Identifier: Apache-2.0
 *
 * arts_lockfree_pool_t's primitives racing one head (lockfree_pool.h):
 * single release, single pop (pop_or_null) and batch_fetch, plus the batch
 * edge cases — want==0, want > available, an empty pool.
 *
 * The pool's lifetime invariant (no node is freed while another thread can
 * pop it) means a node is never freed by a fetch — ownership only transfers.
 * So a fixed universe of UNIVERSE nodes is seeded, many threads churn it
 * through the primitives, and every node must be accounted for exactly once
 * throughout: each node carries an owner stamp a thread takes by CAS when the
 * pool hands it the node and gives back by CAS before releasing it, so a node
 * handed to two threads at once (a lost or duplicated push or pop) fails the
 * CAS.
 */

#include "arts/utils/lockfree_pool.h"

#include <pthread.h>
#include <stdatomic.h>
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
  _Atomic int owner; /* -1 = in pool, else owning thread index */
} node_t;

static arts_lockfree_pool_t g_pool;
static node_t *g_nodes[UNIVERSE];
static atomic_int g_start;

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
                  "FAIL lf_pool_batch: node %u handed out while owned by %d\n",
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
                  "FAIL lf_pool_batch: releasing node %u not owned by me "
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
    switch ((int)(r % 3)) {
    case 0: { /* single pop */
      if (c->hand_n < HAND_CAP) {
        arts_lf_link_t *l = arts_lf_pool_pop_or_null(&g_pool);
        if (l != NULL) {
          if (atomic_load_explicit(&l->next, memory_order_relaxed) != NULL) {
            (void)fprintf(stderr,
                          "FAIL lf_pool_batch: popped node not detached\n");
            abort();
          }
          take(c, l);
        }
      }
      break;
    }
    case 1: { /* single release */
      if (c->hand_n > 0) {
        give(c, c->hand[--c->hand_n]);
      }
      break;
    }
    default: { /* batch_fetch up to 8 */
      uint32_t want = 1 + (uint32_t)((r >> 2) & 7);
      uint32_t got = 0;
      arts_lf_link_t *chain = arts_lf_pool_batch_fetch(&g_pool, want, &got);
      uint32_t cnt = 0;
      while (chain) {
        arts_lf_link_t *next =
            atomic_load_explicit(&chain->next, memory_order_relaxed);
        if (c->hand_n == HAND_CAP) {
          give(c, c->hand[--c->hand_n]);
        }
        take(c, chain);
        cnt++;
        chain = next;
      }
      if (cnt != got || got > want) {
        (void)fprintf(stderr,
                      "FAIL lf_pool_batch: batch_fetch want=%u got=%u but "
                      "chain=%u\n",
                      want, got, cnt);
        abort();
      }
      break;
    }
    }
  }
  return NULL;
}

/* ---- single-thread edge cases ---- */
static int edge_cases(void) {
  arts_lockfree_pool_t p;
  arts_lf_pool_init(&p);

  uint32_t got = 99;
  arts_lf_link_t *c = arts_lf_pool_batch_fetch(&p, 4, &got);
  if (c != NULL || got != 0) {
    (void)fprintf(stderr, "FAIL lf_pool_batch: empty fetch c=%p got=%u\n",
                  (void *)c, got);
    return 1;
  }
  if (arts_lf_pool_pop_or_null(&p) != NULL) {
    (void)fprintf(stderr, "FAIL lf_pool_batch: empty pop returned a node\n");
    return 1;
  }
  got = 99;
  c = arts_lf_pool_batch_fetch(&p, 0, &got);
  if (c != NULL || got != 0) {
    (void)fprintf(stderr, "FAIL lf_pool_batch: want0 c=%p got=%u\n", (void *)c,
                  got);
    return 1;
  }

  /* One node in the pool: a fetch that wants more returns it alone,
   * detached. */
  node_t *one = (node_t *)calloc(1, sizeof(node_t));
  if (one == NULL) {
    return 1;
  }
  arts_lf_pool_release(&p, &one->link);
  got = 0;
  c = arts_lf_pool_batch_fetch(&p, 8, &got);
  if (c != &one->link || got != 1) {
    (void)fprintf(stderr,
                  "FAIL lf_pool_batch: over-want c=%p got=%u (want the one "
                  "node)\n",
                  (void *)c, got);
    return 1;
  }
  if (atomic_load_explicit(&c->next, memory_order_relaxed) != NULL) {
    (void)fprintf(stderr, "FAIL lf_pool_batch: fetched chain not detached\n");
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
  unsigned in_pool = 0;
  uint32_t got = 0;
  arts_lf_link_t *chain = arts_lf_pool_batch_fetch(&g_pool, UNIVERSE + 1, &got);
  for (arts_lf_link_t *l = chain; l != NULL;
       l = atomic_load_explicit(&l->next, memory_order_relaxed)) {
    in_pool++;
  }
  for (int i = 0; i < UNIVERSE; i++) {
    int o = atomic_load_explicit(&g_nodes[i]->owner, memory_order_relaxed);
    if (o != -1) {
      (void)fprintf(stderr, "FAIL lf_pool_batch: node %d still owned by %d\n",
                    i, o);
      return 1;
    }
  }
  if (in_pool != UNIVERSE || got != UNIVERSE) {
    (void)fprintf(stderr,
                  "FAIL lf_pool_batch: pool holds %u nodes (got=%u), "
                  "want %d\n",
                  in_pool, got, UNIVERSE);
    return 1;
  }
  for (int i = 0; i < UNIVERSE; i++) {
    free(g_nodes[i]);
  }
  printf("PASS lf_pool_batch: %d threads x %d iters, release / pop / "
         "batch_fetch raced one head, %d-node universe conserved (no "
         "loss/dup), edge cases OK\n",
         THREADS, ITERS, UNIVERSE);
  return 0;
}
