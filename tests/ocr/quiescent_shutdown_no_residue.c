/* A shutdown that leaves no work undone must leave no coherence residue.
 *
 * Every EDT that touches a data block belongs to a finish scope the shutting
 * down EDT waits on, and that EDT holds no block, so when the runtime tears
 * down nothing is owed to anyone: every release has completed, every parked
 * acquirer has been granted, every hold has been released.  The runtime's
 * teardown walk (a debug build's QUIESCENCE-DEBUG markers) must then find
 * nothing, and the registration fails the test on any marker.
 *
 * The shapes are the ones whose interrupted forms leave residue behind.  On
 * one rank everything is home-local: write turns taken by the home itself and
 * several write holders sharing a turn.  With peers, every rank also writes
 * both counters, one homed on the first rank and one on the last, so each
 * counter has writers off its home:
 *   - write turns handed round the ranks: a release off the home publishes to
 *     it and waits for its acknowledgement where the arm publishes,
 *     requesters park while the turn is elsewhere, and the home runs one
 *     hand-over after another;
 *   - several write holders on one rank, sharing a turn;
 *   - a write first touch of a never-acquired block from a rank that is not
 *     its home;
 *   - a read of the result from the last rank, which checks that every
 *     increment landed.
 */

#include "arts.h"
#include "../test_failure_status.h"

#include <stdint.h>

#define ROUNDS 10u
#define BURST 32u
#define TOUCH_BYTES 64u

static void bump(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  /* Write holders on one rank share a turn, so the increment is atomic. */
  unsigned int *d = (unsigned int *)depv[0].ptr;
  if (d == NULL) {
    arts_printf("FAIL: a write acquire yielded no storage\n");
    arts_test_fail();
    return;
  }
  __atomic_fetch_add(&d[0], 1u, __ATOMIC_RELAXED);
}

static void check(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  unsigned int want = (unsigned int)paramv[0];
  const unsigned int *d = (const unsigned int *)depv[0].ptr;
  if (d == NULL || d[0] != want) {
    arts_printf("FAIL: counter %u, want %u\n", d ? d[0] : 0u, want);
    arts_test_fail();
    return;
  }
  arts_printf("PASS: quiescent_shutdown_no_residue counter=%u\n", d[0]);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int nranks = arts_get_total_ranks();
  unsigned int last = nranks - 1u;

  /* One counter homed on rank 0 and one on the last rank, so a home's own
   * write turns and a non-zero home's hand-overs both occur. */
  arts_guid_t ctr[2];
  for (unsigned int k = 0; k < 2u; k++) {
    void *p = NULL;
    ctr[k] = arts_db_create(&p, sizeof(unsigned int), ARTS_DB_DEFAULT,
                            ARTS_DB_PROP_NONE,
                            &(arts_db_hint_t){.rank = k ? last : 0u});
    ((unsigned int *)p)[0] = 0u;
    arts_db_release(ctr[k], DB_MODE_RW);
  }

  for (unsigned int r = 0; r < ROUNDS; r++) {
    arts_guid_t fe = arts_event_create(&ARTS_EVENT_HINT_FINISH);
    for (unsigned int h = 0; h < BURST; h++) {
      /* Writers go out in pairs, one per counter, so every rank writes
       * both counters and each counter is written from off its home. */
      arts_guid_t e = arts_edt_create(
          bump, 0, NULL, 1,
          &(arts_edt_hint_t){.rank = (h >> 1) % nranks, .finish_event = fe});
      arts_add_dependence(ctr[h & 1u], e, 0, DB_MODE_RW);
    }
    arts_event_wait(fe);
  }

  arts_guid_t fe = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  for (unsigned int k = 0; k < nranks; k++) {
    void *a = NULL;
    arts_guid_t t =
        arts_db_create(&a, TOUCH_BYTES, ARTS_DB_DEFAULT,
                       ARTS_DB_PROP_NO_ACQUIRE, &(arts_db_hint_t){.rank = k});
    arts_guid_t e = arts_edt_create(
        bump, 0, NULL, 1,
        &(arts_edt_hint_t){.rank = (k + 1u) % nranks, .finish_event = fe});
    arts_add_dependence(t, e, 0, DB_MODE_RW);
  }
  for (unsigned int k = 0; k < 2u; k++) {
    uint64_t want = (uint64_t)(BURST / 2u) * ROUNDS;
    arts_guid_t c = arts_edt_create(
        check, 1, &want, 1,
        &(arts_edt_hint_t){.rank = last, .finish_event = fe});
    arts_add_dependence(ctr[k], c, 0, DB_MODE_RO);
  }
  arts_event_wait(fe);
  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
