/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
/// @file shutdown_status_above_record.c
/// @brief A fault after a rank wrote its record ends the rank with the
///        fault's status while the record keeps the status it was written
///        with.
///
/// On rank 1, a holder EDT puts SIGSEGV back to its default action, locks
/// stdout, starts the EDT that ends the run and then waits for rank 1's
/// counter file, n1.json in $counter_folder, which the end writes whole
/// under a temporary name before it ends the process.  The locked stream
/// keeps rank 1's end from finishing (its last flush waits for the lock), so
/// the holder sees the file and raises SIGSEGV, which ends rank 1 by the
/// signal: status 139, the run's, while n1.json says rc 0.  Rank 0 learns of
/// the end and ends with 0.

#include "arts.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "../test_failure_status.h"

void end_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
             arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_shutdown();
}

void holder(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
            arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  const char *folder = getenv("counter_folder");
  if (folder == NULL) {
    arts_printf("  FAIL: counter_folder is not set\n");
    arts_test_fail();
    arts_shutdown();
  }
  char path[4096];
  snprintf(path, sizeof(path), "%s/n1.json", folder);
  (void)signal(SIGSEGV, SIG_DFL);
  arts_printf("STATUS_ABOVE_RECORD: rank 1 waits for %s\n", path);
  flockfile(stdout);
  arts_edt_create(end_edt, 0, NULL, 0, &(arts_edt_hint_t){.rank = 1});
  while (access(path, F_OK) != 0) {
    struct timespec ms = {.tv_sec = 0, .tv_nsec = 1000000L};
    nanosleep(&ms, NULL);
  }
  (void)raise(SIGSEGV);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (arts_get_total_ranks() != 2) {
    arts_printf("  FAIL: needs exactly two ranks\n");
    arts_test_fail();
    arts_shutdown();
  }
  arts_edt_create(holder, 0, NULL, 0, &(arts_edt_hint_t){.rank = 1});
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
