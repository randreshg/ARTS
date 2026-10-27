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

/// @file coherence_owner_confirm_gate.c
/// @brief Exploit + regression for the WB ownership-confirm gate.
///
/// Guards the stale-RO window: when ownership transfers home(0)->W under
/// write-back, the home names W in rw_holder only when W's
/// MSG_DB_GRANT_CONFIRM arrives.  Were W to run its RW EDT before that, an RO
/// acquire causally ordered AFTER W's write but racing W's CONFIRM would be
/// redirected by the home to the OLD owner and read a stale value; W runs only
/// after the home's MSG_DB_GRANT_CONFIRM_ACK, which closes the window.
///
/// Per iteration (a clean write -> read happens-before chain), each EDT in a
/// finish scope of its own:
///   reset  (rank 0, RW): re-own on the home, write vprev   [e_reset]
///   writer (rank W, RW): transfer 0->W, write vnew         [e_writer]
///   reader (rank 0, RO): MUST read vnew                    [e_reader]
/// Each EDT is created by a continuation gated on the previous one's scope.
///
/// Registered on two ranks, with one and with two progress threads per rank;
/// two, which reorder one peer's messages, widen the race.  On one rank there
/// is no transfer.
/// A stranded waiter is caught by the ctest TIMEOUT (no in-test watchdog).

#include "arts.h"

#include <stdint.h>
#include <stdio.h>

#define ITERS 500u
#define RESET_BASE 0x10000000u
#define WRITE_BASE 0x20000000u

/// reset: re-establish ownership on home (rank 0) and stamp a per-iter
/// sentinel.
void reset_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  unsigned int *d = (unsigned int *)depv[0].ptr;
  if (d != NULL) {
    d[0] = (unsigned int)paramv[0];
  }
}

/// writer: RW acquire forces the home->W ownership transfer; write the new
/// value.
void writer_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  unsigned int *d = (unsigned int *)depv[0].ptr;
  if (d != NULL) {
    d[0] = (unsigned int)paramv[0];
  }
}

/// reader: RO acquire, causally after the writer; MUST observe the writer's
/// value.  arts_abort(1) on any mismatch so the exit code is non-zero.
void reader_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  unsigned int *d = (unsigned int *)depv[0].ptr;
  unsigned int expect = (unsigned int)paramv[0];
  if (d == NULL || d[0] != expect) {
    (void)fprintf(stderr, "FAIL: stale RO read — expected 0x%x got 0x%x\n",
                  expect, d ? d[0] : 0u);
    arts_abort(1);
  }
}

/// paramv of a phase: the iteration, the DB.
enum { P_IT, P_DB, P_COUNT };

/// Creates phase `next` on rank 0, gated on the previous phase's scope.
static void then(arts_edt_t next, const uint64_t *pv, arts_guid_t scope) {
  arts_guid_t n =
      arts_edt_create(next, P_COUNT, pv, 1, &(arts_edt_hint_t){.rank = 0});
  arts_add_dependence(scope, n, 0, DB_MODE_NULL);
}

static void writer_phase(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                         arts_edt_dep_t depv[]);

/// Phase 1: reset EDT re-establishes ownership on home (rank 0).
static void reset_phase(const uint64_t *paramv) {
  uint64_t vprev = (uint64_t)(RESET_BASE + paramv[P_IT]);
  arts_guid_t e_reset = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t rst =
      arts_edt_create(reset_edt, 1, &vprev, 1,
                      &(arts_edt_hint_t){.rank = 0, .finish_event = e_reset});
  arts_add_dependence((arts_guid_t)paramv[P_DB], rst, 0, DB_MODE_RW);
  /* The writer phase runs once the reset EDT has completed and e_reset has
   * fired, meaning ownership is back on rank 0. */
  then(writer_phase, paramv, e_reset);
}

static void reader_phase(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                         arts_edt_dep_t depv[]);

/// Gated on the reader's scope: the next iteration, or the end of the run.
static void next_iteration(uint32_t paramc, const uint64_t *paramv,
                           uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  if (paramv[P_IT] + 1u < ITERS) {
    uint64_t pv[P_COUNT] = {paramv[P_IT] + 1u, paramv[P_DB]};
    reset_phase(pv);
    return;
  }
  arts_printf("PASS: coherence_owner_confirm_gate %u iterations\n", ITERS);
  arts_shutdown();
}

/// Phase 2: writer EDT on rank W; forces 0->W ownership transfer.
static void writer_phase(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                         arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  /* Writer rank (remote when possible). */
  unsigned int W = (arts_get_total_ranks() > 1) ? 1u : 0u;
  uint64_t vnew = (uint64_t)(WRITE_BASE + paramv[P_IT]);
  arts_guid_t e_writer = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t wr =
      arts_edt_create(writer_edt, 1, &vnew, 1,
                      &(arts_edt_hint_t){.rank = W, .finish_event = e_writer});
  arts_add_dependence((arts_guid_t)paramv[P_DB], wr, 0, DB_MODE_RW);
  /* The writer EDT acquires RW ownership (home redirects 0->W) and writes
   * vnew.  The window the gate closes: the home flips rw_holder only on W's
   * CONFIRM, which without the gate could arrive AFTER the writer EDT has
   * completed and e_writer has fired. */
  then(reader_phase, paramv, e_writer);
}

/// Phase 3: reader EDT on home (rank 0); MUST see vnew.
static void reader_phase(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                         arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  /* Causally ordered AFTER the writer (this phase runs only once e_writer
   * fired), but the RO acquire's request can race W's in-flight CONFIRM at
   * the home — the window the gate closes. */
  uint64_t vnew = (uint64_t)(WRITE_BASE + paramv[P_IT]);
  arts_guid_t e_reader = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t rd =
      arts_edt_create(reader_edt, 1, &vnew, 1,
                      &(arts_edt_hint_t){.rank = 0, .finish_event = e_reader});
  arts_add_dependence((arts_guid_t)paramv[P_DB], rd, 0, DB_MODE_RO);
  then(next_iteration, paramv, e_reader);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== coherence_owner_confirm_gate ===\n");

  void *ptr = NULL;
  arts_guid_t db =
      arts_db_create(&ptr, sizeof(unsigned int), ARTS_DB, ARTS_DB_PROP_NONE,
                     &(arts_db_hint_t){.rank = 0});
  ((unsigned int *)ptr)[0] = 0u;
  arts_db_release(db, DB_MODE_RW);

  uint64_t pv[P_COUNT] = {0u, (uint64_t)db};
  reset_phase(pv);
}

int main(int argc, char **argv) {
  /* Non-zero when a rank this process spawned ended badly: their exit status
     reaches nobody else, and a run with a dead rank did not succeed. */
  return arts_rt(argc, argv) != 0 ? 1 : 0;
}
