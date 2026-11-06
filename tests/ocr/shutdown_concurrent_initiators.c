/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
/// @file shutdown_concurrent_initiators.c
/// @brief Every rank ends the run at once.
///
/// main_edt hands one EDT to every rank with one instant on the host's
/// CLOCK_REALTIME, 200 ms ahead, which every rank of a local run shares; each
/// EDT sleeps until that instant and calls arts_shutdown.  Each rank either
/// takes its own shutdown point or learns of another's first, and either way
/// ends with status 0; rank 0 prints one marker (its own end, or the first it
/// learned of).  The EDTs leave before any rank's shutdown, so no gate drops
/// one.

#include "arts.h"

#include <errno.h>
#include <stdint.h>
#include <time.h>

void initiator(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  struct timespec at = {.tv_sec = (time_t)(paramv[0] / 1000000000ULL),
                        .tv_nsec = (long)(paramv[0] % 1000000000ULL)};
  while (clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &at, NULL) == EINTR) {
  }
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  uint64_t at = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec +
                200000000ULL;
  unsigned int ranks = arts_get_total_ranks();
  arts_printf("CONCURRENT_INITIATORS: %u ranks end at once\n", ranks);
  for (unsigned int r = 0; r < ranks; r++) {
    arts_edt_create(initiator, 1, &at, 0, &(arts_edt_hint_t){.rank = r});
  }
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
