/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
/// @file signals_sigterm_status.c
/// @brief A single SIGTERM (or SIGINT) to the process ends the rank with
///        128 + the signal's number.
///
/// The signal watcher is the one thread that takes the term signals (every
/// other thread blocks them); its first signal ends the rank with origin
/// SIGNAL after one line naming the signal.  An EDT sends one
/// process-directed signal (SIGTERM, or SIGINT when argv[1] is "int"): a
/// thread-directed one would stay pending on this worker, which blocks the
/// term set.  The run's status is 143 (130 for SIGINT); a watcher that never
/// takes the signal leaves the run to the ctest TIMEOUT.
///
/// Single-node, every configuration (signals sit above the coherence layer).

#include "arts.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void raise_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  int sig = (int)paramv[0];
  arts_printf("  sending %s to this process\n",
              sig == SIGINT ? "SIGINT" : "SIGTERM");
  kill(getpid(), sig);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  int argc = (int)paramv[0];
  char **argv = (char **)paramv[1];
  int sig = SIGTERM;
  if (argc > 1 && strcmp(argv[1], "int") == 0) {
    sig = SIGINT;
  }
  uint64_t pv[1] = {(uint64_t)sig};
  arts_edt_create(raise_edt, 1, pv, 0, &(arts_edt_hint_t){.rank = 0});
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
