/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/

/// @file counter_smoke_value.c
/// @brief A counter VALUE in n0.json, end to end: the increment path
///        (arts_counter_increment_by), node reduction and the JSON write.
///
/// The per-node JSON (n0.json) carries a "NUM_EDT_CREATE" and a
/// "NUM_EDT_FINISH" object whose numeric "value" is reduced (SUM) across this
/// node's threads.  We create a known number N of EDTs; with the main EDT the
/// runtime creates and runs and the continuation that ends the run, each value
/// must be exactly N + 2.  The registration's record check reads the file
/// once the run has ended, so the test is registered only in a tree whose
/// counter config captures both counters.

#include "arts.h"

#include <stdint.h>

#define N_EDTS 64

void worker_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
}

/* Gated on the workers' group. */
static void done_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_event_hint_t gh = ARTS_EVENT_HINT_LATCH(1);
  gh.auto_destroy = true;
  arts_guid_t group = arts_event_create(&gh);
  for (int i = 0; i < N_EDTS; i++) {
    arts_edt_create(worker_edt, 0, NULL, 0,
                    &(arts_edt_hint_t){.rank = 0, .group = group});
  }
  arts_guid_t done =
      arts_edt_create(done_edt, 0, NULL, 1, &(arts_edt_hint_t){.rank = 0});
  arts_add_dependence(group, done, 0, DB_MODE_NULL);
  arts_event_satisfy_slot(group, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
