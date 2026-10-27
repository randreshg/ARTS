/******************************************************************************
** This material was prepared as an account of work sponsored by an agency   **
** of the United States Government.  Neither the United States Government    **
** nor the United States Department of Energy, nor Battelle, nor any of      **
** their employees, nor any jurisdiction or organization that has cooperated **
** in the development of these materials, makes any warranty, express or     **
** implied, or assumes any legal liability or responsibility for the accuracy,*
** completeness, or usefulness or any information, apparatus, product,       **
** software, or process disclosed, or represents that its use would not      **
** infringe privately owned rights.                                          **
**                                                                           **
** Reference herein to any specific commercial product, process, or service  **
** by trade name, trademark, manufacturer, or otherwise does not necessarily **
** constitute or imply its endorsement, recommendation, or favoring by the   **
** United States Government or any agency thereof, or Battelle Memorial      **
** Institute. The views and opinions of authors expressed herein do not      **
** necessarily state or reflect those of the United States Government or     **
** any agency thereof.                                                       **
**                                                                           **
**                      PACIFIC NORTHWEST NATIONAL LABORATORY                **
**                                  operated by                              **
**                                    BATTELLE                               **
**                                     for the                               **
**                      UNITED STATES DEPARTMENT OF ENERGY                   **
**                         under Contract DE-AC05-76RL01830                  **
**                                                                           **
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License");           **
** you may not use this file except in compliance with the License.          **
** You may obtain a copy of the License at                                   **
**                                                                           **
**    https://www.apache.org/licenses/LICENSE-2.0                            **
**                                                                           **
** Unless required by applicable law or agreed to in writing, software       **
** distributed under the License is distributed on an "AS IS" BASIS, WITHOUT **
** WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the  **
** License for the specific language governing permissions and limitations   **
******************************************************************************/

/// @file round_robin_home_distribution.c
/// @brief ARTS_HINT_ROUND_ROBIN + reserve_range: each labeled index's
///        home rank equals idx % nrank on every rank, every rank derives the
///        same members, and an object created at a member lives at its home.
///
/// reserve_range(ARTS_HINT_ROUND_ROBIN) returns a DISTRIBUTED range whose
/// from_index plants GUID i at home = idx % nrank.  Two ranges are reserved
/// once on rank 0, one of DBs and one of events, and their GUIDs handed to a
/// checker on every rank, which recomputes every member: from_index is a pure
/// function of the range GUID and the rank count, so every rank must get the
/// same GUIDs.  The checker
///   (a) asserts the decoded rank of every member is idx % nrank, its kind is
///       the range's, and index_from maps it back to its index;
///   (b) asserts arts_guid_is_local agrees with that home;
///   (c) creates each member it homes: a DB holding its home's rank and its
///       index, and a latch event counting one satisfy per rank;
///   (d) satisfies every event member once, wherever it is homed;
///   (e) sends the members it derived, in a block, to a judge on rank 0, which
///       requires every rank's list to equal rank 0's.
/// Once every checker has finished, a collector acquires every DB member RO
/// through the derived GUIDs and checks the value its home wrote.  An EDT
/// depending on every event member runs only once each latch has counted a
/// satisfy from every rank; a member created or satisfied under another GUID
/// leaves its latch short and the run times out.  The event range holds one
/// member more than a multiple of the rank count, so rank 0 homes one more
/// member than the others.
///
/// Requires multi-node (rank_count >= 2).  Prints SKIP single-node.

#include "arts.h"
#include <stdint.h>

#include "../test_failure_status.h"

#define RR_PER_RANK 3

static unsigned int db_count(unsigned int nrank) { return RR_PER_RANK * nrank; }

static unsigned int event_count(unsigned int nrank) {
  return RR_PER_RANK * nrank + 1;
}

/// Checks (a) and (b) for member i of range; prints a FAIL and returns false
/// on a mismatch.
static bool check_member(arts_guid_t range, unsigned int i, unsigned int nrank,
                         arts_guid_kind_t kind) {
  unsigned int me = arts_get_current_rank();
  arts_guid_t g = arts_guid_from_index(range, i);
  unsigned int expect_home = i % nrank;
  if (arts_guid_get_rank(g) != expect_home) {
    arts_printf("  FAIL: rank %u kind %u idx %u home=%u expected %u\n", me,
                (unsigned int)kind, i, arts_guid_get_rank(g), expect_home);
    arts_test_fail();
    return false;
  }
  if (arts_guid_get_kind(g) != kind ||
      arts_guid_index_from(range, g) != (int64_t)i) {
    arts_printf("  FAIL: rank %u kind %u idx %u kind or index round trip\n", me,
                (unsigned int)kind, i);
    arts_test_fail();
    return false;
  }
  bool should_be_local = (expect_home == me);
  if (arts_guid_is_local(g) != should_be_local) {
    arts_printf("  FAIL: rank %u kind %u idx %u is_local=%d expected %d\n", me,
                (unsigned int)kind, i, (int)arts_guid_is_local(g),
                (int)should_be_local);
    arts_test_fail();
    return false;
  }
  return true;
}

/// Per-rank checker.  paramv: the DB range, the event range, nrank and the
/// judge.
static void checker_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t db_range = (arts_guid_t)paramv[0];
  arts_guid_t ev_range = (arts_guid_t)paramv[1];
  unsigned int nrank = (unsigned int)paramv[2];
  arts_guid_t judge = (arts_guid_t)paramv[3];
  unsigned int me = arts_get_current_rank();
  unsigned int ndb = db_count(nrank);
  unsigned int nev = event_count(nrank);
  bool ok = true;

  uint64_t *list = NULL;
  arts_guid_t list_db =
      arts_db_create((void **)&list, (ndb + nev) * sizeof(uint64_t), ARTS_DB,
                     ARTS_DB_PROP_NONE, &(arts_db_hint_t){.rank = me});

  for (unsigned int i = 0; i < ndb; i++) {
    arts_guid_t g = arts_guid_from_index(db_range, i);
    list[i] = (uint64_t)g;
    if (!check_member(db_range, i, nrank, ARTS_GUID_DB)) {
      ok = false;
      continue;
    }
    if (i % nrank == me) {
      uint64_t *ptr = (uint64_t *)arts_db_create_with_guid(
          g, 2 * sizeof(uint64_t), ARTS_DB, ARTS_DB_PROP_NONE, NULL);
      if (ptr == NULL) {
        arts_printf("  FAIL: rank %u idx %u local DB create returned NULL\n",
                    me, i);
        arts_test_fail();
        ok = false;
      } else {
        ptr[0] = (uint64_t)me;
        ptr[1] = (uint64_t)i;
        arts_db_release(g, DB_MODE_RW);
      }
    }
  }

  for (unsigned int i = 0; i < nev; i++) {
    arts_guid_t e = arts_guid_from_index(ev_range, i);
    list[ndb + i] = (uint64_t)e;
    if (!check_member(ev_range, i, nrank, ARTS_GUID_EVENT)) {
      ok = false;
      continue;
    }
    if (i % nrank == me) {
      arts_event_hint_t h = ARTS_EVENT_HINT_LATCH((int32_t)nrank);
      h.guid = e;
      if (arts_event_create(&h) != e) {
        arts_printf("  FAIL: rank %u idx %u event create returned another "
                    "GUID\n",
                    me, i);
        arts_test_fail();
        ok = false;
      }
    }
  }
  for (unsigned int i = 0; i < nev; i++) {
    arts_event_satisfy(arts_guid_from_index(ev_range, i), NULL_GUID);
  }

  arts_db_release(list_db, DB_MODE_RW);
  arts_add_dependence(list_db, judge, me, DB_MODE_RO);

  if (ok) {
    arts_printf("  PASS: rank %u home == idx %% nrank and home placement "
                "agrees for all %u DB and %u event members\n",
                me, ndb, nev);
  }
}

/// On rank 0.  paramv[0]: the list length.  Slot r: rank r's list, RO.
static void judge_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  unsigned int n = (unsigned int)paramv[0];
  for (uint32_t r = 0; r < depc; r++) {
    if (depv[r].ptr == NULL) {
      arts_printf("  FAIL: judge got no list from rank %u\n", r);
      arts_test_fail();
      return;
    }
  }
  const uint64_t *ref = (const uint64_t *)depv[0].ptr;
  bool ok = true;
  for (uint32_t r = 1; r < depc; r++) {
    const uint64_t *list = (const uint64_t *)depv[r].ptr;
    for (unsigned int k = 0; k < n; k++) {
      if (list[k] != ref[k]) {
        arts_printf("  FAIL: rank %u derived member %u as %lu, rank 0 as %lu\n",
                    r, k, (unsigned long)list[k], (unsigned long)ref[k]);
        arts_test_fail();
        ok = false;
        break;
      }
    }
  }
  if (ok) {
    arts_printf("  PASS: all %u ranks derived the same %u members\n", depc, n);
  }
}

/// paramv: the DB range and nrank.  Slot i: DB member i, RO.
static void collector_edt(uint32_t paramc, const uint64_t *paramv,
                          uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  arts_guid_t db_range = (arts_guid_t)paramv[0];
  unsigned int nrank = (unsigned int)paramv[1];
  bool ok = true;
  for (uint32_t i = 0; i < depc; i++) {
    const uint64_t *p = (const uint64_t *)depv[i].ptr;
    if (depv[i].guid != arts_guid_from_index(db_range, i) || p == NULL) {
      arts_printf("  FAIL: collector slot %u holds no member %u\n", i, i);
      arts_test_fail();
      ok = false;
    } else if (p[0] != i % nrank || p[1] != i) {
      arts_printf("  FAIL: member %u holds home %lu index %lu, expected home "
                  "%u\n",
                  i, (unsigned long)p[0], (unsigned long)p[1], i % nrank);
      arts_test_fail();
      ok = false;
    }
  }
  if (ok) {
    arts_printf("  PASS: every DB member read through its derived GUID holds "
                "its home's rank (%u members)\n",
                depc);
  }
}

/// Runs once every checker has finished.  paramv: the DB range and nrank.
static void collect_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t db_range = (arts_guid_t)paramv[0];
  unsigned int ndb = db_count((unsigned int)paramv[1]);
  arts_guid_t collector =
      arts_edt_create(collector_edt, 2, paramv, ndb, &ARTS_EDT_HINT_DEFAULTS);
  for (unsigned int i = 0; i < ndb; i++) {
    arts_add_dependence(arts_guid_from_index(db_range, i), collector, i,
                        DB_MODE_RO);
  }
}

/// Runs once every event member has fired.  paramv[0]: the event range.
static void events_fired_edt(uint32_t paramc, const uint64_t *paramv,
                             uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depv;
  arts_guid_t ev_range = (arts_guid_t)paramv[0];
  for (uint32_t i = 0; i < depc; i++) {
    arts_event_destroy(arts_guid_from_index(ev_range, i));
  }
  arts_printf("  PASS: every event member counted one satisfy per rank (%u "
              "members)\n",
              depc);
}

static void shutdown_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                         arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  unsigned int nrank = arts_get_total_ranks();
  if (nrank < 2) {
    arts_printf(
        "SKIP: round_robin_home_distribution requires rank_count >= 2\n");
    arts_shutdown();
    return;
  }

  arts_printf("=== round_robin_home_distribution (%u ranks) ===\n", nrank);

  unsigned int ndb = db_count(nrank);
  unsigned int nev = event_count(nrank);
  arts_guid_t db_range =
      arts_guid_reserve_range(ARTS_GUID_DB, ndb, ARTS_HINT_ROUND_ROBIN);
  arts_guid_t ev_range =
      arts_guid_reserve_range(ARTS_GUID_EVENT, nev, ARTS_HINT_ROUND_ROBIN);
  if (db_range == NULL_GUID || ev_range == NULL_GUID) {
    arts_printf("  FAIL: round_robin reserve_range returned NULL_GUID\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }

  arts_guid_t shut = arts_edt_create(shutdown_edt, 0, NULL, 1, NULL);
  arts_guid_t fe = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_add_dependence(fe, shut, 0, DB_MODE_NULL);
  arts_edt_hint_t in_fe = ARTS_EDT_HINT_DEFAULTS;
  in_fe.finish_event = fe;

  uint64_t nlist = ndb + nev;
  arts_guid_t judge = arts_edt_create(judge_edt, 1, &nlist, nrank, &in_fe);

  uint64_t ev_param = (uint64_t)ev_range;
  arts_guid_t fired =
      arts_edt_create(events_fired_edt, 1, &ev_param, nev, &in_fe);
  for (unsigned int i = 0; i < nev; i++) {
    arts_add_dependence(arts_guid_from_index(ev_range, i), fired, i,
                        DB_MODE_NULL);
  }

  arts_guid_t creators = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  uint64_t collect_params[2] = {(uint64_t)db_range, (uint64_t)nrank};
  arts_guid_t collect =
      arts_edt_create(collect_edt, 2, collect_params, 1, &in_fe);
  arts_add_dependence(creators, collect, 0, DB_MODE_NULL);

  uint64_t params[4] = {(uint64_t)db_range, (uint64_t)ev_range, (uint64_t)nrank,
                        (uint64_t)judge};
  for (unsigned int r = 0; r < nrank; r++) {
    arts_edt_create(checker_edt, 4, params, 0,
                    &(arts_edt_hint_t){.rank = r, .finish_event = creators});
  }
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
