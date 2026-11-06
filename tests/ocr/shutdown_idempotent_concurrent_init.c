/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
/// @file shutdown_idempotent_concurrent_init.c
/// @brief Two ranks initiate shutdown concurrently; each rank's end is taken
///        once, by one compare-exchange of its state word, so each posts at
///        most once and the cluster ends cleanly.
///
/// When more than one rank calls arts_shutdown at the same time, each call
/// enters arts_exit(0, APP), which takes the rank's state word by a
/// compare-exchange.  Locally exactly one caller wins and posts; every later
/// entry, a peer's SHUTDOWN reaped by a progress thread included, finds the
/// word taken and posts nothing.  The required property: with multiple ranks
/// initiating at once, the cluster ends cleanly — no post storm, no
/// deadlock.
///
/// Each of the first two ranks schedules a LOCAL shutdown initiator EDT, so two
/// independent initiations race across the cluster.  Each initiator prints
/// its token before it calls arts_shutdown, and the spawner's status is the
/// highest of every rank's (ctest TIMEOUT reaps a storm/deadlock
/// regression).
///
/// Multinode (2n_9w_1p/4n_4w_1p).  A run on one rank fails.  Every
/// configuration (shutdown sits above the coherence layer).

#include "arts.h"

#include "../test_failure_status.h"

void local_initiator_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                         arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("  rank %u initiating shutdown concurrently\n",
              arts_get_current_rank());
  arts_printf("[rank %u] IDEMPOTENT_INIT_EXIT\n", arts_get_current_rank());
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== shutdown_idempotent_concurrent_init ===\n");

  unsigned int ranks = arts_get_total_ranks();
  if (ranks < 2) {
    arts_printf("FAIL shutdown_idempotent_concurrent_init: requires 2+ ranks "
                "(got %u)\n",
                ranks);
    arts_test_fail();
    arts_shutdown();
    return;
  }

  arts_printf("PASS shutdown_idempotent_concurrent_init: %u ranks, ranks 0 and "
              "1 initiating concurrently\n",
              ranks);
  /* Two independent initiators (rank 0 and rank 1) race into shutdown. */
  arts_edt_create(local_initiator_edt, 0, NULL, 0,
                  &(arts_edt_hint_t){.rank = 0});
  arts_edt_create(local_initiator_edt, 0, NULL, 0,
                  &(arts_edt_hint_t){.rank = 1});
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
