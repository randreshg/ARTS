/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
/// @file shutdown_abort_status.c
/// @brief A non-master rank arts_abort()s: its code is the run's status, and
///        every other rank ends with 0.
///
/// The aborting rank posts SHUTDOWN to the peers it contacted and ends with
/// its code; every other rank learns of the end from the post or from the
/// aborted rank's closed connection and ends with 0, which is no failure.
/// The spawner's status is the highest a rank ended with, so the run's status
/// is the code, and a rank that ended with any other non-zero status fails
/// the run.  A rank that never learns of the end leaves the run to the
/// spawner's end grace, whose line fails the test.
///
/// Multinode (2n_9w_1p/4n_4w_1p/2n_8w_2p).  Every configuration (shutdown
/// sits above the coherence layer).

#include "arts.h"

#define ABORT_CODE 37u

void abort_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("  non-master rank %u aborting (code %u)\n",
              arts_get_current_rank(), ABORT_CODE);
  arts_abort((uint8_t)ABORT_CODE);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int ranks = arts_get_total_ranks();
  unsigned int origin = ranks - 1; /* a non-master rank */
  arts_printf("ABORT_STATUS: %u ranks, aborting non-master rank %u\n", ranks,
              origin);
  arts_edt_create(abort_edt, 0, NULL, 0, &(arts_edt_hint_t){.rank = origin});
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
