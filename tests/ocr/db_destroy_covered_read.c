/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* db_destroy_covered_read — a read request whose readers its own rank's write
 * turn served may still be travelling when the program destroys the block,
 * and the shutdown is left with nothing parked.
 *
 * Per block g, created on its home H: on a rank C != H a reader acquires g
 * (C asks H for a read turn) and a writer acquires it right after (C asks for
 * a write turn at once, its request naming the read request still out).  When
 * the write turn reaches C first it serves the reader too, and the read
 * request is still on its way.  Both release, then a destroyer on H or on C
 * destroys g, which is legal once every hold is released.  A round's blocks
 * are all destroyed before the next round starts, so a request still in
 * flight from one round reaches its home while the run goes on.  Run with
 * ARTS_REQUEST_DELAY_MS, requests overtake each other on the wire.
 *
 * Arguments: [rounds [blocks]] (8, 32).  Needs two ranks.  The verdict is the
 * exit status, the PASS line, and a quiescent shutdown (no QUIESCENCE-DEBUG
 * marker): a request parked at a destroyed home is a non-quiescent shutdown.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#include "arts.h"
#include "../test_failure_status.h"

static unsigned int g_rounds = 8, g_blocks = 32;

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
    arts_printf("FAIL: db_destroy_covered_read user got no storage\n");
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

/* runs on C. paramv: g, destroyer rank, round latch */
static void pair(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t g = (arts_guid_t)paramv[0];
  unsigned int where = (unsigned int)paramv[1];
  unsigned int here = arts_get_current_rank();
  arts_guid_t latch = latch_on(where, 2);
  uint64_t upv[1] = {(uint64_t)g};
  arts_guid_t r = edt_on(user, here, 1, upv, 1, latch);
  arts_guid_t w = edt_on(user, here, 1, upv, 1, latch);
  /* Each acquire runs inside its call: the read request leaves first. */
  arts_add_dependence(g, r, 0, DB_MODE_RO);
  arts_add_dependence(g, w, 0, DB_MODE_RW);
  uint64_t dpv[2] = {(uint64_t)g, paramv[2]};
  arts_guid_t d = edt_on(destroyer, where, 2, dpv, 1, NULL_GUID);
  arts_add_dependence(latch, d, 0, DB_MODE_NULL);
}

/* runs on H. paramv: c, destroyer rank, round latch */
static void maker(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  void *p = NULL;
  arts_guid_t g = arts_db_create(&p, 64, ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  atomic_store_explicit((_Atomic(uint32_t) *)p, 7u, memory_order_relaxed);
  arts_db_release(g, DB_MODE_RW);
  uint64_t ppv[3] = {(uint64_t)g, paramv[1], paramv[2]};
  (void)edt_on(pair, (unsigned int)paramv[0], 3, ppv, 0, NULL_GUID);
}

static void terminal(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (arts_test_status() == 0) {
    arts_printf("PASS: db_destroy_covered_read %u rounds x %u blocks\n",
                g_rounds, g_blocks);
  }
  arts_shutdown();
}

/* paramv: round, rounds, blocks */
static void round_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  unsigned int round = (unsigned int)paramv[0];
  unsigned int rounds = (unsigned int)paramv[1];
  unsigned int blocks = (unsigned int)paramv[2];
  unsigned int n = arts_get_total_ranks();
  arts_guid_t all = latch_on(0u, (int32_t)blocks);
  if (round + 1u < rounds) {
    uint64_t npv[3] = {round + 1u, rounds, blocks};
    arts_guid_t nx = edt_on(round_edt, 0u, 3, npv, 1, NULL_GUID);
    arts_add_dependence(all, nx, 0, DB_MODE_NULL);
  } else {
    arts_guid_t t = edt_on(terminal, 0u, 0, NULL, 1, NULL_GUID);
    arts_add_dependence(all, t, 0, DB_MODE_NULL);
  }
  for (unsigned int b = 0; b < blocks; b++) {
    unsigned int h = (b + round) % n;
    unsigned int c = (h + 1u + (b / n) % (n - 1u)) % n;
    unsigned int where = ((b + round) & 1u) ? c : h;
    uint64_t mpv[3] = {c, where, (uint64_t)all};
    (void)edt_on(maker, h, 3, mpv, 0, NULL_GUID);
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
    arts_printf("FAIL: db_destroy_covered_read needs two ranks\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  uint64_t pv[3] = {0u, g_rounds, g_blocks};
  (void)edt_on(round_edt, 0u, 3, pv, 0, NULL_GUID);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
