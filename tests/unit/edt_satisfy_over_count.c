/* SPDX-License-Identifier: Apache-2.0
 *
 * A satisfy beyond an EDT's dependence count names a slot the EDT has, but
 * every dependence of the EDT is already satisfied.  Only the rank holding
 * the EDT can see it, and the sender has no path back, so that rank ends the
 * process with a fatal error naming the EDT, the slot and depc (ARTS_ERROR:
 * exit status 1).
 *
 * Rank 0 creates an EDT with two slots on the last rank and satisfies its
 * slot 0 twice — which makes it ready — and then its slot 1.  The EDT stays
 * live meanwhile: its body waits for the run's end.  On one rank the
 * satisfies run where they are called, so a satisfy that is let through
 * reaches the arts_shutdown after it and the run ends with status 0.  On two
 * ranks they cross the fabric and end rank 1, which the master's arts_rt
 * reports; there a satisfy that is let through leaves the run waiting until
 * the test's timeout.  The error names slot 1 there because one progress
 * thread dispatches one sender's messages in the order they were sent.
 */
#include "arts.h"

#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

static _Atomic(int) g_done;

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Holds the EDT live until the run ends, so the last satisfy finds it. */
static void stays_live(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  uint64_t deadline = now_ns() + 20ull * 1000000000ull;
  while (!atomic_load_explicit(&g_done, memory_order_acquire) &&
         now_ns() < deadline) {
    sched_yield();
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int ranks = arts_get_total_ranks();
  arts_guid_t e = arts_edt_create(stays_live, 0, NULL, 2,
                                  &(arts_edt_hint_t){.rank = ranks - 1});
  arts_edt_satisfy_slot(e, 0, NULL_GUID, DB_MODE_NULL);
  arts_edt_satisfy_slot(e, 0, NULL_GUID, DB_MODE_NULL);
  arts_edt_satisfy_slot(e, 1, NULL_GUID, DB_MODE_NULL);
  if (ranks == 1) {
    atomic_store_explicit(&g_done, 1, memory_order_release);
    arts_shutdown();
  }
}

int main(int argc, char **argv) {
  /* Non-zero when a rank this process spawned ended badly. */
  return arts_rt(argc, argv) != 0 ? 1 : 0;
}
