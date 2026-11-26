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
/// once on rank 0, one of EDTs and one of events, and their GUIDs handed to a
/// checker on every rank, which recomputes every member: from_index is a pure
/// function of the range GUID and the rank count, so every rank must get the
/// same GUIDs.  The checker
///   (a) asserts the decoded rank of every member is idx % nrank, its kind is
///       the range's, and index_from maps it back to its index;
///   (b) asserts arts_guid_is_local agrees with that home;
///   (c) creates each member it homes: an EDT with one control dependence per
///       rank, and a latch event counting one satisfy per rank;
///   (d) satisfies every EDT member's slot for this rank and every event
///       member once, wherever each is homed;
///   (e) sends the members it derived, in a block, to a judge on rank 0, which
///       requires every rank's list to equal rank 0's.
/// A member EDT runs only once every rank has satisfied its slot, and checks
/// that it runs on its home; each reports to a latch, and an EDT depending on
/// that latch runs once all of them have.  An EDT depending on every event
/// member runs only once each latch has counted a satisfy from every rank; a
/// member created or satisfied under another GUID leaves its dependence short
/// and the run times out.  The event range holds one member more than a
/// multiple of the rank count, so rank 0 homes one more member than the
/// others.
///
/// Requires multi-node (rank_count >= 2).  Fails single-node.

#include "arts.h"
#include <stdint.h>

#include "../test_failure_status.h"

#define RR_PER_RANK 3

static unsigned int edt_count(unsigned int nrank) {
  return RR_PER_RANK * nrank;
}

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

/// A member EDT.  paramv: its index, nrank and the latch every member reports
/// to.  Slot r: rank r's satisfy.
static void member_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  unsigned int i = (unsigned int)paramv[0];
  unsigned int nrank = (unsigned int)paramv[1];
  if (arts_get_current_rank() != i % nrank) {
    arts_printf("  FAIL: EDT member %u ran on rank %u, its home is %u\n", i,
                arts_get_current_rank(), i % nrank);
    arts_test_fail();
  }
  arts_event_satisfy((arts_guid_t)paramv[2], NULL_GUID);
}

/// Per-rank checker.  paramv: the EDT range, the event range, nrank, the judge
/// and the latch the member EDTs report to.
static void checker_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t edt_range = (arts_guid_t)paramv[0];
  arts_guid_t ev_range = (arts_guid_t)paramv[1];
  unsigned int nrank = (unsigned int)paramv[2];
  arts_guid_t judge = (arts_guid_t)paramv[3];
  uint64_t member_params[3] = {0, nrank, paramv[4]};
  unsigned int me = arts_get_current_rank();
  unsigned int nedt = edt_count(nrank);
  unsigned int nev = event_count(nrank);
  bool ok = true;

  uint64_t *list = NULL;
  arts_guid_t list_db =
      arts_db_create((void **)&list, (nedt + nev) * sizeof(uint64_t), ARTS_DB,
                     ARTS_DB_PROP_NONE, &(arts_db_hint_t){.rank = me});

  for (unsigned int i = 0; i < nedt; i++) {
    arts_guid_t g = arts_guid_from_index(edt_range, i);
    list[i] = (uint64_t)g;
    if (!check_member(edt_range, i, nrank, ARTS_GUID_EDT)) {
      ok = false;
      continue;
    }
    if (i % nrank == me) {
      member_params[0] = i;
      if (arts_edt_create(member_edt, 3, member_params, nrank,
                          &(arts_edt_hint_t){.guid = g}) != g) {
        arts_printf("  FAIL: rank %u idx %u EDT create returned another "
                    "GUID\n",
                    me, i);
        arts_test_fail();
        ok = false;
      }
    }
  }

  for (unsigned int i = 0; i < nev; i++) {
    arts_guid_t e = arts_guid_from_index(ev_range, i);
    list[nedt + i] = (uint64_t)e;
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
  for (unsigned int i = 0; i < nedt; i++) {
    arts_edt_satisfy_slot(arts_guid_from_index(edt_range, i), me, NULL_GUID,
                          DB_MODE_NULL);
  }
  for (unsigned int i = 0; i < nev; i++) {
    arts_event_satisfy(arts_guid_from_index(ev_range, i), NULL_GUID);
  }

  arts_db_release(list_db);
  arts_add_dependence(list_db, judge, me, DB_MODE_RO);

  if (ok) {
    arts_printf("  PASS: rank %u home == idx %% nrank and home placement "
                "agrees for all %u EDT and %u event members\n",
                me, nedt, nev);
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

/// Runs once every member EDT has run.
static void edts_ran_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                         arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("  PASS: every EDT member ran on its home once every rank "
              "satisfied it\n");
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
    arts_printf("FAIL: round_robin_home_distribution needs 2+ ranks (has %u)\n",
                nrank);
    arts_test_fail();
    arts_shutdown();
    return;
  }

  arts_printf("=== round_robin_home_distribution (%u ranks) ===\n", nrank);

  unsigned int nedt = edt_count(nrank);
  unsigned int nev = event_count(nrank);
  arts_guid_t edt_range =
      arts_guid_reserve_range(ARTS_GUID_EDT, nedt, ARTS_HINT_ROUND_ROBIN);
  arts_guid_t ev_range =
      arts_guid_reserve_range(ARTS_GUID_EVENT, nev, ARTS_HINT_ROUND_ROBIN);
  if (edt_range == NULL_GUID || ev_range == NULL_GUID) {
    arts_printf("  FAIL: round_robin reserve_range returned NULL_GUID\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }

  arts_guid_t shut = arts_edt_create(shutdown_edt, 0, NULL, 1, NULL);
  arts_event_hint_t gh = ARTS_EVENT_HINT_LATCH(1);
  gh.auto_destroy = true;
  arts_guid_t group = arts_event_create(&gh);
  arts_add_dependence(group, shut, 0, DB_MODE_NULL);
  arts_edt_hint_t in_group = ARTS_EDT_HINT_DEFAULTS;
  in_group.group = group;

  uint64_t nlist = nedt + nev;
  arts_guid_t judge = arts_edt_create(judge_edt, 1, &nlist, nrank, &in_group);

  uint64_t ev_param = (uint64_t)ev_range;
  arts_guid_t fired =
      arts_edt_create(events_fired_edt, 1, &ev_param, nev, &in_group);
  for (unsigned int i = 0; i < nev; i++) {
    arts_add_dependence(arts_guid_from_index(ev_range, i), fired, i,
                        DB_MODE_NULL);
  }

  arts_event_hint_t lh = ARTS_EVENT_HINT_LATCH((int32_t)nedt);
  lh.auto_destroy = true;
  arts_guid_t members_ran = arts_event_create(&lh);
  arts_guid_t ran = arts_edt_create(edts_ran_edt, 0, NULL, 1, &in_group);
  arts_add_dependence(members_ran, ran, 0, DB_MODE_NULL);

  uint64_t params[5] = {(uint64_t)edt_range, (uint64_t)ev_range,
                        (uint64_t)nrank, (uint64_t)judge,
                        (uint64_t)members_ran};
  for (unsigned int r = 0; r < nrank; r++) {
    arts_edt_create(checker_edt, 5, params, 0,
                    &(arts_edt_hint_t){.rank = r, .group = in_group.group});
  }
  arts_event_satisfy_slot(group, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
}

int main(int argc, char **argv) {
  return arts_rt(argc, argv);
}
