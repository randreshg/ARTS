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

/// @file db_destroy_implicit_release.c
/// @brief arts_db_destroy implicit release: Path-1 (created_db_list) fires for
///        a DB the destroying EDT created and still holds.
///
/// arts_db_destroy(guid) first releases the calling EDT's own hold on the
/// block (OCR ocrDbDestroy semantics).  The release scans created_db_list
/// (Path 1) before depv (Path 2); on a match it releases the created hold and
/// removes the entry, so the epilogue finds nothing left to release.
///
/// Shape: an EDT creates a fresh DB, writes it, and destroys it mid-body
/// while it still holds the creator's hold.  It must not crash or
/// double-release, and the run must end with its PASS line and status 0.
/// Where a destroyed block stays installed until its holds end (EXCL with
/// PURGE), a hold the destroy failed to release also keeps the block
/// installed, and the teardown's quiescence walk names it (QUIESCENCE-DEBUG:,
/// in the test's fail list).  Elsewhere the destroy retires the block at
/// once, and the program judges only that the path runs and the run ends.
///
/// Every cell, on one rank.  The create and the destroy run in one body, so
/// on one worker (the created_db_list is thread-local).

#include "arts.h"

#include <stdint.h>
#include <stdio.h>

#include "../test_failure_status.h"

#define SENTINEL 0xDED0Du

/// creator_destroyer: creates the block, writes it, destroys it while holding
/// the creator's hold (Path 1 releases it).
void creator_destroyer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  void *raw = NULL;
  arts_guid_t db = arts_db_create(&raw, sizeof(unsigned int), ARTS_DB,
                                  ARTS_DB_PROP_NONE, NULL);
  unsigned int *p = (unsigned int *)raw;
  if (p == NULL) {
    arts_printf("FAIL: db_destroy_implicit_release NULL pointer\n");
    arts_test_fail();
    return;
  }
  p[0] = SENTINEL;
  arts_db_destroy(db);
}

static void finale(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (arts_test_status() == 0) {
    arts_printf("PASS: db_destroy_implicit_release\n");
  }
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== db_destroy_implicit_release ===\n");

  arts_event_hint_t gh = ARTS_EVENT_HINT_LATCH(1);
  gh.auto_destroy = true;
  arts_guid_t e_c = arts_event_create(&gh);
  arts_edt_create(creator_destroyer, 0, NULL, 0,
                  &(arts_edt_hint_t){.rank = 0, .group = e_c});
  arts_guid_t f =
      arts_edt_create(finale, 0, NULL, 1, &(arts_edt_hint_t){.rank = 0});
  arts_add_dependence(e_c, f, 0, DB_MODE_NULL);
  arts_event_satisfy_slot(e_c, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
}

int main(int argc, char **argv) {
  return arts_rt(argc, argv);
}
