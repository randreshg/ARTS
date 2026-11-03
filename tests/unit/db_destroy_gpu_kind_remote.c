/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* db_destroy_gpu_kind_remote — a GPU-kind block's destroy from another rank
 * retires the block on its home.
 *
 * The main task creates an ARTS_DB_GPU_PIN block homed on its own rank (a GPU
 * kind is homed on the rank that creates it) and releases it; a task on
 * another rank, which never acquired it, destroys it.  The home then polls its
 * route table until the block is no longer installed; a block still installed
 * 20 s after the home's first poll fails the run.  A GPU kind keeps no
 * coherence state, so its destroy retires the descriptor itself and never
 * reaches the arm's home teardown.
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
    arts_printf("PASS: db_destroy_gpu_kind_remote\n");
    arts_shutdown();
    return;
  }
  if (now_ms() - (long)pv[1] > SETTLE_MS) {
    arts_printf("FAIL: db_destroy_gpu_kind_remote block %lu still installed "
                "on its home\n",
                (unsigned long)paramv[0]);
    arts_test_fail();
    arts_shutdown();
    return;
  }
  (void)edt_on(poller, arts_get_current_rank(), 2, pv);
}

/* runs on another rank. paramv: g, home */
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
    arts_printf("FAIL: db_destroy_gpu_kind_remote needs two ranks\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  unsigned int home = arts_get_current_rank();
  arts_db_hint_t dh = ARTS_DB_HINT_DEFAULTS;
  dh.rank = home;
  void *p = NULL;
  arts_guid_t g =
      arts_db_create(&p, 64, ARTS_DB_GPU_PIN, ARTS_DB_PROP_NONE, &dh);
  if (g == NULL_GUID || p == NULL) {
    arts_printf("FAIL: db_destroy_gpu_kind_remote create refused\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  memset(p, 1, 64);
  arts_db_release(g);
  uint64_t pv[2] = {(uint64_t)g, home};
  (void)edt_on(destroyer, (home + 1u) % arts_get_total_ranks(), 2, pv);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
