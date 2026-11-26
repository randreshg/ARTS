/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* db_destroy_untouched_rank — a destroy issued on a rank that holds no copy
 * of the block reaches the block's home and tears it down.
 *
 * The main task creates a block homed on its own rank, writes it and
 * releases it; a task on the last rank, which receives the GUID only as a
 * parameter and never acquires it, destroys it.  The home then polls its
 * route table until the block is no longer installed; a block still
 * installed 20 s after the home's first poll fails the run.  Every arm's
 * `arts_db_destroy_arrived` is reached by that destroy, so each arm's home
 * teardown runs for a block no other rank ever touched.
 *
 * Needs two ranks.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "arts.h"
#include "arts/gas/route_table.h"
#include "../test_failure_status.h"

#define SETTLE_MS 20000L
#define BYTES 64u

static long now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long)t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static arts_guid_t edt_on(arts_edt_t fn, unsigned int rank, uint32_t paramc,
                          const uint64_t *paramv) {
  arts_edt_hint_t h = ARTS_EDT_HINT_DEFAULTS;
  h.rank = rank;
  return arts_edt_create(fn, paramc, paramv, 0, &h);
}

/* runs on the home. paramv: g, the home's clock at its first poll (0 before
 * it) */
static void poller(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  uint64_t pv[2] = {paramv[0], paramv[1] ? paramv[1] : (uint64_t)now_ms()};
  arts_shared_ptr_t h = arts_route_table_lookup_db((arts_guid_t)paramv[0]);
  bool live = arts_shared_get(h) != NULL;
  arts_shared_release(&h);
  if (!live) {
    if (arts_test_status() == 0) {
      arts_printf("PASS: db_destroy_untouched_rank destroyed from rank %u\n",
                  arts_get_total_ranks() - 1u);
    }
    arts_shutdown();
    return;
  }
  if (now_ms() - (long)pv[1] > SETTLE_MS) {
    arts_printf("FAIL: db_destroy_untouched_rank block %lu still installed "
                "on its home\n",
                (unsigned long)paramv[0]);
    arts_test_fail();
    arts_shutdown();
    return;
  }
  (void)edt_on(poller, arts_get_current_rank(), 2, pv);
}

/* runs on the last rank. paramv: g, home */
static void destroyer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_db_destroy((arts_guid_t)paramv[0]);
  uint64_t pv[2] = {paramv[0], 0u};
  (void)edt_on(poller, (unsigned int)paramv[1], 2, pv);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (arts_get_total_ranks() < 2u) {
    arts_printf("FAIL: db_destroy_untouched_rank needs two ranks\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  unsigned int home = arts_get_current_rank();
  arts_db_hint_t dh = ARTS_DB_HINT_DEFAULTS;
  dh.rank = home;
  void *p = NULL;
  arts_guid_t g = arts_db_create(&p, BYTES, ARTS_DB, ARTS_DB_PROP_NONE, &dh);
  if (g == NULL_GUID || p == NULL) {
    arts_printf("FAIL: db_destroy_untouched_rank create refused\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  memset(p, 1, BYTES);
  arts_db_release(g);
  uint64_t pv[2] = {(uint64_t)g, home};
  (void)edt_on(destroyer, arts_get_total_ranks() - 1u, 2, pv);
}

int main(int argc, char **argv) {
  return arts_rt(argc, argv);
}
