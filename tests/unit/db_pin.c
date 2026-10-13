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
/// @brief ARTS_DB_PIN blocks: created on a rank and used there.
///
/// A pinned block is a data block of a GUID kind like any other, created with
/// or without a placement hint, and its creator's bytes are what the next
/// acquirer reads.  Two write turns chained by an output event see each
/// other's writes; a pinned block carries no coherence of its own, so the
/// chain is the program's.  Destroying a pinned block the creator still holds
/// releases the hold and does not fail.

#include "arts.h"
#include <string.h>

#include "../test_failure_status.h"

#define DB_SIZE 128
#define NUM_ELEMS (DB_SIZE / sizeof(unsigned int))

/// paramv = {the block's GUID, its first value (element i holds first + i),
/// its element count, whether its create named a rank}.
void check_created(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  arts_guid_t guid = (arts_guid_t)paramv[0];
  unsigned int first = (unsigned int)paramv[1];
  unsigned int elems = (unsigned int)paramv[2];
  const unsigned int *data = (const unsigned int *)depv[0].ptr;
  bool ok = data != NULL && depv[0].guid == guid &&
            arts_guid_get_kind(guid) == ARTS_GUID_DB;
  for (unsigned int i = 0; ok && i < elems; i++) {
    ok = data[i] == first + i;
  }
  if (ok) {
    arts_printf("  PASS: pinned block created with %s carries its bytes\n",
                paramv[3] ? "a rank hint" : "no hint");
  } else {
    arts_printf("  FAIL: pinned block created with %s\n",
                paramv[3] ? "a rank hint" : "no hint");
    arts_test_fail();
  }
}

/// Doubles every element after checking the creator's values.
void check_pin_rw(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  unsigned int *data = (unsigned int *)depv[0].ptr;
  bool ok = (data != NULL);
  for (unsigned int i = 0; ok && i < NUM_ELEMS; i++) {
    ok = data[i] == i;
  }
  if (ok) {
    for (unsigned int i = 0; i < NUM_ELEMS; i++) {
      data[i] *= 2;
    }
    arts_printf("  PASS: DB_PIN with RW mode read/write OK\n");
  } else {
    arts_printf("  FAIL: DB_PIN with RW mode failed\n");
    arts_test_fail();
  }
}

void check_modified(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                    arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  unsigned int *data = (unsigned int *)depv[0].ptr;
  bool ok = (data != NULL);
  for (unsigned int i = 0; ok && i < NUM_ELEMS; i++) {
    ok = data[i] == i * 2;
  }
  if (ok) {
    arts_printf("  PASS: DB_PIN modification persisted\n");
  } else {
    arts_printf("  FAIL: DB_PIN modification lost\n");
    arts_test_fail();
  }
}

static void create_and_check(const arts_db_hint_t *hint, unsigned int first,
                             unsigned int elems, arts_guid_t scope) {
  void *ptr = NULL;
  arts_guid_t g = arts_db_create(&ptr, elems * sizeof(unsigned int),
                                 ARTS_DB_PIN, ARTS_DB_PROP_NONE, hint);
  for (unsigned int i = 0; i < elems; i++) {
    ((unsigned int *)ptr)[i] = first + i;
  }
  arts_db_release(g, DB_MODE_RW);
  uint64_t pv[4] = {(uint64_t)g, first, elems, hint != NULL};
  arts_guid_t e = arts_edt_create(check_created, 4, pv, 1,
                                  &(arts_edt_hint_t){.finish_event = scope});
  arts_add_dependence(g, e, 0, DB_MODE_RO);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== db_pin ===\n");

  arts_guid_t fe = arts_event_create(&ARTS_EVENT_HINT_FINISH);

  arts_db_hint_t here = {.rank = arts_get_current_rank()};
  create_and_check(&here, 1u, NUM_ELEMS, fe);
  create_and_check(NULL, 42u, 1u, fe);

  arts_guid_t pin_guid = arts_guid_reserve(ARTS_GUID_DB, 0);
  unsigned int *pin_data = (unsigned int *)arts_db_create_with_guid(
      pin_guid, DB_SIZE, ARTS_DB_PIN, ARTS_DB_PROP_NONE, NULL);
  for (unsigned int i = 0; i < NUM_ELEMS; i++) {
    pin_data[i] = i;
  }
  arts_db_release(pin_guid, DB_MODE_RW);

  arts_guid_t oe1 = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
  arts_guid_t e2 =
      arts_edt_create(check_modified, 0, NULL, 2,
                      &(arts_edt_hint_t){.rank = 0, .finish_event = fe});
  arts_guid_t e1 = arts_edt_create(
      check_pin_rw, 0, NULL, 1,
      &(arts_edt_hint_t){.rank = 0, .finish_event = fe, .output_event = oe1});
  arts_add_dependence(pin_guid, e1, 0, DB_MODE_RW);
  arts_add_dependence(pin_guid, e2, 0, DB_MODE_RW);
  arts_add_dependence(oe1, e2, 1, DB_MODE_NULL);

  void *held = NULL;
  arts_guid_t doomed =
      arts_db_create(&held, 64, ARTS_DB_PIN, ARTS_DB_PROP_NONE, NULL);
  arts_db_destroy(doomed);

  arts_event_wait(fe);
  if (arts_test_status() == 0) {
    arts_printf("PASS: db_pin\n");
  }
  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
