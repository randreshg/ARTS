/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License");           **
**    https://www.apache.org/licenses/LICENSE-2.0                            **
******************************************************************************/

/// @file cdag_multi_reader.c
/// @brief Minimal reproducer: repeated RO->EW cycles on one reused local DB.
///
/// Known frontier-ordering bug this reproduces:
///   - cycles == 1 (any worker count), and ANY cycles at worker_threads=1, PASS.
///   - cycles >= 2 at high worker counts (e.g. 20) FAIL: a reader sees the
///     next/previous cycle's value. It reproduces even with R == 1 (one reader
///     per gen), so the trigger is multi-generation RO->EW reuse racing
///     dependency registration against EDT dispatch -- NOT a reader undercount.
///
/// Runtime trace shows the EW writer logging "all deps satisfied -- firing" and
/// "Duplicate not added (rank already tracked)" immediately on registration:
/// the EW acquire on a DB+rank already tracked by the head RO generation is
/// deduped into that generation instead of deferring into a new EW generation,
/// so the writer runs before the prior readers drain (a WAR hazard). The
/// fresh-DB variant of multinode_scaling (no reuse) is always correct, which
/// corroborates the root cause. Reproducer only; not in the auto-suite.
///
/// argv[1] = distinct RO readers per generation (default 2).
/// argv[2] = number of RO->EW cycles on the SINGLE reused DB (default 1).
///
/// The DB starts at 0. Each cycle: R RO readers must observe the cycle index c,
/// then an EW writer increments the value to c+1. After C cycles a verifier must
/// observe C. This isolates multi-generation reuse (RO->EW->RO->EW...) on one DB.

#include "arts.h"
#include <stdlib.h>

void reader_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                arts_edt_dep_t depv[]) {
  (void)depc;
  (void)paramc;
  const int *p = (const int *)depv[0].ptr;
  int expect = (int)paramv[0];
  int which = (int)paramv[1];
  if (p && p[0] == expect) {
    arts_printf("  PASS: cycle %d reader %d saw %d\n", expect, which, expect);
  } else {
    arts_printf("  FAIL: cycle %d reader %d saw %d, expected %d "
                "(EW ran before this reader drained, or stale gen)\n",
                expect, which, p ? p[0] : -1, expect);
    abort();
  }
}

// Increments in place: writer for cycle c turns value c into c+1.
void writer_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  int *p = (int *)depv[0].ptr;
  if (p) p[0] = p[0] + 1;
}

void verify_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                arts_edt_dep_t depv[]) {
  (void)paramc; (void)depc;
  int cycles = (int)paramv[0];
  const int *p = (const int *)depv[0].ptr;
  if (p && p[0] == cycles) {
    arts_printf("  PASS: verifier saw final %d after %d cycles\n", p[0], cycles);
  } else {
    arts_printf("  FAIL: verifier saw %d, expected %d after %d cycles\n",
                p ? p[0] : -1, cycles, cycles);
    abort();
  }
}

void shutdown_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc; (void)depv;
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)depc; (void)depv;
  int nreaders = 2, cycles = 1;
  if (paramc >= 2) {
    char **argv = (char **)paramv[1];
    if (argv) {
      if (argv[1]) { int v = atoi(argv[1]); if (v > 0) nreaders = v; }
      if (argv[2]) { int v = atoi(argv[2]); if (v > 0) cycles = v; }
    }
  }
  arts_printf("=== cdag_multi_reader readers=%d cycles=%d ===\n", nreaders,
              cycles);

  arts_guid_t shut = arts_edt_create(shutdown_edt, 0, NULL, 1, NULL);
  arts_guid_t epoch = arts_initialize_and_start_epoch(shut, 0);

  void *ptr = NULL;
  arts_guid_t db =
      arts_db_create(&ptr, sizeof(int), ARTS_DB_DEFAULT, &(arts_hint_t){.route = 0});
  ((int *)ptr)[0] = 0;
  arts_db_release(db);

  for (int c = 0; c < cycles; c++) {
    // R distinct RO readers on this generation; all must see value c.
    for (int i = 0; i < nreaders; i++) {
      uint64_t pv[2] = {(uint64_t)c, (uint64_t)i};
      arts_guid_t rr = arts_edt_create_with_epoch(reader_edt, 2, pv, 1, epoch,
                                                  &(arts_hint_t){.route = 0});
      arts_add_dependence(db, rr, 0, DB_MODE_RO);
    }
    // EW writer: must wait for all readers; turns c into c+1.
    arts_guid_t w = arts_edt_create_with_epoch(writer_edt, 0, NULL, 1, epoch,
                                               &(arts_hint_t){.route = 0});
    arts_add_dependence(db, w, 0, DB_MODE_EW);
  }

  uint64_t cv = (uint64_t)cycles;
  arts_guid_t v = arts_edt_create_with_epoch(verify_edt, 1, &cv, 1, epoch,
                                             &(arts_hint_t){.route = 0});
  arts_add_dependence(db, v, 0, DB_MODE_RO);
}

int main(int argc, char **argv) {
  arts_rt(argc, argv);
  return 0;
}
