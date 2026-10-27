/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* db_destroy_reaches_cachers — a block's destroy retires the block on every
 * rank that holds a descriptor of it, whatever right that rank kept.
 *
 * Rounds of blocks, each homed on a rank H, are written in turn by every other
 * rank (each write turn ordered after the previous one) and then read by every
 * other rank at once, so the last writer keeps the write right and the others
 * keep a read copy or nothing; one block in four has size zero, a block whose
 * caches the home's version bookkeeping never records.  H destroys each block
 * once every hold is released.  After the last round every rank polls its
 * route table for each block it ever held until none is installed and, where
 * the build counts rendezvous landings (ARTS_DEBUG_LANDING_BALANCE), until it
 * has none outstanding; a rank still holding one after 20 s fails the run.  A
 * descriptor a destroy did not reach stays installed whatever it holds, so no
 * label is reused to see it.  Each reader also checks that it sees every
 * write.  Every write turn is ordered, so the program is DB-WRF-valid.
 *
 * Arguments: [rounds [blocks]] (8, 24).  Needs two ranks.
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
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

static unsigned int g_rounds = 8, g_blocks = 24;

/* The blocks this rank held a descriptor of, recorded by the EDT that ran
 * here with the block. */
#define MAX_SEEN 65536u
static arts_guid_t g_seen[MAX_SEEN];
static _Atomic(unsigned int) g_nseen;

static void note_seen(arts_guid_t g) {
  unsigned int i =
      atomic_fetch_add_explicit(&g_nseen, 1u, memory_order_relaxed);
  if (i >= MAX_SEEN) {
    arts_printf("FAIL: db_destroy_reaches_cachers registry full\n");
    arts_test_fail();
    return;
  }
  g_seen[i] = g;
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

/* paramv: g, size */
static void user(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  note_seen((arts_guid_t)paramv[0]);
  if (paramv[1] == 0u) {
    return;
  }
  uint32_t *p = (uint32_t *)depv[0].ptr;
  if (p == NULL) {
    arts_printf("FAIL: db_destroy_reaches_cachers user got no storage\n");
    arts_test_fail();
    return;
  }
  if (depv[0].mode == DB_MODE_RW) {
    *p += 1u;
  } else if (*p != arts_get_total_ranks() - 1u) {
    /* Every other rank wrote once before any reader runs. */
    arts_printf("FAIL: db_destroy_reaches_cachers read %u, expected %u\n", *p,
                arts_get_total_ranks() - 1u);
    arts_test_fail();
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

/* runs on H. paramv: round latch, size */
static void maker(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  uint64_t size = paramv[1];
  void *p = NULL;
  arts_guid_t g = arts_db_create(&p, size, ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  if (size != 0u) {
    *(uint32_t *)p = 0u;
  }
  arts_db_release(g, DB_MODE_RW);
  note_seen(g);
  unsigned int n = arts_get_total_ranks();
  unsigned int here = arts_get_current_rank();
  uint64_t upv[2] = {(uint64_t)g, size};

  arts_guid_t readers = latch_on(here, (int32_t)(n - 1u));
  uint64_t dpv[2] = {(uint64_t)g, paramv[0]};
  arts_guid_t d = edt_on(destroyer, here, 2, dpv, 1, NULL_GUID);
  arts_add_dependence(readers, d, 0, DB_MODE_NULL);

  arts_guid_t prev = NULL_GUID;
  for (unsigned int k = 1; k < n; k++) {
    unsigned int r = (here + k) % n;
    arts_guid_t done = latch_on(r, 1);
    arts_guid_t w = edt_on(user, r, 2, upv, prev == NULL_GUID ? 1u : 2u, done);
    arts_add_dependence(g, w, 0, DB_MODE_RW);
    if (prev != NULL_GUID) {
      arts_add_dependence(prev, w, 1, DB_MODE_NULL);
    }
    prev = done;
  }
  for (unsigned int k = 1; k < n; k++) {
    unsigned int r = (here + k) % n;
    arts_guid_t rd = edt_on(user, r, 2, upv, 2, readers);
    arts_add_dependence(g, rd, 0, DB_MODE_RO);
    arts_add_dependence(prev, rd, 1, DB_MODE_NULL);
  }
}

/* paramv: started_ms, done latch */
static void poller(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  unsigned int n = atomic_load_explicit(&g_nseen, memory_order_relaxed);
  unsigned int live = 0;
  arts_guid_t first_live = NULL_GUID;
  for (unsigned int i = 0; i < n && i < MAX_SEEN; i++) {
    arts_shared_ptr_t h = arts_route_table_lookup_db(g_seen[i]);
    if (arts_shared_get(h) != NULL) {
      if (live++ == 0u) {
        first_live = g_seen[i];
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
    arts_printf("FAIL: db_destroy_reaches_cachers rank %u: %u destroyed "
                "block(s) still installed (first %lu), %lld landing(s) never "
                "ended\n",
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
    arts_printf("PASS: db_destroy_reaches_cachers %u rounds x %u blocks\n",
                g_rounds, g_blocks);
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
    uint64_t mpv[2] = {(uint64_t)all, (b % 4u == 0u) ? 0u : 64u};
    (void)edt_on(maker, h, 2, mpv, 0, NULL_GUID);
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  int argc = (int)paramv[0];
  char **argv = (char **)paramv[1];
  if (argc > 1) {
    g_rounds = (unsigned int)strtoul(argv[1], NULL, 10);
  }
  if (argc > 2) {
    g_blocks = (unsigned int)strtoul(argv[2], NULL, 10);
  }
  if (arts_get_total_ranks() < 2u) {
    arts_printf("FAIL: db_destroy_reaches_cachers needs two ranks\n");
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
