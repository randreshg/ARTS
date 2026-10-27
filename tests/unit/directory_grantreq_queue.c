/* SPDX-License-Identifier: Apache-2.0
 *
 * arts_home_grantreq_queue_{init,push,pop,peek,empty,destroy} (the home
 * OWNERSHIP-REQUEST FIFO; a Vyukov MPSC, drain-one).
 *
 * One body (coherence/grantreq_queue.c) serves VAL, EXCL and INV.  The test
 * links it into a standalone TU (no ARTS runtime) and runs under each of those
 * three protocols; FLUSH has no such queue and does not build the test.
 *
 * Properties exercised:
 *   1. Functional single-thread: empty on init, peek/pop false on empty,
 *      FIFO order (oldest popped first = next ownership target), peek
 *      non-destructive, empty() tracks pop draining.
 *   2. Stub never freed: drain-to-empty then re-push then drain again works
 *      (the embedded sentinel stays usable; ASan would catch a stub free).
 *   3. Concurrent N producers + 1 consumer: no loss, no dup, and per-producer
 *      FIFO preserved (each producer pushes a strictly increasing sequence;
 *      the consumer must observe each producer's values in increasing order).
 *      This also exercises the transient-NULL mid-link wait in pop().
 *
 * Build: the build's own protocol defines, with -DARTS_UNIT_STANDALONE_SHIMS
 * for the libc alloc shims.
 */

#include <stdio.h>

#include "arts/coherence/directory.h"
#include "arts/transport/protocol.h" /* ARTS_GRANT_VERSION_NONE */

#include "arts/utils/stdatomic_compat.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* pop() waits out a producer between its exchange and its link store, without
 * bound under a stalled producer; a hung consumer would otherwise wedge ctest,
 * so arm a watchdog. */
#include <unistd.h>
static void *watchdog(void *arg) {
  (void)arg;
  struct timespec ts = {15, 0};
  nanosleep(&ts, NULL);
  (void)fprintf(stderr,
                "FAIL directory_grantreq_queue: TIMEOUT (suspected deadlock)\n");
  _exit(1);
  return NULL;
}

/* ---- Part 1: single-thread functional + FIFO + peek + empty ---- */
static int part1_functional(void) {
  struct arts_home_grantreq_queue_s q;
  arts_home_grantreq_queue_init(&q);
  int rc = 0;
  unsigned int r = 0xdeadbeef;

  if (!arts_home_grantreq_queue_empty(&q)) {
    (void)fprintf(stderr, "FAIL: fresh queue not empty\n");
    rc = 1;
  }
  if (arts_home_grantreq_queue_peek(&q, &r, NULL, NULL)) {
    (void)fprintf(stderr, "FAIL: peek on empty returned true\n");
    rc = 1;
  }
  if (arts_home_grantreq_queue_pop(&q, &r, NULL, NULL)) {
    (void)fprintf(stderr, "FAIL: pop on empty returned true\n");
    rc = 1;
  }

  /* Push 0..9 — FIFO: pop must yield 0,1,...,9. */
  for (unsigned int i = 0; i < 10; i++) {
    arts_home_grantreq_queue_push(&q, i, NULL, ARTS_GRANT_VERSION_NONE);
  }
  if (arts_home_grantreq_queue_empty(&q)) {
    (void)fprintf(stderr, "FAIL: queue empty after 10 pushes\n");
    rc = 1;
  }
  /* peek is non-destructive: two peeks return the same front (oldest). */
  unsigned int p1 = 0, p2 = 0;
  if (!arts_home_grantreq_queue_peek(&q, &p1, NULL, NULL) ||
      !arts_home_grantreq_queue_peek(&q, &p2, NULL, NULL) || p1 != 0 || p2 != 0) {
    (void)fprintf(stderr,
                  "FAIL: peek not non-destructive / not FIFO front "
                  "(p1=%u p2=%u)\n",
                  p1, p2);
    rc = 1;
  }
  for (unsigned int i = 0; i < 10; i++) {
    unsigned int got = 0xffffffff;
    if (!arts_home_grantreq_queue_pop(&q, &got, NULL, NULL)) {
      (void)fprintf(stderr, "FAIL: pop %u returned false\n", i);
      rc = 1;
      break;
    }
    if (got != i) {
      (void)fprintf(stderr, "FAIL: FIFO violated, expected %u got %u\n", i,
                    got);
      rc = 1;
    }
  }
  if (!arts_home_grantreq_queue_empty(&q)) {
    (void)fprintf(stderr, "FAIL: queue not empty after draining all\n");
    rc = 1;
  }
  if (arts_home_grantreq_queue_pop(&q, &r, NULL, NULL)) {
    (void)fprintf(stderr, "FAIL: pop after drain returned true\n");
    rc = 1;
  }
  arts_home_grantreq_queue_destroy(&q);
  return rc;
}

/* ---- Part 2: stub-never-freed — drain, re-push, drain again ---- */
static int part2_stub_reuse(void) {
  struct arts_home_grantreq_queue_s q;
  arts_home_grantreq_queue_init(&q);
  int rc = 0;
  for (int round = 0; round < 3; round++) {
    for (unsigned int i = 0; i < 5; i++) {
      arts_home_grantreq_queue_push(&q, round * 100 + i, NULL, ARTS_GRANT_VERSION_NONE);
    }
    for (unsigned int i = 0; i < 5; i++) {
      unsigned int got = 0;
      if (!arts_home_grantreq_queue_pop(&q, &got, NULL, NULL) ||
          got != (unsigned int)(round * 100 + i)) {
        (void)fprintf(stderr, "FAIL: stub-reuse round %d idx %u got %u\n",
                      round, i, got);
        rc = 1;
      }
    }
    if (!arts_home_grantreq_queue_empty(&q)) {
      (void)fprintf(stderr, "FAIL: stub-reuse not empty after round %d\n",
                    round);
      rc = 1;
    }
  }
  arts_home_grantreq_queue_destroy(&q);
  return rc;
}

/* ---- Part 3: N producers + 1 consumer, per-producer FIFO ---- */
#define NPROD 6
#define PER_PROD 20000
/* Encode (producer, seq) into a single rank value: rank = prod*PER_PROD + seq.
 * Consumer recovers producer = rank / PER_PROD, seq = rank % PER_PROD and
 * checks per-producer monotonicity (FIFO) + counts every value exactly once. */

static struct arts_home_grantreq_queue_s g_q;
static _Atomic(int) g_start;
static _Atomic(int) g_produced;

static void *producer(void *arg) {
  unsigned int prod = (unsigned int)(uintptr_t)arg;
  while (!atomic_load_explicit(&g_start, memory_order_acquire)) {
  }
  for (unsigned int s = 0; s < PER_PROD; s++) {
    arts_home_grantreq_queue_push(&g_q, prod * PER_PROD + s, NULL, ARTS_GRANT_VERSION_NONE);
    atomic_fetch_add_explicit(&g_produced, 1, memory_order_relaxed);
  }
  return NULL;
}

static int part3_mpsc(void) {
  arts_home_grantreq_queue_init(&g_q);
  atomic_store_explicit(&g_start, 0, memory_order_release);
  atomic_store_explicit(&g_produced, 0, memory_order_release);

  pthread_t th[NPROD];
  for (unsigned int i = 0; i < NPROD; i++) {
    pthread_create(&th[i], NULL, producer, (void *)(uintptr_t)i);
  }

  int rc = 0;
  int *last_seq = (int *)malloc(sizeof(int) * NPROD);
  int *count = (int *)calloc(NPROD, sizeof(int));
  for (int i = 0; i < NPROD; i++) {
    last_seq[i] = -1;
  }
  const long total = (long)NPROD * PER_PROD;
  long consumed = 0;

  atomic_store_explicit(&g_start, 1, memory_order_release);

  /* Single consumer drains until it has seen every value.  pop() exercises the
   * mid-link wait while producers are still mid-push. */
  while (consumed < total) {
    unsigned int v;
    if (!arts_home_grantreq_queue_pop(&g_q, &v, NULL, NULL)) {
      /* Truly empty (producers between pushes) — retry. */
      continue;
    }
    unsigned int prod = v / PER_PROD;
    int seq = (int)(v % PER_PROD);
    if (prod >= NPROD) {
      (void)fprintf(stderr, "FAIL: bogus value %u (prod %u)\n", v, prod);
      rc = 1;
      break;
    }
    if (seq <= last_seq[prod]) {
      (void)fprintf(stderr,
                    "FAIL: per-producer FIFO violated prod=%u seq=%d last=%d\n",
                    prod, seq, last_seq[prod]);
      rc = 1;
      break;
    }
    last_seq[prod] = seq;
    count[prod]++;
    consumed++;
  }

  for (unsigned int i = 0; i < NPROD; i++) {
    pthread_join(th[i], NULL);
  }
  if (!rc) {
    for (int i = 0; i < NPROD; i++) {
      if (count[i] != PER_PROD) {
        (void)fprintf(stderr, "FAIL: prod %d count %d != %d (loss/dup)\n", i,
                      count[i], PER_PROD);
        rc = 1;
      }
    }
    if (!arts_home_grantreq_queue_empty(&g_q)) {
      (void)fprintf(stderr, "FAIL: queue not empty after full drain\n");
      rc = 1;
    }
  }
  free(last_seq);
  free(count);
  arts_home_grantreq_queue_destroy(&g_q);
  return rc;
}

int main(void) {
  pthread_t wd;
  pthread_create(&wd, NULL, watchdog, NULL);
  pthread_detach(wd);

  if (part1_functional() != 0) {
    return 1;
  }
  if (part2_stub_reuse() != 0) {
    return 1;
  }
  if (part3_mpsc() != 0) {
    return 1;
  }
  printf("PASS directory_grantreq_queue: FIFO + peek-nondestructive + empty + "
         "stub-reuse + %d-producer MPSC per-producer-FIFO (%d each)\n",
         NPROD, PER_PROD);
  return 0;
}

/* libc-backed alloc shims for the queue nodes.  Provided only in the
 * standalone build. */
#ifdef ARTS_UNIT_STANDALONE_SHIMS
void arts_free(void *ptr) { free(ptr); }
void *arts_malloc(size_t size) { return malloc(size); }
#endif
