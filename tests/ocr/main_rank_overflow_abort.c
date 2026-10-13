/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
/// @file main_rank_overflow_abort.c
/// @brief arts_rt aborts (exit 1) when the configured rank count exceeds the
///        GUID layout limit (ARTS_GUID_RANK_MASK - 1 == 16382).
///
/// main.c sets arts_global_rank_count = config.table_length and, BEFORE any
/// transport setup or process launch, guards it:
///   if (rank_count > ARTS_GUID_RANK_MASK - 1U) ARTS_ERROR(...);
/// ARTS_ERROR calls arts_abort(1) (process exits 1).  The check sits at the
/// very top so an over-limit node count aborts before the launcher would ever
/// fork that many processes.
///
/// ARTS reads each config variable from the environment when present
/// (arts_config_find_variable does getenv(name)), so this test sets
/// node_count=16383 in its own main() before calling arts_rt: the first count
/// the guard refuses, so a guard that admits one count too many fails the
/// test.  The guard runs before transport setup and process launch, so it
/// guarantees that no process is launched.  A regressed guard is bounded by the
/// tripwire at the top of main(): main() marks the environment before arts_rt,
/// and every rank the launcher starts inherits the mark and exits at once
/// reporting RANK_OVERFLOW_NO_ABORT.  The startup budget is cut to one second,
/// so the spawning rank stops waiting for those ranks at once and a guard that
/// admits the count fails the test within seconds.  The test is judged on exit
/// status 1 and the guard's message.
///
/// Single-node; no coherence protocol, write or release policy or payload
/// store changes the guard (it sits above the coherence layer).  Asserts the
/// guard works (clean abort, exit 1).

#include "arts.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define SPAWNED_MARK "MAIN_RANK_OVERFLOW_ABORT_SPAWNED"

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  /* Never reached: the rank-count overflow guard aborts during arts_rt bring-up
   * before main_edt is ever scheduled. */
  arts_printf("UNEXPECTED: main_edt ran despite rank overflow\n");
  arts_shutdown();
}

int main(int argc, char **argv) {
  /* A launched rank exists only if the guard admitted the count. */
  if (getenv(SPAWNED_MARK) != NULL) {
    printf("RANK_OVERFLOW_NO_ABORT: a rank was launched\n");
    fflush(stdout);
    _exit(1);
  }
  setenv(SPAWNED_MARK, "1", 1);
  /* Only a regressed guard reaches the transport, which then waits this long
   * for ranks the tripwire has already ended. */
  setenv("ARTS_STARTUP_TIMEOUT_S", "1", 1);

  /* Force an over-limit configured rank count via the env override path.  The
   * GUID rank field is 14 bits (max usable 16382); 16383 is one past it. */
  setenv("node_count", "16383", 1);

  arts_rt(argc, argv);

  /* If arts_rt returned, the overflow guard did NOT abort -> regression. */
  printf("RANK_OVERFLOW_NO_ABORT\n");
  return 0;
}
