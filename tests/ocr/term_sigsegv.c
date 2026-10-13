/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
/* Termination-robustness test: trigger EDT on rank argv[1] raises SIGSEGV
 * via raise(3).  (A NULL store is undefined behaviour, which the
 * undefined-behaviour sanitizer reports before the fault.)  The rank prints
 * ARTS's "[ARTS] Crashed: SIGSEGV" line and its backtrace to stderr and dies
 * by the signal. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "arts.h"

void trigger_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  (void)raise(SIGSEGV);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  int argc = (int)paramv[0];
  char **argv = (char **)paramv[1];
  unsigned int origin =
      (argc > 1) ? (unsigned int)strtoul(argv[1], NULL, 10) : 0;
  if (origin >= arts_get_total_ranks()) {
    arts_printf("origin_rank %u >= total_nodes %u — cfg mismatch\n", origin,
                arts_get_total_ranks());
    arts_shutdown();
    return;
  }
  arts_edt_create(trigger_edt, 0, NULL, 0, &(arts_edt_hint_t){.rank = origin});
}

int main(int argc, char **argv) {
  arts_rt(argc, argv);
  printf("[rank %u] TERM_TEST_EXIT\n", arts_get_current_rank());
  return 0;
}
