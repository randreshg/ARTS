/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* db_read_dismissed_before_grant — a read request its rank's write request
 * covers is dismissed while that write request still waits at the home, so
 * the dismiss overtakes the grant and the readers parked on the rank ride the
 * write request.
 *
 * Per block g, created on its home H: a holder EDT on rank D takes the write
 * turn and keeps it for the hold (400 ms).  As it starts it makes a pair EDT
 * on rank C (C, D, H distinct) that acquires g three times: reader R1 (C's
 * read request leaves), writer W (C's write request leaves at once and waits
 * behind D's turn) and reader R2 (it parks on C behind W).  Run with
 * ARTS_REQUEST_DELAY_MS=<n>:ro, n below the hold, and the argument `strict`:
 * the read request reaches H after W declared it and while D still holds the
 * turn, H dismisses it, the dismiss reaches C before the grant, and R1 and R2
 * ride W, so every reader must see a value a writer stored (9 by D, 11 by W),
 * never the creator's 7.  Without the hook R1's request reaches H first and
 * is served as a read; R1 and R2 are unordered with D's writer, and where a
 * read answer serves every reader parked on the rank R2 can be served by it,
 * so 7 is legal for both.  The verdict: exit status, PASS line, no FAIL line
 * and no QUIESCENCE-DEBUG marker.
 *
 * Arguments: [strict] [blocks] (8).  Needs three ranks.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "arts.h"
#include "../test_failure_status.h"

#define HOLD_NS 400000000ull

static unsigned int g_blocks = 8;

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

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* C's acquirers. paramv: whether the creator's 7 is legal */
static void user(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  if (depv[0].ptr == NULL) {
    arts_printf("FAIL: db_read_dismissed_before_grant user got no storage\n");
    arts_test_fail();
    return;
  }
  _Atomic(uint32_t) *w = (_Atomic(uint32_t) *)depv[0].ptr;
  if (depv[0].mode == DB_MODE_RW) {
    atomic_store_explicit(w, 11u, memory_order_relaxed);
  } else {
    uint32_t v = atomic_load_explicit(w, memory_order_relaxed);
    if (v != 9u && v != 11u && !(v == 7u && paramv[0] != 0u)) {
      arts_printf("FAIL: db_read_dismissed_before_grant reader saw %u\n", v);
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

/* runs on C. paramv: g, latch, strict */
static void pair(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t g = (arts_guid_t)paramv[0];
  arts_guid_t latch = (arts_guid_t)paramv[1];
  unsigned int here = arts_get_current_rank();
  uint64_t loose[1] = {paramv[2] ? 0u : 1u};
  arts_guid_t r1 = edt_on(user, here, 1, loose, 1, latch);
  arts_guid_t w = edt_on(user, here, 1, loose, 1, latch);
  arts_guid_t r2 = edt_on(user, here, 1, loose, 1, latch);
  /* Each acquire runs inside its call: R1's read request leaves, then W's
   * write request, then R2 parks behind W. */
  arts_add_dependence(g, r1, 0, DB_MODE_RO);
  arts_add_dependence(g, w, 0, DB_MODE_RW);
  arts_add_dependence(g, r2, 0, DB_MODE_RO);
}

/* runs on D under the write turn. paramv: g, c, latch, strict */
static void holder(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  if (depv[0].ptr == NULL || depv[0].mode != DB_MODE_RW) {
    arts_printf("FAIL: db_read_dismissed_before_grant holder got no write "
                "turn\n");
    arts_test_fail();
    return;
  }
  uint64_t ppv[3] = {paramv[0], paramv[2], paramv[3]};
  (void)edt_on(pair, (unsigned int)paramv[1], 3, ppv, 0, NULL_GUID);
  uint64_t until = now_ns() + HOLD_NS;
  while (now_ns() < until) {
  }
  atomic_store_explicit((_Atomic(uint32_t) *)depv[0].ptr, 9u,
                        memory_order_relaxed);
}

/* runs on H. paramv: d, c, all, strict */
static void maker(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  void *p = NULL;
  arts_guid_t g = arts_db_create(&p, 64, ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  atomic_store_explicit((_Atomic(uint32_t) *)p, 7u, memory_order_relaxed);
  arts_db_release(g, DB_MODE_RW);
  unsigned int c = (unsigned int)paramv[1];
  arts_guid_t latch = latch_on(c, 4);
  uint64_t dpv[2] = {(uint64_t)g, paramv[2]};
  arts_guid_t d = edt_on(destroyer, c, 2, dpv, 1, NULL_GUID);
  arts_add_dependence(latch, d, 0, DB_MODE_NULL);
  uint64_t hpv[4] = {(uint64_t)g, c, (uint64_t)latch, paramv[3]};
  arts_guid_t h = edt_on(holder, (unsigned int)paramv[0], 4, hpv, 1, latch);
  arts_add_dependence(g, h, 0, DB_MODE_RW);
}

static void terminal(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (arts_test_status() == 0) {
    arts_printf("PASS: db_read_dismissed_before_grant %u blocks\n", g_blocks);
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
  uint64_t strict = 0u;
  int a = 1;
  if (argc > a && strcmp(argv[a], "strict") == 0) {
    strict = 1u;
    a++;
  }
  if (argc > a) {
    g_blocks = (unsigned int)strtoul(argv[a], NULL, 10);
  }
  unsigned int n = arts_get_total_ranks();
  if (n < 3u) {
    arts_printf("FAIL: db_read_dismissed_before_grant needs three ranks\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  arts_guid_t all = latch_on(0u, (int32_t)g_blocks);
  arts_guid_t t = edt_on(terminal, 0u, 0, NULL, 1, NULL_GUID);
  arts_add_dependence(all, t, 0, DB_MODE_NULL);
  for (unsigned int i = 0; i < g_blocks; i++) {
    unsigned int h = i % n;
    unsigned int d = (h + 1u) % n;
    unsigned int c = (h + 2u) % n;
    uint64_t mpv[4] = {d, c, (uint64_t)all, strict};
    (void)edt_on(maker, h, 4, mpv, 0, NULL_GUID);
  }
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
