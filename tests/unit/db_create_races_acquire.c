/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* db_create_races_acquire — acquires that race a create's claim of the
 * first-touch cache their own rank made leave no request behind that nobody
 * waits on, and every destroyed block's home goes.
 *
 * Per block g (reserved, home H): on rank C != H a setup EDT makes readers
 * and writers ready on g through a trigger EDT that runs on another worker of
 * C, spins a little (a different length per block), then creates g on C.
 * The acquires run beside the create's claim: each is served by the claim's
 * hold or by a request of its own, and a request the claim found already sent
 * is discarded by the home, which the create's announce told it to expect.
 * After every block is destroyed, each rank polls the route table for its
 * own homes of the blocks until none is installed and, where the build counts
 * rendezvous landings (ARTS_DEBUG_LANDING_BALANCE), until it has none
 * outstanding: a request no acquire waits on, or any count too many, keeps a
 * home installed, and a rank still holding one after 20 s fails the run.  Run
 * with ARTS_SCHED_FUZZ, the acquires and the claim interleave at every
 * decision point.
 *
 * Arguments: [readers writers [blocks]] (0, 6, 32); with `wfirst` before them
 * the writers are made ready first.  Needs two ranks.
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
#define SPIN_US 200L

#define MAX_HOMED 65536u
static arts_guid_t g_homed[MAX_HOMED];
static _Atomic(unsigned int) g_nhomed;

static unsigned int g_blocks = 32;

static long now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long)t.tv_sec * 1000L + t.tv_nsec / 1000000L;
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

static void spin_us(long us) {
  struct timespec a, b;
  clock_gettime(CLOCK_MONOTONIC, &a);
  do {
    clock_gettime(CLOCK_MONOTONIC, &b);
  } while ((b.tv_sec - a.tv_sec) * 1000000L + (b.tv_nsec - a.tv_nsec) / 1000L <
           us);
}

/* paramv: g */
static void user(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  if (depv[0].guid != (arts_guid_t)paramv[0]) {
    arts_printf("FAIL: db_create_races_acquire slot guid mismatch\n");
    arts_test_fail();
    return;
  }
  if (depv[0].ptr == NULL) {
    arts_printf("FAIL: db_create_races_acquire acquirer got no storage\n");
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

/* paramv: e */
static void trigger(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                    arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_event_satisfy((arts_guid_t)paramv[0], NULL_GUID);
}

/* paramv: g, all */
static void destroyer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_db_destroy((arts_guid_t)paramv[0]);
  arts_event_satisfy((arts_guid_t)paramv[1], NULL_GUID);
}

/* paramv: g, latch, destroyer rank, all, spin_us, readers, writers, wfirst */
static void setup(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t g = (arts_guid_t)paramv[0];
  arts_guid_t latch = (arts_guid_t)paramv[1];
  unsigned int nr = (unsigned int)paramv[5];
  unsigned int nw = (unsigned int)paramv[6];
  bool wfirst = paramv[7] != 0u;
  unsigned int here = arts_get_current_rank();
  arts_event_hint_t eh = ARTS_EVENT_HINT_LATCH(1);
  eh.rank = here;
  arts_guid_t e = arts_event_create(&eh);
  for (unsigned int i = 0; i < nr + nw; i++) {
    uint64_t upv[1] = {(uint64_t)g};
    arts_guid_t u = edt_on(user, here, 1, upv, 2, latch);
    bool ro = wfirst ? (i >= nw) : (i < nr);
    arts_add_dependence(g, u, 0, ro ? DB_MODE_RO : DB_MODE_RW);
    arts_add_dependence(e, u, 1, DB_MODE_NULL);
  }
  uint64_t tpv[1] = {(uint64_t)e};
  (void)edt_on(trigger, here, 1, tpv, 0, latch);
  spin_us((long)paramv[4]);
  void *p = arts_db_create_with_guid(g, 64u, ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  if (p == NULL) {
    arts_printf("FAIL: db_create_races_acquire create gave no storage\n");
    arts_test_fail();
    return;
  }
  atomic_store_explicit((_Atomic(uint32_t) *)p, 0u, memory_order_relaxed);
  uint64_t dpv[2] = {(uint64_t)g, paramv[3]};
  arts_guid_t d = edt_on(destroyer, (unsigned int)paramv[2], 2, dpv, 1,
                         NULL_GUID);
  arts_add_dependence(latch, d, 0, DB_MODE_NULL);
}

/* runs on the block's home. paramv: g */
static void note_homed(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  unsigned int i =
      atomic_fetch_add_explicit(&g_nhomed, 1u, memory_order_relaxed);
  if (i >= MAX_HOMED) {
    arts_printf("FAIL: db_create_races_acquire registry full\n");
    arts_test_fail();
    return;
  }
  g_homed[i] = (arts_guid_t)paramv[0];
}

static void finish(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (arts_test_status() == 0) {
    arts_printf("PASS: db_create_races_acquire %u blocks\n", g_blocks);
  }
  arts_shutdown();
}

/* paramv: started_ms, done latch */
static void poller(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  unsigned int n = atomic_load_explicit(&g_nhomed, memory_order_relaxed);
  unsigned int live = 0;
  for (unsigned int i = 0; i < n && i < MAX_HOMED; i++) {
    arts_shared_ptr_t h = arts_route_table_lookup_db(g_homed[i]);
    if (arts_shared_get(h) != NULL) {
      live++;
    }
    arts_shared_release(&h);
  }
  int64_t out = LANDINGS_OUT();
  if (live == 0u && out == 0) {
    arts_event_satisfy((arts_guid_t)paramv[1], NULL_GUID);
    return;
  }
  if (now_ms() - (long)paramv[0] > SETTLE_MS) {
    arts_printf("FAIL: db_create_races_acquire rank %u: %u destroyed block(s) "
                "never torn down at their home, %lld landing(s) never ended\n",
                arts_get_current_rank(), live, (long long)out);
    arts_test_fail();
    arts_event_satisfy((arts_guid_t)paramv[1], NULL_GUID);
    return;
  }
  (void)edt_on(poller, arts_get_current_rank(), 2, paramv, 0, NULL_GUID);
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

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  int argc = (int)paramv[0];
  char **argv = (char **)paramv[1];
  unsigned int readers = 0, writers = 6;
  uint64_t wfirst = 0u;
  int a = 1;
  if (argc > a && strcmp(argv[a], "wfirst") == 0) {
    wfirst = 1u;
    a++;
  }
  if (argc > a + 1) {
    readers = (unsigned int)strtoul(argv[a], NULL, 10);
    writers = (unsigned int)strtoul(argv[a + 1], NULL, 10);
    a += 2;
  }
  if (argc > a) {
    g_blocks = (unsigned int)strtoul(argv[a], NULL, 10);
  }
  unsigned int n = arts_get_total_ranks();
  if (n < 2u) {
    arts_printf("FAIL: db_create_races_acquire needs two ranks\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  arts_guid_t all = latch_on(0u, (int32_t)g_blocks);
  arts_guid_t t = edt_on(settle, 0u, 0, NULL, 1, NULL_GUID);
  arts_add_dependence(all, t, 0, DB_MODE_NULL);
  for (unsigned int b = 0; b < g_blocks; b++) {
    unsigned int h = b % n;
    unsigned int c = (h + 1u) % n;
    unsigned int where = (b & 1u) ? c : h;
    arts_guid_t g = arts_guid_reserve(ARTS_GUID_DB, h);
    /* The acquirers, the trigger and the setup. */
    arts_guid_t latch = latch_on(where, (int32_t)(readers + writers + 2u));
    uint64_t pv[8] = {(uint64_t)g,
                      (uint64_t)latch,
                      where,
                      (uint64_t)all,
                      (uint64_t)(SPIN_US + (long)(b % 4u) * SPIN_US / 2),
                      readers,
                      writers,
                      wfirst};
    arts_guid_t noted = latch_on(c, 1);
    uint64_t npv[1] = {(uint64_t)g};
    (void)edt_on(note_homed, h, 1, npv, 0, noted);
    arts_guid_t su = edt_on(setup, c, 8, pv, 1, latch);
    arts_add_dependence(noted, su, 0, DB_MODE_NULL);
  }
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
