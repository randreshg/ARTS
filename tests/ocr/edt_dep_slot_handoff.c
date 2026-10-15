/* SPDX-License-Identifier: Apache-2.0
 *
 * What an EDT's body finds in its dependence vector: every slot the acquire
 * phase resolved carries its payload, and every slot it did not acquire keeps
 * what its satisfy delivered.
 *
 * One EDT with three slots: an event's value (a dependence that acquires
 * nothing), a block read, and a second read of the same block, which aliases
 * the first slot's acquisition.  The EDT runs on the last rank, so on two
 * ranks the block is fetched from its home and the read resolves on a wake;
 * on one rank it resolves on the spot.  The body checks every slot's GUID,
 * mode and pointer, and the block's bytes.
 */
#include "arts.h"

#include "../test_failure_status.h"

#include <stdint.h>

#define WORDS 8u
#define SEED 0x5EEDu

static void body(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  arts_guid_t value = (arts_guid_t)paramv[0];
  arts_guid_t block = (arts_guid_t)paramv[1];
  bool ok = true;
  if (depv[0].guid != value || depv[0].ptr != NULL ||
      depv[0].mode != DB_MODE_NULL) {
    arts_printf("FAIL: edt_dep_slot_handoff: the value slot holds guid %lu "
                "ptr %p mode %u, want guid %lu ptr NULL mode NULL\n",
                (unsigned long)depv[0].guid, depv[0].ptr,
                (unsigned)depv[0].mode, (unsigned long)value);
    ok = false;
  }
  const uint64_t *words = (const uint64_t *)depv[1].ptr;
  if (depv[1].guid != block || depv[1].mode != DB_MODE_RO || words == NULL) {
    arts_printf("FAIL: edt_dep_slot_handoff: the read slot holds guid %lu "
                "ptr %p mode %u\n",
                (unsigned long)depv[1].guid, depv[1].ptr,
                (unsigned)depv[1].mode);
    ok = false;
  } else {
    for (unsigned int i = 0; i < WORDS; i++) {
      if (words[i] != SEED + i) {
        arts_printf("FAIL: edt_dep_slot_handoff: word %u is %llu\n", i,
                    (unsigned long long)words[i]);
        ok = false;
        break;
      }
    }
  }
  if (depv[2].guid != block || depv[2].mode != DB_MODE_RO ||
      depv[2].ptr != depv[1].ptr) {
    arts_printf("FAIL: edt_dep_slot_handoff: the alias slot holds guid %lu "
                "ptr %p mode %u, want the read slot's payload %p\n",
                (unsigned long)depv[2].guid, depv[2].ptr,
                (unsigned)depv[2].mode, depv[1].ptr);
    ok = false;
  }
  if (!ok) {
    arts_test_fail();
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_guid_t scope = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  void *p = NULL;
  arts_guid_t block = arts_db_create(&p, WORDS * sizeof(uint64_t), ARTS_DB,
                                     ARTS_DB_PROP_NONE, NULL);
  for (unsigned int i = 0; i < WORDS; i++) {
    ((uint64_t *)p)[i] = SEED + i;
  }
  arts_db_release(block, DB_MODE_RW);
  /* The event's value: any GUID; a dependence of mode NULL acquires nothing
   * it names. */
  arts_guid_t value = scope;
  arts_guid_t ev = arts_event_create(NULL);
  uint64_t pv[2] = {(uint64_t)value, (uint64_t)block};
  arts_guid_t e = arts_edt_create(
      body, 2, pv, 3,
      &(arts_edt_hint_t){.rank = arts_get_total_ranks() - 1u,
                         .finish_event = scope});
  arts_add_dependence(ev, e, 0, DB_MODE_NULL);
  arts_add_dependence(block, e, 1, DB_MODE_RO);
  arts_add_dependence(block, e, 2, DB_MODE_RO);
  arts_event_satisfy(ev, value);
  arts_event_wait(scope);
  arts_event_destroy(ev);
  arts_db_destroy(block);
  if (arts_test_status() == 0) {
    arts_printf("PASS: edt_dep_slot_handoff\n");
  }
  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
