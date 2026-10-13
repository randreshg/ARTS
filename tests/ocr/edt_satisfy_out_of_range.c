/* SPDX-License-Identifier: Apache-2.0
 *
 * A satisfy naming a slot at or above an EDT's depc names no dependence of
 * that EDT.  Only the rank holding the EDT can see it, and the sender has no
 * path back, so that rank ends the process with a fatal error naming the EDT,
 * the slot and depc (ARTS_ERROR: exit status 1).
 *
 * Rank 0 creates an EDT with two slots on the last rank and satisfies its
 * slot 5.  On one rank the satisfy runs where it is called, so a satisfy that
 * is let through reaches the arts_shutdown after it and the run ends with
 * status 0.  On two ranks it crosses the fabric and ends rank 1, which the
 * master's arts_rt reports; there the EDT's real slots are never satisfied,
 * so a satisfy that is let through leaves the run waiting until the test's
 * timeout.
 */
#include "arts.h"

static void never_runs(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("FAIL: edt_satisfy_out_of_range ran an EDT whose slots were "
              "never satisfied\n");
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int ranks = arts_get_total_ranks();
  arts_guid_t e = arts_edt_create(never_runs, 0, NULL, 2,
                                  &(arts_edt_hint_t){.rank = ranks - 1});
  arts_edt_satisfy_slot(e, 5, NULL_GUID, DB_MODE_NULL);
  if (ranks == 1) {
    arts_shutdown();
  }
}

int main(int argc, char **argv) {
  /* Non-zero when a rank this process spawned ended badly. */
  return arts_rt(argc, argv) != 0 ? 1 : 0;
}
