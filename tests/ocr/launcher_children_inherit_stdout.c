/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "arts.h"

#include "../test_failure_status.h"

/* Rank 1 prints a sentinel via arts_printf.  Every rank the spawner starts
 * writes to the stdout it inherited from the spawner, so the output the
 * spawner's caller captures holds:
 *   [1] INHERITED_STDOUT_SENTINEL 42
 * and rank 1's fd 1 is its parent's open file, not a pipe of its own. */

static int stdout_is_parents(void) {
  char parent_fd[64];
  (void)snprintf(parent_fd, sizeof(parent_fd), "/proc/%d/fd/1", (int)getppid());
  struct stat mine;
  struct stat parents;
  if (stat("/proc/self/fd/1", &mine) != 0 || stat(parent_fd, &parents) != 0) {
    return 0;
  }
  return mine.st_dev == parents.st_dev && mine.st_ino == parents.st_ino;
}

void remote_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  if (!stdout_is_parents()) {
    arts_printf("FAIL: launcher_children_inherit_stdout rank %u's stdout is "
                "not its parent's\n",
                arts_get_current_rank());
    arts_test_fail();
  }
  arts_printf("INHERITED_STDOUT_SENTINEL %lu\n", paramv[0]);
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  /* The sentinel EDT runs on rank 1 if nodes >= 2, else on rank 0. */
  unsigned int target = (arts_get_total_ranks() > 1) ? 1u : 0u;
  uint64_t sentinel[1] = {42};
  arts_edt_create(remote_edt, 1, sentinel, 0,
                  &(arts_edt_hint_t){.rank = target});
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
