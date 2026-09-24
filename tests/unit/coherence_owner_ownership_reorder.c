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
/// @file coherence_owner_ownership_reorder.c
/// @brief Unordered write turns from every rank on one block homed on rank 0:
///        every hand-over of the write right between ranks carries the latest
///        bytes and strands no queued requester.
///
/// Two shapes, each ending in an exact sum read after every writer finished:
///  (1) Deep batches: INCS_PER_BATCH incrementers per batch, round-robin over
///      every rank and gated only on the block, so the home always holds a
///      queue of remote requesters and the write right moves rank to rank
///      back to back.  With two progress threads (2n_io) the messages of one
///      hand-over and of the next one addressed to the same rank can be
///      dispatched in either order.
///  (2) Short contended steps: three incrementers per step, one on the home and
///      two remote contenders -- on ranks 1 and 2 when there are three or more
///      ranks, both on rank 1 with two -- so a holder's release meets the
///      home's demand for the next requester at every step and the write right
///      also returns to its home between remote holders.
///
/// A lost hand-over strands a queued requester (the run times out); a hand-over
/// that ships stale bytes drops increments (the sum is short).
///
/// A write dependence is exclusive per rank, not per EDT: several incrementers
/// holding the block on one rank run concurrently on that rank's workers, so
/// the increment is atomic, as for any shared-memory counter.
///
/// Needs 2+ ranks (SKIP on one).  Its writers are unordered, so it is outside
/// the DB-WRF model.

#include "arts.h"

#include <stdint.h>
#include <stdio.h>

#include "../test_failure_status.h"

#define N_BATCHES 40
#define INCS_PER_BATCH 64
#define STEPS 80u

static void inc_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                    arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  uint64_t *data = (uint64_t *)depv[0].ptr;
  if (data == NULL) {
    arts_printf("FAIL: coherence_owner_ownership_reorder incrementer got a "
                "NULL block\n");
    arts_test_fail();
    return;
  }
  __atomic_fetch_add(&data[0], (uint64_t)1, __ATOMIC_RELAXED);
}

static void check_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  const uint64_t *data = (const uint64_t *)depv[0].ptr;
  uint64_t expected = paramv[0];
  if (data == NULL || data[0] != expected) {
    arts_printf("FAIL: coherence_owner_ownership_reorder sum %lld != %llu "
                "(a hand-over shipped stale bytes or a write was lost)\n",
                data ? (long long)data[0] : -1LL, (unsigned long long)expected);
    arts_test_fail();
    return;
  }
  arts_printf("PASS: coherence_owner_ownership_reorder summed to %llu\n",
              (unsigned long long)expected);
}

static arts_guid_t spawn_inc(arts_guid_t db, unsigned int rank,
                             arts_guid_t fe) {
  arts_guid_t w =
      arts_edt_create(inc_edt, 0, NULL, 1,
                      &(arts_edt_hint_t){.rank = rank, .finish_event = fe});
  arts_add_dependence(db, w, 0, DB_MODE_RW);
  return w;
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  unsigned int nranks = arts_get_total_ranks();
  if (nranks < 2) {
    arts_printf("SKIP: coherence_owner_ownership_reorder requires 2+ ranks "
                "(got %u)\n",
                nranks);
    arts_shutdown();
    return;
  }

  void *ptr = NULL;
  arts_guid_t db =
      arts_db_create(&ptr, sizeof(uint64_t), ARTS_DB, ARTS_DB_PROP_NONE,
                     &(arts_db_hint_t){.rank = 0});
  ((uint64_t *)ptr)[0] = 0;
  arts_db_release(db, DB_MODE_RW);

  uint64_t total = 0;
  for (int b = 0; b < N_BATCHES; b++) {
    arts_guid_t fe = arts_event_create(&ARTS_EVENT_HINT_FINISH);
    for (int i = 0; i < INCS_PER_BATCH; i++) {
      spawn_inc(db, ((unsigned)b * INCS_PER_BATCH + (unsigned)i) % nranks, fe);
      total++;
    }
    arts_event_wait(fe);
  }

  unsigned int r1 = 1u;
  unsigned int r2 = (nranks > 2) ? 2u : 1u;
  for (unsigned int s = 0; s < STEPS; s++) {
    arts_guid_t fe = arts_event_create(&ARTS_EVENT_HINT_FINISH);
    spawn_inc(db, 0u, fe);
    spawn_inc(db, r1, fe);
    spawn_inc(db, r2, fe);
    total += 3;
    arts_event_wait(fe);
  }

  arts_guid_t fe = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t chk =
      arts_edt_create(check_edt, 1, &total, 1,
                      &(arts_edt_hint_t){.rank = 0, .finish_event = fe});
  arts_add_dependence(db, chk, 0, DB_MODE_RO);
  arts_event_wait(fe);

  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
