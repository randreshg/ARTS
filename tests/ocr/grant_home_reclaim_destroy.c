/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* grant_home_reclaim_destroy — the home takes a block's write right back from
 * another rank, and the writer it serves destroys the block in its own body.
 *
 * Per block g, homed on rank 0 and created there without a hold: a writer on
 * rank 1 acquires g, stamps it and completes.  A writer on rank 0, made
 * runnable by that completion, then acquires g, and the write right comes
 * back to the home the way the cell moves it: a home that revokes its holder
 * installs rank 1's ship and runs its own CONFIRM inline -- under write-back
 * the CONFIRM_ACK that wakes the writer too -- and a home the right is
 * handed back to serves its own request from its queue.  The writer checks
 * rank 1's stamp, writes its own and destroys g, which is legal once its hold
 * is released and nothing else holds or awaits g.  A round's blocks are
 * destroyed before the next round starts.  The two write acquisitions of g
 * are ordered by the first one's completion, so every write is ordered by
 * happens-before (the program is valid under DB-WRF too).
 *
 * What the home does after it woke the writer runs on a descriptor the
 * writer's destroy and completion may already have given up; a frame that
 * holds no count of its own on it reads freed memory, which the descriptor
 * quarantine and the sanitizers report.
 *
 * Arguments: [rounds [blocks]] (8, 32).  Needs two ranks.  The verdict is the
 * exit status, the PASS line, and a quiescent shutdown (no QUIESCENCE-DEBUG
 * marker).
 */

#include <stdint.h>
#include <stdlib.h>

#include "../test_failure_status.h"
#include "arts.h"

#define STAMP(b) (0x5EC0000u + (uint32_t)(b))

static unsigned int g_rounds = 8, g_blocks = 32;

static arts_guid_t latch_on(unsigned int rank, int32_t n) {
  arts_event_hint_t h = ARTS_EVENT_HINT_LATCH(n);
  h.rank = rank;
  return arts_event_create(&h);
}

static arts_guid_t edt_on(arts_edt_t fn, unsigned int rank, uint32_t paramc,
                          const uint64_t *paramv, uint32_t depc,
                          arts_guid_t completion_event) {
  arts_edt_hint_t h = ARTS_EDT_HINT_DEFAULTS;
  h.rank = rank;
  h.completion_event = completion_event;
  return arts_edt_create(fn, paramc, paramv, depc, &h);
}

/* rank 1.  paramv: block index.  depv[0]: g, RW. */
static void remote_writer(uint32_t paramc, const uint64_t *paramv,
                          uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint32_t *v = (uint32_t *)depv[0].ptr;
  if (v == NULL) {
    arts_printf("FAIL: grant_home_reclaim_destroy rank 1 got no storage\n");
    arts_test_fail();
    return;
  }
  *v = STAMP(paramv[0]);
}

/* rank 0.  paramv: g, block index, round latch.  depv[0]: rank 1's
 * completion; depv[1]: g, RW. */
static void home_writer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  arts_guid_t g = (arts_guid_t)paramv[0];
  uint32_t *v = (uint32_t *)depv[1].ptr;
  if (v == NULL || *v != STAMP(paramv[1])) {
    arts_printf("FAIL: grant_home_reclaim_destroy block %u read 0x%x at the "
                "home, want 0x%x\n",
                (unsigned int)paramv[1], v ? *v : 0u,
                (unsigned int)STAMP(paramv[1]));
    arts_test_fail();
  } else {
    *v = STAMP(paramv[1]) + 1u;
  }
  arts_db_destroy(g);
  arts_event_satisfy((arts_guid_t)paramv[2], NULL_GUID);
}

static void terminal(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (arts_test_status() == 0) {
    arts_printf("PASS: grant_home_reclaim_destroy %u rounds x %u blocks\n",
                g_rounds, g_blocks);
  }
  arts_shutdown();
}

/* rank 0.  paramv: round, rounds, blocks */
static void round_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  unsigned int round = (unsigned int)paramv[0];
  unsigned int rounds = (unsigned int)paramv[1];
  unsigned int blocks = (unsigned int)paramv[2];
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
    void *p = NULL;
    arts_db_hint_t dh = ARTS_DB_HINT_DEFAULTS;
    dh.rank = 0u;
    arts_guid_t g =
        arts_db_create(&p, 64, ARTS_DB, ARTS_DB_PROP_NO_ACQUIRE, &dh);
    arts_guid_t written = arts_event_create(&ARTS_EVENT_HINT_DEFAULTS);
    uint64_t hpv[3] = {(uint64_t)g, b, (uint64_t)all};
    arts_guid_t hw = edt_on(home_writer, 0u, 3, hpv, 2, NULL_GUID);
    arts_add_dependence(written, hw, 0, DB_MODE_NULL);
    arts_add_dependence(g, hw, 1, DB_MODE_RW);
    uint64_t rpv[1] = {b};
    arts_guid_t rw = edt_on(remote_writer, 1u, 1, rpv, 1, written);
    arts_add_dependence(g, rw, 0, DB_MODE_RW);
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
    arts_printf("FAIL: grant_home_reclaim_destroy needs two ranks\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  uint64_t pv[3] = {0u, g_rounds, g_blocks};
  (void)edt_on(round_edt, 0u, 3, pv, 0, NULL_GUID);
}

int main(int argc, char **argv) {
  return arts_rt(argc, argv);
}
