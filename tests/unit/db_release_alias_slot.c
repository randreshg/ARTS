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

/// @file db_release_alias_slot.c
/// @brief Mid-EDT arts_db_release of a block one EDT names twice: the single
///        coherence hold must be dropped exactly once.
///
/// An EDT acquires each distinct block ONCE.  Naming it in slot 0 (RW) and slot
/// 1 (RW) gives one acquisition: slot 0 owns it, slot 1 is an alias holding
/// only its own buffer ref and descriptor pin.  A mid-EDT
/// arts_db_release(guid, mode) therefore has exactly one slot to release — the
/// OWNER — and must retire the alias with it, because an alias carries no hold
/// to give back and leaving it behind would let the epilogue mistake it for the
/// acquirer.
///
/// The failure this guards is a double drop of the one hold: release the alias
/// mid-body (or release the owner and leave the alias for the epilogue to
/// release as if it were the owner) and the count falls twice for one
/// acquisition.  A subsequent RW writer is then either granted ownership
/// prematurely (corruption -> reader mismatch -> arts_abort) or the count wraps
/// and the writer never gets ownership (hang -> ctest TIMEOUT).
///
/// Scenario: the EDT writes a value through slot 0, releases the GUID once
/// mid-body, and returns; the epilogue must find nothing left to release for
/// that block.  Then a follow-up RW writer and an RO reader prove the block is
/// grantable again and carries the follow-up's value.  The aliasing EDT and
/// the follow-up writer run on the last rank and the reader on the block's
/// home, so with peers the hold being dropped is a remote cache's.
///
/// No in-test watchdog: a hang is reaped by the ctest TIMEOUT.

#include "arts.h"

#include <stdint.h>
#include <stdio.h>

#include "../test_failure_status.h"

#define V0 0xAB0u
#define V1 0xAB1u

/// alias_releaser: same DB in slots 0 and 1 (both RW).  Writes V0, then mid-EDT
/// releases the DB once.
void alias_releaser(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                    arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  if (depc < 2 || depv[0].ptr == NULL) {
    (void)fprintf(stderr, "FAIL: db_release_alias_slot bad deps\n");
    arts_abort(1);
    return;
  }
  unsigned int *d = (unsigned int *)depv[0].ptr;
  d[0] = V0;
  /* Mid-EDT release of the block this EDT names twice: it names the block's one
   * acquisition, so the owner slot is released and the alias retires with it —
   * each dropping its own buffer ref and pin exactly once. */
  arts_db_release(depv[0].guid, DB_MODE_RW);
  /* The epilogue must now find nothing left to release for this block: both
   * slots are retired, and the single hold fell exactly once. */
}

/// followup_writer: a plain RW writer.  Only schedulable if the block's hold is
/// in a sane (grantable, non-wrapped) state after the aliasing EDT released.
void followup_writer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  unsigned int *d = (unsigned int *)depv[0].ptr;
  if (d != NULL) {
    d[0] = V1;
  }
}

/// checker: RO reader; MUST observe the follow-up writer's value (no
/// corruption from a premature ownership transfer caused by a double drop).
void checker_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  unsigned int *d = (unsigned int *)depv[0].ptr;
  if (d == NULL || d[0] != V1) {
    (void)fprintf(stderr,
                  "FAIL: db_release_alias_slot follow-up mismatch got 0x%x "
                  "(hold dropped twice / premature transfer)\n",
                  d ? d[0] : 0u);
    arts_abort(1);
    return;
  }
  arts_printf("PASS: db_release_alias_slot\n");
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== db_release_alias_slot ===\n");
  unsigned int last = arts_get_total_ranks() - 1;

  void *ptr = NULL;
  arts_guid_t db =
      arts_db_create(&ptr, sizeof(unsigned int), ARTS_DB, ARTS_DB_PROP_NONE,
                     &(arts_db_hint_t){.rank = 0});
  ((unsigned int *)ptr)[0] = 0u;
  arts_db_release(db, DB_MODE_RW);

  /* Phase 1: aliased EDT releases one alias slot mid-EDT, the other at
   * epilogue. */
  arts_guid_t e_a = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t a =
      arts_edt_create(alias_releaser, 0, NULL, 2,
                      &(arts_edt_hint_t){.rank = last, .finish_event = e_a});
  arts_add_dependence(db, a, 0, DB_MODE_RW);
  arts_add_dependence(db, a, 1, DB_MODE_RW);
  arts_event_wait(e_a);

  /* Phase 2: follow-up RW writer — needs a clean writer_count. */
  arts_guid_t e_w = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t w =
      arts_edt_create(followup_writer, 0, NULL, 1,
                      &(arts_edt_hint_t){.rank = last, .finish_event = e_w});
  arts_add_dependence(db, w, 0, DB_MODE_RW);
  arts_event_wait(e_w);

  /* Phase 3: RO reader verifies the follow-up value. */
  arts_guid_t e_r = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t r =
      arts_edt_create(checker_edt, 0, NULL, 1,
                      &(arts_edt_hint_t){.rank = 0, .finish_event = e_r});
  arts_add_dependence(db, r, 0, DB_MODE_RO);
  arts_event_wait(e_r);

  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
