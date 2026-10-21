/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* db_destroy_cover_settles — every destroyed block's home is torn down once
 * the requests it was told are still coming have arrived.  A home that counts
 * one request too many never tears down, which no shutdown check can see (the
 * shutdown reclaims it), so after its last destroy the program waits for
 * every home of its blocks to go.
 *
 * Rounds of blocks on fresh homes, in one of two shapes.  `t2`: on a rank
 * C != H a reader then a writer acquire a block H created, so C's write turn
 * can serve the reader while C's read request is still on its way.  `t1`: C's
 * reader and writer first touch a reserved GUID homed on H, then C creates
 * it, so the create's hold serves both while their requests are on their way.
 * Each block is destroyed by H or by C once every hold is released.  After
 * the last round every rank polls the route table for its own homes of those
 * blocks until none is installed and, where the build counts rendezvous
 * landings (ARTS_DEBUG_LANDING_BALANCE), until it has none outstanding; a
 * rank still holding one after 20 s fails the run.  A healthy transport
 * delivers every request eventually, so the deadline detects a hang and makes
 * no timing claim.  With `hop` (t2, three ranks or more) a third rank D first
 * takes and releases a write turn on each block, so C's read request reaches
 * H while D holds the block and H's answer is a hop (a redirect or forward to
 * D); held with ARTS_REQUEST_DELAY_MS=<n>:hop, that hop lands after the
 * teardown.
 *
 * Arguments: t2|t1 [hop] [rounds [blocks]] (t2, 8, 32).  Needs two ranks.
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "arts.h"
#include "arts/coherence/buffer.h"
#include "arts/gas/route_table.h"
#include "../test_failure_status.h"

#ifdef ARTS_DEBUG_LANDING_BALANCE
#define LANDINGS_OUT() arts_db_landings_outstanding()
#else
#define LANDINGS_OUT() ((int64_t)0)
#endif

#define SETTLE_MS 20000L

static unsigned int g_rounds = 8, g_blocks = 32;
static bool g_first_touch = false, g_hop = false;

/* The blocks this rank homes, recorded by the EDT that runs on the home. */
#define MAX_HOMED 65536u
static arts_guid_t g_homed[MAX_HOMED];
static _Atomic(unsigned int) g_nhomed;

static void note_homed(arts_guid_t g) {
  unsigned int i =
      atomic_fetch_add_explicit(&g_nhomed, 1u, memory_order_relaxed);
  if (i >= MAX_HOMED) {
    arts_printf("FAIL: db_destroy_cover_settles registry full\n");
    arts_test_fail();
    return;
  }
  g_homed[i] = g;
}

static arts_guid_t latch_on(unsigned int rank, int32_t n) {
  arts_event_hint_t h = ARTS_EVENT_HINT_LATCH(n);
  h.rank = rank;
  return arts_event_create(&h);
}

static arts_guid_t edt_on(arts_edt_t fn, unsigned int rank, uint32_t paramc,
                          const uint64_t *paramv, uint32_t depc,
                          arts_guid_t output_event) {
  arts_edt_hint_t h = ARTS_EDT_HINT_DEFAULTS;
  h.rank = rank;
  h.output_event = output_event;
  return arts_edt_create(fn, paramc, paramv, depc, &h);
}

static long now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long)t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static void user(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  if (depv[0].ptr == NULL) {
    arts_printf("FAIL: db_destroy_cover_settles user got no storage\n");
    arts_test_fail();
    return;
  }
  if (depv[0].mode == DB_MODE_RW) {
    atomic_fetch_add_explicit((_Atomic(uint32_t) *)depv[0].ptr, 1u,
                              memory_order_relaxed);
  } else {
    (void)atomic_load_explicit((_Atomic(uint32_t) *)depv[0].ptr,
                               memory_order_relaxed);
  }
}

/* paramv: g, round latch */
static void destroyer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_db_destroy((arts_guid_t)paramv[0]);
  arts_event_satisfy((arts_guid_t)paramv[1], NULL_GUID);
}

/* runs on C. paramv: g, destroyer rank, round latch, first touch */
static void pair(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t g = (arts_guid_t)paramv[0];
  unsigned int where = (unsigned int)paramv[1];
  bool first_touch = paramv[3] != 0u;
  unsigned int here = arts_get_current_rank();
  /* t1: the creator's own hold is a third holder the destroy follows. */
  arts_guid_t latch = latch_on(where, first_touch ? 3 : 2);
  uint64_t upv[1] = {(uint64_t)g};
  arts_guid_t r = edt_on(user, here, 1, upv, 1, latch);
  arts_guid_t w = edt_on(user, here, 1, upv, 1, latch);
  arts_add_dependence(g, r, 0, DB_MODE_RO);
  arts_add_dependence(g, w, 0, DB_MODE_RW);
  uint64_t dpv[2] = {(uint64_t)g, paramv[2]};
  arts_guid_t d = edt_on(destroyer, where, 2, dpv, 1, NULL_GUID);
  arts_add_dependence(latch, d, 0, DB_MODE_NULL);
  if (first_touch) {
    void *p =
        arts_db_create_with_guid(g, 64u, ARTS_DB, ARTS_DB_PROP_NONE, NULL);
    if (p == NULL) {
      arts_printf("FAIL: db_destroy_cover_settles create gave no storage\n");
      arts_test_fail();
    } else {
      atomic_store_explicit((_Atomic(uint32_t) *)p, 7u, memory_order_relaxed);
    }
    arts_db_release(g, DB_MODE_RW);
    arts_event_satisfy(latch, NULL_GUID);
  }
}

/* runs on H. paramv: c, destroyer rank, round latch, reserved g or 0, hop.
 * The settings live on the first rank only, so they travel as parameters. */
static void maker(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t g = (arts_guid_t)paramv[3];
  if (g == NULL_GUID) {
    void *p = NULL;
    g = arts_db_create(&p, 64, ARTS_DB, ARTS_DB_PROP_NONE, NULL);
    atomic_store_explicit((_Atomic(uint32_t) *)p, 7u, memory_order_relaxed);
    arts_db_release(g, DB_MODE_RW);
  }
  note_homed(g);
  unsigned int c = (unsigned int)paramv[0];
  uint64_t ppv[4] = {(uint64_t)g, paramv[1], paramv[2],
                     paramv[3] != 0u ? 1u : 0u};
  unsigned int n = arts_get_total_ranks();
  unsigned int here = arts_get_current_rank();
  unsigned int d = (c + 1u) % n;
  if (paramv[4] != 0u && paramv[3] == 0u && n >= 3u) {
    if (d == here) {
      d = (d + 1u) % n;
    }
    /* D's write turn first; C's pair starts once D released. */
    arts_guid_t after_d = latch_on(c, 1);
    uint64_t upv[1] = {(uint64_t)g};
    arts_guid_t pre = edt_on(user, d, 1, upv, 1, after_d);
    arts_add_dependence(g, pre, 0, DB_MODE_RW);
    arts_guid_t pr = edt_on(pair, c, 4, ppv, 1, NULL_GUID);
    arts_add_dependence(after_d, pr, 0, DB_MODE_NULL);
    return;
  }
  (void)edt_on(pair, c, 4, ppv, 0, NULL_GUID);
}

/* paramv: started_ms, done latch */
static void poller(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  unsigned int n = atomic_load_explicit(&g_nhomed, memory_order_relaxed);
  unsigned int live = 0;
  arts_guid_t first_live = NULL_GUID;
  for (unsigned int i = 0; i < n && i < MAX_HOMED; i++) {
    arts_shared_ptr_t h = arts_route_table_lookup_db(g_homed[i]);
    if (arts_shared_get(h) != NULL) {
      if (live++ == 0u) {
        first_live = g_homed[i];
      }
    }
    arts_shared_release(&h);
  }
  int64_t out = LANDINGS_OUT();
  if (live == 0u && out == 0) {
    arts_event_satisfy((arts_guid_t)paramv[1], NULL_GUID);
    return;
  }
  if (now_ms() - (long)paramv[0] > SETTLE_MS) {
    arts_printf("FAIL: db_destroy_cover_settles rank %u: %u destroyed "
                "block(s) never torn down at their home (first %lu), %lld "
                "landing(s) never ended\n",
                arts_get_current_rank(), live, (unsigned long)first_live,
                (long long)out);
    arts_test_fail();
    arts_event_satisfy((arts_guid_t)paramv[1], NULL_GUID);
    return;
  }
  (void)edt_on(poller, arts_get_current_rank(), 2, paramv, 0, NULL_GUID);
}

static void finish(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (arts_test_status() == 0) {
    arts_printf("PASS: db_destroy_cover_settles %u rounds x %u blocks (%s%s)\n",
                g_rounds, g_blocks, g_first_touch ? "t1" : "t2",
                g_hop ? ", hop" : "");
  }
  arts_shutdown();
}

static void settle(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int n = arts_get_total_ranks();
  arts_guid_t done = latch_on(0u, (int32_t)n);
  arts_guid_t f = edt_on(finish, 0u, 0, NULL, 1, NULL_GUID);
  arts_add_dependence(done, f, 0, DB_MODE_NULL);
  for (unsigned int r = 0; r < n; r++) {
    uint64_t pv[2] = {(uint64_t)now_ms(), (uint64_t)done};
    (void)edt_on(poller, r, 2, pv, 0, NULL_GUID);
  }
}

/* paramv: round */
static void round_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  unsigned int round = (unsigned int)paramv[0];
  unsigned int n = arts_get_total_ranks();
  arts_guid_t all = latch_on(0u, (int32_t)g_blocks);
  if (round + 1u < g_rounds) {
    uint64_t npv[1] = {round + 1u};
    arts_guid_t nx = edt_on(round_edt, 0u, 1, npv, 1, NULL_GUID);
    arts_add_dependence(all, nx, 0, DB_MODE_NULL);
  } else {
    arts_guid_t t = edt_on(settle, 0u, 0, NULL, 1, NULL_GUID);
    arts_add_dependence(all, t, 0, DB_MODE_NULL);
  }
  for (unsigned int b = 0; b < g_blocks; b++) {
    unsigned int h = (b + round) % n;
    unsigned int c = (h + 1u + (b / n) % (n - 1u)) % n;
    unsigned int where = ((b + round) & 1u) ? c : h;
    arts_guid_t g =
        g_first_touch ? arts_guid_reserve(ARTS_GUID_DB, h) : NULL_GUID;
    uint64_t mpv[5] = {c, where, (uint64_t)all, (uint64_t)g, g_hop ? 1u : 0u};
    (void)edt_on(maker, h, 5, mpv, 0, NULL_GUID);
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  int argc = (int)paramv[0];
  char **argv = (char **)paramv[1];
  int a = 1;
  if (argc > a && strcmp(argv[a], "t1") == 0) {
    g_first_touch = true;
    a++;
  } else if (argc > a && strcmp(argv[a], "t2") == 0) {
    a++;
  }
  if (argc > a && strcmp(argv[a], "hop") == 0) {
    g_hop = true;
    a++;
  }
  if (argc > a) {
    g_rounds = (unsigned int)strtoul(argv[a++], NULL, 10);
  }
  if (argc > a) {
    g_blocks = (unsigned int)strtoul(argv[a], NULL, 10);
  }
  if (arts_get_total_ranks() < 2u) {
    arts_printf("FAIL: db_destroy_cover_settles needs two ranks\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  uint64_t pv[1] = {0u};
  (void)edt_on(round_edt, 0u, 1, pv, 0, NULL_GUID);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
