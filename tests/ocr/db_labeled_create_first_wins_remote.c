/* One label names one object for its lifetime: a create of a label that
 * already exists creates nothing — no hold, no pointer, success.  Away from
 * the label's home the route-table install cannot arbitrate that, because the
 * descriptor the first create left is already installed and the second create
 * finds it; only the rank's own hold word can answer.  So the pair of creates
 * here runs on a rank that is NOT the label's home.
 *
 * Two verdicts:
 *   - the losing create's pointer.  Every arm must hand back NULL, or a
 *     program that branches on it takes a different branch on one arm than on
 *     the others.
 *   - the block's value.  The loser writes its own sentinel wherever it is
 *     handed one, so a hold taken over a live one surfaces as the second
 *     sentinel reaching the home, or as the first creator's bytes never
 *     getting there: the reader runs back on the creating rank after the
 *     creator's hold was released, so its bytes come from the home.
 *
 * Exclusive-write by construction: the only write acquisition of the block is
 * the one hold the first create takes, released before any dependence on the
 * label is asked for.
 */
#include <stdint.h>

#include "arts.h"
#include "../test_failure_status.h"

#define FIRST_SENTINEL 0x1111u
#define SECOND_SENTINEL 0x2222u

static void reader_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  const uint64_t *v = (const uint64_t *)depv[0].ptr;
  uint64_t got = v ? *v : (uint64_t)-1;
  if (got == (uint64_t)FIRST_SENTINEL) {
    arts_printf("PASS: db_labeled_create_first_wins_remote value=%llu\n",
                (unsigned long long)got);
  } else {
    arts_test_fail();
    arts_printf("FAIL: db_labeled_create_first_wins_remote reader got=%llu "
                "expected=%llu\n",
                (unsigned long long)got, (unsigned long long)FIRST_SENTINEL);
  }
}

static void shutdown_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                         arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc; (void)depv;
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc; (void)depv;
  unsigned int home = (arts_get_total_ranks() > 1) ? 1u : 0u;
  arts_guid_t label = arts_guid_reserve(ARTS_GUID_DB, home);

  arts_db_hint_t hint = ARTS_DB_HINT_DEFAULTS;
  hint.guid = label;

  void *first = NULL;
  arts_db_create(&first, sizeof(uint64_t), ARTS_DB, ARTS_DB_PROP_NONE, &hint);
  if (first == NULL) {
    arts_test_fail();
    arts_printf("FAIL: db_labeled_create_first_wins_remote the first create of "
                "a label was handed no pointer\n");
  } else {
    *(uint64_t *)first = FIRST_SENTINEL;
  }

  /* The block exists now, so this creates nothing and is handed nothing. */
  void *second = NULL;
  arts_db_create(&second, sizeof(uint64_t), ARTS_DB, ARTS_DB_PROP_NONE, &hint);
  if (second != NULL) {
    arts_test_fail();
    arts_printf("FAIL: db_labeled_create_first_wins_remote a create of a live "
                "label took a hold and a pointer\n");
    *(uint64_t *)second = SECOND_SENTINEL;
  }

  /* The one write acquisition ends here, before anything depends on the
   * label. */
  arts_db_release(label, DB_MODE_RW);

  arts_guid_t shut = arts_edt_create(shutdown_edt, 0, NULL, 1, NULL);
  arts_guid_t scope = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_add_dependence(scope, shut, 0, DB_MODE_NULL);

  arts_guid_t rdr = arts_edt_create(
      reader_edt, 0, NULL, 1,
      &(arts_edt_hint_t){.rank = 0, .finish_event = scope});
  arts_add_dependence(label, rdr, 0, DB_MODE_RO);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
