/* SPDX-License-Identifier: Apache-2.0
 *
 * A hand-back that reaches the home while the serving thread still holds the
 * baton is accepted by that thread's own re-check, exactly once.
 *
 * Where a holder hands the write right back at its own idle edge, the home's
 * serve puts its baton down only after the grant has left, and the holder can
 * install, run, release and hand the right back before that.  Such a
 * hand-back finds the baton held and parks in the single-slot return latch;
 * nothing but the serving thread's re-check after it puts the baton down will
 * ever take it.  A real run meets that window rarely, so this drives the
 * home's serving tail (coherence/grant_purge.c, compiled in) on one thread
 * with the wire and the engine stubbed, and delivers the hand-back inside the
 * serve's own send of the grant, then, as the control, after the serve has
 * finished.  Either way the hand-back must be accepted once — one
 * acknowledgement to the holder — and leave the home possessing the right,
 * idle, with the baton down and the latch empty.
 *
 * PURE test: no arts_rt, no ports, no threads.
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
void arts_sched_fuzz_point(void) {}
#endif
bool arts_db_await_ack(sem_t *cv) {
  (void)cv;
  return true;
}
void arts_db_buf_landing_return(struct arts_db_buffer_s *b) { (void)b; }
void arts_db_drain_pending_rw_after_grant(struct arts_db_cache_s *c) {
  (void)c;
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
                                void (*cb)(arts_guid_t, unsigned int, void *),
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
void mark_edt_ready_by_guid(arts_guid_t g, unsigned int s) {
  (void)g;
  (void)s;
}
#if ARTS_DB_CENSUS
ARTS_THREAD_LOCAL uint8_t arts_db_census_serve;
#endif

/* One requester queued at a time is all either scenario needs. */
static int q_n = 0;
static unsigned int q_rank;
bool arts_home_grantreq_queue_peek(const struct arts_home_grantreq_queue_s *q,
                                   unsigned int *r,
                                   struct arts_rdzv_landing_s *l, uint64_t *h) {
  (void)q;
  if (!q_n) {
    return false;
  }
  *r = q_rank;
  if (l) {
    memset(l, 0, sizeof(*l));
  }
  if (h) {
    *h = ARTS_GRANT_VERSION_NONE;
  }
  return true;
}
bool arts_home_grantreq_queue_pop(struct arts_home_grantreq_queue_s *q,
                                  unsigned int *r,
                                  struct arts_rdzv_landing_s *l, uint64_t *h) {
  bool ok = arts_home_grantreq_queue_peek(q, r, l, h);
  q_n = 0;
  return ok;
}
bool arts_home_grantreq_queue_pending(
    const struct arts_home_grantreq_queue_s *q) {
  (void)q;
  return q_n != 0;
}

static struct arts_db_s *g_db;
static int g_handback_in_send;
static int g_grants, g_acks;
static unsigned int g_ack_to;

static void hand_back(unsigned int holder) {
  struct arts_db_grant_return_reply_s reply = {
      .cv = 7u, .version = 2u, .ack = true, .credit = false};
  arts_db_grant_return_arrived(g_db, holder, &reply);
}

void arts_db_send_grant_response(struct arts_db_cache_s *c, unsigned int t,
                                 const struct arts_rdzv_landing_s *l, bool dl) {
  (void)c;
  (void)l;
  (void)dl;
  g_grants++;
  if (g_handback_in_send) {
    /* The holder installed, ran and handed back before this thread left. */
    hand_back(t);
  }
}
void arts_send_db_publish_ack(unsigned int r, arts_guid_t g, uint64_t cv,
                              uint64_t v, struct arts_db_cache_s *hc) {
  (void)g;
  (void)cv;
  (void)v;
  (void)hc;
  g_acks++;
  g_ack_to = r;
}

static int scenario(int in_send) {
  g_db = calloc(1, sizeof(*g_db));
  g_db->home_initialized = true;
  atomic_init(&g_db->cache.writer_count, ARTS_GRANT_SEED_IDLE);
  atomic_init(&g_db->invalidate_in_flight, 0u);
  atomic_init(&g_db->pending_return_from, ARTS_GRANT_NO_RETURNER);
  g_grants = g_acks = 0;
  g_ack_to = ~0u;
  g_handback_in_send = in_send;
  q_n = 1;
  q_rank = 1u;
  arts_db_grant_request_arrived(&g_db->cache, g_db, 1u);
  if (!in_send) {
    hand_back(1u);
  }
  unsigned int w =
      atomic_load_explicit(&g_db->cache.writer_count, memory_order_acquire);
  unsigned int baton =
      atomic_load_explicit(&g_db->invalidate_in_flight, memory_order_relaxed);
  unsigned int latch =
      atomic_load_explicit(&g_db->pending_return_from, memory_order_relaxed);
  int ok = (w == ARTS_GRANT_SEED_IDLE && baton == 0u &&
            latch == ARTS_GRANT_NO_RETURNER && g_grants == 1 && g_acks == 1 &&
            g_ack_to == 1u);
  if (!ok) {
    printf("FAIL grant_purge_latch_handoff: hand-back %s: word=0x%x baton=%u "
           "latch=0x%x grants=%d acks=%d\n",
           in_send ? "while the serve holds the baton" : "after the serve", w,
           baton, latch, g_grants, g_acks);
  }
  free(g_db);
  return ok ? 0 : 1;
}

int main(void) {
  int rc = scenario(1) | scenario(0);
  if (rc == 0) {
    printf("PASS grant_purge_latch_handoff: a hand-back inside the serve and "
           "one after it are each accepted once\n");
  }
  return rc;
}
