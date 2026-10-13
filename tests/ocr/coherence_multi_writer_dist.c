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
** necessarily state or reflect those of the United States Government or any **
** agency thereof.                                                           **
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

/// @file coherence_multi_writer_dist.c
/// @brief Cross-rank writers of one block, with the exact count as the oracle.
///
/// Per iteration, N_DBS fresh blocks: the first half homed round-robin on
/// ranks 1..n-1 (never on rank 0, so every rank-0 writer's acquisition crosses
/// the wire), the second half round-robin on every rank, rank 0 included (a
/// home-local writer contends with remote ones).  The creator writes each
/// counter's starting value, 0, through the hold its create took, before it
/// wires any writer: it releases the second half's holds at once and keeps the
/// first half's to its end, so remote write requests for those blocks queue
/// behind a creator that has not released yet.  Then
/// N_WORKERS write turns, spread over every rank, each acquire one block and
/// increment it; every block gets WORKERS_PER_DB of them.  All iterations are
/// issued at once, so turns on many blocks and from many ranks are in flight
/// together.  Once an iteration's writers have finished, a reader on rank 0
/// acquires its blocks and must find each at exactly WORKERS_PER_DB: a lost or
/// doubled update, or a stale copy served to the reader, changes the count.
///
/// The model makes a write turn exclusive between ranks, not between the
/// writers on one rank, so the increment is atomic.  The writers are not
/// ordered among themselves, which the DB-WRF model does not admit.

#include "arts.h"

#include "../test_failure_status.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#define N_DBS 40
#define N_WORKERS 160
#define K_ITERS 10
/* i * 13 mod N_DBS visits every block equally often: 13 is coprime to N_DBS. */
#define WORKERS_PER_DB (N_WORKERS / N_DBS)

static void rw_worker_edt(uint32_t paramc, const uint64_t *paramv,
                          uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  _Atomic int *d = (_Atomic int *)depv[0].ptr;
  if (d == NULL) {
    arts_printf("FAIL: coherence_multi_writer_dist writer got no storage\n");
    arts_abort(1);
  }
  atomic_fetch_add_explicit(d, 1, memory_order_relaxed);
}

/* depv[0] = the iteration's finish event; depv[1..N_DBS] = its blocks, RO. */
static void verify_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  for (uint32_t i = 1; i < depc; i++) {
    const _Atomic int *d = (const _Atomic int *)depv[i].ptr;
    int observed =
        (d != NULL) ? atomic_load_explicit(d, memory_order_relaxed) : -1;
    if (observed != WORKERS_PER_DB) {
      arts_printf("FAIL: coherence_multi_writer_dist iteration %u block %u "
                  "= %d, want %d\n",
                  (unsigned int)paramv[0], i - 1u, observed, WORKERS_PER_DB);
      arts_abort(1);
    }
  }
}

static void done_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("PASS: coherence_multi_writer_dist %d iterations x %d blocks x "
              "%d writers each\n",
              K_ITERS, N_DBS, WORKERS_PER_DB);
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  unsigned int nranks = arts_get_total_ranks();
  if (nranks < 2u) {
    arts_printf("FAIL: coherence_multi_writer_dist needs 2+ ranks (got %u)\n",
                nranks);
    arts_test_fail();
    arts_shutdown();
    return;
  }

  arts_guid_t all = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t done =
      arts_edt_create(done_edt, 0, NULL, 1, &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(all, done, 0, DB_MODE_NULL);

  for (unsigned int iter = 0; iter < K_ITERS; iter++) {
    arts_guid_t dbs[N_DBS];
    for (unsigned int i = 0; i < N_DBS; i++) {
      unsigned int home =
          (i < N_DBS / 2u) ? 1u + i % (nranks - 1u) : i % nranks;
      void *raw = NULL;
      dbs[i] = arts_db_create(&raw, sizeof(int), ARTS_DB, ARTS_DB_PROP_NONE,
                              &(arts_db_hint_t){.rank = home});
      if (raw == NULL) {
        arts_printf("FAIL: coherence_multi_writer_dist create returned no "
                    "pointer\n");
        arts_abort(1);
      }
      *(int *)raw = 0;
      if (i >= N_DBS / 2u) {
        arts_db_release(dbs[i], DB_MODE_RW);
      }
    }

    arts_guid_t writers = arts_event_create(&ARTS_EVENT_HINT_FINISH);
    for (unsigned int i = 0; i < N_WORKERS; i++) {
      unsigned int db_idx = (iter * 7u + i * 13u) % N_DBS;
      arts_guid_t w = arts_edt_create(
          rw_worker_edt, 0, NULL, 1,
          &(arts_edt_hint_t){.rank = i % nranks, .finish_event = writers});
      arts_add_dependence(dbs[db_idx], w, 0, DB_MODE_RW);
    }

    uint64_t pv[1] = {iter};
    arts_guid_t v =
        arts_edt_create(verify_edt, 1, pv, 1u + N_DBS,
                        &(arts_edt_hint_t){.rank = 0u, .finish_event = all});
    arts_add_dependence(writers, v, 0, DB_MODE_NULL);
    for (unsigned int i = 0; i < N_DBS; i++) {
      arts_add_dependence(dbs[i], v, 1u + i, DB_MODE_RO);
    }
  }
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
