/* SPDX-License-Identifier: Apache-2.0
 *
 * The PURGE home hub under concurrency: the serving tail
 * (coherence/grant_purge.c) and the request FIFO (coherence/grantreq_queue.c),
 * compiled in with the wire stubbed, driven by threads standing in for
 * everything that reaches a home at once:
 *   - progress threads taking GRANT_REQUEST and GRANT_RETURN messages from one
 *     mailbox in whatever order they win them;
 *   - remote ranks, each asking for the write right, holding it a while once
 *     its grant arrives, handing it back, and asking again at once, so a
 *     rank's next request can reach the home before its hand-back;
 *   - home writers, taking write turns on the local fast path while the home
 *     possesses the right, otherwise parking and opening one home request per
 *     take of the parked writers, and releasing through the count-dropping
 *     edge.
 * Checks: the right is in one place at a time (the home's writers, or one
 * remote rank); every request is granted once and every hand-back
 * acknowledged once, with the reply it carried; no thread waits for ever (a
 * watchdog); at the end the home possesses the right idle, the baton is down
 * and the latch, the FIFO and the parked writers are empty.  In an injection
 * tree every fuzz point yields at random.
 *
 * The progress threads push and claim themselves, so the order of the two in
 * the GRANT_REQUEST handler (coherence/grant.c) is not under test here; the
 * runtime's multinode loops are its check.
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

#include "coherence/grant_purge.c"
#include "coherence/grantreq_queue.c"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RANKS 3        /* remote ranks 1..RANKS; the home is rank 0 */
#define GRANTS 8000    /* write turns each remote rank asks for */
#define WRITERS 2      /* home writer threads */
#define TURNS 24000    /* write turns each home writer takes */
#define PROGRESS 2     /* home progress threads */
#define WATCHDOG_S 100 /* below the test's TIMEOUT */

unsigned int arts_global_rank_id = 0u;
unsigned int arts_global_rank_count = RANKS + 1u;
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;
void arts_abort(uint8_t code) { _exit(code ? code : 1); }

#include "counter_stubs.h"

void *arts_malloc(size_t n) { return malloc(n); }
void arts_free(void *p) { free(p); }
unsigned int arts_guid_get_rank(arts_guid_t g) {
  (void)g;
  return 0u;
}
#ifdef ARTS_DEBUG_INJECTION
void arts_sched_fuzz_point(void) {
  static _Thread_local unsigned int seed;
  if (seed == 0u) {
    seed = (unsigned int)(uintptr_t)&seed ^ (unsigned int)time(NULL);
  }
  unsigned int r = (unsigned int)rand_r(&seed) % 100u;
  if (r < 20u) {
    sched_yield();
  } else if (r < 23u) {
    struct timespec ts = {0, 2000L};
    nanosleep(&ts, NULL);
  }
}
#endif
bool arts_db_await_ack(sem_t *cv) {
  (void)cv;
  return true;
}
void arts_db_buf_landing_return(struct arts_db_buffer_s *b) { (void)b; }
void arts_db_grant_note_ex_holder(struct arts_db_s *db, unsigned int r) {
  (void)db;
  (void)r;
}
uint64_t arts_db_grant_serve_version(struct arts_db_s *db) {
  (void)db;
  return 1u;
}
arts_lf_link_t *arts_db_grant_take_waiters(struct arts_db_cache_s *c) {
  (void)c;
  return NULL;
}
unsigned int arts_pending_rw_chain_count(arts_lf_link_t *ch) {
  (void)ch;
  return 0u;
}
void arts_pending_rw_chain_wake(arts_lf_link_t *ch,
                                void (*cb)(const struct arts_db_rw_waiter_s *,
                                           void *),
                                void *ctx) {
  (void)ch;
  (void)cb;
  (void)ctx;
}
/* The home never hands the right back, so it never sends a return. */
void arts_transport_send_async(int r, char *m, unsigned int n) {
  (void)r;
  (void)m;
  (void)n;
  abort();
}
void arts_db_resolve_slot_cached(struct arts_edt_s *e, unsigned int s) {
  (void)e;
  (void)s;
}
#ifdef ARTS_DEBUG_WAKE_GUID
void arts_dep_waiter_check(const struct arts_edt_s *e, arts_guid_t g,
                           unsigned int s) {
  (void)e;
  (void)g;
  (void)s;
}
#endif
#if ARTS_DB_CENSUS
ARTS_THREAD_LOCAL uint8_t arts_db_census_serve;
#endif

static struct arts_db_s *g_db;
static _Atomic(int) g_failed;
static _Atomic(int) g_done;

static void fail(const char *what) {
  if (atomic_exchange_explicit(&g_failed, 1, memory_order_acq_rel) == 0) {
    printf("FAIL grant_purge_hub_stress: %s\n", what);
    fflush(stdout);
  }
}

/* ---- the mailbox the progress threads take messages from ---- */

enum { MSG_REQUEST, MSG_RETURN };
struct msg {
  int type;
  unsigned int rank;
  uint64_t cv;
};
#define MBOX 64
static pthread_mutex_t g_mbox_mu = PTHREAD_MUTEX_INITIALIZER;
static struct msg g_mbox[MBOX];
static unsigned int g_mbox_head, g_mbox_tail;

static void post(int type, unsigned int rank, uint64_t cv) {
  pthread_mutex_lock(&g_mbox_mu);
  if (g_mbox_tail - g_mbox_head == MBOX) {
    fail("mailbox overflow");
  }
  g_mbox[g_mbox_tail++ % MBOX] = (struct msg){type, rank, cv};
  pthread_mutex_unlock(&g_mbox_mu);
}

static int take(struct msg *m) {
  pthread_mutex_lock(&g_mbox_mu);
  int got = g_mbox_head != g_mbox_tail;
  if (got) {
    *m = g_mbox[g_mbox_head++ % MBOX];
  }
  pthread_mutex_unlock(&g_mbox_mu);
  return got;
}

static _Atomic(int) g_stop_progress;

static void *progress(void *arg) {
  (void)arg;
  for (;;) {
    struct msg m;
    if (!take(&m)) {
      if (atomic_load_explicit(&g_stop_progress, memory_order_acquire)) {
        return NULL;
      }
      sched_yield();
      continue;
    }
    if (m.type == MSG_REQUEST) {
      /* arts_handler_db_grant_request: queued, then the release policy. */
      arts_home_grantreq_queue_push(&g_db->pending_rw, m.rank, NULL,
                                    ARTS_GRANT_VERSION_NONE);
      arts_sched_fuzz_point();
      arts_db_grant_request_arrived(&g_db->cache, g_db, m.rank);
    } else {
      struct arts_db_grant_return_reply_s reply = {
          .cv = m.cv, .version = 0u, .ack = true, .credit = false};
      arts_db_grant_return_arrived(g_db, m.rank, &reply);
    }
  }
}

/* ---- where the right is ---- */

/* The remote rank holding the right, or -1. */
static _Atomic(int) g_holder = -1;
/* Home write turns in progress. */
static _Atomic(int) g_home_live;

static _Atomic(uint64_t) g_grants_to[RANKS + 1];
static _Atomic(uint64_t) g_acks_to[RANKS + 1];

void arts_db_send_grant_response(struct arts_db_cache_s *c, unsigned int t,
                                 const struct arts_rdzv_landing_s *l, bool dl) {
  (void)l;
  (void)dl;
  if (t == 0u || t > RANKS) {
    fail("a grant to a rank that never asked remotely");
    return;
  }
  if (atomic_load_explicit(&c->writer_count, memory_order_acquire) != 0u) {
    fail("a grant leaves while the home's word is not empty");
  }
  atomic_fetch_add_explicit(&g_grants_to[t], 1u, memory_order_release);
}

void arts_send_db_publish_ack(unsigned int r, arts_guid_t g, uint64_t cv,
                              uint64_t v, struct arts_db_cache_s *hc) {
  (void)g;
  (void)v;
  (void)hc;
  if (r == 0u || r > RANKS) {
    fail("an acknowledgement to a rank that never handed back");
    return;
  }
  /* A rank has one hand-back out at a time, so its replies come back in the
   * order it sent them; each carries its own sequence number. */
  uint64_t n = atomic_load_explicit(&g_acks_to[r], memory_order_relaxed);
  if (cv != (((uint64_t)r << 32) | n)) {
    fail("an acknowledgement carries another hand-back's reply");
  }
  atomic_fetch_add_explicit(&g_acks_to[r], 1u, memory_order_release);
}

/* ---- remote ranks ---- */

static void hold_for_a_while(unsigned int *seed) {
  int n = rand_r(seed) % 64;
  for (int i = 0; i < n; i++) {
    if ((i & 15) == 0) {
      sched_yield();
    }
  }
}

static void *remote_rank(void *arg) {
  unsigned int r = (unsigned int)(uintptr_t)arg;
  unsigned int seed = r * 7919u + 1u;
  for (uint64_t k = 0; k < GRANTS; k++) {
    post(MSG_REQUEST, r, 0u);
    while (atomic_load_explicit(&g_grants_to[r], memory_order_acquire) <= k) {
      if (atomic_load_explicit(&g_failed, memory_order_relaxed)) {
        return NULL;
      }
      sched_yield();
    }
    int none = -1;
    if (!atomic_compare_exchange_strong_explicit(&g_holder, &none, (int)r,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
      fail("two remote ranks hold the right");
    }
    if (atomic_load_explicit(&g_home_live, memory_order_acquire) != 0 ||
        ARTS_GRANT_OWN_OF(atomic_load_explicit(&g_db->cache.writer_count,
                                               memory_order_acquire))) {
      fail("a remote rank holds the right while the home has it");
    }
    hold_for_a_while(&seed);
    atomic_store_explicit(&g_holder, -1, memory_order_release);
    post(MSG_RETURN, r, ((uint64_t)r << 32) | k);
  }
  while (atomic_load_explicit(&g_acks_to[r], memory_order_acquire) < GRANTS) {
    if (atomic_load_explicit(&g_failed, memory_order_relaxed)) {
      return NULL;
    }
    sched_yield();
  }
  return NULL;
}

/* ---- home writers ---- */

/* The home's parked writers and its one open request: a request is opened
 * only for a writer no earlier take removed (arts_db_acquire_remote_rw). */
static pthread_mutex_t g_park_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_parked[WRITERS];
static int g_home_req_open;
static _Atomic(int) g_woken[WRITERS];
static _Atomic(uint64_t) g_drains, g_empty_drains;

/* The self front's drain: each parked writer is woken with a hold of its
 * own. */
void arts_db_drain_pending_rw_after_grant(struct arts_db_cache_s *c) {
  int taken[WRITERS];
  unsigned int k = 0u;
  pthread_mutex_lock(&g_park_mu);
  for (int w = 0; w < WRITERS; w++) {
    taken[w] = g_parked[w];
    g_parked[w] = 0;
    k += (unsigned int)taken[w];
  }
  g_home_req_open = 0;
  pthread_mutex_unlock(&g_park_mu);
  atomic_fetch_add_explicit(&g_drains, 1u, memory_order_relaxed);
  if (k == 0u) {
    atomic_fetch_add_explicit(&g_empty_drains, 1u, memory_order_relaxed);
  }
  if (!ARTS_GRANT_OWN_OF(
          atomic_load_explicit(&c->writer_count, memory_order_acquire))) {
    fail("a self front drained while the home does not possess");
  }
  atomic_fetch_add_explicit(&c->writer_count, k, memory_order_acq_rel);
  for (int w = 0; w < WRITERS; w++) {
    if (taken[w]) {
      atomic_store_explicit(&g_woken[w], 1, memory_order_release);
    }
  }
}

static void *home_writer(void *arg) {
  int w = (int)(uintptr_t)arg;
  unsigned int seed = (unsigned int)w * 104729u + 3u;
  struct arts_db_cache_s *c = &g_db->cache;
  for (int t = 0; t < TURNS; t++) {
    /* arts_db_acquire_rw_local_fast: a positive word is possession. */
    bool got = false;
    unsigned int wc =
        atomic_load_explicit(&c->writer_count, memory_order_acquire);
    while ((int)wc > 0) {
      if (atomic_compare_exchange_strong_explicit(&c->writer_count, &wc,
                                                  wc + 1u, memory_order_acq_rel,
                                                  memory_order_acquire)) {
        got = true;
        break;
      }
    }
    if (!got) {
      pthread_mutex_lock(&g_park_mu);
      g_parked[w] = 1;
      int open = !g_home_req_open;
      g_home_req_open = 1;
      pthread_mutex_unlock(&g_park_mu);
      if (open) {
        /* The home's own GRANT_REQUEST, dispatched inline. */
        arts_home_grantreq_queue_push(&g_db->pending_rw, 0u, NULL,
                                      ARTS_GRANT_VERSION_NONE);
        arts_sched_fuzz_point();
        arts_db_grant_request_arrived(c, g_db, 0u);
      }
      while (!atomic_exchange_explicit(&g_woken[w], 0, memory_order_acq_rel)) {
        if (atomic_load_explicit(&g_failed, memory_order_relaxed)) {
          return NULL;
        }
        sched_yield();
      }
    }
    atomic_fetch_add_explicit(&g_home_live, 1, memory_order_acq_rel);
    if (atomic_load_explicit(&g_holder, memory_order_acquire) != -1 ||
        !ARTS_GRANT_OWN_OF(
            atomic_load_explicit(&c->writer_count, memory_order_acquire))) {
      fail("a home writer runs while a remote rank holds the right");
    }
    hold_for_a_while(&seed);
    atomic_fetch_sub_explicit(&g_home_live, 1, memory_order_acq_rel);
    arts_db_grant_release_commit(c);
  }
  return NULL;
}

static void dump(void) {
  pthread_mutex_lock(&g_mbox_mu);
  pthread_mutex_lock(&g_park_mu);
  printf("  word=0x%x baton=%llu latch=0x%x fifo_pending=%d mailbox=%u "
         "holder=%d home_live=%d req_open=%d\n",
         atomic_load_explicit(&g_db->cache.writer_count, memory_order_acquire),
         (unsigned long long)atomic_load_explicit(&g_db->grant_baton,
                                                  memory_order_acquire),
         atomic_load_explicit(&g_db->pending_return_from, memory_order_acquire),
         (int)arts_home_grantreq_queue_pending(&g_db->pending_rw),
         g_mbox_tail - g_mbox_head,
         atomic_load_explicit(&g_holder, memory_order_acquire),
         atomic_load_explicit(&g_home_live, memory_order_acquire),
         g_home_req_open);
  for (unsigned int r = 1; r <= RANKS; r++) {
    printf("  rank %u: grants=%llu acks=%llu\n", r,
           (unsigned long long)atomic_load_explicit(&g_grants_to[r],
                                                    memory_order_acquire),
           (unsigned long long)atomic_load_explicit(&g_acks_to[r],
                                                    memory_order_acquire));
  }
  for (int w = 0; w < WRITERS; w++) {
    printf("  writer %d: parked=%d\n", w, g_parked[w]);
  }
  pthread_mutex_unlock(&g_park_mu);
  pthread_mutex_unlock(&g_mbox_mu);
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
  printf("FAIL grant_purge_hub_stress: TIMEOUT (a request, a hand-back or a "
         "home writer is stranded)\n");
  dump();
  fflush(stdout);
  _exit(1);
  return NULL;
}

int main(void) {
  g_db = calloc(1, sizeof(*g_db));
  g_db->home_initialized = true;
  atomic_init(&g_db->cache.writer_count, ARTS_GRANT_SEED_IDLE);
  atomic_init(&g_db->grant_baton, 0u);
  atomic_init(&g_db->pending_return_from, ARTS_GRANT_NO_RETURNER);
  arts_home_grantreq_queue_init(&g_db->pending_rw);

  pthread_t wd, pt[PROGRESS], rt[RANKS], ht[WRITERS];
  pthread_create(&wd, NULL, watchdog, NULL);
  for (int i = 0; i < PROGRESS; i++) {
    pthread_create(&pt[i], NULL, progress, NULL);
  }
  for (unsigned int r = 0; r < RANKS; r++) {
    pthread_create(&rt[r], NULL, remote_rank, (void *)(uintptr_t)(r + 1u));
  }
  for (int w = 0; w < WRITERS; w++) {
    pthread_create(&ht[w], NULL, home_writer, (void *)(uintptr_t)w);
  }
  for (unsigned int r = 0; r < RANKS; r++) {
    pthread_join(rt[r], NULL);
  }
  for (int w = 0; w < WRITERS; w++) {
    pthread_join(ht[w], NULL);
  }
  /* Every message was taken before the ranks saw their last
   * acknowledgement; let the progress threads finish what they hold. */
  atomic_store_explicit(&g_stop_progress, 1, memory_order_release);
  for (int i = 0; i < PROGRESS; i++) {
    pthread_join(pt[i], NULL);
  }
  atomic_store_explicit(&g_done, 1, memory_order_release);
  pthread_join(wd, NULL);

  int rc = atomic_load_explicit(&g_failed, memory_order_acquire);
  for (unsigned int r = 1; r <= RANKS; r++) {
    if (atomic_load_explicit(&g_grants_to[r], memory_order_acquire) != GRANTS ||
        atomic_load_explicit(&g_acks_to[r], memory_order_acquire) != GRANTS) {
      fail("a request was granted, or a hand-back acknowledged, other than "
           "once");
      rc = 1;
    }
  }
  bool parked = false;
  for (int w = 0; w < WRITERS; w++) {
    parked |= g_parked[w] != 0;
  }
  if (atomic_load_explicit(&g_db->cache.writer_count, memory_order_acquire) !=
          ARTS_GRANT_SEED_IDLE ||
      atomic_load_explicit(&g_db->grant_baton, memory_order_acquire) != 0u ||
      atomic_load_explicit(&g_db->pending_return_from, memory_order_acquire) !=
          ARTS_GRANT_NO_RETURNER ||
      !arts_home_grantreq_queue_empty(&g_db->pending_rw) ||
      g_mbox_head != g_mbox_tail || parked || g_home_req_open != 0) {
    fail("the home does not rest idle with the baton down");
    rc = 1;
  }
  if (atomic_load_explicit(&g_empty_drains, memory_order_relaxed) != 0u) {
    fail("a home self front found no parked writer");
    rc = 1;
  }
  if (rc) {
    dump();
    return 1;
  }
  arts_home_grantreq_queue_destroy(&g_db->pending_rw);
  free(g_db);
  printf(
      "PASS grant_purge_hub_stress: %d ranks x %d grants, %d home writers x "
      "%d turns; %llu self-front drains, %llu re-looks\n",
      RANKS, GRANTS, WRITERS, TURNS,
      (unsigned long long)atomic_load_explicit(&g_drains, memory_order_relaxed),
      (unsigned long long)atomic_load_explicit(&g_reclaims,
                                               memory_order_relaxed));
  return 0;
}
