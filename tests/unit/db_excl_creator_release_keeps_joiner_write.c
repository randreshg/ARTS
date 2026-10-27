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
/// @brief EXCL-only: an RW release with nothing to publish is still an
///        ordinary RW release, and a later real write through an RW
///        dependency survives it.
///
/// Under the EXCL protocol a dependency's RW hold and the creator's implicit
/// hold are the arm's ordinary write hold, released through the same
/// RW-release path.  A release is real even when its holder wrote nothing:
/// the point pinned here is that such a release still behaves like an
/// ordinary RW release and does not disturb a write that follows it.
///
/// Scenario, each phase in a finish scope of its own, the next phase created
/// by a continuation gated on it:
///   main_edt:    creates the block (its create hold, released at its
///                epilogue) and phase 1, which becomes ready inside main_edt's
///                body and so joins the write turn the create hold keeps
///                open: of the two holds, the first released is a non-final
///                release;
///   phase 1:     an RW dependency holder that writes nothing;
///   joiner EDT:  a real RW dependency on the same DB, acquired after both
///                holds were released; writes a sentinel;
///   reader EDT:  RO; MUST observe the joiner's update survived (not
///                clobbered by an earlier release).
///
/// If a release with nothing to publish corrupted the lock state, the
/// joiner's update would be lost -> reader mismatch -> arts_abort.  A stuck
/// lock counter instead manifests as a hang caught by the ctest TIMEOUT.
///
/// Meaningful only under EXCL, so built and registered only in an EXCL tree.

#include "arts.h"

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

/// reader: RO; the JOINer's update MUST survive the earlier releases.
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

/// Creates the next phase on rank 0, gated on the previous phase's finish
/// scope.  Every phase's paramv[0] is the DB.
static void then(arts_edt_t next, const uint64_t *param, arts_guid_t scope) {
  arts_guid_t n =
      arts_edt_create(next, 1, param, 1, &(arts_edt_hint_t){.rank = 0});
  arts_add_dependence(scope, n, 0, DB_MODE_NULL);
}

static void finale(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_shutdown();
}

/// Phase 3: RO reader confirms the JOINer's update survived.
static void read_phase(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t e_rd = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t rd =
      arts_edt_create(reader_edt, 0, NULL, 1,
                      &(arts_edt_hint_t){.rank = 0, .finish_event = e_rd});
  arts_add_dependence((arts_guid_t)paramv[0], rd, 0, DB_MODE_RO);
  then(finale, paramv, e_rd);
}

/// Phase 2: a real RW JOINer writes JOINER_VAL.  Runs once the creator EDT and
/// phase 1 have both completed, so both their releases precede this write.
static void join_phase(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t e_jn = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t jn =
      arts_edt_create(joiner_writer, 0, NULL, 1,
                      &(arts_edt_hint_t){.rank = 0, .finish_event = e_jn});
  arts_add_dependence((arts_guid_t)paramv[0], jn, 0, DB_MODE_RW);
  then(read_phase, paramv, e_jn);
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

  /* Phase 1: an EDT that holds the DB as an RW dep but writes nothing.  Its
   * epilogue's release must not disturb a write that follows it. */
  arts_guid_t e_cr = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t cr =
      arts_edt_create(creator_writes_nothing, 0, NULL, 1,
                      &(arts_edt_hint_t){.rank = 0, .finish_event = e_cr});
  arts_add_dependence(db, cr, 0, DB_MODE_RW);
  /* e_cr fires only once this EDT returns (its epilogue releases the create
   * hold and the creator-token) and phase 1 has released its RW hold. */
  uint64_t param = (uint64_t)db;
  then(join_phase, &param, e_cr);
}

int main(int argc, char **argv) {
  /* Non-zero when a rank this process spawned ended badly: their exit status
     reaches nobody else, and a run with a dead rank did not succeed. */
  return arts_rt(argc, argv) != 0 ? 1 : 0;
}
