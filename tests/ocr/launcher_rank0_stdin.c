/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "arts.h"

#include "../test_failure_status.h"

/* Under the local launcher only rank 0 keeps the stdin of the spawner it is
 * the child of; every other rank reads /dev/null.  The ranks check in turn
 * (rank r's check creates rank r+1's), and rank 0 prints the pass token after
 * the last one. */

static bool stdin_is_parents(void) {
  char parent_fd[64];
  (void)snprintf(parent_fd, sizeof(parent_fd), "/proc/%d/fd/0", (int)getppid());
  struct stat mine;
  struct stat parents;
  if (stat("/proc/self/fd/0", &mine) != 0 || stat(parent_fd, &parents) != 0) {
    return false;
  }
  return mine.st_dev == parents.st_dev && mine.st_ino == parents.st_ino;
}

static bool stdin_is_dev_null(void) {
  struct stat mine;
  struct stat null_device;
  if (stat("/proc/self/fd/0", &mine) != 0 ||
      stat("/dev/null", &null_device) != 0) {
    return false;
  }
  return S_ISCHR(mine.st_mode) && mine.st_rdev == null_device.st_rdev;
}

void check_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  const uint32_t rank = arts_get_current_rank();
  const uint32_t ranks = arts_get_total_ranks();
  const uint64_t stage = paramv[0];
  if (stage == ranks) {
    arts_printf("LAUNCHER_RANK0_STDIN_CHECKED %u\n", ranks);
    arts_shutdown();
    return;
  }
  if (rank == 0 ? !stdin_is_parents() : !stdin_is_dev_null()) {
    arts_printf("FAIL: launcher_rank0_stdin rank %u's stdin is not %s\n", rank,
                rank == 0 ? "its parent's" : "/dev/null");
    arts_test_fail();
  }
  uint64_t next[1] = {stage + 1};
  arts_edt_create(check_edt, 1, next, 0,
                  &(arts_edt_hint_t){.rank = (uint32_t)(stage + 1) % ranks});
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  uint64_t first[1] = {0};
  arts_edt_create(check_edt, 1, first, 0, &(arts_edt_hint_t){.rank = 0});
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
