/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
/// @file shutdown_under_flood.c
/// @brief A run ends cleanly while one rank floods the rank that ends it.
///
/// Rank 2 makes a sink and hands its GUID to rank 0, which makes one sender
/// EDT per worker on rank 1, so rank 2 never contacts rank 1; each sender
/// satisfies PER_SENDER distinct slots of the sink, one message each.  The
/// sink has one dependence more than the senders satisfy, so it never runs.
/// FLOOD_MS after making its sink, rank 2 ends the run while rank 1 still
/// floods it.  Rank 2's post skips rank 1, which learns of the end only when
/// rank 2's connection closes, at a probe of its progress loop or of a send
/// held in the ring-full spin.  A send rank 1 makes after rank 2's endpoint
/// closed and before rank 1 learned meets a peer that has gone: the provider
/// refuses it while it reconnects, so a sender whose ring fills in that window
/// sits in the spin until a probe sees the closed connection.  Timing decides
/// whether a ring fills before rank 1 learns, so a pass does not show that the
/// spin ran.  Every send after rank 1 learns is dropped at the gate.  No
/// rank's sends wait on a peer that has gone: the run ends with status 0, and
/// no rank reports a refused inject, a transfer lost on a live run or a send
/// after its teardown began.

#include "arts.h"

#include <stdint.h>
#include <time.h>

#include "../test_failure_status.h"

#define PER_SENDER 20000u
#define FLOOD_MS 200

/// paramv = {sink, sender index}.
void sender(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
            arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t sink = (arts_guid_t)paramv[0];
  uint32_t base = (uint32_t)paramv[1] * PER_SENDER;
  for (uint32_t i = 0; i < PER_SENDER; i++) {
    arts_edt_satisfy_slot(sink, base + i, NULL_GUID, DB_MODE_NULL);
  }
}

void sink(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
          arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("  FAIL: the sink ran with a slot never satisfied\n");
  arts_test_fail();
}

void initiator(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  struct timespec ts = {.tv_sec = FLOOD_MS / 1000,
                        .tv_nsec = (long)(FLOOD_MS % 1000) * 1000000L};
  nanosleep(&ts, NULL);
  arts_printf("SHUTDOWN_UNDER_FLOOD: rank %u ends the run after %d ms\n",
              arts_get_current_rank(), FLOOD_MS);
  arts_shutdown();
}

/// Runs on rank 0: the senders on rank 1.  paramv = {sink}.
void relay(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
           arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  uint32_t senders = arts_get_workers_per_rank();
  for (uint32_t k = 0; k < senders; k++) {
    uint64_t p[2] = {paramv[0], k};
    arts_edt_create(sender, 2, p, 0, &(arts_edt_hint_t){.rank = 1});
  }
}

/// Runs on rank 2: the sink there, the relay on rank 0, then the initiator.
void setup(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
           arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int here = arts_get_current_rank();
  uint32_t senders = arts_get_workers_per_rank();
  arts_guid_t s = arts_edt_create(sink, 0, NULL, senders * PER_SENDER + 1u,
                                  &(arts_edt_hint_t){.rank = here});
  uint64_t p[1] = {(uint64_t)s};
  arts_edt_create(relay, 1, p, 0, &(arts_edt_hint_t){.rank = 0});
  arts_edt_create(initiator, 0, NULL, 0, &(arts_edt_hint_t){.rank = here});
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (arts_get_total_ranks() < 3) {
    arts_printf("  FAIL: needs at least three ranks\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  arts_edt_create(setup, 0, NULL, 0, &(arts_edt_hint_t){.rank = 2});
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
