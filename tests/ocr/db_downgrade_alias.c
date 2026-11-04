/* SPDX-License-Identifier: Apache-2.0
 *
 * arts_db_downgrade of a block three slots of one EDT name: slots 0 and 1 in
 * DB_MODE_RW and slot 2 in DB_MODE_RO share the EDT's one acquisition of it.
 * After the call every slot reads DB_MODE_RO with its pointer unchanged (all
 * three the same), and D still reads what it wrote.  D then hands the block
 * to a reader on another rank, and a writer on D's rank, ordered after both,
 * acquires it RW and writes it.  A count the downgrade left behind keeps the
 * turn open: another rank's reader waits for it under exclusion with
 * hand-back, and the teardown's quiescence check reports it.
 */
#include "arts.h"

#include "../test_failure_status.h"

#include <stdint.h>
#include <stdio.h>

#define NAME "db_downgrade_alias"
#define WORDS 64u

static uint64_t pattern(uint64_t base, unsigned int j) { return base + j; }

static void check(const uint64_t *p, uint64_t base, const char *who) {
  for (unsigned int j = 0; j < WORDS; j++) {
    if (p == NULL || p[j] != pattern(base, j)) {
      printf("FAIL: " NAME " %s: word %u is 0x%llx, expected 0x%llx\n", who, j,
             p ? (unsigned long long)p[j] : 0ull,
             (unsigned long long)pattern(base, j));
      arts_test_fail();
      return;
    }
  }
}

/* paramv[0]: the event that wakes the reader. */
static void downgrader_edt(uint32_t paramc, const uint64_t *paramv,
                           uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  void *ptr = depv[0].ptr;
  arts_guid_t g = depv[0].guid;
  if (ptr == NULL || depv[1].ptr != ptr || depv[2].ptr != ptr ||
      depv[1].guid != g || depv[2].guid != g) {
    printf("FAIL: " NAME " the three slots do not alias one block\n");
    arts_test_fail();
    return;
  }
  uint64_t *p = (uint64_t *)ptr;
  for (unsigned int j = 0; j < WORDS; j++) {
    p[j] = pattern(0xA11A5000u, j);
  }
  arts_db_downgrade(g);
  for (unsigned int i = 0; i < 3u; i++) {
    if (depv[i].mode != DB_MODE_RO || depv[i].ptr != ptr || depv[i].guid != g) {
      printf("FAIL: " NAME " slot %u reads mode %d ptr %p after the "
             "downgrade\n",
             i, (int)depv[i].mode, depv[i].ptr);
      arts_test_fail();
    }
  }
  check(p, 0xA11A5000u, "D after its downgrade");
  arts_event_satisfy((arts_guid_t)paramv[0], g);
  check(p, 0xA11A5000u, "D after the hand-off");
}

static void reader_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  check((const uint64_t *)depv[0].ptr, 0xA11A5000u, "the reader");
}

/* depv[0]: D's and the reader's completions; depv[1]: the block, RW. */
static void writer_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  uint64_t *p = (uint64_t *)depv[1].ptr;
  check(p, 0xA11A5000u, "the later writer");
  if (p != NULL) {
    for (unsigned int j = 0; j < WORDS; j++) {
      p[j] = pattern(0xB0B0000u, j);
    }
  }
}

/* paramv[0]: the block. */
static void finale_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_db_destroy((arts_guid_t)paramv[0]);
  printf("PASS: " NAME "\n");
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int n = arts_get_total_ranks();
  unsigned int dr = n - 1u;

  arts_event_hint_t gh = ARTS_EVENT_HINT_LATCH(1);
  gh.auto_destroy = true;
  arts_guid_t group = arts_event_create(&gh);

  void *p = NULL;
  arts_db_hint_t dh = ARTS_DB_HINT_DEFAULTS;
  dh.rank = 0u;
  arts_guid_t db = arts_db_create(&p, WORDS * sizeof(uint64_t), ARTS_DB,
                                  ARTS_DB_PROP_NONE, &dh);
  if (p != NULL) {
    for (unsigned int j = 0; j < WORDS; j++) {
      ((uint64_t *)p)[j] = 0;
    }
  }
  arts_db_release(db);

  uint64_t fparam = (uint64_t)db;
  arts_guid_t fin = arts_edt_create(finale_edt, 1, &fparam, 1,
                                    &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(group, fin, 0, DB_MODE_NULL);

  /* Both completions gate the writer, so its writes race neither read. */
  arts_event_hint_t ch = ARTS_EVENT_HINT_LATCH(2);
  arts_guid_t done = arts_event_create(&ch);
  arts_guid_t w = arts_edt_create(
      writer_edt, 0, NULL, 2, &(arts_edt_hint_t){.rank = dr, .group = group});
  arts_add_dependence(done, w, 0, DB_MODE_NULL);
  arts_add_dependence(db, w, 1, DB_MODE_RW);

  arts_event_hint_t eh = ARTS_EVENT_HINT_LATCH(1);
  arts_guid_t wake = arts_event_create(&eh);
  arts_guid_t r = arts_edt_create(
      reader_edt, 0, NULL, 1,
      &(arts_edt_hint_t){.rank = 0u, .group = group, .completion_event = done});
  arts_add_dependence(wake, r, 0, DB_MODE_RO);

  uint64_t dparam = (uint64_t)wake;
  arts_guid_t d = arts_edt_create(
      downgrader_edt, 1, &dparam, 3,
      &(arts_edt_hint_t){.rank = dr, .group = group, .completion_event = done});
  arts_add_dependence(db, d, 0, DB_MODE_RW);
  arts_add_dependence(db, d, 1, DB_MODE_RW);
  arts_add_dependence(db, d, 2, DB_MODE_RO);
  arts_event_satisfy_slot(group, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
