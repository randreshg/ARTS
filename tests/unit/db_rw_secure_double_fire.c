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

/// @file db_rw_secure_double_fire.c
/// @brief Several remote-homed RW dependences of one EDT all see their
///        writes.
///
/// An EDT on rank 0 acquires SEVERAL distinct DBs in RW mode.  Rank 0 creates
/// them and homes them round-robin over the ranks, so with more than one rank
/// some live on another rank: where the write right goes back to the home at
/// release (PURGE) each of those takes a request/grant round with its home,
/// and where the creating rank keeps it (RETAIN) the writer takes it locally.
/// Where RW acquisitions are serialized (VAL, INV, EXCL) the EDT's
/// walk takes them one at a time in GUID order, and a grant that arrives on
/// another thread lets the walk past its slot (arts_db_rw_secure) only through
/// the wake that wins that slot's claim, and only while the walk is still at
/// that slot.  A walk that advanced wrongly would schedule the EDT before a
/// dep's data arrived (the writer sees a NULL or stale buffer) or never
/// (a hang).
///
/// The writer EDT stamps each DB with a per-DB sentinel; a per-DB RO reader
/// then verifies every DB got its write.  A NULL slot or a missing write ->
/// arts_abort; a stalled walk -> ctest TIMEOUT.
///
/// Runs in every node count and every arm: with one rank every home is local,
/// so the run checks the same multi-RW accounting through local hits, and
/// FLUSH serializes no acquisition, so there it is a plain multi-RW check.

#include "arts.h"

#include <stdint.h>
#include <stdio.h>

#define NDBS 4u
#define BASE 0x5EC00u

/// multi_writer: holds NDBS distinct DBs in RW; writes a per-DB sentinel into
/// each.  Every slot must carry a non-NULL, owned buffer.
void multi_writer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  for (uint32_t i = 0; i < depc; i++) {
    unsigned int *d = (unsigned int *)depv[i].ptr;
    if (d == NULL) {
      (void)fprintf(stderr,
                    "FAIL: db_rw_secure_double_fire slot %u NULL (premature "
                    "schedule)\n",
                    i);
      arts_abort(1);
      return;
    }
    d[0] = BASE + i;
  }
}

/// checker: RO on one DB; MUST observe that DB's sentinel.
void checker_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  unsigned int idx = (unsigned int)paramv[0];
  unsigned int *d = (unsigned int *)depv[0].ptr;
  if (d == NULL || d[0] != BASE + idx) {
    (void)fprintf(stderr,
                  "FAIL: db_rw_secure_double_fire DB %u got 0x%x want 0x%x\n",
                  idx, d ? d[0] : 0u, BASE + idx);
    arts_abort(1);
    return;
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== db_rw_secure_double_fire ===\n");

  unsigned int nranks = arts_get_total_ranks();

  /* Create NDBS distinct DBs, homed round-robin over the ranks. */
  arts_guid_t dbs[NDBS];
  for (unsigned int i = 0; i < NDBS; i++) {
    unsigned int home = (nranks > 1) ? (i % nranks) : 0u;
    void *p = NULL;
    dbs[i] = arts_db_create(&p, sizeof(unsigned int), ARTS_DB,
                            ARTS_DB_PROP_NONE, &(arts_db_hint_t){.rank = home});
    if (p != NULL) {
      ((unsigned int *)p)[0] = 0u;
    }
    arts_db_release(dbs[i], DB_MODE_RW);
  }

  /* The writer holds all NDBS in RW; where RW acquisitions are serialized its
   * walk takes them one at a time. */
  arts_guid_t e_w = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t w =
      arts_edt_create(multi_writer, 0, NULL, NDBS,
                      &(arts_edt_hint_t){.rank = 0, .finish_event = e_w});
  for (unsigned int i = 0; i < NDBS; i++) {
    arts_add_dependence(dbs[i], w, i, DB_MODE_RW);
  }
  arts_event_wait(e_w);

  /* Verify every DB received its sentinel (all RW deps secured + accounted). */
  arts_guid_t e_r = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  for (unsigned int i = 0; i < NDBS; i++) {
    uint64_t idx = i;
    arts_guid_t r =
        arts_edt_create(checker_edt, 1, &idx, 1,
                        &(arts_edt_hint_t){.rank = 0, .finish_event = e_r});
    arts_add_dependence(dbs[i], r, 0, DB_MODE_RO);
  }
  arts_event_wait(e_r);

  arts_printf("PASS: db_rw_secure_double_fire\n");
  arts_shutdown();
}

int main(int argc, char **argv) {
  /* Non-zero when a rank this process spawned ended badly: their exit status
     reaches nobody else, and a run with a dead rank did not succeed. */
  return arts_rt(argc, argv) != 0 ? 1 : 0;
}
