/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
/// @file shutdown_fork_child_abort.c
/// @brief A child a rank forks that calls arts_abort ends with its code
///        alone, and the rank's run is unaffected.
///
/// An EDT forks; the child calls arts_abort(5).  The child is not the process
/// the runtime started in, so its end is its code and nothing else: no
/// marker, no post, no counter file, no flush of the streams it inherited.
/// The parent waits for the child, checks its status, and ends the run with
/// arts_shutdown: status 0 and, with ARTS_E2E_MARKER set, one marker.

#include "arts.h"

#include <stdint.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../test_failure_status.h"

void fork_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  pid_t child = fork();
  if (child == 0) {
    arts_abort(5);
  }
  if (child < 0) {
    arts_printf("  FAIL: fork failed\n");
    arts_test_fail();
    arts_shutdown();
  }
  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 5) {
    arts_printf("  FAIL: the child ended with wait status 0x%x, not exit 5\n",
                status);
    arts_test_fail();
  } else {
    arts_printf("FORK_CHILD_ABORT: the child ended with 5\n");
  }
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_edt_create(fork_edt, 0, NULL, 0, &(arts_edt_hint_t){.rank = 0});
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
