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

/// @file coherence_cxl_grant_chain.c
/// @brief Cross-rank visibility for ARTS_DB_CXL under EXCL x PURGE, where the
///        home moves PERMISSION and the payload never leaves the CXL window.
///
/// The three stages each isolate one link of the visibility chain:
///
///   1. Write-then-read across ranks.  A writer on one rank mutates the shared
///      payload; a reader on a different rank must see it.  Nothing ships the
///      bytes — the only reason the reader sees them is that the writer's
///      release flushed before the home heard about it, and the reader's grant
///      invalidated before the EDT ran.
///
///   2. Concurrent readers.  Several ranks hold RO grants at once over the same
///      payload and must all observe the last write; this is the case a naive
///      "one holder at a time" lock would serialize and a broken RO fan-out
///      would drop.
///
///   3. Writer after readers.  A writer must wait for every outstanding read
///      grant to come back before the home lets it in, and the increment it
///      then performs must be visible to a later reader.
///
/// Cross-rank RW order is not implied by registration order — an unordered
/// program is racy by design — so every stage that must observe the previous
/// one is gated on that stage's finish event.  What is being tested is
/// visibility under an order the program states, not an order the runtime
/// invents.
///
/// Requires ARTS_USE_CXL (hence EXCL x PURGE) and more than one rank to be
/// meaningful; on one rank it still exercises the local arbiter and the flush
/// pair, and passes.

#include "arts.h"
#include "../test_failure_status.h"

#define ELEMS 16u

/// Writes paramv[0] into every element, so a partially-invalidated reader is
/// caught rather than passing on a stale first word.
void cxl_writer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
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

/// Adds 1 to every element (read-modify-write: it must see the prior value).
void cxl_incrementer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  uint64_t *d = (uint64_t *)depv[0].ptr;
  if (d == NULL) {
    arts_printf("  FAIL: incrementer got a NULL payload pointer\n");
    arts_test_fail();
    return;
  }
  for (unsigned int i = 0; i < ELEMS; i++) {
    d[i] = d[i] + 1u;
  }
}

/// Asserts every element equals paramv[0] + index.  paramv[1] labels the stage.
void cxl_reader(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  const uint64_t *d = (const uint64_t *)depv[0].ptr;
  uint64_t base = paramv[0];
  unsigned int stage = (unsigned int)paramv[1];
  if (d == NULL) {
    arts_printf("  FAIL: stage %u reader got a NULL payload pointer\n", stage);
    arts_test_fail();
    return;
  }
  for (unsigned int i = 0; i < ELEMS; i++) {
    if (d[i] != base + i) {
      arts_printf("  FAIL: stage %u reader saw [%u]=%lu, expected %lu\n", stage,
                  i, (unsigned long)d[i], (unsigned long)(base + i));
      arts_test_fail();
      return;
    }
  }
  arts_printf("  PASS: stage %u reader saw base %lu\n", stage,
              (unsigned long)base);
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
  arts_printf("=== coherence_cxl_grant_chain (%u ranks) ===\n", nranks);

  /* A rank other than this one wherever the run has one; on a single-rank run
   * everything collapses onto rank 0, which still drives the whole state
   * machine (the home just happens to be local). */
  unsigned int other = (nranks > 1) ? 1u : 0u;
  unsigned int third = (nranks > 2) ? 2u : other;

  arts_guid_t shut = arts_edt_create(shutdown_edt, 0, NULL, 1, NULL);
  arts_guid_t all_done = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_add_dependence(all_done, shut, 0, DB_MODE_NULL);

  /* One CXL block for all three stages: the payload stays at one address in
   * the shared window for its whole life, which is the property under test. */
  void *ptr = NULL;
  arts_guid_t db = arts_db_create(&ptr, ELEMS * sizeof(uint64_t), ARTS_DB_CXL,
                                  ARTS_DB_PROP_NONE, NULL);
  if (db == NULL_GUID || ptr == NULL) {
    arts_printf("  FAIL: CXL create returned no payload\n");
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
      arts_edt_create(cxl_writer, 1, &base1, 1,
                      &(arts_edt_hint_t){.rank = other, .finish_event = e_w1});
  arts_add_dependence(db, w1, 0, DB_MODE_RW);

  arts_guid_t e_r1 = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  uint64_t p_r1[2] = {base1, 1};
  arts_guid_t r1 =
      arts_edt_create(cxl_reader, 2, p_r1, 2,
                      &(arts_edt_hint_t){.rank = third, .finish_event = e_r1});
  arts_add_dependence(db, r1, 0, DB_MODE_RO);
  arts_add_dependence(e_w1, r1, 1, DB_MODE_NULL);

  /* ── Stage 2: a second write, then every rank reads it concurrently ───── */
  arts_guid_t e_w2 = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  uint64_t base2 = 2000;
  arts_guid_t w2 = arts_edt_create(
      cxl_writer, 1, &base2, 2,
      &(arts_edt_hint_t){.rank = third, .finish_event = e_w2});
  arts_add_dependence(db, w2, 0, DB_MODE_RW);
  arts_add_dependence(e_r1, w2, 1, DB_MODE_NULL);

  arts_guid_t e_ro_all = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  for (unsigned int rank = 0; rank < nranks; rank++) {
    uint64_t p[2] = {base2, 2};
    arts_guid_t rd = arts_edt_create(
        cxl_reader, 2, p, 2,
        &(arts_edt_hint_t){.rank = rank, .finish_event = e_ro_all});
    arts_add_dependence(db, rd, 0, DB_MODE_RO);
    arts_add_dependence(e_w2, rd, 1, DB_MODE_NULL);
  }

  /* ── Stage 3: a writer behind the whole reader cohort ─────────────────── */
  arts_guid_t e_w3 = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t w3 =
      arts_edt_create(cxl_incrementer, 0, NULL, 2,
                      &(arts_edt_hint_t){.rank = other, .finish_event = e_w3});
  arts_add_dependence(db, w3, 0, DB_MODE_RW);
  arts_add_dependence(e_ro_all, w3, 1, DB_MODE_NULL);

  uint64_t p_r3[2] = {base2 + 1, 3};
  arts_guid_t r3 = arts_edt_create(
      cxl_reader, 2, p_r3, 2,
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
