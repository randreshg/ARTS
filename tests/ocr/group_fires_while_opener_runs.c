/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* group_fires_while_opener_runs — a group fires once its members are done and
 * its opener has DECRed it, while the opener still runs.
 *
 * main_edt opens a group, puts one member on the last rank, wires a successor
 * on rank 0 to the group and DECRs it.  The member creates a child on rank 0,
 * which inherits the group (through the member's local proxy when the member
 * runs on another rank).  main_edt then keeps running: the successor must
 * find the child done, and must run before main_edt gives up after a bound.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

#include "../test_failure_status.h"
#include "arts.h"

#define WAIT_NS (30ull * 1000000000ull)

static _Atomic(unsigned int) g_child_done;
static _Atomic(unsigned int) g_fired;

static uint64_t now_ns(void) {
  struct timespec t;
  (void)clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void child(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  atomic_store_explicit(&g_child_done, 1u, memory_order_release);
}

static void member(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  (void)arts_edt_create(child, 0, NULL, 0, &(arts_edt_hint_t){.rank = 0u});
}

static void successor(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (atomic_load_explicit(&g_child_done, memory_order_acquire) != 1u) {
    arts_printf("FAIL: group_fires_while_opener_runs: the group fired before "
                "the member's child finished\n");
    arts_test_fail();
  }
  atomic_store_explicit(&g_fired, 1u, memory_order_release);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int last = arts_get_total_ranks() - 1u;
  arts_event_hint_t gh = ARTS_EVENT_HINT_LATCH(1);
  arts_guid_t g = arts_event_create(&gh);
  (void)arts_edt_create(member, 0, NULL, 0,
                        &(arts_edt_hint_t){.rank = last, .group = g});
  arts_guid_t succ =
      arts_edt_create(successor, 0, NULL, 1, &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(g, succ, 0, DB_MODE_NULL);
  arts_event_satisfy_slot(g, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);

  uint64_t start = now_ns();
  while (atomic_load_explicit(&g_fired, memory_order_acquire) == 0u &&
         now_ns() - start < WAIT_NS) {
  }
  if (atomic_load_explicit(&g_fired, memory_order_acquire) == 0u) {
    arts_printf("FAIL: group_fires_while_opener_runs: the group did not fire "
                "while its opener ran\n");
    arts_test_fail();
  } else if (arts_test_status() == 0) {
    arts_printf("PASS: group_fires_while_opener_runs\n");
  }
  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
