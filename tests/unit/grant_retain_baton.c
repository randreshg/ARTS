/* SPDX-License-Identifier: Apache-2.0
 *
 * The RETAIN home's serving baton as a claim counter: the request arrival and
 * the baton holder's step (coherence/grant_retain.c), the write-back CONFIRM
 * handler (coherence/grant_wb.c) and the request FIFO
 * (coherence/grantreq_queue.c), compiled in with the wire stubbed.
 *
 * Producer threads stand in for the GRANT_REQUEST handlers of ranks with one
 * write request out at a time: each pushes a request, claims the baton and
 * waits until the request is served; one of them stops between its push's
 * exchange and its link store, so a holder meets a front that is not linked
 * yet and must not wait for it.  A request whose claim were lost would leave
 * its producer waiting for ever.  A CONFIRM thread receives each
 * round's INVALIDATE and inherits the hold, as the home's CONFIRM handler
 * does.  In the write-back shape it runs that handler itself
 * (arts_handler_db_grant_confirm: the count read before the look, a
 * piggybacked next target on the CONFIRM_ACK, else the holder's step after
 * that look).  The write-through handler cannot be compiled in this cell, so
 * the write-through shape mirrors it (the count read, then the holder's step);
 * the runtime's multinode loops run the real one.  The producers push and
 * claim themselves, so the order of the two in the GRANT_REQUEST handler
 * (coherence/grant.c) is likewise left to those loops.
 *
 * Checks: every request is popped once and each producer's in order; one
 * round at a time, each under a held baton; the round's target reaches the
 * CONFIRM through pending_install_owner; at the end the baton is down and the
 * FIFO empty; nothing waits for ever (a watchdog); in an injection tree some
 * put-down met a newer claim and looked again.
 *
 * PURE test: no arts_rt, no ports.
 */
#include "arts/utils/stdatomic_compat.h"

#include "arts/counter/Preamble.h"

/* The baton holder's re-looks, counted whatever the tree's counter set. */
static _Atomic(uint64_t) g_reclaims;
#undef INCREMENT_NUM_GRANT_BATON_RECLAIM_BY
#define INCREMENT_NUM_GRANT_BATON_RECLAIM_BY(v)                                \
  atomic_fetch_add_explicit(&g_reclaims, (v), memory_order_relaxed)

#include "coherence/grant_retain.c"
#include "coherence/grant_wb.c"
#include "coherence/grantreq_queue.c"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NPROD 4
#define PER_PROD 20000u
#define STALL_EVERY 8u /* the stalling producer's stride */
#define WATCHDOG_S 100 /* below the test's TIMEOUT */

/* No request names this rank, so every CONFIRM_ACK leaves by the wire. */
unsigned int arts_global_rank_id = 0xffffff00u;
unsigned int arts_global_rank_count = NPROD + 1u;
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;
void arts_abort(uint8_t code) { _exit(code ? code : 1); }

#include "counter_stubs.h"

void *arts_malloc(size_t n) { return malloc(n); }
void arts_free(void *p) { free(p); }
#ifdef ARTS_DEBUG_INJECTION
void arts_sched_fuzz_point(void) {
  static _Thread_local unsigned int seed;
  if (seed == 0u) {
    seed = (unsigned int)(uintptr_t)&seed ^ (unsigned int)time(NULL);
  }
  if ((unsigned int)rand_r(&seed) % 100u < 20u) {
    sched_yield();
  }
}
#endif
unsigned int arts_guid_get_rank(arts_guid_t g) {
  (void)g;
  return 0u;
}
#if ARTS_DB_CENSUS
ARTS_THREAD_LOCAL uint8_t arts_db_census_serve;
#endif
void arts_db_grant_note_ex_holder(struct arts_db_s *db, unsigned int r) {
  (void)db;
  (void)r;
}

/* The holder-side edges and the grant_wb.c handlers this test does not
 * reach. */
#define UNREACHED(sig)                                                         \
  sig {                                                                        \
    printf("FAIL grant_retain_baton: %s reached\n", __func__);                 \
    fflush(stdout);                                                            \
    _exit(1);                                                                  \
  }
UNREACHED(void arts_db_grant_ship_pending(struct arts_db_cache_s *c))
UNREACHED(void arts_db_drain_pending_rw_after_grant(struct arts_db_cache_s *c))
UNREACHED(arts_shared_ptr_t arts_route_table_lookup_db(arts_guid_t g))
UNREACHED(arts_shared_ptr_t arts_db_cache_stub_install(arts_guid_t g,
                                                       uint64_t s))
UNREACHED(void arts_db_rdzv_discard_landing(uint64_t t, uint64_t s, uint64_t c))
UNREACHED(void arts_send_db_grant_confirm(unsigned int h, arts_guid_t g))
UNREACHED(void *arts_shared_get(arts_shared_ptr_t h))
UNREACHED(void arts_shared_release(arts_shared_ptr_t *p))
UNREACHED(bool arts_rank_u64_map_load(struct arts_rank_to_u64_map_s *m,
                                      const void *in, size_t size))
UNREACHED(void arts_net_rdzv_expect(uint64_t txid, uint64_t len,
                                    void (*on_data)(void *), void *arg))
UNREACHED(bool arts_db_buf_adopt_landing(struct arts_db_cache_s *c, uint64_t v,
                                         struct arts_db_buffer_s *l,
                                         uint64_t s))
UNREACHED(struct arts_db_buffer_s *arts_db_buf_install(
    struct arts_db_cache_s *c, uint64_t v, const void *d, uint64_t s))
UNREACHED(struct arts_db_buffer_s *arts_db_buf_install_landed(
    struct arts_db_cache_s *c, uint64_t v, struct arts_db_buffer_s *l,
    uint64_t s))

static struct arts_db_s *g_db;
static _Atomic(int) g_failed;
static _Atomic(int) g_done;

static void fail(const char *what) {
  if (atomic_exchange_explicit(&g_failed, 1, memory_order_acq_rel) == 0) {
    printf("FAIL grant_retain_baton: %s\n", what);
    fflush(stdout);
  }
}

/* ---- what the holders popped ---- */

/* Written by the baton holders, read by their producers. */
static _Atomic(unsigned int) g_next_seq[NPROD];
static _Atomic(uint64_t) g_popped;

static void popped(unsigned int v) {
  unsigned int p = v / PER_PROD;
  unsigned int seq = v % PER_PROD;
  if (p >= NPROD) {
    fail("a popped request names no producer");
    return;
  }
  if (seq != atomic_load_explicit(&g_next_seq[p], memory_order_relaxed)) {
    fail("a request was popped twice, lost, or out of its producer's order");
  }
  atomic_store_explicit(&g_next_seq[p], seq + 1u, memory_order_release);
  atomic_fetch_add_explicit(&g_popped, 1u, memory_order_relaxed);
}

/* ---- the round in flight: INVALIDATE, then the CONFIRM ---- */

static _Atomic(int) g_round_out;    /* a round has left a holder */
static _Atomic(int) g_round_posted; /* and reached the CONFIRM thread */
static unsigned int g_round_target; /* published by g_round_posted */

static void post_round(unsigned int target) {
  int none = 0;
  if (!atomic_compare_exchange_strong_explicit(
          &g_round_out, &none, 1, memory_order_acq_rel, memory_order_acquire)) {
    fail("two rounds in flight: two baton holders");
  }
  if (atomic_load_explicit(&g_db->grant_baton, memory_order_acquire) == 0u) {
    fail("a round leaves with the baton down");
  }
  g_round_target = target;
  atomic_store_explicit(&g_round_posted, 1, memory_order_release);
}

void arts_send_db_grant_invalidate(unsigned int current, arts_guid_t db_guid,
                                   unsigned int new_owner,
                                   const struct arts_rdzv_landing_s *rdzv,
                                   uint64_t have) {
  (void)current;
  (void)db_guid;
  (void)rdzv;
  (void)have;
  popped(new_owner);
  post_round(new_owner);
}

/* The CONFIRM_ACK the write-back handler sends the owner it confirmed; a
 * piggybacked target continues the round, which this thread holds. */
void arts_transport_send_async(int r, char *m, unsigned int n) {
  struct arts_msg_grant_confirm_ack_packet_s *p =
      (struct arts_msg_grant_confirm_ack_packet_s *)m;
  if (n != sizeof(*p) || p->header.message_type != MSG_DB_GRANT_CONFIRM_ACK) {
    fail("a message other than a CONFIRM_ACK left the home");
    return;
  }
  if ((unsigned int)r != g_round_target) {
    fail("the CONFIRM_ACK goes to another round's target");
  }
  if (p->new_owner_rank != ARTS_NO_PENDING_OWNER) {
    popped(p->new_owner_rank);
    post_round(p->new_owner_rank);
  }
}

static int g_wb_shape;
static _Atomic(int) g_stop_confirm;

static void *confirm(void *arg) {
  (void)arg;
  struct arts_db_cache_s *cache = &g_db->cache;
  for (;;) {
    if (!atomic_exchange_explicit(&g_round_posted, 0, memory_order_acq_rel)) {
      if (atomic_load_explicit(&g_stop_confirm, memory_order_acquire)) {
        return NULL;
      }
      sched_yield();
      continue;
    }
    unsigned int target = g_round_target;
    atomic_store_explicit(&g_round_out, 0, memory_order_release);
    if (g_wb_shape) {
      arts_handler_db_grant_confirm(g_db, NULL);
      continue;
    }
    /* arts_handler_db_grant_confirm of the write-through policy. */
    unsigned int new_owner = atomic_load_explicit(&g_db->pending_install_owner,
                                                  memory_order_relaxed);
    if (new_owner != target) {
      fail("the CONFIRM reads another round's target");
    }
    atomic_store_explicit(&g_db->rw_holder, new_owner, memory_order_release);
    arts_db_grant_advance(
        cache, g_db, false,
        atomic_load_explicit(&g_db->grant_baton, memory_order_acquire));
  }
}

/* ---- producers ---- */

static _Atomic(int) g_start;

/* The push of arts_home_grantreq_queue_push, in its two halves with a stop
 * between them. */
static void push_stalled(unsigned int v) {
  struct arts_home_grantreq_node_s *n =
      (struct arts_home_grantreq_node_s *)malloc(sizeof(*n));
  n->rank = v;
  n->have_version = ARTS_GRANT_VERSION_NONE;
  n->rdzv = (struct arts_rdzv_landing_s){0, 0, 0, 0};
  atomic_store_explicit(&n->next, (struct arts_home_grantreq_node_s *)NULL,
                        memory_order_relaxed);
  struct arts_home_grantreq_node_s *prev =
      atomic_exchange_explicit(&g_db->pending_rw.tail, n, memory_order_acq_rel);
  struct timespec ts = {0, 20000L};
  nanosleep(&ts, NULL);
  atomic_store_explicit(&prev->next, n, memory_order_release);
}

static void *producer(void *arg) {
  unsigned int p = (unsigned int)(uintptr_t)arg;
  while (!atomic_load_explicit(&g_start, memory_order_acquire)) {
  }
  for (unsigned int s = 0; s < PER_PROD; s++) {
    unsigned int v = p * PER_PROD + s;
    if (p == 0u && s % STALL_EVERY == 0u) {
      push_stalled(v);
    } else {
      arts_home_grantreq_queue_push(&g_db->pending_rw, v, NULL,
                                    ARTS_GRANT_VERSION_NONE);
    }
    arts_sched_fuzz_point();
    arts_db_grant_request_arrived(&g_db->cache, g_db, v);
    /* A rank has one write request out: the next waits for this one. */
    while (atomic_load_explicit(&g_next_seq[p], memory_order_acquire) <= s) {
      if (atomic_load_explicit(&g_failed, memory_order_relaxed)) {
        return NULL;
      }
      sched_yield();
    }
  }
  return NULL;
}

static void *watchdog(void *arg) {
  (void)arg;
  for (int i = 0; i < WATCHDOG_S * 10; i++) {
    if (atomic_load_explicit(&g_done, memory_order_acquire)) {
      return NULL;
    }
    struct timespec ts = {0, 100000000L};
    nanosleep(&ts, NULL);
  }
  printf(
      "FAIL grant_retain_baton: TIMEOUT (a request is stranded): "
      "popped=%llu baton=%llu round_out=%d\n",
      (unsigned long long)atomic_load_explicit(&g_popped, memory_order_relaxed),
      (unsigned long long)atomic_load_explicit(&g_db->grant_baton,
                                               memory_order_relaxed),
      atomic_load_explicit(&g_round_out, memory_order_relaxed));
  fflush(stdout);
  _exit(1);
  return NULL;
}

static int run(int wb_shape) {
  g_db = calloc(1, sizeof(*g_db));
  arts_home_grantreq_queue_init(&g_db->pending_rw);
  atomic_init(&g_db->grant_baton, 0u);
  atomic_init(&g_db->rw_holder, 0u);
  atomic_init(&g_db->pending_install_owner, 0u);
  for (unsigned int p = 0; p < NPROD; p++) {
    atomic_store_explicit(&g_next_seq[p], 0u, memory_order_relaxed);
  }
  atomic_store_explicit(&g_popped, 0u, memory_order_relaxed);
  atomic_store_explicit(&g_round_out, 0, memory_order_relaxed);
  atomic_store_explicit(&g_round_posted, 0, memory_order_relaxed);
  atomic_store_explicit(&g_stop_confirm, 0, memory_order_relaxed);
  atomic_store_explicit(&g_start, 0, memory_order_relaxed);
  g_wb_shape = wb_shape;

  pthread_t ct, pt[NPROD];
  pthread_create(&ct, NULL, confirm, NULL);
  for (unsigned int p = 0; p < NPROD; p++) {
    pthread_create(&pt[p], NULL, producer, (void *)(uintptr_t)p);
  }
  atomic_store_explicit(&g_start, 1, memory_order_release);
  for (unsigned int p = 0; p < NPROD; p++) {
    pthread_join(pt[p], NULL);
  }
  /* Every claim is made; the last hold ends with the baton down. */
  while (atomic_load_explicit(&g_db->grant_baton, memory_order_acquire) != 0u &&
         !atomic_load_explicit(&g_failed, memory_order_relaxed)) {
    sched_yield();
  }
  atomic_store_explicit(&g_stop_confirm, 1, memory_order_release);
  pthread_join(ct, NULL);

  int rc = atomic_load_explicit(&g_failed, memory_order_acquire);
  if (atomic_load_explicit(&g_popped, memory_order_relaxed) !=
      (uint64_t)NPROD * PER_PROD) {
    fail("not every request was popped");
    rc = 1;
  }
  if (atomic_load_explicit(&g_db->grant_baton, memory_order_acquire) != 0u ||
      atomic_load_explicit(&g_round_out, memory_order_acquire) != 0 ||
      !arts_home_grantreq_queue_empty(&g_db->pending_rw)) {
    fail("the home does not rest with the baton down and the FIFO empty");
    rc = 1;
  }
  arts_home_grantreq_queue_destroy(&g_db->pending_rw);
  free(g_db);
  return rc;
}

int main(void) {
  pthread_t wd;
  pthread_create(&wd, NULL, watchdog, NULL);
  int rc = run(0);
  uint64_t wt_reclaims =
      atomic_load_explicit(&g_reclaims, memory_order_relaxed);
  if (!rc) {
    rc = run(1);
  }
  uint64_t reclaims = atomic_load_explicit(&g_reclaims, memory_order_relaxed);
  atomic_store_explicit(&g_done, 1, memory_order_release);
  pthread_join(wd, NULL);
#ifdef ARTS_DEBUG_INJECTION
  if (!rc && (wt_reclaims == 0u || reclaims == wt_reclaims)) {
    fail("no put-down met a newer claim: the look<->put-down window went "
         "unexercised");
    rc = 1;
  }
#endif
  if (rc) {
    return 1;
  }
  printf("PASS grant_retain_baton: %d producers x %u requests, write-through "
         "and write-back CONFIRM shapes; %llu + %llu re-looks\n",
         NPROD, PER_PROD, (unsigned long long)wt_reclaims,
         (unsigned long long)(reclaims - wt_reclaims));
  return 0;
}
