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
/// arts_db_destroy(guid) unconditionally calls arts_db_release(guid, RW) first
/// (OCR ocrDbDestroy semantics).  arts_db_release scans created_db_list (Path
/// 1) before depv (Path 2); on a match it releases the created hold and removes
/// the entry, so the epilogue finds nothing left to release.
///
/// Shape: an EDT creates a fresh labeled DB, writes it, and destroys it
/// mid-body while it still holds the creator's hold.  It must not crash,
/// double-release, or leak: a second EDT then creates the same label again
/// (the first block is gone, so that create installs at once), writes a new
/// value, and a reader checks it.  A hold the destroy failed to release keeps
/// the first block's teardown from completing and the reader never sees the
/// second value — the ctest TIMEOUT or the value check catches it.
///
/// All configs.  Home the DB on rank 0 so create + destroy are co-located
/// on the running EDT's worker (the created_db_list is thread-local).

#include "arts.h"

#include <stdint.h>
#include <stdio.h>

#include "../test_failure_status.h"

#define SENTINEL 0xDED0Du

/// creator_destroyer: creates the label, writes it, destroys it while holding
/// the creator's hold (Path 1 releases it).
void creator_destroyer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t db = (arts_guid_t)paramv[0];
  unsigned int *p = (unsigned int *)arts_db_create_with_guid(
      db, sizeof(unsigned int), ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  if (p == NULL) {
    arts_printf("FAIL: db_destroy_implicit_release NULL pointer\n");
    arts_test_fail();
    return;
  }
  p[0] = SENTINEL;
  arts_db_destroy(db);
}

/// recreator: the label's next block, created after the destroy.
void recreator(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t db = (arts_guid_t)paramv[0];
  unsigned int *p = (unsigned int *)arts_db_create_with_guid(
      db, sizeof(unsigned int), ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  if (p == NULL) {
    arts_printf("FAIL: db_destroy_implicit_release NULL pointer on the "
                "re-create\n");
    arts_test_fail();
    return;
  }
  p[0] = SENTINEL + 1u;
}

/// reader: RO dependence on the second block.
void reader(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
            arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  const unsigned int *v = (const unsigned int *)depv[0].ptr;
  if (v == NULL || v[0] != SENTINEL + 1u) {
    arts_printf("FAIL: db_destroy_implicit_release read 0x%x, expected "
                "0x%x\n",
                v ? v[0] : 0u, SENTINEL + 1u);
    arts_test_fail();
    return;
  }
  arts_printf("PASS: db_destroy_implicit_release\n");
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== db_destroy_implicit_release ===\n");

  arts_guid_t db = arts_guid_reserve(ARTS_GUID_DB, 0);
  uint64_t param = (uint64_t)db;

  arts_guid_t e_c = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_edt_create(creator_destroyer, 1, &param, 0,
                  &(arts_edt_hint_t){.rank = 0, .finish_event = e_c});
  arts_event_wait(e_c);

  arts_guid_t e_r = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_edt_create(recreator, 1, &param, 0,
                  &(arts_edt_hint_t){.rank = 0, .finish_event = e_r});
  arts_event_wait(e_r);

  arts_guid_t e_a = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t r = arts_edt_create(
      reader, 0, NULL, 1, &(arts_edt_hint_t){.rank = 0, .finish_event = e_a});
  arts_add_dependence(db, r, 0, DB_MODE_RO);
  arts_event_wait(e_a);

  arts_db_destroy(db);
  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
