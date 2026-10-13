/* SPDX-License-Identifier: Apache-2.0
 *
 * arts_pending_rw_queue_* (cache-side RW waiter chain).
 *
 * A Treiber stack (arts_lf_stack_t) of arts_db_rw_waiter_s nodes (link FIRST).
 * Producers (foreign-rank acquire_remote_rw) prepend via release-CAS; the
 * single consumer detaches the whole chain (take), may count it before waking
 * anyone, then wakes and frees every waiter once (chain_wake); teardown frees
 * whatever is left (destroy).  Order is immaterial (every waiter is woken
 * regardless), which is why a LIFO stack suffices vs the home grantreq Vyukov
 * FIFO.
 *
 * Defined in coherence/grant_queue.c, which this TU #includes standalone
 * (precedent: rank_bitset.c).  One body serves every arm that keeps the
 * grant's cache-side queue (VAL, INV), so the test runs in the reference tree.
 *
 * Properties exercised:
 *   1. conservation: push K waiters; one take detaches a chain that counts K
 *      and is still whole after the count, and its wake wakes + frees EXACTLY
 *      K, each once (counted via a per-waiter id seen-array; ASan/LSan catch a
 *      missed free or double-free).  A take on the emptied stack detaches
 *      nothing.
 *   2. container_of: each woken cb receives the (edt_guid, slot) the producer
 *      pushed — proves link-is-first ARTS_CONTAINER_OF recovery.
 *   3. concurrency: N producers push concurrently with repeated takes; a push
 *      racing a take forms a fresh stack picked up by the NEXT take — so the
 *      union over all takes wakes EXACTLY the pushed set, no loss, no dup.
 *   4. destroy frees a chain nobody took.
 *
 * Build: -DARTS_UNIT_STANDALONE_SHIMS.
 */

#include <stdio.h>

#include "arts/coherence/coherence.h"
#include "arts/coherence/directory.h"
#include "arts/utils/lockfree_lifo.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void *watchdog(void *arg) {
  (void)arg;
  struct timespec ts = {15, 0};
  nanosleep(&ts, NULL);
  (void)fprintf(stderr,
                "FAIL pending_rw_treiber: TIMEOUT (suspected deadlock)\n");
  _exit(1);
  return NULL;
}

/* Build a GUID-shaped value that round-trips through the cb without
 * interpretation — the queue only stores/forwards it. */
static arts_guid_t mk_guid(uint64_t v) { return (arts_guid_t)v; }

/* ---- wake cb: records (guid,slot) per woken waiter ---- */
struct wake_ctx {
  int *seen;    /* seen[id]++ */
  int cap;      /* capacity of seen */
  int count;    /* total woken */
  int bad_guid; /* count of guid!=encoded(slot) container_of mismatches */
  int oob;      /* out-of-range ids */
};

/* We encode id into BOTH guid (id) and slot (id ^ 0x5555) so the cb can verify
 * the (guid,slot) pair was delivered together (container_of integrity). */
static void wake_cb(arts_guid_t edt_guid, unsigned int slot, void *ctxv) {
  struct wake_ctx *c = (struct wake_ctx *)ctxv;
  long id = (long)(uint64_t)edt_guid;
  if (id < 0 || id >= c->cap) {
    c->oob++;
    return;
  }
  if (slot != (unsigned int)(id ^ 0x5555)) {
    c->bad_guid++;
  }
  c->seen[id]++;
  c->count++;
}

static struct arts_db_rw_waiter_s *mk_waiter(long id) {
  struct arts_db_rw_waiter_s *w =
      (struct arts_db_rw_waiter_s *)malloc(sizeof(*w));
  w->edt_guid = mk_guid((uint64_t)id);
  w->slot = (unsigned int)(id ^ 0x5555);
  return w;
}

/* Detaches the chain, then wakes + frees it; returns how many it counted. */
static unsigned int take_and_wake(arts_lf_stack_t *q,
                                  void (*cb)(arts_guid_t, unsigned int, void *),
                                  void *ctx) {
  arts_lf_link_t *chain = arts_pending_rw_queue_take(q);
  unsigned int n = arts_pending_rw_chain_count(chain);
  arts_pending_rw_chain_wake(chain, cb, ctx);
  return n;
}

/* ---- Parts 1, 2 and 4: conservation, container_of, destroy ---- */
static int part1_take_all(void) {
  arts_lf_stack_t q;
  arts_pending_rw_queue_init(&q);
  const int K = 1000;
  for (long i = 0; i < K; i++) {
    arts_pending_rw_queue_push(&q, mk_waiter(i));
  }
  int seen[1000];
  memset(seen, 0, sizeof(seen));
  struct wake_ctx c = {seen, K, 0, 0, 0};
  unsigned int counted = take_and_wake(&q, wake_cb, &c);
  int rc = 0;
  if (counted != (unsigned int)K || c.count != K || c.bad_guid || c.oob) {
    (void)fprintf(stderr,
                  "FAIL: take counted %u, woke %d (want %d) bad_guid=%d "
                  "oob=%d\n",
                  counted, c.count, K, c.bad_guid, c.oob);
    rc = 1;
  }
  for (int i = 0; i < K; i++) {
    if (seen[i] != 1) {
      (void)fprintf(stderr, "FAIL: waiter %d woken %d times\n", i, seen[i]);
      rc = 1;
    }
  }
  if (arts_pending_rw_queue_pending(&q) ||
      arts_pending_rw_queue_take(&q) != NULL) {
    (void)fprintf(stderr, "FAIL: the stack is not empty after a take\n");
    rc = 1;
  }
  /* A chain nobody takes is freed by destroy (LSan reports a leak). */
  for (long i = 0; i < 16; i++) {
    arts_pending_rw_queue_push(&q, mk_waiter(i));
  }
  arts_pending_rw_queue_destroy(&q);
  return rc;
}

/* ---- Part 3: push concurrent with take — survives to the next take ---- */
#define NPROD 6
#define PER_PROD 5000
#define TOTAL (NPROD * PER_PROD)

static arts_lf_stack_t g_q;
static _Atomic int g_start;
static _Atomic int g_prod_done;
static _Atomic int g_seen[TOTAL];
static _Atomic long g_woken;

static void conc_cb(arts_guid_t edt_guid, unsigned int slot, void *ctxv) {
  (void)slot;
  (void)ctxv;
  long id = (long)(uint64_t)edt_guid;
  if (id >= 0 && id < TOTAL) {
    atomic_fetch_add_explicit(&g_seen[id], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_woken, 1, memory_order_relaxed);
  }
}

static void *conc_producer(void *arg) {
  long base = (long)(intptr_t)arg * PER_PROD;
  while (!atomic_load_explicit(&g_start, memory_order_acquire)) {
  }
  for (long s = 0; s < PER_PROD; s++) {
    long id = base + s;
    struct arts_db_rw_waiter_s *w =
        (struct arts_db_rw_waiter_s *)malloc(sizeof(*w));
    w->edt_guid = mk_guid((uint64_t)id);
    w->slot = (unsigned int)id;
    arts_pending_rw_queue_push(&g_q, w);
  }
  atomic_fetch_add_explicit(&g_prod_done, 1, memory_order_release);
  return NULL;
}

static int part3_concurrent(void) {
  arts_pending_rw_queue_init(&g_q);
  atomic_store_explicit(&g_start, 0, memory_order_release);
  atomic_store_explicit(&g_prod_done, 0, memory_order_release);
  atomic_store_explicit(&g_woken, 0, memory_order_release);
  for (int i = 0; i < TOTAL; i++) {
    atomic_store_explicit(&g_seen[i], 0, memory_order_relaxed);
  }
  pthread_t th[NPROD];
  for (int i = 0; i < NPROD; i++) {
    pthread_create(&th[i], NULL, conc_producer, (void *)(intptr_t)i);
  }
  atomic_store_explicit(&g_start, 1, memory_order_release);

  /* Single consumer: keep taking while producers run, then once more after
   * they finish to absorb any fresh-stack remainder.  A push racing the take's
   * atomic-exchange forms a new stack the next take claims. */
  while (atomic_load_explicit(&g_prod_done, memory_order_acquire) < NPROD) {
    (void)take_and_wake(&g_q, conc_cb, NULL);
  }
  for (int i = 0; i < NPROD; i++) {
    pthread_join(th[i], NULL);
  }
  (void)take_and_wake(&g_q, conc_cb, NULL);

  int rc = 0;
  long w = atomic_load_explicit(&g_woken, memory_order_acquire);
  if (w != TOTAL) {
    (void)fprintf(stderr, "FAIL: concurrent woke %ld (want %d)\n", w, TOTAL);
    rc = 1;
  }
  for (int i = 0; i < TOTAL; i++) {
    int s = atomic_load_explicit(&g_seen[i], memory_order_relaxed);
    if (s != 1) {
      (void)fprintf(stderr, "FAIL: waiter %d woken %d times (loss/dup)\n", i,
                    s);
      rc = 1;
      break;
    }
  }
  arts_pending_rw_queue_destroy(&g_q);
  return rc;
}

int main(void) {
  pthread_t wd;
  pthread_create(&wd, NULL, watchdog, NULL);
  pthread_detach(wd);

  if (part1_take_all() != 0) {
    return 1;
  }
  if (part3_concurrent() != 0) {
    return 1;
  }
  printf("PASS pending_rw_treiber: take-count-wake conservation + "
         "container_of + destroy + %d-producer concurrent (%d each) no "
         "loss/dup\n",
         NPROD, PER_PROD);
  return 0;
}

#ifdef ARTS_UNIT_STANDALONE_SHIMS
void *arts_calloc(size_t nmemb, size_t size) { return calloc(nmemb, size); }
void arts_free(void *ptr) { free(ptr); }
void *arts_malloc(size_t size) { return malloc(size); }
#endif

#include "core/coherence/grant_queue.c"
