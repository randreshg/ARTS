/* SPDX-License-Identifier: Apache-2.0
 *
 * excl_self_home_no_landing — a request whose home is the requesting rank
 * advertises no rendezvous landing, while a request to another rank's home
 * still does, and a payload that must move to a home requester from a remote
 * holder still arrives.
 *
 * Observed through this rank's txid draw counter (every advertised landing
 * draws one) and through values.  A self-home request that drew a landing
 * moves the counter; one the home answered with a size reply instead of
 * queueing it would never be granted (ctest TIMEOUT).
 *
 *   A  block homed here, written here K times   -> counter unchanged
 *   B  block homed on the last rank, written here -> counter advances
 *   C  block homed here, written here, then on the last rank, then taken
 *      here to write, then written on the last rank again, then taken here
 *      to read                                   -> each remote write is read
 *
 * Needs a second rank: a single rank brings up no fabric and draws nothing.
 */

#include <stdint.h>
#include <stdio.h>

#include "arts.h"
#include "arts/transport/net.h"

#include "../test_failure_status.h"

#define K 64u
#define SEED 7u
#define MAGIC 42u

static uint64_t g_base;
static unsigned int g_far;

static void check(int ok, const char *what) {
  if (!ok) {
    (void)fprintf(stderr, "FAIL excl_self_home_no_landing: %s\n", what);
    arts_test_fail();
    arts_abort(1);
  }
}

static void check_value(uint64_t seen, uint64_t expected, const char *what) {
  if (seen != expected) {
    (void)fprintf(stderr,
                  "FAIL excl_self_home_no_landing: %s (read %llu, expected "
                  "%llu)\n",
                  what, (unsigned long long)seen, (unsigned long long)expected);
    arts_test_fail();
    arts_abort(1);
  }
}

static arts_guid_t make_db(unsigned int home) {
  return arts_db_create(
      &(void *){NULL}, sizeof(uint64_t), ARTS_DB, ARTS_DB_PROP_NO_ACQUIRE,
      &(arts_db_hint_t){
          .rank = home, .access_size = UINT64_MAX, .guid = NULL_GUID});
}

static void touch_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  ((uint64_t *)depv[0].ptr)[0] += 1u;
}

/* paramv[0]: the value to store. */
static void write_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  ((uint64_t *)depv[0].ptr)[0] = paramv[0];
}

/* paramv[0]: the value the last remote write stored. */
static void take_rw_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  check_value(((uint64_t *)depv[0].ptr)[0], paramv[0],
              "block written on a remote rank read back stale by a writer at "
              "its home");
}

static void take_ro_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  check_value(((const uint64_t *)depv[0].ptr)[0], paramv[0],
              "block written on a remote rank read back stale by a reader at "
              "its home");
  printf("PASS excl_self_home_no_landing\n");
  arts_shutdown();
}

/* An EDT on `rank` taking `db` in `mode`, after `prev` when there is one;
 * returns the finish event that its completion satisfies. */
static arts_guid_t chain(arts_guid_t db, arts_edt_t body, uint64_t value,
                         unsigned int rank, arts_guid_t prev,
                         arts_db_access_mode_t mode) {
  arts_guid_t e = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t w =
      arts_edt_create(body, 1, &value, prev == NULL_GUID ? 1 : 2,
                      &(arts_edt_hint_t){.rank = rank, .finish_event = e});
  arts_add_dependence(db, w, 0, mode);
  if (prev != NULL_GUID) {
    arts_add_dependence(prev, w, 1, DB_MODE_NULL);
  }
  return e;
}

/* Each value a home take checks differs from every value the home's copy
 * held before it, so a payload that never arrives is read as a mismatch. */
static void phase_c(void) {
  arts_guid_t db = make_db(0);
  arts_guid_t e = chain(db, write_edt, SEED, 0, NULL_GUID, DB_MODE_RW);
  e = chain(db, write_edt, MAGIC, g_far, e, DB_MODE_RW);
  e = chain(db, take_rw_edt, MAGIC, 0, e, DB_MODE_RW);
  e = chain(db, write_edt, MAGIC + 1u, g_far, e, DB_MODE_RW);
  (void)chain(db, take_ro_edt, MAGIC + 1u, 0, e, DB_MODE_RO);
}

static void phase_b_done(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                         arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  uint64_t drew = arts_net_rdzv_txid_peek() - g_base;
  printf("excl_self_home_no_landing: remote-home phase drew %llu txids\n",
         (unsigned long long)drew);
  check(drew > 0, "a request to another rank's home advertised no landing");
  phase_c();
}

static void phase_a_done(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                         arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  uint64_t drew = arts_net_rdzv_txid_peek() - g_base;
  printf("excl_self_home_no_landing: self-home phase drew %llu txids\n",
         (unsigned long long)drew);
  check(drew == 0, "a request to this rank's own home advertised a landing");
  g_base = arts_net_rdzv_txid_peek();
  arts_guid_t db = make_db(g_far);
  arts_guid_t prev = NULL_GUID;
  for (unsigned int i = 0; i < K; i++) {
    prev = chain(db, touch_edt, 0u, 0, prev, DB_MODE_RW);
  }
  arts_guid_t d =
      arts_edt_create(phase_b_done, 0, NULL, 1, &(arts_edt_hint_t){.rank = 0});
  arts_add_dependence(prev, d, 0, DB_MODE_NULL);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  check(arts_get_total_ranks() > 1u, "needs at least two ranks");
  g_far = arts_get_total_ranks() - 1u;
  arts_guid_t db = make_db(0);
  g_base = arts_net_rdzv_txid_peek();
  arts_guid_t prev = NULL_GUID;
  for (unsigned int i = 0; i < K; i++) {
    prev = chain(db, touch_edt, 0u, 0, prev, DB_MODE_RW);
  }
  arts_guid_t d =
      arts_edt_create(phase_a_done, 0, NULL, 1, &(arts_edt_hint_t){.rank = 0});
  arts_add_dependence(prev, d, 0, DB_MODE_NULL);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
