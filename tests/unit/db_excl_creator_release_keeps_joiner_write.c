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

/// @file db_excl_creator_release_keeps_joiner_write.c
/// @brief EXCL-only: a creator EDT that writes nothing through its DB
///        dependency is still tracked and still runs an ordinary RW release
///        at its epilogue; a JOINer's later real write through an RW
///        dependency must survive that release.
///
/// Under the EXCL protocol the creator's implicit hold is the arm's ordinary
/// write hold, seeded when the block is made and released through the same
/// RW-release path any dependency's release uses.  The release is real, not
/// skipped: the point pinned here is that a release with nothing to publish
/// still behaves like an ordinary RW release and does not disturb a write
/// that follows it.
///
/// Scenario (single creator EDT that does NOT write through an RW dep, plus a
/// JOINer that DOES write through a real RW dep):
///   creator EDT: arts_db_create (ARTS_DB, default props) but writes nothing
///                via a dependency; its epilogue's RW release must not
///                disturb a write that comes after it.
///   joiner EDT:  a real RW dependency on the same DB; writes a sentinel.
///   reader EDT:  RO; MUST observe the JOINer's update survived (not
///                clobbered by the creator's release).
///
/// If the creator's release corrupted the lock state, the JOINer's update
/// would be lost -> reader mismatch -> arts_abort.  A stuck lock counter
/// instead manifests as a hang caught by the ctest TIMEOUT.
///
/// config_specific: meaningful only under EXCL.  Self-skips (prints SKIP and
/// returns 0) under every other protocol.

#include "arts.h"

#if !defined(ARTS_PROTOCOL_EXCL)

#include <stdio.h>

int main(void) {
  (void)printf("SKIP db_excl_creator_release_keeps_joiner_write: EXCL-only\n");
  return 0;
}

#else /* ARTS_PROTOCOL_EXCL */

#include <stdint.h>
#include <stdio.h>

#define JOINER_VAL 0x707070u

/// creator_writes_nothing: takes the DB as an RW dep on purpose but performs
/// no write.  The point is purely that its epilogue's release does not
/// corrupt the lock for a writer that follows.
void creator_writes_nothing(uint32_t paramc, const uint64_t *paramv,
                            uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  /* Intentionally no DB write, to isolate the release itself. */
}

/// joiner_writer: a real RW writer through a dependency — takes the lock,
/// publishes JOINER_VAL, releases at epilogue.
void joiner_writer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  unsigned int *d = (unsigned int *)depv[0].ptr;
  if (d != NULL) {
    d[0] = JOINER_VAL;
  }
}

/// reader: RO; the JOINer's update MUST survive the creator's earlier release.
void reader_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  unsigned int *d = (unsigned int *)depv[0].ptr;
  if (d == NULL || d[0] != JOINER_VAL) {
    (void)fprintf(
        stderr,
        "FAIL: db_excl_creator_release_keeps_joiner_write joiner update "
        "lost (got 0x%x want 0x%x)\n",
        d ? d[0] : 0u, JOINER_VAL);
    arts_abort(1);
    return;
  }
  arts_printf("PASS: db_excl_creator_release_keeps_joiner_write\n");
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== db_excl_creator_release_keeps_joiner_write ===\n");

  /* Create the DB on rank 0 (home == self when run single-node; the
   * creator's release is local to its own epilogue regardless). */
  void *ptr = NULL;
  arts_guid_t db =
      arts_db_create(&ptr, sizeof(unsigned int), ARTS_DB, ARTS_DB_PROP_NONE,
                     &(arts_db_hint_t){.rank = 0});
  /* Under EXCL *addr is the writable stub buffer; pre-seed a non-sentinel so a
   * lost JOINer write would be detectable. */
  if (ptr != NULL) {
    ((unsigned int *)ptr)[0] = 0u;
  }

  /* Phase 1: a creator EDT that holds the DB as an RW dep but writes
   * nothing.  Its epilogue's release must not disturb a write that follows
   * it. */
  arts_guid_t e_cr = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t cr =
      arts_edt_create(creator_writes_nothing, 0, NULL, 1,
                      &(arts_edt_hint_t){.rank = 0, .finish_event = e_cr});
  arts_add_dependence(db, cr, 0, DB_MODE_RW);
  arts_event_wait(e_cr);

  /* Phase 2: a real RW JOINer writes JOINER_VAL. */
  arts_guid_t e_jn = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t jn =
      arts_edt_create(joiner_writer, 0, NULL, 1,
                      &(arts_edt_hint_t){.rank = 0, .finish_event = e_jn});
  arts_add_dependence(db, jn, 0, DB_MODE_RW);
  arts_event_wait(e_jn);

  /* Phase 3: RO reader confirms the JOINer's update survived. */
  arts_guid_t e_rd = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t rd =
      arts_edt_create(reader_edt, 0, NULL, 1,
                      &(arts_edt_hint_t){.rank = 0, .finish_event = e_rd});
  arts_add_dependence(db, rd, 0, DB_MODE_RO);
  arts_event_wait(e_rd);

  arts_shutdown();
}

int main(int argc, char **argv) {
  /* Non-zero when a rank this process spawned ended badly: their exit status
     reaches nobody else, and a run with a dead rank did not succeed. */
  return arts_rt(argc, argv) != 0 ? 1 : 0;
}

#endif /* ARTS_PROTOCOL_EXCL */
