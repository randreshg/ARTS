/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
/// @file shutdown_peer_stopped.c
/// @brief A run whose rank 0 is stopped while rank 1 floods it still ends
///        once its last rank is signalled, and rank 0 ends once continued.
///
/// Once every rank has printed `RANK_PID <r> <pid>`, rank 0 makes a sink EDT
/// whose slots no sender fills, and each worker of rank 1 satisfies a fixed
/// PER_SENDER of them; rank 1 prints FLOODING as the senders start.  The
/// test's driver stops rank 0 at FLOODING, so rank 1's sends back up into its
/// rings, then sends SIGTERM to the last rank: that rank ends with 143 and
/// every rank but 0 learns of the end and ends with 0, rank 1's senders
/// dropping their sends at its gate.  Once those ranks are gone the driver
/// continues rank 0, which learns of the end and ends with 0.

#include "arts.h"

#include <stdint.h>
#include <unistd.h>

#include "../test_failure_status.h"

#define PER_SENDER 20000u

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

/// Runs on rank 1: one sender per worker.  paramv = {sink}.
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
  arts_printf("FLOODING\n");
}

/// Runs on rank 0 once every rank named its pid: the sink there, then the
/// relay on rank 1.
void ready_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  uint32_t senders = arts_get_workers_per_rank();
  arts_guid_t s = arts_edt_create(sink, 0, NULL, senders * PER_SENDER + 1u,
                                  &(arts_edt_hint_t){.rank = 0});
  uint64_t p[1] = {(uint64_t)s};
  arts_edt_create(relay, 1, p, 0, &(arts_edt_hint_t){.rank = 1});
}

void pid_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
             arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_printf("RANK_PID %u %d\n", arts_get_current_rank(), (int)getpid());
  arts_event_satisfy_slot((arts_guid_t)paramv[0], NULL_GUID,
                          ARTS_EVENT_LATCH_DECR_SLOT);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int total = arts_get_total_ranks();
  if (total < 2) {
    arts_printf("  FAIL: needs at least two ranks\n");
    arts_test_fail();
    arts_shutdown();
  }
  arts_guid_t all = arts_event_create(&ARTS_EVENT_HINT_LATCH(total));
  arts_guid_t ready =
      arts_edt_create(ready_edt, 0, NULL, 1, &(arts_edt_hint_t){.rank = 0});
  arts_add_dependence(all, ready, 0, DB_MODE_NULL);
  for (unsigned int r = 0; r < total; r++) {
    uint64_t latch = (uint64_t)all;
    arts_edt_create(pid_edt, 1, &latch, 0, &(arts_edt_hint_t){.rank = r});
  }
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
