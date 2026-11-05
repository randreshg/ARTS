/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
/// @file shutdown_sends_after_shutdown.c
/// @brief A run whose every rank is still sending when the shutdown begins
///        ends cleanly: each rank drops what it sends after its shutdown
///        point.
///
/// Every rank makes an EDT, the sink, and one sender EDT per worker of the
/// previous rank but one; each sender satisfies PER_SENDER distinct slots of
/// the sink, one message each.  The sink has one dependence more than the
/// senders satisfy, so no sink runs.  The worker left over on rank 0 runs
/// main_edt's successor as soon as the first of rank 0's senders has made
/// FIRST satisfies and satisfied the event it waits on, so the successor ends
/// the run while every rank's senders still send.  A rank's sends after its
/// shutdown point are dropped, so no rank's sends wait on a peer that has
/// gone: the run ends with status 0, and no rank reports a refused inject, a
/// transfer lost on a live run or a send after its teardown began.

#include "arts.h"

#include <stdint.h>

#include "../test_failure_status.h"

#define PER_SENDER 20000u
#define FIRST 1000u

/* One sender per worker but one, so a worker stays free for the successor. */
static uint32_t senders_per_rank(void) {
  unsigned int workers = arts_get_workers_per_rank();
  return workers > 1u ? workers - 1u : 1u;
}

/// paramv = {sink, sender index, the successor's event or NULL_GUID}.
void sender(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
            arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t sink = (arts_guid_t)paramv[0];
  uint32_t base = (uint32_t)paramv[1] * PER_SENDER;
  arts_guid_t go = (arts_guid_t)paramv[2];
  for (uint32_t i = 0; i < PER_SENDER; i++) {
    arts_edt_satisfy_slot(sink, base + i, NULL_GUID, DB_MODE_NULL);
    if (i + 1u == FIRST && go != NULL_GUID) {
      arts_event_satisfy_slot(go, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
    }
  }
}

void sink(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
          arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("  FAIL: a sink ran with a slot never satisfied\n");
  arts_test_fail();
}

/// Runs on each rank: makes its sink there, then the senders on the previous
/// rank.  paramv = {the successor's event}.
void setup(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
           arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  unsigned int rank = arts_get_current_rank();
  unsigned int ranks = arts_get_total_ranks();
  unsigned int prev = (rank + ranks - 1u) % ranks;
  uint32_t senders = senders_per_rank();
  arts_guid_t s = arts_edt_create(sink, 0, NULL, senders * PER_SENDER + 1u,
                                  &(arts_edt_hint_t){.rank = rank});
  for (uint32_t k = 0; k < senders; k++) {
    uint64_t p[3] = {
        (uint64_t)s, k,
        (uint64_t)(prev == 0 && k == 0 ? (arts_guid_t)paramv[0] : NULL_GUID)};
    arts_edt_create(sender, 3, p, 0, &(arts_edt_hint_t){.rank = prev});
  }
}

void successor(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("SENDS_AFTER_SHUTDOWN: ending the run after %u satisfies of "
              "rank 0's first sender\n",
              FIRST);
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int ranks = arts_get_total_ranks();
  if (ranks < 2) {
    arts_printf("  FAIL: needs at least two ranks\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  arts_event_hint_t eh = ARTS_EVENT_HINT_DEFAULTS;
  arts_guid_t go = arts_event_create(&eh);
  arts_guid_t succ =
      arts_edt_create(successor, 0, NULL, 1, &(arts_edt_hint_t){.rank = 0});
  arts_add_dependence(go, succ, 0, DB_MODE_NULL);
  uint64_t p[1] = {(uint64_t)go};
  for (unsigned int r = 0; r < ranks; r++) {
    arts_edt_create(setup, 1, p, 0, &(arts_edt_hint_t){.rank = r});
  }
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
