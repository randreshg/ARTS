/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
/// @file shutdown_abort_eof.c
/// @brief A non-master rank arts_abort()s; the surviving ranks detect its
///        departure and exit cleanly.
///
/// arts_abort exits at once with no broadcast or drain, so the other ranks
/// learn of the departure only through the liveness sentinel: the departed
/// rank's connection closes, and the probe enters the same idempotent shutdown
/// an inbound shutdown message does.  The property under test: a non-master
/// abort makes every surviving rank detect the departure and exit cleanly (no
/// hang).
///
/// An EDT placed on a NON-master rank (the last rank) calls arts_abort with a
/// sentinel code.  That rank exits with the code, which the registration names
/// as the exit it provokes; the master's arts_rt reports the aborted rank,
/// which main deliberately ignores, and the surviving peers detect the
/// departure and return 0.  Each surviving rank prints a clean token.  ctest
/// TIMEOUT reaps a departure-detection hang regression.
///
/// runtime_multinode (2n_9w_1p/4n_4w_1p/2n_8w_2p).  Self-skips cleanly to a single-node
/// clean shutdown when only one rank is present.
/// all configs (shutdown sits above the coherence layer).

#include "arts.h"

#include <stdio.h>

#define ABORT_CODE 37u

void abort_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("  non-master rank %u aborting (code %u)\n",
              arts_get_current_rank(), ABORT_CODE);
  /* Hard exit: no broadcast/drain; peers discover via socket EOF. */
  arts_abort((uint8_t)ABORT_CODE);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== shutdown_abort_eof ===\n");

  unsigned int ranks = arts_get_total_ranks();
  if (ranks < 2) {
    arts_printf("SKIP shutdown_abort_eof: requires 2+ ranks (got %u)\n", ranks);
    arts_shutdown();
    return;
  }

  unsigned int origin = ranks - 1; /* a non-master rank */
  arts_printf(
      "PASS shutdown_abort_eof: %u ranks, aborting non-master rank %u\n", ranks,
      origin);
  arts_edt_create(abort_edt, 0, NULL, 0, &(arts_edt_hint_t){.rank = origin});
}

int main(int argc, char **argv) {
  arts_rt(argc, argv);
  /* Surviving ranks reach here after detecting EOF and exiting cleanly. */
  printf("[rank %u] ABORT_EOF_EXIT\n", arts_get_current_rank());
  return 0;
}
