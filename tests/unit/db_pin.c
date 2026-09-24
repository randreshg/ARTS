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

/// @file db_pin.c
/// @brief Tests ARTS_DB_PIN (node-pinned) datablocks: RW access on home
///        rank and modification persistence.

#include "arts.h"
#include <string.h>

#include "../test_failure_status.h"

#define DB_SIZE 128

/// Verify ARTS_DB_PIN with RW mode.
void check_pin_rw(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  unsigned int *data = (unsigned int *)depv[0].ptr;
  bool ok = (data != NULL);
  if (ok) {
    for (unsigned int i = 0; i < DB_SIZE / sizeof(unsigned int); i++) {
      if (data[i] != i) {
        ok = false;
        break;
      }
    }
    if (ok) {
      for (unsigned int i = 0; i < DB_SIZE / sizeof(unsigned int); i++) {
        data[i] *= 2;
      }
    }
  }
  if (ok) {
    arts_printf("  PASS: DB_PIN with RW mode read/write OK\n");
  } else {
    arts_printf("  FAIL: DB_PIN with RW mode failed\n");
    arts_test_fail();
  }
}

/// Verify after RW modification.
void check_modified(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                    arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  unsigned int *data = (unsigned int *)depv[0].ptr;
  bool ok = (data != NULL);
  if (ok) {
    for (unsigned int i = 0; i < DB_SIZE / sizeof(unsigned int); i++) {
      if (data[i] != i * 2) {
        ok = false;
        break;
      }
    }
  }
  if (ok) {
    arts_printf("  PASS: DB_PIN modification persisted\n");
  } else {
    arts_printf("  FAIL: DB_PIN modification lost\n");
    arts_test_fail();
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== db_pin ===\n");

  arts_guid_t fe = arts_event_create(&ARTS_EVENT_HINT_FINISH);

  // Test 1: Create ARTS_DB_PIN and use DB_MODE_RW.  PIN home == this
  // rank by default; route hint forces it explicitly here so the test
  // is robust to hint-default changes.
  arts_guid_t pin_guid = arts_guid_reserve(ARTS_GUID_DB, 0);
  unsigned int *pin_data = (unsigned int *)arts_db_create_with_guid(
      pin_guid, DB_SIZE, ARTS_DB_PIN, ARTS_DB_PROP_NONE, NULL);
  for (unsigned int i = 0; i < DB_SIZE / sizeof(unsigned int); i++) {
    pin_data[i] = i;
  }
  arts_db_release(pin_guid, DB_MODE_RW);

  // Test 2: Verify RW modifications persisted.  A PIN datablock carries no
  // DB-level coherence, so two RW EDTs on the same DB are unordered unless an
  // explicit completion edge chains them.  e1 (modify) publishes an
  // output_event that gates e2 (verify), so e2 deterministically observes the
  // modified data.
  arts_guid_t oe1 = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
  arts_guid_t e2 =
      arts_edt_create(check_modified, 0, NULL, 2,
                      &(arts_edt_hint_t){.rank = 0, .finish_event = fe});

  arts_guid_t e1 = arts_edt_create(
      check_pin_rw, 0, NULL, 1,
      &(arts_edt_hint_t){.rank = 0, .finish_event = fe, .output_event = oe1});
  arts_add_dependence(pin_guid, e1, 0, DB_MODE_RW);
  arts_add_dependence(pin_guid, e2, 0, DB_MODE_RW);
  arts_add_dependence(oe1, e2, 1, DB_MODE_NULL); /* e2 runs after e1 releases */

  arts_event_wait(fe);
  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
