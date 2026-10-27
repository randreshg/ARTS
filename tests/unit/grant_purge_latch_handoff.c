/* SPDX-License-Identifier: Apache-2.0
 *
 * The PURGE home's serving tail (coherence/grant_purge.c, compiled in) driven
 * on one thread with the wire and the engine stubbed, through the schedules a
 * real run meets only rarely.  Every claimant publishes before it claims the
 * baton and a holder puts it down only from the claim count it looked behind,
 * so each of these must end with every request served and every hand-back
 * accepted once, and the home possessing the right, idle, with the baton down
 * and the latch empty:
 *   - a hand-back inside the serve's own send of the grant (its claim makes
 *     the put-down fail, and the holder's re-examination accepts it), and,
 *     as the control, one after the serve;
 *   - a request queued while possession is clear, served by the accept;
 *   - a request and the hand-back both inside the send: the put-down fails,
 *     the hand-back is accepted and the request served after it;
 *   - a remote request behind a home self front: served by the second look,
 *     or, when the drained writers are live, left to their idle edge by the
 *     give-up;
 *   - a home writer's idle edge with nothing queued, which leaves the baton
 *     untouched and enters no tail;
 *   - in an injection tree, a hand-back held between its latch and its claim
 *     (by the fuzz point there) while the baton holder, whose put-down a
 *     request's claim made fail, takes it from the latch: its own consume
 *     then fails and it only puts the baton down; or the rank is granted
 *     again and hands back again first, and its consume accepts that later
 *     hand-back, with that hand-back's reply, whose own arrival then only puts
 *     the baton down.
 *
 * PURE test: no arts_rt, no ports; the scripted hand-back runs on a second
 * thread, which runs only while this one waits for it.
 */
#include "coherence/grant_purge.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned int arts_global_rank_id = 0u;
unsigned int arts_global_rank_count = 2u;
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;
void arts_abort(uint8_t code) { exit(code ? code : 1); }

#include "counter_stubs.h"

void *arts_malloc(size_t n) { return malloc(n); }
void arts_free(void *p) { free(p); }
unsigned int arts_guid_get_rank(arts_guid_t g) {
  (void)g;
  return 0u;
}
#ifdef ARTS_DEBUG_INJECTION
#include <pthread.h>
#include <sched.h>
/* The scripted hand-back's thread stops at its first fuzz point, the one
 * between its latch CAS and its claim, until this thread lets it go: at its
 * own next fuzz point when g_release_at_next is set, or by release_helper. */
static _Thread_local int t_helper;
static _Thread_local int t_helper_held;
static _Atomic(int) g_helper_held, g_helper_go;
static int g_release_at_next;
static pthread_t g_helper;
static void release_helper(void) {
  atomic_store_explicit(&g_helper_go, 1, memory_order_release);
  pthread_join(g_helper, NULL);
}
void arts_sched_fuzz_point(void) {
  if (t_helper) {
    if (!t_helper_held) {
      t_helper_held = 1;
      atomic_store_explicit(&g_helper_held, 1, memory_order_release);
      while (!atomic_load_explicit(&g_helper_go, memory_order_acquire)) {
        sched_yield();
      }
    }
    return;
  }
  if (g_release_at_next) {
    g_release_at_next = 0;
    release_helper();
  }
}
#endif
bool arts_db_await_ack(sem_t *cv) {
  (void)cv;
  return true;
}
void arts_db_buf_landing_return(struct arts_db_buffer_s *b) { (void)b; }
/* The drained writers stay live when a scenario asks: the drain adds the
 * hold a woken home writer would take. */
static int g_drain_adds_hold;
static int g_drains;
void arts_db_drain_pending_rw_after_grant(struct arts_db_cache_s *c) {
  g_drains++;
  if (g_drain_adds_hold) {
    atomic_fetch_add_explicit(&c->writer_count, 1u, memory_order_acq_rel);
  }
}
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

/* The request FIFO, stubbed as a ring: one thread pushes and pops, so every
 * pushed node is linked. */
#define QCAP 8
static unsigned int q_ring[QCAP];
static unsigned int q_head, q_tail;
static int g_looks;
bool arts_home_grantreq_queue_peek_linked(
    const struct arts_home_grantreq_queue_s *q, unsigned int *r,
    struct arts_rdzv_landing_s *l, uint64_t *h) {
  (void)q;
  g_looks++;
  if (q_head == q_tail) {
    return false;
  }
  *r = q_ring[q_head % QCAP];
  if (l) {
    memset(l, 0, sizeof(*l));
  }
  if (h) {
    *h = ARTS_GRANT_VERSION_NONE;
  }
  return true;
}
bool arts_home_grantreq_queue_pop_linked(struct arts_home_grantreq_queue_s *q,
                                         unsigned int *r,
                                         struct arts_rdzv_landing_s *l,
                                         uint64_t *h) {
  bool ok = arts_home_grantreq_queue_peek_linked(q, r, l, h);
  if (ok) {
    q_head++;
  }
  return ok;
}
bool arts_home_grantreq_queue_pending(
    const struct arts_home_grantreq_queue_s *q) {
  (void)q;
  return q_head != q_tail;
}

static struct arts_db_s *g_db;
/* What the stubbed send of the n-th grant does before it returns. */
enum { IN_SEND_NONE, IN_SEND_HANDBACK, IN_SEND_REQUEST_HANDBACK };
static int g_in_send;
static unsigned int g_granted[QCAP];
static int g_grants, g_acks;
static unsigned int g_ack_to[QCAP];
static uint64_t g_ack_cv[QCAP];
static int g_ack_by_helper[QCAP];
/* Accepts on this thread that a request from rank 2 follows, still to come. */
static int g_ack_requests;

static void hand_back_cv(unsigned int holder, uint64_t cv) {
  struct arts_db_grant_return_reply_s reply = {
      .cv = cv, .version = 2u, .ack = true, .credit = false};
  arts_db_grant_return_arrived(g_db, holder, &reply);
}

static void hand_back(unsigned int holder) { hand_back_cv(holder, 7u); }

/* A GRANT_REQUEST reaching the home: queued, then the release policy's
 * answer (arts_handler_db_grant_request). */
static void request(unsigned int rank) {
  q_ring[q_tail++ % QCAP] = rank;
  arts_db_grant_request_arrived(&g_db->cache, g_db, rank);
}

#ifdef ARTS_DEBUG_INJECTION
/* The grant to this rank starts its scripted hand-back on the helper. */
static unsigned int g_helper_for = ~0u;
static void *helper_main(void *arg) {
  t_helper = 1;
  hand_back_cv((unsigned int)(uintptr_t)arg, 21u);
  return NULL;
}
#endif

void arts_db_send_grant_response(struct arts_db_cache_s *c, unsigned int t,
                                 const struct arts_rdzv_landing_s *l, bool dl) {
  (void)c;
  (void)l;
  (void)dl;
  g_granted[g_grants++ % QCAP] = t;
#ifdef ARTS_DEBUG_INJECTION
  if (t == g_helper_for) {
    g_helper_for = ~0u;
    atomic_store_explicit(&g_helper_held, 0, memory_order_relaxed);
    atomic_store_explicit(&g_helper_go, 0, memory_order_relaxed);
    pthread_create(&g_helper, NULL, helper_main, (void *)(uintptr_t)t);
    while (!atomic_load_explicit(&g_helper_held, memory_order_acquire)) {
      sched_yield();
    }
  }
#endif
  int what = g_in_send;
  g_in_send = IN_SEND_NONE; /* the first send only */
  if (what == IN_SEND_REQUEST_HANDBACK) {
    /* Possession is clear here, so the request queues without a claim. */
    request(2u);
  }
  if (what != IN_SEND_NONE) {
    /* The holder installed, ran and handed back before this thread left. */
    hand_back(t);
  }
}
void arts_send_db_publish_ack(unsigned int r, arts_guid_t g, uint64_t cv,
                              uint64_t v, struct arts_db_cache_s *hc) {
  (void)g;
  (void)v;
  (void)hc;
  int helper = 0;
#ifdef ARTS_DEBUG_INJECTION
  helper = t_helper;
#endif
  g_ack_to[g_acks % QCAP] = r;
  g_ack_cv[g_acks % QCAP] = cv;
  g_ack_by_helper[g_acks % QCAP] = helper;
  g_acks++;
  if (!helper && g_ack_requests > 0) {
    /* Possession is back: a request arriving now claims the baton. */
    g_ack_requests--;
    request(2u);
  }
}

static void reset(void) {
  g_db = calloc(1, sizeof(*g_db));
  g_db->home_initialized = true;
  atomic_init(&g_db->cache.writer_count, ARTS_GRANT_SEED_IDLE);
  atomic_init(&g_db->grant_baton, 0u);
  atomic_init(&g_db->pending_return_from, ARTS_GRANT_NO_RETURNER);
  q_head = q_tail = 0u;
  g_in_send = IN_SEND_NONE;
  g_grants = g_acks = g_looks = g_drains = 0;
  g_drain_adds_hold = 0;
  g_ack_requests = 0;
  memset(g_granted, 0xff, sizeof(g_granted));
  memset(g_ack_to, 0xff, sizeof(g_ack_to));
  memset(g_ack_cv, 0, sizeof(g_ack_cv));
  memset(g_ack_by_helper, 0, sizeof(g_ack_by_helper));
}

/* The home's grant plane at rest: the given word, the baton down, the latch
 * and the FIFO empty. */
static int at_rest(const char *what, unsigned int want_word) {
  unsigned int w =
      atomic_load_explicit(&g_db->cache.writer_count, memory_order_acquire);
  uint64_t baton =
      atomic_load_explicit(&g_db->grant_baton, memory_order_relaxed);
  unsigned int latch =
      atomic_load_explicit(&g_db->pending_return_from, memory_order_relaxed);
  if (w == want_word && baton == 0u && latch == ARTS_GRANT_NO_RETURNER &&
      q_head == q_tail) {
    return 0;
  }
  printf("FAIL grant_purge_latch_handoff: %s: word=0x%x baton=%llu latch=0x%x "
         "queued=%u grants=%d acks=%d\n",
         what, w, (unsigned long long)baton, latch, q_tail - q_head, g_grants,
         g_acks);
  fflush(stdout);
  return 1;
}

static int expect(int ok, const char *what) {
  if (!ok) {
    printf("FAIL grant_purge_latch_handoff: %s: grants=%d acks=%d drains=%d\n",
           what, g_grants, g_acks, g_drains);
    fflush(stdout);
  }
  return ok ? 0 : 1;
}

/* One request from rank 1, its hand-back inside the serve's send or after. */
static int handback(int in_send) {
  const char *what = in_send ? "hand-back while the serve holds the baton"
                             : "hand-back after the serve";
  reset();
  g_in_send = in_send ? IN_SEND_HANDBACK : IN_SEND_NONE;
  request(1u);
  if (!in_send) {
    hand_back(1u);
  }
  int rc = at_rest(what, ARTS_GRANT_SEED_IDLE);
  rc |= expect(g_grants == 1 && g_granted[0] == 1u && g_acks == 1 &&
                   g_ack_to[0] == 1u,
               what);
  free(g_db);
  return rc;
}

/* Rank 2 asks while rank 1 holds the right: it queues with no claim, and the
 * acceptance of rank 1's hand-back serves it. */
static int queued_while_clear(void) {
  const char *what = "request queued while possession is clear";
  reset();
  request(1u);
  request(2u);
  int rc = expect(
      atomic_load_explicit(&g_db->grant_baton, memory_order_relaxed) == 0u &&
          g_grants == 1 && q_tail - q_head == 1u,
      "a request at a home that does not possess claims nothing");
  hand_back(1u);
  rc |= expect(g_grants == 2 && g_granted[1] == 2u && g_acks == 1, what);
  rc |= at_rest(what, 0u);
  hand_back(2u);
  rc |= at_rest(what, ARTS_GRANT_SEED_IDLE);
  rc |= expect(g_acks == 2 && g_ack_to[1] == 2u, what);
  free(g_db);
  return rc;
}

/* Rank 2's request and rank 1's hand-back both land inside the send: the
 * hand-back's claim makes the put-down fail, the re-examination accepts it,
 * and the serve after it grants rank 2. */
static int request_and_handback_in_send(void) {
  const char *what = "request and hand-back inside the serve's send";
  reset();
  g_in_send = IN_SEND_REQUEST_HANDBACK;
  request(1u);
  int rc = expect(g_grants == 2 && g_granted[0] == 1u && g_granted[1] == 2u &&
                      g_acks == 1 && g_ack_to[0] == 1u,
                  what);
  rc |= at_rest(what, 0u);
  hand_back(2u);
  rc |= at_rest(what, ARTS_GRANT_SEED_IDLE);
  free(g_db);
  return rc;
}

/* A home self front with rank 1 queued behind it.  With the drained writers
 * done at once the second look serves rank 1; with them live the give-up
 * leaves the front to their idle edge, which serves it. */
static int behind_self_front(int writers_live) {
  const char *what = writers_live
                         ? "request behind a self front, drained writers live"
                         : "request behind a self front";
  reset();
  g_drain_adds_hold = writers_live;
  q_ring[q_tail++ % QCAP] = 0u; /* the home's own request, queued first */
  request(1u);                  /* finds possession and the baton free */
  int rc = expect(g_drains == 1, what);
  if (writers_live) {
    rc |= expect(g_grants == 0 && q_tail - q_head == 1u &&
                     atomic_load_explicit(&g_db->grant_baton,
                                          memory_order_relaxed) == 0u,
                 "the give-up leaves the front queued with the baton down");
    arts_db_grant_release_commit(&g_db->cache); /* the writer's idle edge */
  }
  rc |= expect(g_grants == 1 && g_granted[0] == 1u, what);
  rc |= at_rest(what, 0u);
  hand_back(1u);
  rc |= at_rest(what, ARTS_GRANT_SEED_IDLE);
  free(g_db);
  return rc;
}

/* A home writer's idle edge with nothing queued. */
static int idle_edge_empty(void) {
  const char *what = "idle edge with nothing queued";
  reset();
  atomic_store_explicit(&g_db->cache.writer_count, ARTS_GRANT_SEED_HOLDING,
                        memory_order_relaxed);
  arts_db_grant_release_commit(&g_db->cache);
  int rc = expect(g_looks == 0 && g_grants == 0, what);
  rc |= at_rest(what, ARTS_GRANT_SEED_IDLE);
  free(g_db);
  return rc;
}

#ifdef ARTS_DEBUG_INJECTION
/* Rank 1 holds the right.  Its hand-back is accepted on this thread, where a
 * request from rank 2 claims the baton behind the accept; the serve grants
 * rank 2, whose hand-back (on the helper) latches and stops before its claim.
 * The put-down fails on rank 2's request's claim, and the re-examination
 * takes the latch.  With `again` the accept of that hand-back is followed by
 * another request from rank 2, which is granted, and rank 2 hands back again
 * on this thread; the helper's claim and consume run between that hand-back's
 * latch and its claim.  Otherwise the helper resumes when this thread is
 * done. */
static int latch_taken_before_claim(int again) {
  const char *what = again ? "a hand-back's consume takes its rank's next one"
                           : "a hand-back's consume finds the latch taken";
  reset();
  request(1u);
  g_ack_requests = again ? 2 : 1;
  g_helper_for = 2u;
  hand_back_cv(1u, 11u);
  int rc = expect(g_acks == 2 && g_ack_to[1] == 2u && g_ack_cv[1] == 21u &&
                      !g_ack_by_helper[1],
                  "the holder takes the latched hand-back before its claim");
  if (again) {
    rc |= expect(g_grants == 3 && g_granted[2] == 2u,
                 "the rank is granted again before the hand-back's claim");
    g_release_at_next = 1;
    hand_back_cv(2u, 22u);
    rc |= expect(g_acks == 3 && g_ack_to[2] == 2u && g_ack_cv[2] == 22u &&
                     g_ack_by_helper[2],
                 "the held consume accepts the later hand-back, its reply");
  } else {
    release_helper();
    rc |= expect(g_acks == 2 && g_grants == 2,
                 "a consume that finds the latch empty accepts nothing");
  }
  rc |= expect(g_ack_to[0] == 1u && g_ack_cv[0] == 11u, what);
  rc |= at_rest(what, ARTS_GRANT_SEED_IDLE);
  free(g_db);
  return rc;
}
#endif

int main(void) {
  int rc = handback(1) | handback(0) | queued_while_clear() |
           request_and_handback_in_send() | behind_self_front(0) |
           behind_self_front(1) | idle_edge_empty();
#ifdef ARTS_DEBUG_INJECTION
  rc |= latch_taken_before_claim(0) | latch_taken_before_claim(1);
#endif
  if (rc == 0) {
    printf("PASS grant_purge_latch_handoff: hand-backs, requests and idle "
           "edges in the serve's windows each settle once\n");
  }
  return rc;
}
