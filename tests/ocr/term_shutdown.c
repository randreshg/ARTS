/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
/* A run that does nothing but shut down: an EDT on the rank argv[1] names (0
 * by default) calls arts_shutdown() argv[2] milliseconds after it starts (0 by
 * default), and main() prints TERM_TEST_EXIT after arts_rt returns.  The
 * injection-hook refusal tests run it: a refused run must end at startup,
 * before any of this. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "arts.h"

void trigger_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  uint64_t delay_ms = paramv[0];
  struct timespec left = {.tv_sec = (time_t)(delay_ms / 1000),
                          .tv_nsec = (long)(delay_ms % 1000) * 1000000L};
  while (clock_nanosleep(CLOCK_MONOTONIC, 0, &left, &left) == EINTR) {
  }
  arts_shutdown();
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
  uint64_t delay_ms = (argc > 2) ? strtoull(argv[2], NULL, 10) : 0;
  arts_edt_create(trigger_edt, 1, &delay_ms, 0,
                  &(arts_edt_hint_t){.rank = origin});
}

int main(int argc, char **argv) {
  arts_rt(argc, argv);
  printf("[rank %u] TERM_TEST_EXIT\n", arts_get_current_rank());
  return 0;
}
