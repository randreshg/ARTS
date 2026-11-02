/* SPDX-License-Identifier: Apache-2.0
 *
 * arts_current_group NULL_GUID outside a group.
 *
 * Target: the public current-group accessor arts_current_group(),
 * which "returns the current group GUID inherited or joined by the
 * running EDT, or NULL_GUID if it belongs to no group."  The NULL_GUID
 * (no-group) return is the branch no other test reaches.
 *
 * Correct behavior pinned:
 *   - main_edt is created with ARTS_EDT_HINT_DEFAULTS (group =
 *     NULL_GUID, inheriting the caller's current group), and the top-level
 *     runtime has NO current group -> arts_current_group()
 *     MUST return NULL_GUID inside main_edt.
 *   - An EDT created with hint.group = fe joins that group ->
 *     arts_current_group() inside it MUST return exactly fe.
 *   - A child EDT created (without an explicit group) inside the group
 *     EDT inherits the current group -> also returns fe.
 *
 * Config-agnostic single-node public-API check.
 * Pins the NULL-group and inherit return values.
 */
#include "arts.h"
#include <stdint.h>

static int g_failed = 0;

/* Runs inside group fe (passed both as paramv[0] and as the EDT's
 * group).  Verifies the accessor returns fe, then spawns a child that
 * must INHERIT the same current group. */
void member_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                arts_edt_dep_t depv[]);
void child_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]);

void child_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)depc;
  (void)depv;
  arts_guid_t fe = (paramc >= 1) ? (arts_guid_t)paramv[0] : NULL_GUID;
  arts_guid_t cur = arts_current_group();
  if (cur != fe) {
    arts_printf("FAIL api_current_group_null: child did not inherit "
                "current group (got %ld want %ld)\n",
                (long)cur, (long)fe);
    g_failed = 1;
  } else {
    arts_printf("  ok: child inherited current group %ld\n", (long)fe);
  }
}

void member_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                arts_edt_dep_t depv[]) {
  (void)depc;
  (void)depv;
  arts_guid_t fe = (paramc >= 1) ? (arts_guid_t)paramv[0] : NULL_GUID;
  arts_guid_t cur = arts_current_group();
  if (cur != fe) {
    arts_printf("FAIL api_current_group_null: group member EDT current group "
                "%ld != joined fe %ld\n",
                (long)cur, (long)fe);
    g_failed = 1;
  } else {
    arts_printf("  ok: group member EDT reports joined group %ld\n", (long)fe);
  }
  /* Child inherits the current group (no explicit group in its hint). */
  uint64_t pv[1] = {(uint64_t)fe};
  arts_edt_create(child_edt, 1, pv, 0, NULL);
}

/* Finalizer: gated on the finish event draining; reports the verdict. */
void done_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (!g_failed) {
    arts_printf("PASS api_current_group_null: NULL outside group, fe "
                "inside, inherited by child\n");
  }
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== api_current_group_null ===\n");

  /* (1) Top-level: main_edt belongs to NO group. */
  arts_guid_t top = arts_current_group();
  if (top != NULL_GUID) {
    arts_printf("FAIL api_current_group_null: top-level group is %ld, "
                "expected NULL_GUID\n",
                (long)top);
    g_failed = 1;
  } else {
    arts_printf("  ok: top-level main_edt has no group (NULL_GUID)\n");
  }

  /* (2) Create a group and an EDT that joins it. */
  arts_guid_t fe = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  uint64_t pv[1] = {(uint64_t)fe};
  arts_edt_hint_t sh = ARTS_EDT_HINT_DEFAULTS;
  sh.group = fe;
  arts_edt_create(member_edt, 1, pv, 0, &sh);

  /* (3) done_edt fires when the group drains (member + child done). */
  arts_edt_hint_t dh = ARTS_EDT_HINT_DEFAULTS;
  arts_guid_t done = arts_edt_create(done_edt, 0, NULL, 1, &dh);
  arts_add_dependence(fe, done, 0, DB_MODE_NULL);
}

int main(int argc, char **argv) {
  arts_rt(argc, argv);
  return g_failed;
}
