/* SPDX-License-Identifier: Apache-2.0
 *
 * net_rdzv_slots — the rendezvous pairing table of the libfabric transport
 * (transport/net.c): the two events of a txid, its expectation and its data
 * arrival, meet in one slot of a chain of hashed levels, decided by one CAS
 * of the slot's word each, and the event that comes second runs the
 * expectation's callback on its own thread.
 *
 * Compiled with one slot at level 0 and two at every deeper level, so every
 * txid collides at level 0 and chains grow several levels deep.  Three
 * threads, standing in for two progress threads and a worker, raise both
 * events of about a million txids, rounds of a few dozen at a time: each
 * round's events are dealt to the threads at random and each thread raises
 * its share in random order, so either event can come first, on any thread,
 * one thread can raise both, and the two can race.  Checks: each callback
 * runs exactly once, after both of its events were raised, with its own
 * expectation's argument and length; once the rounds end every slot of every
 * level is EMPTY and no pass is left; levels grew below level 0.  Under
 * ThreadSanitizer a report here is a defect.
 *
 * Standalone: #includes net.c (its statics) with the shims net_rendezvous
 * uses; no fabric is brought up.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "arts/utils/stdatomic_compat.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arts/memory/regpool.h"
#include "arts/runtime_state.h"
#include "arts/system/threads.h"
#include "arts/transport/dispatcher.h"
#include "arts/transport/protocol.h"

/* ---- runtime externs the module (and regpool) reference ------------------ */
unsigned int arts_global_rank_id = 0;
unsigned int arts_global_rank_count = 1;
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;
struct arts_runtime_shared_s arts_node_info;
struct arts_shutdown_abandon_s arts_shutdown_abandon;
void arts_abort(uint8_t code) { exit(code ? code : 1); }
void *arts_malloc(size_t size) { return malloc(size); }
void *arts_calloc(size_t n, size_t size) { return calloc(n, size); }
void *arts_calloc_aligned(size_t n, size_t size, size_t align) {
  void *p = NULL;
  if (posix_memalign(&p, align, n * size) != 0) {
    return NULL;
  }
  memset(p, 0, n * size);
  return p;
}
void arts_free(void *ptr) { free(ptr); }
#ifdef ARTS_DEBUG_INJECTION
void arts_sched_fuzz_point(void) {}
#endif
void arts_transport_dispatch_packet(struct arts_msg_header_s *packet) {
  (void)packet;
}

/* Every txid collides at level 0; deeper levels separate txids by hash. */
#define ARTS_NET_RDZV_LEVEL0_LOG2 0
#define ARTS_NET_RDZV_LEVEL_LOG2 1
#include "../../libs/src/core/transport/net.c"

#include "counter_stubs.h"

/* ------------------------------------------------------------------------- */

#define THREADS 3
#define ROUND 48 /* txids per round */
#if defined(__SANITIZE_THREAD__)
#define TXIDS (ROUND * 4096)
#else
#define TXIDS (ROUND * 21846)
#endif
#define ROUNDS (TXIDS / ROUND)

/* One txid's expectation: its callback's argument and the length its
 * expectation names; `raised` holds a bit per event, set before the event. */
struct rec {
  _Atomic(unsigned) raised;
  _Atomic(unsigned) fired;
  uint64_t len;
};
static struct rec g_rec[TXIDS + 1];

/* Events: txid << 1 | is_expect, dealt per round and thread. */
static uint32_t *g_ev[THREADS];
static uint32_t *g_round_end[THREADS]; /* end index of each round's share */
static pthread_barrier_t g_round;
static _Atomic(unsigned long) g_bad;

static void bad(const char *what, uint32_t t) {
  if (atomic_fetch_add_explicit(&g_bad, 1, memory_order_relaxed) < 10) {
    printf("FAIL net_rdzv_slots: txid %u: %s\n", t, what);
  }
}

static void on_data(void *arg) {
  struct rec *r = (struct rec *)arg;
  uint32_t t = (uint32_t)(r - g_rec);
  if (atomic_load_explicit(&r->raised, memory_order_relaxed) != 3u) {
    bad("callback before both events", t);
  }
  if (atomic_fetch_add_explicit(&r->fired, 1, memory_order_relaxed) != 0) {
    bad("callback ran twice", t);
  }
}

static void raise_event(uint32_t ev) {
  uint32_t t = ev >> 1;
  bool expect = (ev & 1u) != 0;
  struct rec *r = &g_rec[t];
  atomic_fetch_or_explicit(&r->raised, expect ? 1u : 2u, memory_order_relaxed);
  struct net_rdzv_fire_s exp = {.cb = on_data, .arg = r, .len = r->len};
  struct net_rdzv_fire_s fire;
  if (net_rdzv_event(t, expect ? &exp : NULL, &fire)) {
    if (fire.cb != on_data || fire.arg != r || fire.len != r->len) {
      bad("the callback carries another expectation's fields", t);
      return;
    }
    fire.cb(fire.arg);
  }
}

static void *worker(void *arg) {
  unsigned id = (unsigned)(uintptr_t)arg;
  uint32_t i = 0;
  for (uint32_t round = 0; round < ROUNDS; round++) {
    for (; i < g_round_end[id][round]; i++) {
      raise_event(g_ev[id][i]);
    }
    pthread_barrier_wait(&g_round);
  }
  return NULL;
}

static uint64_t g_rng = 0x2545F4914F6CDD1DULL;
static uint32_t rnd(uint32_t n) {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return (uint32_t)(g_rng % n);
}

int main(void) {
  g_rdzv_level0 = net_rdzv_level_new(0);
  for (unsigned th = 0; th < THREADS; th++) {
    g_ev[th] = (uint32_t *)malloc(sizeof(uint32_t) * 2u * TXIDS);
    g_round_end[th] = (uint32_t *)malloc(sizeof(uint32_t) * ROUNDS);
  }
  uint32_t fill[THREADS] = {0};
  uint32_t deck[2 * ROUND];
  for (uint32_t round = 0; round < ROUNDS; round++) {
    for (uint32_t k = 0; k < ROUND; k++) {
      uint32_t t = 1u + round * ROUND + k;
      g_rec[t].len = (uint64_t)t * 64u + 7u;
      deck[2 * k] = t << 1 | 1u;
      deck[2 * k + 1] = t << 1;
    }
    for (uint32_t k = 2 * ROUND - 1; k > 0; k--) {
      uint32_t j = rnd(k + 1);
      uint32_t x = deck[k];
      deck[k] = deck[j];
      deck[j] = x;
    }
    for (uint32_t k = 0; k < 2 * ROUND; k++) {
      unsigned th = rnd(THREADS);
      g_ev[th][fill[th]++] = deck[k];
    }
    for (unsigned th = 0; th < THREADS; th++) {
      g_round_end[th][round] = fill[th];
    }
  }

  pthread_barrier_init(&g_round, NULL, THREADS);
  pthread_t tid[THREADS];
  for (unsigned th = 0; th < THREADS; th++) {
    pthread_create(&tid[th], NULL, worker, (void *)(uintptr_t)th);
  }
  for (unsigned th = 0; th < THREADS; th++) {
    pthread_join(tid[th], NULL);
  }
  pthread_barrier_destroy(&g_round);

  for (uint32_t t = 1; t <= TXIDS; t++) {
    if (atomic_load_explicit(&g_rec[t].fired, memory_order_relaxed) != 1u) {
      bad("callback did not run exactly once", t);
    }
  }
  unsigned levels = 0, left = 0;
  for (struct net_rdzv_level_s *lv = g_rdzv_level0; lv != NULL;) {
    size_t n = (size_t)1 << net_rdzv_level_log2(levels);
    for (size_t k = 0; k < n; k++) {
      uint64_t w =
          atomic_load_explicit(&lv->slot[k].word, memory_order_relaxed);
      if (net_rdzv_word_state(w) != NET_RDZV_EMPTY ||
          net_rdzv_word_pass(w) != 0) {
        left++;
      }
    }
    struct net_rdzv_level_s *next =
        atomic_load_explicit(&lv->next, memory_order_relaxed);
    arts_free(lv);
    lv = next;
    levels++;
  }
  g_rdzv_level0 = NULL;
  if (left != 0) {
    printf("FAIL net_rdzv_slots: %u slot(s) hold a record or a pass after "
           "every pairing completed\n",
           left);
    atomic_fetch_add_explicit(&g_bad, 1, memory_order_relaxed);
  }
  if (levels < 2) {
    printf("FAIL net_rdzv_slots: no level grew below level 0\n");
    atomic_fetch_add_explicit(&g_bad, 1, memory_order_relaxed);
  }
  for (unsigned th = 0; th < THREADS; th++) {
    free(g_ev[th]);
    free(g_round_end[th]);
  }
  printf("INFO net_rdzv_slots: %u txids, %u levels\n", (unsigned)TXIDS, levels);
  unsigned long nbad = atomic_load_explicit(&g_bad, memory_order_relaxed);
  if (nbad != 0) {
    printf("FAIL net_rdzv_slots: %lu failure(s)\n", nbad);
    return 1;
  }
  printf("PASS net_rdzv_slots\n");
  return 0;
}
