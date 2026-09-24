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

/// @file launcher_no_respawn.c
/// @brief The local launcher brings up exactly the configured ranks, each once,
///        with its identity handed down, and no spawned rank launches again.
///
/// The launcher spawns the configured number of non-master ranks and hands
/// each its rank through ARTS_RANK, which is also what keeps a spawned rank
/// from running the launcher itself (a rank that did would re-spawn the whole
/// cluster).  Asserted per rank, by a probe placed on it:
///   1. it runs on the rank it was placed on, and sees the configured total;
///   2. on every non-master rank, ARTS_RANK is set and names that rank;
///   3. it stamps its own slot of a marker block, and the master then reads
///      every slot 0..total-1 filled exactly once with its own rank's marker,
///      so the live ranks are exactly 0..total-1 with no gap and no duplicate.
/// A rank that never came up leaves its probe unrun (the run times out).
/// Protocol-agnostic: the launcher carries no coherence state.

#include "arts.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../test_failure_status.h"

#define MARKER_BASE 0x5A5A0000u

void rank_probe_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                    arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  unsigned int expected_rank = (unsigned int)paramv[0];
  unsigned int total = (unsigned int)paramv[1];
  uint32_t *marks = (uint32_t *)depv[0].ptr;

  unsigned int me = arts_get_current_rank();
  unsigned int seen_total = arts_get_total_ranks();

  if (me != expected_rank) {
    arts_printf("FAIL: launcher_no_respawn probe ran on %u, expected %u\n", me,
                expected_rank);
    arts_test_fail();
    return;
  }
  if (seen_total != total) {
    arts_printf(
        "FAIL: launcher_no_respawn rank %u sees %u ranks, expected %u\n", me,
        seen_total, total);
    arts_test_fail();
    return;
  }

  /* The master has no ARTS_RANK; every spawned rank must carry its own. */
  const char *rank_env = getenv("ARTS_RANK");
  if (me != 0) {
    if (rank_env == NULL || (unsigned int)strtoul(rank_env, NULL, 10) != me) {
      arts_printf("FAIL: launcher_no_respawn rank %u has ARTS_RANK=%s\n", me,
                  rank_env ? rank_env : "(unset)");
      arts_test_fail();
      return;
    }
  }
  if (marks == NULL) {
    arts_printf("FAIL: launcher_no_respawn rank %u got a NULL marker block\n",
                me);
    arts_test_fail();
    return;
  }
  marks[me] = MARKER_BASE | me;
}

void verify_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  unsigned int total = (unsigned int)paramv[0];
  const uint32_t *marks = (const uint32_t *)depv[0].ptr;
  if (marks == NULL) {
    arts_printf("FAIL: launcher_no_respawn verifier got a NULL marker block\n");
    arts_test_fail();
    return;
  }
  for (unsigned int r = 0; r < total; r++) {
    if (marks[r] != (MARKER_BASE | r)) {
      arts_printf("FAIL: launcher_no_respawn slot %u = 0x%x, expected 0x%x\n",
                  r, marks[r], MARKER_BASE | r);
      arts_test_fail();
    }
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  unsigned int total = arts_get_total_ranks();

  void *ptr = NULL;
  arts_guid_t db =
      arts_db_create(&ptr, total * sizeof(uint32_t), ARTS_DB, ARTS_DB_PROP_NONE,
                     &(arts_db_hint_t){.rank = 0});
  for (unsigned int r = 0; r < total; r++) {
    ((uint32_t *)ptr)[r] = 0u;
  }
  arts_db_release(db, DB_MODE_RW);

  /* One probe at a time: each write turn is ordered before the next. */
  for (unsigned int r = 0; r < total; r++) {
    arts_guid_t fe = arts_event_create(&ARTS_EVENT_HINT_FINISH);
    uint64_t params[2] = {(uint64_t)r, (uint64_t)total};
    arts_guid_t p =
        arts_edt_create(rank_probe_edt, 2, params, 1,
                        &(arts_edt_hint_t){.rank = r, .finish_event = fe});
    arts_add_dependence(db, p, 0, DB_MODE_RW);
    arts_event_wait(fe);
  }

  arts_guid_t vfe = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  uint64_t vparams[1] = {(uint64_t)total};
  arts_guid_t v =
      arts_edt_create(verify_edt, 1, vparams, 1,
                      &(arts_edt_hint_t){.rank = 0, .finish_event = vfe});
  arts_add_dependence(db, v, 0, DB_MODE_RO);
  arts_event_wait(vfe);

  arts_printf("PASS: launcher_no_respawn (%u ranks, each once, no recursive "
              "spawn)\n",
              total);
  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
