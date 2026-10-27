/* SPDX-License-Identifier: Apache-2.0
 *
 * The parked-reader pool of a VAL or INV cache, driven directly with no
 * runtime started.
 *
 *   1. More than 2^18 readers park on one cache: each takes a pool index,
 *      writes its node and links itself as the chain head in the arm's own
 *      decision CAS (the arm's arbiter on its request word), and the word
 *      carries the newest index whole.  The transitions that serve the
 *      chains take them, and a walk finds every reader once, with the
 *      fields it wrote.
 *   2. Each served chain goes back to the pool through the runtime's serve
 *      (one push after its wakes), and the next allocations take exactly
 *      those nodes before any fresh index.
 *   3. Threads allocating, chaining and returning nodes at once never hold
 *      one index together, and afterwards the free list holds every index
 *      ever handed out, once.
 *   4. The cache's own destructor returns the pool's storage while readers
 *      are still parked on it: the pool's directory is cleared, and in a
 *      tree whose allocations LeakSanitizer sees, a storage the destructor
 *      missed fails the run.
 *
 * Links the tree's runtime for the cell's arbiter, pool and destructor.
 */
#include "arts/coherence/coherence.h"
#include "arts/coherence/types.h"
#include "arts/gas/guid.h"
#include "arts/system/identity.h"
#include "arts/utils/stdatomic_compat.h"

#include <pthread.h>
#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(ARTS_PROTOCOL_VAL) && !defined(ARTS_PROTOCOL_INV)
#error "db_reader_pool is built only where the arm chains readers by index"
#endif

#define NPARK ((1u << 18) + 4096u)
#define NTHREADS 4
#define NITER 400
#define MAXHOLD 300u

static int g_fail;

#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      (void)fprintf(stderr, "FAIL db_reader_pool: " __VA_ARGS__);              \
      (void)fprintf(stderr, "\n");                                             \
      g_fail = 1;                                                              \
    }                                                                          \
  } while (0)

static arts_guid_t guid_of(uint32_t idx) {
  return (arts_guid_t)(0x5A00000000000000ULL | idx);
}
static unsigned int slot_of(uint32_t idx) { return idx ^ 0x2B5u; }

/* ---- the arm's request word -------------------------------------------- */

#if defined(ARTS_PROTOCOL_INV)
#define WORD(c) (&(c)->cache_state)
#define OP_ACQ_RO INV_CACHE_OP_ACQ_RO
#define ACT_PARK INV_CACHE_ACT_PARK
#define ACT_SEND INV_CACHE_ACT_SEND_RO
static uint64_t arbiter(uint64_t cur, int op, uint32_t arg, uint32_t *act) {
  return inv_cache_compute_next(cur, op, arg, act);
}
/* The newest parked index, wherever the arm keeps it. */
static uint32_t newest(uint64_t w) { return INV_CACHE_HEAD_RO(w); }
static uint32_t link_of(uint64_t cur, uint32_t act) {
  (void)act;
  return INV_CACHE_HEAD_RO(cur);
}
#else
#define WORD(c) (&(c)->req_state)
#define OP_ACQ_RO VREQ_OP_ACQ_RO
#define ACT_PARK VREQ_ACT_PARK
#define ACT_SEND VREQ_ACT_SEND_RO
static uint64_t arbiter(uint64_t cur, int op, uint32_t arg, uint32_t *act) {
  return val_req_compute_next(cur, op, arg, act);
}
static uint32_t newest(uint64_t w) { return VREQ_NEXT(w); }
static uint32_t link_of(uint64_t cur, uint32_t act) {
  return act == VREQ_ACT_PARK ? VREQ_NEXT(cur) : 0u;
}
#endif

/* Commit `op`; returns the pre-image, *post the image. */
static uint64_t step(struct arts_db_cache_s *c, int op, uint32_t arg,
                     uint64_t *post, uint32_t *act) {
  uint64_t cur = atomic_load_explicit(WORD(c), memory_order_acquire);
  for (;;) {
    uint64_t next = arbiter(cur, op, arg, act);
    if (next == cur ||
        atomic_compare_exchange_weak_explicit(WORD(c), &cur, next,
                                              memory_order_acq_rel,
                                              memory_order_acquire)) {
      *post = next;
      return cur;
    }
  }
}

/* Park one reader as the engine does: the node's link is written before
 * every attempt of the CAS that links it. */
static uint32_t park(struct arts_db_cache_s *c) {
  uint32_t idx = arts_db_reader_alloc(&c->reader_pool);
  struct arts_db_reader_node_s *n = arts_db_reader_node(&c->reader_pool, idx);
  n->edt_guid = guid_of(idx);
  n->slot = slot_of(idx);
  uint64_t cur = atomic_load_explicit(WORD(c), memory_order_acquire);
  uint32_t act;
  for (;;) {
    uint64_t next = arbiter(cur, OP_ACQ_RO, idx, &act);
    atomic_store_explicit(&n->next, link_of(cur, act), memory_order_relaxed);
    if (atomic_compare_exchange_weak_explicit(WORD(c), &cur, next,
                                              memory_order_acq_rel,
                                              memory_order_acquire)) {
      break;
    }
  }
  CHECK(act == ACT_PARK || act == ACT_SEND, "reader %u neither parked nor "
        "opened a request (act %u)", idx, act);
  return idx;
}

/* Walk a taken chain: every index once, every node as its parker wrote it. */
static uint32_t walk(struct arts_db_reader_pool_s *p, uint32_t head,
                     uint8_t *seen, uint32_t cap, uint32_t *tail) {
  uint32_t n = 0u;
  for (uint32_t i = head; i != 0u;) {
    if (i > cap || seen[i]) {
      CHECK(0, "chain names index %u twice or out of range", i);
      return n;
    }
    seen[i] = 1u;
    struct arts_db_reader_node_s *r = arts_db_reader_node(p, i);
    CHECK(r->edt_guid == guid_of(i) && r->slot == slot_of(i),
          "node %u lost its fields", i);
    *tail = i;
    n++;
    i = atomic_load_explicit(&r->next, memory_order_relaxed);
  }
  return n;
}

/* Check a taken chain, then serve it through the runtime's own serve: with
 * no EDT named, each wake returns at once, and the chain goes back whole. */
static void take_back(struct arts_db_reader_pool_s *p, uint32_t head,
                      uint8_t *seen, uint32_t *served) {
  if (head == 0u) {
    return;
  }
  uint32_t tail = head;
  *served += walk(p, head, seen, NPARK, &tail);
  for (uint32_t i = head; i != 0u;) {
    struct arts_db_reader_node_s *r = arts_db_reader_node(p, i);
    r->edt_guid = NULL_GUID;
    i = atomic_load_explicit(&r->next, memory_order_relaxed);
  }
  arts_db_reader_serve_chain(p, head);
}

/* ---- 1 + 2: a wide chain on the cell's word ----------------------------- */

static void part_wide(struct arts_db_cache_s *c) {
  uint32_t last = 0u;
  for (uint32_t k = 0; k < NPARK; k++) {
    last = park(c);
  }
  uint64_t w = atomic_load_explicit(WORD(c), memory_order_acquire);
  CHECK(last == NPARK && newest(w) == last,
        "the word carries %u, the newest reader is %u", newest(w), last);

  uint8_t *seen = (uint8_t *)calloc(NPARK + 1u, 1u);
  uint32_t served = 0u;
  uint64_t pre, post;
  uint32_t act;
#if defined(ARTS_PROTOCOL_INV)
  /* The reply publishes the copy and takes the whole chain. */
  pre = step(c, INV_CACHE_OP_DELIVER, 0u, &post, &act);
  CHECK(act == INV_CACHE_ACT_PUBLISH && INV_CACHE_HEAD_RO(post) == 0u,
        "the reply did not take the chain (act %u)", act);
  take_back(&c->reader_pool, INV_CACHE_HEAD_RO(pre), seen, &served);
#else
  /* The answer serves the window's cohort and moves the wide chain into the
   * next window's cohort, so it crosses both head fields; the write
   * permission's cover then takes it. */
  pre = step(c, VREQ_OP_ANSWER, VREQ_PAR(w), &post, &act);
  CHECK(act == VREQ_ACT_SERVE_SEND && VREQ_GROUP(post) == last &&
            VREQ_NEXT(post) == 0u,
        "the answer left group %u next %u (act %u)", VREQ_GROUP(post),
        VREQ_NEXT(post), act);
  take_back(&c->reader_pool, VREQ_GROUP(pre), seen, &served);
  pre = step(c, VREQ_OP_COVER, 0u, &post, &act);
  CHECK(VREQ_GROUP(post) == 0u && VREQ_NEXT(post) == 0u,
        "the cover left a chain on the word");
  take_back(&c->reader_pool, VREQ_GROUP(pre), seen, &served);
  take_back(&c->reader_pool, VREQ_NEXT(pre), seen, &served);
#endif
  CHECK(served == NPARK, "served %u of %u parked readers", served, NPARK);

  /* The pool holds exactly the served nodes: the next NPARK allocations
   * take them all, and none is a fresh index. */
  uint32_t fresh = atomic_load_explicit(&c->reader_pool.next_fresh,
                                        memory_order_relaxed);
  memset(seen, 0, NPARK + 1u);
  uint32_t *held = (uint32_t *)malloc(NPARK * sizeof(uint32_t));
  for (uint32_t k = 0; k < NPARK; k++) {
    uint32_t idx = arts_db_reader_alloc(&c->reader_pool);
    CHECK(idx >= 1u && idx <= NPARK && !seen[idx],
          "re-allocation %u handed out index %u", k, idx);
    if (idx >= 1u && idx <= NPARK) {
      seen[idx] = 1u;
    }
    held[k] = idx;
  }
  CHECK(atomic_load_explicit(&c->reader_pool.next_fresh,
                             memory_order_relaxed) == fresh,
        "re-allocation took a fresh index although the pool held %u", NPARK);
  for (uint32_t k = 0; k < NPARK; k++) {
    arts_db_reader_free(&c->reader_pool, held[k]);
  }
  free(held);
  free(seen);
}

/* ---- 3: concurrent alloc / chain / return -------------------------------- */

struct stress_s {
  struct arts_db_reader_pool_s pool;
  _Atomic(uint32_t) *owner; /* index -> holding thread + 1, 0 = free */
  uint32_t cap;
  _Atomic(int) bad;
};

struct worker_arg_s {
  struct stress_s *s;
  uint32_t tid;
};

static void *stress_worker(void *v) {
  struct worker_arg_s *a = (struct worker_arg_s *)v;
  struct stress_s *s = a->s;
  uint32_t me = a->tid + 1u;
  uint32_t held[MAXHOLD];
  uint32_t rng = 0x9E3779B9u * me;
  for (uint32_t it = 0; it < NITER; it++) {
    rng = rng * 1103515245u + 12345u;
    uint32_t k = 1u + (rng >> 8) % MAXHOLD;
    for (uint32_t j = 0; j < k; j++) {
      uint32_t idx = arts_db_reader_alloc(&s->pool);
      uint32_t none = 0u;
      if (idx == 0u || idx >= s->cap ||
          !atomic_compare_exchange_strong_explicit(
              &s->owner[idx], &none, me, memory_order_acq_rel,
              memory_order_relaxed)) {
        atomic_store_explicit(&s->bad, 1, memory_order_relaxed);
        return NULL;
      }
      struct arts_db_reader_node_s *n = arts_db_reader_node(&s->pool, idx);
      n->edt_guid = (arts_guid_t)(((uint64_t)me << 32) | idx);
      n->slot = it;
      atomic_store_explicit(&n->next, j ? held[j - 1] : 0u,
                            memory_order_relaxed);
      held[j] = idx;
    }
    /* The private chain runs held[k-1] -> ... -> held[0]. */
    uint32_t i = held[k - 1];
    for (uint32_t j = k; j-- > 0;) {
      struct arts_db_reader_node_s *n = arts_db_reader_node(&s->pool, i);
      if (i != held[j] ||
          n->edt_guid != (arts_guid_t)(((uint64_t)me << 32) | i) ||
          n->slot != it) {
        atomic_store_explicit(&s->bad, 1, memory_order_relaxed);
        return NULL;
      }
      i = atomic_load_explicit(&n->next, memory_order_relaxed);
    }
    for (uint32_t j = 0; j < k; j++) {
      atomic_store_explicit(&s->owner[held[j]], 0u, memory_order_release);
    }
    if (it & 1u) {
      arts_db_reader_free_chain(&s->pool, held[k - 1], held[0]);
    } else {
      for (uint32_t j = 0; j < k; j++) {
        arts_db_reader_free(&s->pool, held[j]);
      }
    }
  }
  return NULL;
}

static void part_concurrent(void) {
  struct stress_s *s = (struct stress_s *)calloc(1, sizeof(*s));
  /* A fresh index is taken only when the free list was empty, with every
   * index then held, so the index space stays near NTHREADS * MAXHOLD. */
  s->cap = 8u * NTHREADS * MAXHOLD;
  s->owner = (_Atomic(uint32_t) *)calloc(s->cap, sizeof(*s->owner));
  arts_db_reader_pool_init(&s->pool);
  pthread_t th[NTHREADS];
  struct worker_arg_s args[NTHREADS];
  for (uint32_t t = 0; t < NTHREADS; t++) {
    args[t].s = s;
    args[t].tid = t;
    if (pthread_create(&th[t], NULL, stress_worker, &args[t]) != 0) {
      (void)fprintf(stderr, "FAIL db_reader_pool: pthread_create\n");
      exit(1);
    }
  }
  for (uint32_t t = 0; t < NTHREADS; t++) {
    (void)pthread_join(th[t], NULL);
  }
  CHECK(atomic_load_explicit(&s->bad, memory_order_relaxed) == 0,
        "two threads held one index, or a held node changed under its "
        "holder");

  /* Every index ever handed out is on the free list exactly once. */
  uint32_t fresh =
      atomic_load_explicit(&s->pool.next_fresh, memory_order_relaxed);
  uint8_t *seen = (uint8_t *)calloc(fresh + 1u, 1u);
  uint32_t n = 0u;
  uint64_t h = atomic_load_explicit(&s->pool.free_head, memory_order_relaxed);
  for (uint32_t i = (uint32_t)h; i != 0u;) {
    if (i >= fresh || seen[i]) {
      CHECK(0, "free list names index %u twice or never handed out", i);
      break;
    }
    seen[i] = 1u;
    n++;
    i = atomic_load_explicit(&arts_db_reader_node(&s->pool, i)->next,
                             memory_order_relaxed);
  }
  CHECK(n == fresh - 1u, "free list holds %u of %u indices", n, fresh - 1u);
  free(seen);
  arts_db_reader_pool_destroy(&s->pool);
  CHECK(atomic_load_explicit(&s->pool.chunks, memory_order_relaxed) == 0,
        "pool destroy left its directory");
  free((void *)s->owner);
  free(s);
}

int main(void) {
  arts_global_rank_id = 0u;
  arts_global_rank_count = 2u;
  /* A first-touch cache of a block homed on the other rank: no home
   * directory here, so the destructor tears down the cache alone. */
  arts_guid_t g = ARTS_GUID_MAKE(ARTS_GUID_DB, 1u, ARTS_GUID_DB_KEY(1u, 42u));
  size_t sz = (sizeof(struct arts_db_s) + alignof(struct arts_db_s) - 1u) /
              alignof(struct arts_db_s) * alignof(struct arts_db_s);
  struct arts_db_s *db =
      (struct arts_db_s *)aligned_alloc(alignof(struct arts_db_s), sz);
  memset(db, 0, sz);
  db->db_type = ARTS_DB;
  arts_db_cache_init(&db->cache, g, 64u, ARTS_DB_INIT_STUB, 1u);

  part_wide(&db->cache);
  part_concurrent();

  /* 4: readers still parked when the cache goes. */
#if defined(ARTS_PROTOCOL_INV)
  {
    /* The copy part 1 left valid would serve them: retire it first. */
    uint64_t post;
    uint32_t act;
    (void)step(&db->cache, INV_CACHE_OP_INVALIDATE, 0u, &post, &act);
    CHECK(act == INV_CACHE_ACT_PURGE_ACK, "the copy did not retire (act %u)",
          act);
  }
#endif
  for (uint32_t k = 0; k < 1000u; k++) {
    (void)park(&db->cache);
  }
  CHECK(atomic_load_explicit(&db->cache.reader_pool.chunks,
                             memory_order_relaxed) != 0,
        "the pool has no storage to return");
  arts_db_cache_destructor(&db->cache);
  CHECK(atomic_load_explicit(&db->cache.reader_pool.chunks,
                             memory_order_relaxed) == 0,
        "the cache destructor left the reader pool's storage");
  free(db);

  if (g_fail) {
    return 1;
  }
  printf("PASS db_reader_pool: %u parked on one word, chains returned whole, "
         "%d threads x %d rounds, storage freed with the cache\n",
         NPARK, NTHREADS, NITER);
  return 0;
}
