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

/// @file self_send_vs_dispatcher_parity.c
/// @brief A message sent to the sending rank itself must have the effect the
///        receive path gives it when it crosses ranks.
///
/// A coherence message whose destination is the local rank does not travel:
/// the sender runs the handler inline.  The same message from another rank is
/// decoded and dispatched by the receive path.  The two must decide the same
/// way (defer or run inline, and what a lookup that finds nothing does), or a
/// defect shows only on one rank or only across ranks.  Each iteration drives
/// every message family through both: a create, a write turn handed off
/// (publish and ownership transfer), read turns on every rank (rank 0's is
/// home-local, the others travel), and a destroy fanned out to every cache.
/// One rank exercises the inline path for all of them, several ranks the
/// dispatched one, and the oracle is the same on both.
///
/// Beside the fresh block, two blocks persist across iterations, written and
/// read every iteration in the same rounds: a sized one, whose reader ranks
/// keep the copy the previous iteration left, so every read is answered
/// against an older installed version, and a zero-size one, which has no bytes
/// to answer with.  Every reader of a sized block must see the iteration's
/// value; every reader of the zero-size block must be woken.
///
/// Every write turn is ordered before the reads that follow it, and the reads
/// before the next iteration's write, so it holds under every protocol.

#include "arts.h"

#include <stdint.h>
#include <stdio.h>

#define ITERS 200u
#define BASE 0x40000000u

/// writer: RW acquire; stamps the iteration's value into a sized block, takes
/// the turn and nothing else on the zero-size one.
static void writer_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  unsigned int *d = (unsigned int *)depv[0].ptr;
  if (d != NULL) {
    d[0] = (unsigned int)paramv[0];
  }
}

/// reader: RO acquire, ordered after the writer; must observe the new value.
static void reader_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  unsigned int *d = (unsigned int *)depv[0].ptr;
  unsigned int expect = (unsigned int)paramv[0];
  if (d == NULL || d[0] != expect) {
    (void)fprintf(stderr, "FAIL: parity mismatch — expected 0x%x got 0x%x\n",
                  expect, d ? d[0] : 0u);
    arts_abort(1);
  }
}

/// A read turn of the zero-size block: there is nothing to read, and the turn
/// running at all is the assertion.
static void empty_reader_edt(uint32_t paramc, const uint64_t *paramv,
                             uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
}

static arts_guid_t make_block(unsigned int bytes) {
  void *ptr = NULL;
  arts_guid_t db = arts_db_create(&ptr, bytes, ARTS_DB, ARTS_DB_PROP_NONE,
                                  &(arts_db_hint_t){.rank = 0});
  if (bytes != 0u) {
    ((unsigned int *)ptr)[0] = 0u;
  }
  arts_db_release(db, DB_MODE_RW);
  return db;
}

static void write_turn(arts_guid_t db, uint64_t *value, unsigned int rank,
                       arts_guid_t scope) {
  arts_guid_t w =
      arts_edt_create(writer_edt, 1, value, 1,
                      &(arts_edt_hint_t){.rank = rank, .finish_event = scope});
  arts_add_dependence(db, w, 0, DB_MODE_RW);
}

static void read_turn(arts_edt_t body, arts_guid_t db, uint64_t *value,
                      unsigned int rank, arts_guid_t scope) {
  arts_guid_t r =
      arts_edt_create(body, 1, value, 1,
                      &(arts_edt_hint_t){.rank = rank, .finish_event = scope});
  arts_add_dependence(db, r, 0, DB_MODE_RO);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== self_send_vs_dispatcher_parity ===\n");

  unsigned int nranks = arts_get_total_ranks();
  unsigned int W = (nranks > 1) ? 1u : 0u; /* remote writer when possible */
  arts_guid_t persistent = make_block(sizeof(unsigned int));
  arts_guid_t empty = make_block(0u);

  for (unsigned int it = 0; it < ITERS; it++) {
    uint64_t vnew = (uint64_t)(BASE + it);
    arts_guid_t db = make_block(sizeof(unsigned int));

    arts_guid_t e_w = arts_event_create(&ARTS_EVENT_HINT_FINISH);
    write_turn(db, &vnew, W, e_w);
    write_turn(persistent, &vnew, W, e_w);
    write_turn(empty, &vnew, W, e_w);
    arts_event_wait(e_w);

    arts_guid_t e_r = arts_event_create(&ARTS_EVENT_HINT_FINISH);
    for (unsigned int r = 0; r < nranks; r++) {
      read_turn(reader_edt, db, &vnew, r, e_r);
      read_turn(reader_edt, persistent, &vnew, r, e_r);
      read_turn(empty_reader_edt, empty, &vnew, r, e_r);
    }
    arts_event_wait(e_r);

    arts_db_destroy(db);
  }

  arts_printf("PASS: self_send_vs_dispatcher_parity %u iters x %u ranks\n",
              ITERS, nranks);
  arts_shutdown();
}

int main(int argc, char **argv) {
  /* Non-zero when a rank this process spawned ended badly: their exit status
     reaches nobody else, and a run with a dead rank did not succeed. */
  return arts_rt(argc, argv) != 0 ? 1 : 0;
}
