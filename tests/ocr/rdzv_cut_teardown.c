/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* rdzv_cut_teardown -- one-sided payloads whose metadata and bytes both
 * reached their target, cut by the shutdown before they paired.  Run with
 * ARTS_NET_RDZV_CUT=<n>, which raises no payload's arrival and has the rank
 * holding n unpaired expectations start the shutdown; its teardown runs
 * their discards.
 *
 *   requester: two blocks homed on rank 0 and written by their creator; on
 *     rank 1 a reader acquires one and a writer the other.  Each answer
 *     carries the bytes to rank 1 (n = 2 there).  Neither EDT may run.
 *   home: an EDT on rank 1 creates a block homed on rank 0, writes it and
 *     returns; its release carries the bytes to rank 0 (n = 1 there) in the
 *     arms whose release moves the payload to the home.
 *
 * No step of the program ends the run: the hook does.  The verdict is the
 * teardown's TEARDOWN-INFO line, judged by the test's regexes, and the exit
 * status.  Needs two ranks.
 */

#include <stdint.h>
#include <string.h>

#include "arts.h"
#include "../test_failure_status.h"

#define BYTES 64u

static void must_not_run(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                         arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("FAIL: rdzv_cut_teardown: an EDT whose payload was withheld "
              "ran\n");
  arts_test_fail();
  arts_shutdown();
}

static arts_guid_t edt_on(arts_edt_t fn, unsigned int rank, uint32_t depc) {
  arts_edt_hint_t h = ARTS_EDT_HINT_DEFAULTS;
  h.rank = rank;
  return arts_edt_create(fn, 0, NULL, depc, &h);
}

static arts_guid_t block_on(unsigned int home, unsigned char fill) {
  arts_db_hint_t h = ARTS_DB_HINT_DEFAULTS;
  h.rank = home;
  void *p = NULL;
  arts_guid_t g = arts_db_create(&p, BYTES, ARTS_DB, ARTS_DB_PROP_NONE, &h);
  memset(p, fill, BYTES);
  return g;
}

static void writer_on_1(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  (void)block_on(0u, 0x5a);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)depc;
  (void)depv;
  int argc = (int)paramv[0];
  char **argv = (char **)paramv[1];
  (void)paramc;
  if (arts_get_total_ranks() != 2u || argc < 2) {
    arts_printf("FAIL: rdzv_cut_teardown needs two ranks and a shape\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  if (strcmp(argv[1], "requester") == 0) {
    arts_guid_t ro = block_on(0u, 0x11);
    arts_guid_t rw = block_on(0u, 0x22);
    arts_db_release(ro, DB_MODE_RW);
    arts_db_release(rw, DB_MODE_RW);
    arts_guid_t r = edt_on(must_not_run, 1u, 1);
    arts_guid_t w = edt_on(must_not_run, 1u, 1);
    arts_add_dependence(ro, r, 0, DB_MODE_RO);
    arts_add_dependence(rw, w, 0, DB_MODE_RW);
  } else if (strcmp(argv[1], "home") == 0) {
    (void)edt_on(writer_on_1, 1u, 0);
  } else {
    arts_printf("FAIL: rdzv_cut_teardown: unknown shape %s\n", argv[1]);
    arts_test_fail();
    arts_shutdown();
  }
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
