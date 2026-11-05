/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* completion_event_latch_n — a completion event whose latch counts N fires
 * after N completions.
 *
 * main_edt creates one latch(N) event E and a successor on rank 0 that waits
 * on it, then N EDTs on rank 0, each naming E as its completion event.  Each
 * EDT counts itself; the successor must run once and find all N counts.
 */

#include <stdatomic.h>
#include <stdint.h>

#include "../test_failure_status.h"
#include "arts.h"

#define N 32

static _Atomic(unsigned int) g_done;
static _Atomic(unsigned int) g_successor_runs;

static void worker(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  atomic_fetch_add_explicit(&g_done, 1u, memory_order_release);
}

static void successor(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int runs =
      atomic_fetch_add_explicit(&g_successor_runs, 1u, memory_order_relaxed);
  if (runs != 0u) {
    arts_printf("FAIL: completion_event_latch_n: the successor ran twice\n");
    arts_test_fail();
  }
  unsigned int done = atomic_load_explicit(&g_done, memory_order_acquire);
  if (done != (unsigned int)N) {
    arts_printf("FAIL: completion_event_latch_n: %u of %d EDTs had completed "
                "when the event fired\n",
                done, N);
    arts_test_fail();
  } else if (arts_test_status() == 0) {
    arts_printf("PASS: completion_event_latch_n %d completions\n", N);
  }
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_event_hint_t eh = ARTS_EVENT_HINT_LATCH(N);
  arts_guid_t e = arts_event_create(&eh);
  arts_guid_t succ =
      arts_edt_create(successor, 0, NULL, 1, &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(e, succ, 0, DB_MODE_NULL);
  for (int i = 0; i < N; i++) {
    (void)arts_edt_create(
        worker, 0, NULL, 0,
        &(arts_edt_hint_t){.rank = 0u, .completion_event = e});
  }
}

int main(int argc, char **argv) {
  return arts_rt(argc, argv);
}
