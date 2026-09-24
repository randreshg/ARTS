/// @file fam_home_turn_costs.c
/// @brief A turn taken on the block's own home costs the same as any other:
/// one read of the block's store and one write back.
///
/// Whitebox, because the tallies are the arm's own observables and no public
/// call reports them.  Single rank on purpose: with one rank every turn is
/// home-local, so the numbers measure exactly the path a home-side shortcut
/// would make free.
///
/// The turns are counted, not merely non-zero: the writer's RW turn is one
/// fetch and one purge, and the checker's own RO turn is one more fetch taken
/// before its body runs, because the copy precedes admission.  The two EDTs
/// are ordered by the writer's output event, so nothing else is in flight
/// while the tallies are read.

#include "arts.h"

#include "arts/coherence/excl/types.h"

#include "../test_failure_status.h"

#include <stdint.h>
#include <stdio.h>

static uint64_t g_fetch0;
static uint64_t g_purge0;

/* depv[0] = the block, RW. */
static void writer_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  unsigned int *p = (unsigned int *)depv[0].ptr;
  if (p == NULL) {
    (void)fprintf(stderr, "FAIL: a home-local write turn got no storage\n");
    arts_test_fail();
    return;
  }
  p[0] = 7u;
}

/* depv[0] = the writer's output event, depv[1] = the block, RO. */
static void check_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  const unsigned int *p = (const unsigned int *)depv[1].ptr;
  if (p == NULL || p[0] != 7u) {
    (void)fprintf(stderr, "FAIL: the home's own turn lost its bytes\n");
    arts_test_fail();
  }
  uint64_t f = __atomic_load_n(&arts_fam_fetches, __ATOMIC_RELAXED) - g_fetch0;
  uint64_t pu = __atomic_load_n(&arts_fam_purges, __ATOMIC_RELAXED) - g_purge0;
  if (f != 2u || pu != 1u) {
    (void)fprintf(stderr,
                  "FAIL: a home-local write turn plus a read turn cost %llu "
                  "fetches and %llu purges (want 2 and 1)\n",
                  (unsigned long long)f, (unsigned long long)pu);
    arts_test_fail();
  }
  arts_printf("fam_home_turn_costs: %llu fetches, %llu purges on the home's "
              "own turns\n",
              (unsigned long long)f, (unsigned long long)pu);
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  g_fetch0 = __atomic_load_n(&arts_fam_fetches, __ATOMIC_RELAXED);
  g_purge0 = __atomic_load_n(&arts_fam_purges, __ATOMIC_RELAXED);

  /* Acquiring nothing, so the create itself takes no turn and the tallies
   * start where they were read. */
  void *q = NULL;
  arts_guid_t db =
      arts_db_create(&q, sizeof(unsigned int), ARTS_DB,
                     ARTS_DB_PROP_NO_ACQUIRE, &(arts_db_hint_t){.rank = 0u});
  if (db == NULL_GUID) {
    (void)fprintf(stderr, "FAIL: fam_home_turn_costs create\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }

  arts_guid_t oe = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
  arts_guid_t w = arts_edt_create(
      writer_edt, 0, NULL, 1,
      &(arts_edt_hint_t){.rank = 0u, .output_event = oe});
  arts_add_dependence(db, w, 0, DB_MODE_RW);

  /* The chain is explicit -- the checker's DB dependence is slot 1 behind the
   * writer's output event in slot 0 -- because dependence ORDER is not an
   * ordering. */
  arts_guid_t c =
      arts_edt_create(check_edt, 0, NULL, 2, &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(oe, c, 0, DB_MODE_NULL);
  arts_add_dependence(db, c, 1, DB_MODE_RO);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
