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

/// @file coherence_grant_chain_visibility.c
/// @brief A grant chain: remote write then remote read, a read cohort spread
///        over every rank, and a write turn requested while that cohort still
///        holds the block.
///
///   1. Write-then-read across ranks.  A writer on one rank mutates the shared
///      payload; a reader on a different rank must see it.
///
///   2. A distributed read cohort.  One reader per rank acquires the block
///      read-only and must observe the last write.  Each reader, once it holds
///      the block and has checked it, counts itself down on a "started" latch
///      and then keeps its hold for a bounded stretch before returning.
///
///   3. A writer requested under the cohort.  The incrementer depends on the
///      started latch and on the block in RW — not on the cohort's
///      completion — so its write request is issued while the readers still
///      hold.  It must see the cohort-era value and add 1 exactly once, and a
///      reader gated on it must see that increment.
///
/// Cross-rank order is not implied by registration order, so every stage that
/// must observe the previous one is gated on an event the program satisfies.
/// The verdict is on values only: every reader checks before it counts itself
/// down, so no write can precede its check whatever the protocol, and nothing
/// is asserted about timing.  The hold is what makes the write request meet
/// live read grants: under an exclusion protocol the request then waits at the
/// block's home behind the cohort and is granted by the cohort's last read
/// release.  Nothing here makes a reader join a write grant on the writer's
/// own rank, so the release path of such a joiner is not exercised.
///
/// On a single rank everything collapses onto rank 0, which still drives the
/// same sequence with a local home.

#include <time.h>

#include "arts.h"
#include "../test_failure_status.h"

#define ELEMS 16u
#define HOLD_NS 100000000ull

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/// Writes paramv[0] into every element, so a partially-invalidated reader is
/// caught rather than passing on a stale first word.
void chain_writer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint64_t *d = (uint64_t *)depv[0].ptr;
  if (d == NULL) {
    arts_printf("  FAIL: writer got a NULL payload pointer\n");
    arts_test_fail();
    return;
  }
  for (unsigned int i = 0; i < ELEMS; i++) {
    d[i] = paramv[0] + i;
  }
}

/// Checks every element equals paramv[0] + index, then adds 1 to each.
void chain_incrementer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint64_t *d = (uint64_t *)depv[0].ptr;
  if (d == NULL) {
    arts_printf("  FAIL: incrementer got a NULL payload pointer\n");
    arts_test_fail();
    return;
  }
  for (unsigned int i = 0; i < ELEMS; i++) {
    if (d[i] != paramv[0] + i) {
      arts_printf("  FAIL: incrementer saw [%u]=%lu, expected %lu\n", i,
                  (unsigned long)d[i], (unsigned long)(paramv[0] + i));
      arts_test_fail();
      return;
    }
  }
  for (unsigned int i = 0; i < ELEMS; i++) {
    d[i] = d[i] + 1u;
  }
}

static bool check_payload(const uint64_t *d, uint64_t base,
                          unsigned int stage) {
  if (d == NULL) {
    arts_printf("  FAIL: stage %u reader got a NULL payload pointer\n", stage);
    return false;
  }
  for (unsigned int i = 0; i < ELEMS; i++) {
    if (d[i] != base + i) {
      arts_printf("  FAIL: stage %u reader saw [%u]=%lu, expected %lu\n", stage,
                  i, (unsigned long)d[i], (unsigned long)(base + i));
      return false;
    }
  }
  return true;
}

/// Asserts every element equals paramv[0] + index.  paramv[1] labels the stage.
void chain_reader(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  unsigned int stage = (unsigned int)paramv[1];
  if (!check_payload((const uint64_t *)depv[0].ptr, paramv[0], stage)) {
    arts_test_fail();
    return;
  }
  arts_printf("  PASS: stage %u reader saw base %lu\n", stage,
              (unsigned long)paramv[0]);
}

/// A cohort reader: checks the block, counts itself down on the started latch
/// (paramv[2]) on every path so a failure never strands the writer, then holds
/// the block for a bounded stretch.  The payload is not read after the count
/// down: the writer may run from then on, and under a protocol that does not
/// exclude it a later read would race it.
void cohort_reader(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  bool ok = check_payload((const uint64_t *)depv[0].ptr, paramv[0], 2u);
  if (!ok) {
    arts_test_fail();
  }
  arts_event_satisfy((arts_guid_t)paramv[2], NULL_GUID);
  uint64_t until = now_ns() + HOLD_NS;
  while (now_ns() < until) {
  }
  if (ok) {
    arts_printf("  PASS: stage 2 reader saw base %lu\n",
                (unsigned long)paramv[0]);
  }
}

void shutdown_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  unsigned int nranks = arts_get_total_ranks();
  arts_printf("=== coherence_grant_chain_visibility (%u ranks) ===\n", nranks);

  /* A rank other than this one wherever the run has one; on a single-rank run
   * everything collapses onto rank 0. */
  unsigned int other = (nranks > 1) ? 1u : 0u;
  unsigned int third = (nranks > 2) ? 2u : other;

  /* Shutdown waits for the final reader AND every cohort reader: under a
   * protocol that lets the writer in beside live readers, the chain can end
   * while a reader still holds. */
  arts_guid_t shut = arts_edt_create(shutdown_edt, 0, NULL, 1, NULL);
  arts_guid_t all_done = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_add_dependence(all_done, shut, 0, DB_MODE_NULL);

  /* One block for all three stages: its identity stays fixed for its whole
   * life, which is what lets each stage build on the previous one's state. */
  void *ptr = NULL;
  arts_guid_t db = arts_db_create(&ptr, ELEMS * sizeof(uint64_t), ARTS_DB,
                                  ARTS_DB_PROP_NONE, NULL);
  if (db == NULL_GUID || ptr == NULL) {
    arts_printf("  FAIL: create returned no payload\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  for (unsigned int i = 0; i < ELEMS; i++) {
    ((uint64_t *)ptr)[i] = 0;
  }
  arts_db_release(db, DB_MODE_RW);

  /* ── Stage 1: remote write, remote read ───────────────────────────────── */
  arts_guid_t e_w1 = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  uint64_t base1 = 1000;
  arts_guid_t w1 =
      arts_edt_create(chain_writer, 1, &base1, 1,
                      &(arts_edt_hint_t){.rank = other, .finish_event = e_w1});
  arts_add_dependence(db, w1, 0, DB_MODE_RW);

  arts_guid_t e_r1 = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  uint64_t p_r1[2] = {base1, 1};
  arts_guid_t r1 =
      arts_edt_create(chain_reader, 2, p_r1, 2,
                      &(arts_edt_hint_t){.rank = third, .finish_event = e_r1});
  arts_add_dependence(db, r1, 0, DB_MODE_RO);
  arts_add_dependence(e_w1, r1, 1, DB_MODE_NULL);

  /* ── Stage 2: a second write, then one holding reader per rank ────────── */
  arts_guid_t e_w2 = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  uint64_t base2 = 2000;
  arts_guid_t w2 = arts_edt_create(
      chain_writer, 1, &base2, 2,
      &(arts_edt_hint_t){.rank = third, .finish_event = e_w2});
  arts_add_dependence(db, w2, 0, DB_MODE_RW);
  arts_add_dependence(e_r1, w2, 1, DB_MODE_NULL);

  arts_guid_t started =
      arts_event_create(&ARTS_EVENT_HINT_LATCH((int32_t)nranks));
  for (unsigned int rank = 0; rank < nranks; rank++) {
    uint64_t p[3] = {base2, 2, (uint64_t)started};
    arts_guid_t rd = arts_edt_create(
        cohort_reader, 3, p, 2,
        &(arts_edt_hint_t){.rank = rank, .finish_event = all_done});
    arts_add_dependence(db, rd, 0, DB_MODE_RO);
    arts_add_dependence(e_w2, rd, 1, DB_MODE_NULL);
  }

  /* ── Stage 3: a write turn requested while the cohort holds ───────────── */
  arts_guid_t e_w3 = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t w3 =
      arts_edt_create(chain_incrementer, 1, &base2, 2,
                      &(arts_edt_hint_t){.rank = other, .finish_event = e_w3});
  arts_add_dependence(db, w3, 0, DB_MODE_RW);
  arts_add_dependence(started, w3, 1, DB_MODE_NULL);

  uint64_t p_r3[2] = {base2 + 1, 3};
  arts_guid_t r3 = arts_edt_create(
      chain_reader, 2, p_r3, 2,
      &(arts_edt_hint_t){.rank = 0, .finish_event = all_done});
  arts_add_dependence(db, r3, 0, DB_MODE_RO);
  arts_add_dependence(e_w3, r3, 1, DB_MODE_NULL);
}

int main(int argc, char **argv) {
  /* Non-zero when a rank this process spawned ended badly (their exit status
     reaches nobody else) or when a check inside an EDT on this rank failed. */
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
