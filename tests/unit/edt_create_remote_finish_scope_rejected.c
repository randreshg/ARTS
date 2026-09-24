/* SPDX-License-Identifier: Apache-2.0
 *
 * edt_create_remote_finish_scope_rejected — a create's hint.finish_event must
 * name a scope homed on the creating rank: the join INCR is local, and an
 * INCR shipped to another rank's scope is not ordered against the new EDT's
 * completion DECR.  arts_edt_create_core raises a fatal error instead of
 * silently shipping such an INCR.
 *
 * Debug-only (the check compiles in only at ARTS_LOG_LEVEL >= DEBUG, the
 * default for a Debug build): rank 0 creates a finish scope homed on itself
 * and hands its GUID to an EDT it places on rank 1.  That EDT, running on
 * rank 1, tries to create a further EDT naming the rank-0 scope as
 * hint.finish_event — the mismatch (scope home 0 != creating rank 1) raises
 * the error on rank 1 and that process exits non-zero.  PASS is judged by
 * the error text reaching the captured output, not by exit status (only the
 * master rank's own status reaches ctest; a peer's death is what the
 * master's own arts_rt() return code reports, and its printed text is what a
 * regex here can see).
 */
#include <stdint.h>
#include <stdio.h>

#include "arts.h"

#include "../test_failure_status.h"

static void dummy_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
}

static void prober_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t remote_scope = (arts_guid_t)paramv[0];
  /* This call runs on rank 1; remote_scope is homed on rank 0, so
   * arts_edt_create_core's remote-scope check must reject it. */
  (void)arts_edt_create(
      dummy_edt, 0, NULL, 0,
      &(arts_edt_hint_t){.rank = 1, .finish_event = remote_scope});
  /* Unreached if the check fires as expected. */
  arts_printf(
      "FAIL: edt_create_remote_finish_scope_rejected accepted a scope "
      "homed on another rank\n");
  arts_test_fail();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  if (arts_get_total_ranks() < 2) {
    arts_printf(
        "SKIP: edt_create_remote_finish_scope_rejected requires "
        "node_count >= 2\n");
    arts_shutdown();
    return;
  }

  /* Homed on rank 0 (the creating rank here, via ARTS_HINT_CURRENT_RANK). */
  arts_guid_t scope = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  uint64_t param = (uint64_t)scope;
  arts_edt_create(prober_edt, 1, &param, 0, &(arts_edt_hint_t){.rank = 1});
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
