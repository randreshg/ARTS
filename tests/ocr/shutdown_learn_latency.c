/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/

/// @file shutdown_learn_latency.c
/// @brief Every rank learns of the end within a second of the rank that ends
///        the run, a rank flooded with messages included.
///
/// Four ranks.  Rank 2 creates one EDT on each other rank, so it has sent to
/// every rank when it ends the run, and its SHUTDOWN goes to each of them.
/// The EDT on rank 1 makes one sender per worker of rank 1; each sender
/// satisfies PER_SENDER distinct slots of a sink on rank 0, one message each,
/// and the sink has one dependence more than the senders satisfy, so it never
/// runs.  Rank 2 ends the run once the EDTs on ranks 0 and 3 and every sender,
/// after its first FIRST satisfies, have satisfied its latch, so rank 0 is
/// taking in the flood when the end begins.  The registration's record check
/// reads each rank's shutdown stamp from its counter file and requires the
/// four to lie within one second.  Every rank prints `RANK_PID <r> <pid>`
/// before the end, for a driver that polls the ranks' ends.

#include "arts.h"

#include <stdint.h>
#include <unistd.h>

#include "../test_failure_status.h"

#define PER_SENDER 20000u
#define FIRST 1000u
#define RANKS 4u
#define INITIATOR 2u
#define FLOODER 1u
#define FLOODED 0u

/// paramv = {the initiator's latch, sink, sender index}.
void sender(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
            arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t sink = (arts_guid_t)paramv[1];
  uint32_t base = (uint32_t)paramv[2] * PER_SENDER;
  for (uint32_t i = 0; i < PER_SENDER; i++) {
    arts_edt_satisfy_slot(sink, base + i, NULL_GUID, DB_MODE_NULL);
    if (i + 1u == FIRST) {
      arts_event_satisfy_slot((arts_guid_t)paramv[0], NULL_GUID,
                              ARTS_EVENT_LATCH_DECR_SLOT);
    }
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
  arts_printf("LEARN_LATENCY: rank %u ends the run\n", arts_get_current_rank());
  arts_shutdown();
}

static void name_pid(void) {
  arts_printf("RANK_PID %u %d\n", arts_get_current_rank(), (int)getpid());
}

/// Runs on each rank but the initiator's.  paramv = {the initiator's latch,
/// sink}.
void touch(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
           arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  name_pid();
  if (arts_get_current_rank() == FLOODER) {
    unsigned int workers = arts_get_workers_per_rank();
    for (unsigned int k = 0; k < workers; k++) {
      uint64_t p[3] = {paramv[0], paramv[1], k};
      arts_edt_create(sender, 3, p, 0, &(arts_edt_hint_t){.rank = FLOODER});
    }
    return;
  }
  arts_event_satisfy_slot((arts_guid_t)paramv[0], NULL_GUID,
                          ARTS_EVENT_LATCH_DECR_SLOT);
}

/// Runs on the initiator's rank.  paramv = {sink}.
void start(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
           arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  name_pid();
  arts_event_hint_t lh = ARTS_EVENT_HINT_LATCH(
      (int32_t)(RANKS - 2u + arts_get_workers_per_rank()));
  arts_guid_t touched = arts_event_create(&lh);
  arts_guid_t end = arts_edt_create(initiator, 0, NULL, 1,
                                    &(arts_edt_hint_t){.rank = INITIATOR});
  arts_add_dependence(touched, end, 0, DB_MODE_NULL);
  uint64_t p[2] = {(uint64_t)touched, paramv[0]};
  for (unsigned int r = 0; r < RANKS; r++) {
    if (r != INITIATOR) {
      arts_edt_create(touch, 2, p, 0, &(arts_edt_hint_t){.rank = r});
    }
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (arts_get_total_ranks() != RANKS) {
    arts_printf("  FAIL: needs exactly %u ranks, has %u\n", RANKS,
                arts_get_total_ranks());
    arts_test_fail();
    arts_shutdown();
    return;
  }
  uint32_t slots = arts_get_workers_per_rank() * PER_SENDER + 1u;
  arts_guid_t s = arts_edt_create(sink, 0, NULL, slots,
                                  &(arts_edt_hint_t){.rank = FLOODED});
  uint64_t p[1] = {(uint64_t)s};
  arts_edt_create(start, 1, p, 0, &(arts_edt_hint_t){.rank = INITIATOR});
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
