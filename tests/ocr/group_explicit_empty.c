/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* group_explicit_empty — a group with no member fires at its opener's DECR.
 *
 * main_edt opens a group (a latch whose count of 1 is the opener's), wires a
 * successor to it, and DECRs it.  The group's only count is then gone, so the
 * successor runs while main_edt is still running: main_edt waits for it, up
 * to a bound, and fails if it has not run.  The successor runs exactly once.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

#include "../test_failure_status.h"
#include "arts.h"

#define WAIT_NS (30ull * 1000000000ull)

static _Atomic(unsigned int) g_runs;

static uint64_t now_ns(void) {
  struct timespec t;
  (void)clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void successor(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (atomic_fetch_add_explicit(&g_runs, 1u, memory_order_release) != 0u) {
    arts_printf("FAIL: group_explicit_empty: the successor ran twice\n");
    arts_test_fail();
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_event_hint_t gh = ARTS_EVENT_HINT_LATCH(1);
  arts_guid_t g = arts_event_create(&gh);
  arts_guid_t succ =
      arts_edt_create(successor, 0, NULL, 1, &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(g, succ, 0, DB_MODE_NULL);
  arts_event_satisfy_slot(g, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);

  uint64_t start = now_ns();
  while (atomic_load_explicit(&g_runs, memory_order_acquire) == 0u &&
         now_ns() - start < WAIT_NS) {
  }
  unsigned int runs = atomic_load_explicit(&g_runs, memory_order_acquire);
  if (runs != 1u) {
    arts_printf("FAIL: group_explicit_empty: the successor ran %u times by "
                "the bound, want 1 (an empty group fires at its opener's "
                "DECR)\n",
                runs);
    arts_test_fail();
  } else if (arts_test_status() == 0) {
    arts_printf("PASS: group_explicit_empty\n");
  }
  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
