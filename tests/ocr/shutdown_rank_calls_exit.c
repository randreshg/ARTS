/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
/// @file shutdown_rank_calls_exit.c
/// @brief A rank whose program calls _exit(0) mid-run ends every rank's run.
///
/// Once every rank has printed `RANK_PID <r> <pid>`, an EDT on rank 1 calls
/// _exit(0): no runtime code runs on its way out, so nothing is posted, and
/// every other rank learns of the end from rank 1's closed connection and
/// ends with 0.  The run's status is 0.

#include "arts.h"

#include <stdint.h>
#include <unistd.h>

void exit_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("RANK_CALLS_EXIT: rank %u calls _exit(0)\n",
              arts_get_current_rank());
  _exit(0);
}

void ready_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_edt_create(exit_edt, 0, NULL, 0, &(arts_edt_hint_t){.rank = 1});
}

void pid_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
             arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_printf("RANK_PID %u %d\n", arts_get_current_rank(), (int)getpid());
  arts_event_satisfy_slot((arts_guid_t)paramv[0], NULL_GUID,
                          ARTS_EVENT_LATCH_DECR_SLOT);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int total = arts_get_total_ranks();
  arts_guid_t all = arts_event_create(&ARTS_EVENT_HINT_LATCH(total));
  arts_guid_t ready =
      arts_edt_create(ready_edt, 0, NULL, 1, &(arts_edt_hint_t){.rank = 0});
  arts_add_dependence(all, ready, 0, DB_MODE_NULL);
  for (unsigned int r = 0; r < total; r++) {
    uint64_t latch = (uint64_t)all;
    arts_edt_create(pid_edt, 1, &latch, 0, &(arts_edt_hint_t){.rank = r});
  }
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
