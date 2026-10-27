/* SPDX-License-Identifier: Apache-2.0
 *
 * A dependence that delivers no data block but keeps a read or write mode.
 *
 * One EDT with four slots: an event satisfied with NULL_GUID feeding a write
 * dependence, another feeding a read dependence, a write dependence added on
 * NULL_GUID itself, and a real block in write mode.  The acquire phase
 * acquires only the block; the three NULL slots keep their satisfy's value
 * (NULL_GUID, a NULL pointer) and their mode, and the EDT's release must skip
 * them as slots that name no block.  The body checks every slot and writes
 * the block.  The EDT runs on the last rank, so on two ranks the satisfies
 * that carry NULL_GUID arrive from another rank.
 */
#include "arts.h"

#include "../test_failure_status.h"

#include <stdint.h>

#define SEED 0x5EEDu

static void body(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  arts_guid_t block = (arts_guid_t)paramv[0];
  static const arts_db_access_mode_t want_mode[3] = {DB_MODE_RW, DB_MODE_RO,
                                                     DB_MODE_RW};
  bool ok = true;
  for (unsigned int i = 0; i < 3u; i++) {
    if (depv[i].guid != NULL_GUID || depv[i].ptr != NULL ||
        depv[i].mode != want_mode[i]) {
      arts_printf("FAIL: edt_null_guid_db_mode: slot %u holds guid %lu ptr %p "
                  "mode %u, want NULL_GUID, NULL and mode %u\n",
                  i, (unsigned long)depv[i].guid, depv[i].ptr,
                  (unsigned)depv[i].mode, (unsigned)want_mode[i]);
      ok = false;
    }
  }
  uint64_t *words = (uint64_t *)depv[3].ptr;
  if (depv[3].guid != block || depv[3].mode != DB_MODE_RW || words == NULL) {
    arts_printf("FAIL: edt_null_guid_db_mode: the block slot holds guid %lu "
                "ptr %p mode %u\n",
                (unsigned long)depv[3].guid, depv[3].ptr,
                (unsigned)depv[3].mode);
    ok = false;
  } else if (words[0] != SEED) {
    arts_printf("FAIL: edt_null_guid_db_mode: the block reads %llu\n",
                (unsigned long long)words[0]);
    ok = false;
  } else {
    words[0] = SEED + 1u;
  }
  if (!ok) {
    arts_test_fail();
  }
}

/* Runs once the body's scope drained.  paramv: the two events, the block. */
static void finale(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_event_destroy((arts_guid_t)paramv[0]);
  arts_event_destroy((arts_guid_t)paramv[1]);
  arts_db_destroy((arts_guid_t)paramv[2]);
  if (arts_test_status() == 0) {
    arts_printf("PASS: edt_null_guid_db_mode\n");
  }
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_guid_t scope = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  void *p = NULL;
  arts_guid_t block =
      arts_db_create(&p, sizeof(uint64_t), ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  ((uint64_t *)p)[0] = SEED;
  arts_db_release(block, DB_MODE_RW);
  arts_guid_t ev_rw = arts_event_create(NULL);
  arts_guid_t ev_ro = arts_event_create(NULL);
  uint64_t pv[1] = {(uint64_t)block};
  arts_guid_t e = arts_edt_create(
      body, 1, pv, 4,
      &(arts_edt_hint_t){.rank = arts_get_total_ranks() - 1u,
                         .finish_event = scope});
  arts_add_dependence(ev_rw, e, 0, DB_MODE_RW);
  arts_add_dependence(ev_ro, e, 1, DB_MODE_RO);
  arts_add_dependence(NULL_GUID, e, 2, DB_MODE_RW);
  arts_add_dependence(block, e, 3, DB_MODE_RW);
  arts_event_satisfy(ev_rw, NULL_GUID);
  arts_event_satisfy(ev_ro, NULL_GUID);
  uint64_t fv[3] = {(uint64_t)ev_rw, (uint64_t)ev_ro, (uint64_t)block};
  arts_guid_t f =
      arts_edt_create(finale, 3, fv, 1, &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(scope, f, 0, DB_MODE_NULL);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
