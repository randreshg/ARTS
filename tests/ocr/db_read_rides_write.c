/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* db_read_rides_write — a write turn that reaches a rank first serves the
 * readers whose read request is still out.
 *
 * Per block g, created on its home H: on a rank C != H a reader acquires g --
 * C's read request leaves -- and then a writer acquires it -- C's write
 * request leaves at once.  Run with ARTS_REQUEST_DELAY_MS=<n>:ro, which holds
 * every read request n ms at its sender and no write request, so the write
 * turn answers first: it serves the reader too, and the run ends long before
 * n ms.  A reader left waiting for its own read answer waits n ms on every
 * block.  The verdict is the run's time budget (the test's TIMEOUT, below n),
 * the exit status, the PASS line and no QUIESCENCE-DEBUG marker.  The reader
 * is unordered with the writer, so it may see the creator's 7 or the
 * writer's 9.
 *
 * Arguments: [blocks] (16).  Needs two ranks.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#include "arts.h"
#include "../test_failure_status.h"

static unsigned int g_blocks = 16;

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

static void user(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  if (depv[0].ptr == NULL) {
    arts_printf("FAIL: db_read_rides_write user got no storage\n");
    arts_test_fail();
    return;
  }
  _Atomic(uint32_t) *w = (_Atomic(uint32_t) *)depv[0].ptr;
  if (depv[0].mode == DB_MODE_RW) {
    atomic_store_explicit(w, 9u, memory_order_relaxed);
  } else {
    uint32_t v = atomic_load_explicit(w, memory_order_relaxed);
    if (v != 7u && v != 9u) {
      arts_printf("FAIL: db_read_rides_write reader saw %u\n", v);
      arts_test_fail();
    }
  }
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

/* runs on C. paramv: g, all */
static void pair(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t g = (arts_guid_t)paramv[0];
  unsigned int here = arts_get_current_rank();
  arts_guid_t latch = latch_on(here, 2);
  uint64_t upv[1] = {(uint64_t)g};
  arts_guid_t r = edt_on(user, here, 1, upv, 1, latch);
  arts_guid_t w = edt_on(user, here, 1, upv, 1, latch);
  /* The reader's acquire runs inside this call, so its read request leaves
   * before the writer's write request. */
  arts_add_dependence(g, r, 0, DB_MODE_RO);
  arts_add_dependence(g, w, 0, DB_MODE_RW);
  uint64_t dpv[2] = {(uint64_t)g, paramv[1]};
  arts_guid_t d = edt_on(destroyer, here, 2, dpv, 1, NULL_GUID);
  arts_add_dependence(latch, d, 0, DB_MODE_NULL);
}

/* runs on H. paramv: c, all */
static void maker(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  void *p = NULL;
  arts_guid_t g = arts_db_create(&p, 64, ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  atomic_store_explicit((_Atomic(uint32_t) *)p, 7u, memory_order_relaxed);
  arts_db_release(g, DB_MODE_RW);
  uint64_t ppv[2] = {(uint64_t)g, paramv[1]};
  (void)edt_on(pair, (unsigned int)paramv[0], 2, ppv, 0, NULL_GUID);
}

static void terminal(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (arts_test_status() == 0) {
    arts_printf("PASS: db_read_rides_write %u blocks\n", g_blocks);
  }
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  int argc = (int)paramv[0];
  char **argv = (char **)paramv[1];
  if (argc > 1) {
    g_blocks = (unsigned int)strtoul(argv[1], NULL, 10);
  }
  unsigned int n = arts_get_total_ranks();
  if (n < 2u) {
    arts_printf("FAIL: db_read_rides_write needs two ranks\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  arts_guid_t all = latch_on(0u, (int32_t)g_blocks);
  arts_guid_t t = edt_on(terminal, 0u, 0, NULL, 1, NULL_GUID);
  arts_add_dependence(all, t, 0, DB_MODE_NULL);
  for (unsigned int b = 0; b < g_blocks; b++) {
    unsigned int h = b % n;
    unsigned int c = (h + 1u) % n;
    uint64_t mpv[2] = {c, (uint64_t)all};
    (void)edt_on(maker, h, 2, mpv, 0, NULL_GUID);
  }
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
