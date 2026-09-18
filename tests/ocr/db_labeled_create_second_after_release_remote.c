/* One label names one object for its LIFETIME: a create of a label that
 * already exists creates nothing — no hold, no pointer, success.  The
 * lifetime outlives the creator's own hold, because a rank whose create
 * released the block holds no image of it that a later create could be
 * handed; so a second create on that rank must still create nothing long
 * after the first one released.  Away from the label's home the route-table
 * install cannot say that — the descriptor the first create left is already
 * installed and the second create finds it — so the answer comes from the
 * rank's own create mark.  The pair of creates here therefore runs on a rank
 * that is NOT the label's home.
 *
 * Between the two creates another rank acquires the block for writing.  That
 * takes the block away from the creating rank, which is the state in which a
 * hold answered from a permission word rather than from the mark would be
 * possession the home never granted.
 *
 * Two verdicts:
 *   - the second create's pointer.  Every arm must hand back NULL, or a
 *     program that branches on it takes a different branch on one arm than on
 *     the others.
 *   - the block's value, read back ON THE CREATING RANK.  The other rank's
 *     write is the block's current value; a create that was handed the
 *     creating rank's own retained image instead either publishes that image
 *     over the current one or serves it to the rank's own reads, and either
 *     way the reader sees it.
 *
 * Exclusive-write by construction: the create's hold is released before the
 * writing EDT's dependence is asked for, and that write is complete before
 * the only remaining acquisition, the reader's, is asked for.
 */
#include <stdint.h>

#include "arts.h"
#include "../test_failure_status.h"

#define FIRST_SENTINEL 0x1111u
#define SECOND_SENTINEL 0x2222u
#define THIRD_SENTINEL 0x3333u

static void writer_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  uint64_t *v = (uint64_t *)depv[0].ptr;
  if (v == NULL) {
    arts_test_fail();
    arts_printf("FAIL: db_labeled_create_second_after_release_remote the "
                "writing acquire was handed no block\n");
    return;
  }
  *v = THIRD_SENTINEL;
}

static void reader_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  const uint64_t *v = (const uint64_t *)depv[0].ptr;
  uint64_t got = v ? *v : (uint64_t)-1;
  if (got == (uint64_t)THIRD_SENTINEL) {
    arts_printf("PASS: db_labeled_create_second_after_release_remote "
                "value=%llu\n", (unsigned long long)got);
  } else {
    arts_test_fail();
    arts_printf("FAIL: db_labeled_create_second_after_release_remote reader "
                "got=%llu expected=%llu\n",
                (unsigned long long)got, (unsigned long long)THIRD_SENTINEL);
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc; (void)depv;
  unsigned int ranks = arts_get_total_ranks();
  unsigned int home = (ranks > 1) ? 1u : 0u;
  /* Someone other than the creating rank writes the block: a third rank
   * where there is one, the home itself otherwise.  Either way the block
   * leaves the creating rank. */
  unsigned int writer_rank = (ranks > 2) ? 2u : home;
  arts_guid_t label = arts_guid_reserve(ARTS_GUID_DB, home);

  arts_db_hint_t hint = ARTS_DB_HINT_DEFAULTS;
  hint.guid = label;

  void *first = NULL;
  arts_db_create(&first, sizeof(uint64_t), ARTS_DB, ARTS_DB_PROP_NONE, &hint);
  if (first == NULL) {
    arts_test_fail();
    arts_printf("FAIL: db_labeled_create_second_after_release_remote the "
                "first create of a label was handed no pointer\n");
  } else {
    *(uint64_t *)first = FIRST_SENTINEL;
  }
  /* The create's write acquisition ends here, before anything depends on the
   * label. */
  arts_db_release(label, DB_MODE_RW);

  arts_guid_t e_w = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t w = arts_edt_create(
      writer_edt, 0, NULL, 1,
      &(arts_edt_hint_t){.rank = writer_rank, .finish_event = e_w});
  arts_add_dependence(label, w, 0, DB_MODE_RW);
  arts_event_wait(e_w);

  /* The block exists and lives elsewhere now, so this creates nothing and is
   * handed nothing. */
  void *second = NULL;
  arts_db_create(&second, sizeof(uint64_t), ARTS_DB, ARTS_DB_PROP_NONE, &hint);
  if (second != NULL) {
    arts_test_fail();
    arts_printf("FAIL: db_labeled_create_second_after_release_remote a create "
                "of a label whose block already exists took a hold and a "
                "pointer\n");
    *(uint64_t *)second = SECOND_SENTINEL;
    arts_db_release(label, DB_MODE_RW);
  }

  arts_guid_t e_r = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t r = arts_edt_create(
      reader_edt, 0, NULL, 1,
      &(arts_edt_hint_t){.rank = 0, .finish_event = e_r});
  arts_add_dependence(label, r, 0, DB_MODE_RO);
  arts_event_wait(e_r);

  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
