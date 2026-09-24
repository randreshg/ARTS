/* SPDX-License-Identifier: Apache-2.0
 *
 * Concurrent write acquisitions of one block on a rank that holds no write
 * right: every one of them must be served, and the requests they open must
 * number one per grant.
 *
 * Each round places WRITERS writers on a rank that is not the block's home.
 * They find no write right there and park, and the first of them asks the home
 * for it; a waiter that parks while that request is outstanding is covered
 * by the grant it will bring, and one that parks after the grant has taken
 * the waiting set must open a request of its own.  A finish scope per round
 * lets the next round start only after every turn of this one has been
 * released, so the coalescing window opens afresh every round.
 *
 * Two oracles.  A waiter no request covers is never served, so the run does
 * not finish and the final count is never checked.  A request opened for a
 * waiter a grant already served brings a grant that finds nobody waiting,
 * which the runtime asserts against in a debug build ("every grant finds at
 * least one waiter").  The final count, WRITERS * ROUNDS, checks that every
 * turn ran exactly once.
 */
#include <stdint.h>

#include "arts.h"
#include "../test_failure_status.h"

#define WRITERS 8u
#define ROUNDS 40u

static void writer_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  uint64_t *v = (uint64_t *)depv[0].ptr;
  if (v == NULL) {
    arts_test_fail();
    arts_printf("FAIL: grant_one_request_per_grant writer got NULL\n");
    return;
  }
  /* Write holders on one rank may overlap, so each adds atomically. */
  __atomic_fetch_add(v, (uint64_t)1, __ATOMIC_RELAXED);
}

static void verify_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  uint64_t *v = (uint64_t *)depv[0].ptr;
  uint64_t got = v ? __atomic_load_n(v, __ATOMIC_RELAXED) : (uint64_t)-1;
  if (got == (uint64_t)WRITERS * ROUNDS) {
    arts_printf("PASS: grant_one_request_per_grant count=%llu\n",
                (unsigned long long)got);
  } else {
    arts_test_fail();
    arts_printf("FAIL: grant_one_request_per_grant count=%llu expected=%u\n",
                (unsigned long long)got, WRITERS * ROUNDS);
  }
}

/* paramv = { db, round, away } */
static void round_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc; (void)depc; (void)depv;
  arts_guid_t db = (arts_guid_t)paramv[0];
  uint64_t round = paramv[1];
  unsigned int away = (unsigned int)paramv[2];

  arts_guid_t scope = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  if (round + 1u < ROUNDS) {
    uint64_t pv[3] = {(uint64_t)db, round + 1u, (uint64_t)away};
    arts_guid_t next = arts_edt_create(round_edt, 3, pv, 1,
                                       &(arts_edt_hint_t){.rank = away});
    arts_add_dependence(scope, next, 0, DB_MODE_NULL);
  } else {
    arts_guid_t ver =
        arts_edt_create(verify_edt, 0, NULL, 2, &(arts_edt_hint_t){.rank = 0});
    arts_add_dependence(db, ver, 0, DB_MODE_RO);
    arts_add_dependence(scope, ver, 1, DB_MODE_NULL);
  }
  for (unsigned int k = 0; k < WRITERS; ++k) {
    arts_guid_t w = arts_edt_create(
        writer_edt, 0, NULL, 1,
        &(arts_edt_hint_t){.rank = away, .finish_event = scope});
    arts_add_dependence(db, w, 0, DB_MODE_RW);
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
  if (arts_get_total_ranks() < 2) {
    arts_printf("SKIP grant_one_request_per_grant: needs a rank that is not "
                "the home\n");
    arts_shutdown();
    return;
  }
  unsigned int away = 1u;

  void *addr = NULL;
  arts_guid_t db = arts_db_create(&addr, 64, ARTS_DB, ARTS_DB_PROP_NONE,
                                  &(arts_db_hint_t){.rank = 0});
  *(uint64_t *)addr = 0;
  arts_db_release(db, DB_MODE_RW);

  arts_guid_t all = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t shut = arts_edt_create(shutdown_edt, 0, NULL, 1, NULL);
  arts_add_dependence(all, shut, 0, DB_MODE_NULL);

  uint64_t pv[3] = {(uint64_t)db, 0u, (uint64_t)away};
  arts_edt_create(round_edt, 3, pv, 0,
                  &(arts_edt_hint_t){.rank = away, .finish_event = all});
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
