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

/// @file guid_keygen_table_coupling.c
/// @brief Every GUID a rank reserves is distinct from every other GUID any
///        rank reserves, whichever thread reserves it and for whichever home.
///        Regression for a past bug where the key blocks of two threads
///        overlapped and produced DUPLICATE GUIDs.
///
/// An event or EDT GUID is minted from a key block its minting thread owns:
/// each rank owns one slice of the key space, divided into one block per
/// thread of the rank, workers and progress threads alike.  A DB GUID's
/// sequence number comes from a slice its minting rank owns.  If a rank's
/// slice were divided into fewer blocks than it has threads that mint, two
/// threads' blocks would overlap; if two ranks' slices overlapped, two ranks
/// would mint the same GUID.  Either way identical GUIDs come out.
///
/// The test spawns many reserver EDTs per rank (more than workers, so the
/// scheduler spreads them across every worker thread) that each reserve a batch
/// of GUIDs, alternating EVENT and DB, for every home in turn, writing them
/// into a per-rank collection block.  A checker on each rank asserts its own
/// collection holds no duplicate; once every rank has finished, a checker on
/// rank 0 acquires every rank's collection and asserts that all of them
/// together hold no duplicate and that each GUID carries the kind and home it
/// was reserved with.
///
/// Multinode by registration (correct single-node too).

#include "arts.h"
#include <stdint.h>
#include <stdlib.h>

#include "../test_failure_status.h"

#define RESERVERS_PER_RANK 32 /* > workers so all worker threads participate   \
                               */
#define GUIDS_PER_RESERVER 16
#define TOTAL_PER_RANK (RESERVERS_PER_RANK * GUIDS_PER_RESERVER)

/// The kind and home of slot `slot` of rank `rank`'s collection.  A reserver
/// alternates kinds and moves to the next home every two GUIDs, starting from
/// a home that depends on its rank and its batch, so each kind is reserved for
/// every home.
static arts_guid_kind_t slot_kind(unsigned int slot) {
  return ((slot % GUIDS_PER_RESERVER) & 1u) ? ARTS_GUID_EVENT : ARTS_GUID_DB;
}

static unsigned int slot_home(unsigned int rank, unsigned int slot,
                              unsigned int nrank) {
  unsigned int batch = slot / GUIDS_PER_RESERVER;
  unsigned int i = slot % GUIDS_PER_RESERVER;
  return ((i >> 1) + batch + rank) % nrank;
}

/// Reserver EDT: depv[0] is the per-rank collection DB (RW).  paramv[0] = base
/// slot index into the array.  Reserves GUIDS_PER_RESERVER GUIDs and stores
/// them contiguously starting at the base slot.
static void reserver_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                         arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  unsigned int base = (unsigned int)paramv[0];
  uint64_t *slots = (uint64_t *)depv[0].ptr;
  unsigned int me = arts_get_current_rank();
  unsigned int nrank = arts_get_total_ranks();
  if (!slots) {
    return;
  }
  for (unsigned int i = 0; i < GUIDS_PER_RESERVER; i++) {
    unsigned int slot = base + i;
    arts_guid_t g =
        arts_guid_reserve(slot_kind(slot), slot_home(me, slot, nrank));
    slots[slot] = (uint64_t)g;
  }
}

/// Checker EDT: depv[0] is the per-rank collection DB (RO).  Asserts every
/// reserved GUID is non-NULL and globally unique on this rank.
static void checker_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  const uint64_t *slots = (const uint64_t *)depv[0].ptr;
  unsigned int me = arts_get_current_rank();
  if (!slots) {
    arts_printf("  FAIL: rank %u checker got NULL collection\n", me);
    arts_test_fail();
    return;
  }

  bool ok = true;
  for (unsigned int i = 0; i < TOTAL_PER_RANK && ok; i++) {
    if (slots[i] == (uint64_t)NULL_GUID) {
      arts_printf("  FAIL: rank %u slot %u never filled (NULL_GUID)\n", me, i);
      arts_test_fail();
      ok = false;
      break;
    }
    for (unsigned int j = i + 1; j < TOTAL_PER_RANK; j++) {
      if (slots[i] == slots[j]) {
        arts_printf("  FAIL: rank %u duplicate GUID at slots %u,%u "
                    "(key-block aliasing)\n",
                    me, i, j);
        arts_test_fail();
        ok = false;
        break;
      }
    }
  }
  if (ok) {
    arts_printf("  PASS: rank %u all %d reserved GUIDs unique (no key-block "
                "aliasing)\n",
                me, TOTAL_PER_RANK);
  }
}

static int u64_cmp(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a;
  uint64_t y = *(const uint64_t *)b;
  return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/// On rank 0, once every rank has finished.  Slot r: rank r's collection, RO.
static void final_checker_edt(uint32_t paramc, const uint64_t *paramv,
                              uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  unsigned int nrank = depc;
  unsigned int total = nrank * TOTAL_PER_RANK;
  uint64_t *all = (uint64_t *)malloc(total * sizeof(uint64_t));
  bool ok = (all != NULL);
  if (!ok) {
    arts_printf("  FAIL: final checker cannot allocate %u GUIDs\n", total);
    arts_test_fail();
  }
  for (unsigned int r = 0; r < nrank && ok; r++) {
    const uint64_t *slots = (const uint64_t *)depv[r].ptr;
    if (slots == NULL) {
      arts_printf("  FAIL: final checker got no collection from rank %u\n", r);
      arts_test_fail();
      ok = false;
      break;
    }
    for (unsigned int s = 0; s < TOTAL_PER_RANK; s++) {
      arts_guid_t g = (arts_guid_t)slots[s];
      if (g == NULL_GUID || arts_guid_get_kind(g) != slot_kind(s) ||
          arts_guid_get_rank(g) != slot_home(r, s, nrank)) {
        arts_printf("  FAIL: rank %u slot %u holds %lu: kind %u home %u, "
                    "reserved as kind %u home %u\n",
                    r, s, (unsigned long)slots[s],
                    (unsigned int)arts_guid_get_kind(g), arts_guid_get_rank(g),
                    (unsigned int)slot_kind(s), slot_home(r, s, nrank));
        arts_test_fail();
        ok = false;
        break;
      }
      all[r * TOTAL_PER_RANK + s] = slots[s];
    }
  }
  if (ok) {
    qsort(all, total, sizeof(uint64_t), u64_cmp);
    for (unsigned int k = 1; k < total; k++) {
      if (all[k] == all[k - 1]) {
        arts_printf("  FAIL: GUID %lu reserved twice across the ranks\n",
                    (unsigned long)all[k]);
        arts_test_fail();
        ok = false;
        break;
      }
    }
  }
  free(all);
  if (ok) {
    arts_printf("  PASS: all %u GUIDs of %u ranks distinct, each of its "
                "reserved kind and home\n",
                total, nrank);
  }
  arts_shutdown();
}

/// Runs once every driver, reserver and per-rank checker has finished.
/// paramv: the collections' range and nrank.
static void final_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t colls = (arts_guid_t)paramv[0];
  unsigned int nrank = (unsigned int)paramv[1];
  arts_guid_t checker = arts_edt_create(final_checker_edt, 0, NULL, nrank,
                                        &ARTS_EDT_HINT_DEFAULTS);
  for (unsigned int r = 0; r < nrank; r++) {
    arts_add_dependence(arts_guid_from_index(colls, r), checker, r, DB_MODE_RO);
  }
}

/// Per-rank driver: builds this rank's collection DB, fans out reservers under
/// an inner finish scope, and chains a checker after they complete.
/// paramv[0]: the collections' range, whose member r is rank r's collection.
static void rank_driver_edt(uint32_t paramc, const uint64_t *paramv,
                            uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  unsigned int me = arts_get_current_rank();

  arts_guid_t coll = arts_guid_from_index((arts_guid_t)paramv[0], me);
  uint64_t *slots = (uint64_t *)arts_db_create_with_guid(
      coll, TOTAL_PER_RANK * sizeof(uint64_t), ARTS_DB, ARTS_DB_PROP_NONE,
      NULL);
  for (unsigned int i = 0; i < TOTAL_PER_RANK; i++) {
    slots[i] = (uint64_t)NULL_GUID;
  }
  arts_db_release(coll, DB_MODE_RW);

  /* Checker runs after all reservers (slot 1 = inner finish); it inherits
   * this driver's scope (outer, joined through this rank's proxy), so the
   * final check waits for it. */
  arts_guid_t checker =
      arts_edt_create(checker_edt, 0, NULL, 2, &(arts_edt_hint_t){.rank = me});
  arts_add_dependence(coll, checker, 0, DB_MODE_RO);

  arts_guid_t inner = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_add_dependence(inner, checker, 1, DB_MODE_NULL);

  /* Fan out reservers inside the inner finish scope, all on this rank. */
  for (unsigned int k = 0; k < RESERVERS_PER_RANK; k++) {
    uint64_t base = (uint64_t)(k * GUIDS_PER_RESERVER);
    arts_guid_t r =
        arts_edt_create(reserver_edt, 1, &base, 1,
                        &(arts_edt_hint_t){.rank = me, .finish_event = inner});
    arts_add_dependence(coll, r, 0, DB_MODE_RW);
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  unsigned int nrank = arts_get_total_ranks();
  arts_printf("=== guid_keygen_table_coupling (%u ranks) ===\n", nrank);

  arts_guid_t colls =
      arts_guid_reserve_range(ARTS_GUID_DB, nrank, ARTS_HINT_ROUND_ROBIN);
  uint64_t final_params[2] = {(uint64_t)colls, (uint64_t)nrank};
  arts_guid_t final =
      arts_edt_create(final_edt, 2, final_params, 1, &ARTS_EDT_HINT_DEFAULTS);
  arts_guid_t outer = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_add_dependence(outer, final, 0, DB_MODE_NULL);

  uint64_t driver_param = (uint64_t)colls;
  for (unsigned int r = 0; r < nrank; r++) {
    arts_edt_create(rank_driver_edt, 1, &driver_param, 0,
                    &(arts_edt_hint_t){.rank = r, .finish_event = outer});
  }
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
