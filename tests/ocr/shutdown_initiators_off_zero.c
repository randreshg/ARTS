/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
/// @file shutdown_initiators_off_zero.c
/// @brief Two ranks other than rank 0 end the run at once, and rank 0 never
///        does: every rank ends with status 0.
///
/// An EDT on rank 1 and one on rank 2 each call arts_shutdown.  Each of the
/// two posts SHUTDOWN to the peers it contacted; every other rank, rank 0
/// included, learns of the end from whichever post or closed connection
/// reaches it first, and either one may also reach the other initiator
/// before its own call.

#include "arts.h"

#include <stdint.h>

#include "../test_failure_status.h"

void initiator(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("  rank %u ends the run\n", arts_get_current_rank());
  arts_shutdown();
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
  for (unsigned int r = 1; r <= 2; r++) {
    arts_edt_create(initiator, 0, NULL, 0, &(arts_edt_hint_t){.rank = r});
  }
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
