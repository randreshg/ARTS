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

/// @file coherence_cxl_alias_slots.c
/// @brief A coherent ARTS_DB_CXL block under the two dependence shapes the
///        acquire engine treats specially: a slot pair naming one block, and
///        two EDTs naming one pair of blocks in opposite orders.
///
/// WHAT THIS PROGRAM CHECKS, exactly: that both shapes COMPLETE, that the bytes
/// each EDT wrote are all readable afterwards, and that nothing crashes.  It is
/// a value and liveness guard, not a discriminator.
///
/// Phase 1 — one block in slot 0 (RW) and slot 1 (RW) of one EDT.  Both slots
/// must yield the same non-NULL payload address, a write through one must be
/// visible through the other, and a later writer and reader must still get the
/// block, which is what an acquire/release accounting error would break.
///
/// Phase 2 — two blocks and two EDTs, one taking them (A,B) and the other
/// (B,A), released together through their own gate events so their acquisition
/// windows overlap.  Each EDT owns one word index and writes its tag at that
/// index in BOTH blocks, so there is no write-write race and the checker must
/// read every EDT's tag out of every block.
///
/// What it does NOT check: whether the engine classified slot 1 as an alias, or
/// whether it took the blocks through the serialized walk.  At ONE RANK neither
/// is observable from a program.  An EXCL grant is held by a RANK, not by an
/// EDT, so a second acquisition of a block this rank already holds joins the
/// held grant instead of waiting: two EDTs can hold both blocks at once, no
/// acquisition ever blocks, and no hold-and-wait cycle can form whatever order
/// they are taken in.  A run that distinguishes the classifications needs a
/// second rank.
///
/// Requires ARTS_USE_CXL (hence EXCL x PURGE).  Single rank.

#include "arts.h"
#include "../test_failure_status.h"

#include <stdint.h>
#include <stdio.h>

#define MAGIC 0xC0FFEEu
#define ELEMS 4u
#define TAG_A 0xA1A1A1A1u
#define TAG_B 0xB2B2B2B2u

void aliased_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  if (depc < 2) {
    (void)fprintf(stderr, "FAIL: expected depc>=2 got %u\n", depc);
    arts_test_fail();
    return;
  }
  unsigned int *a = (unsigned int *)depv[0].ptr;
  unsigned int *b = (unsigned int *)depv[1].ptr;
  if (a == NULL || b == NULL) {
    (void)fprintf(stderr, "FAIL: NULL alias ptr (%p,%p)\n", (void *)a,
                  (void *)b);
    arts_test_fail();
    return;
  }
  if (a != b) {
    (void)fprintf(stderr, "FAIL: alias slots address different bytes\n");
    arts_test_fail();
    return;
  }
  a[0] = MAGIC;
  if (b[0] != MAGIC) {
    (void)fprintf(stderr, "FAIL: alias slots not the same block (b=0x%x)\n",
                  b[0]);
    arts_test_fail();
  }
}

void followup_writer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  unsigned int *d = (unsigned int *)depv[0].ptr;
  if (d == NULL) {
    (void)fprintf(stderr, "FAIL: follow-up writer got NULL\n");
    arts_test_fail();
    return;
  }
  d[0] = MAGIC + 1u;
}

void checker_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  unsigned int *d = (unsigned int *)depv[0].ptr;
  if (d == NULL || d[0] != MAGIC + 1u) {
    (void)fprintf(stderr, "FAIL: follow-up mismatch got 0x%x\n",
                  d ? d[0] : 0u);
    arts_test_fail();
    return;
  }
  arts_printf("PASS: alias\n");
}

/// Slots 0 and 1 are the two blocks in THIS EDT's order; slot 2 is the gate.
/// paramv = {word index this EDT owns, its tag}.
void pair_worker_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  if (depc < 2) {
    (void)fprintf(stderr, "FAIL: pair worker expected depc>=2 got %u\n", depc);
    arts_test_fail();
    return;
  }
  uint64_t *first = (uint64_t *)depv[0].ptr;
  uint64_t *second = (uint64_t *)depv[1].ptr;
  if (first == NULL || second == NULL) {
    (void)fprintf(stderr, "FAIL: pair worker got NULL (%p,%p)\n", (void *)first,
                  (void *)second);
    arts_test_fail();
    return;
  }
  if (first == second) {
    (void)fprintf(stderr, "FAIL: pair worker got one block twice\n");
    arts_test_fail();
    return;
  }
  unsigned int idx = (unsigned int)paramv[0];
  uint64_t tag = paramv[1];
  first[idx] = tag;
  second[idx] = tag;
}

/// Slots 0 and 1 are the two blocks RO.
void pair_checker_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  if (depc < 2) {
    (void)fprintf(stderr, "FAIL: pair checker expected depc>=2 got %u\n",
                  depc);
    arts_test_fail();
    return;
  }
  const uint64_t *a = (const uint64_t *)depv[0].ptr;
  const uint64_t *b = (const uint64_t *)depv[1].ptr;
  if (a == NULL || b == NULL) {
    (void)fprintf(stderr, "FAIL: pair checker got NULL\n");
    arts_test_fail();
    return;
  }
  if (a[0] != TAG_A || b[0] != TAG_A) {
    (void)fprintf(stderr, "FAIL: first worker's tag missing (a=0x%lx b=0x%lx)\n",
                  (unsigned long)a[0], (unsigned long)b[0]);
    arts_test_fail();
    return;
  }
  if (a[1] != TAG_B || b[1] != TAG_B) {
    (void)fprintf(stderr,
                  "FAIL: second worker's tag missing (a=0x%lx b=0x%lx)\n",
                  (unsigned long)a[1], (unsigned long)b[1]);
    arts_test_fail();
    return;
  }
  arts_printf("PASS: coherence_cxl_alias_slots\n");
}

static arts_guid_t make_cxl_block(uint64_t elems) {
  void *ptr = NULL;
  arts_guid_t g = arts_db_create(&ptr, elems * sizeof(uint64_t), ARTS_DB_CXL,
                                 ARTS_DB_PROP_NONE, NULL);
  if (g == NULL_GUID || ptr == NULL) {
    (void)fprintf(stderr, "FAIL: CXL create returned no payload\n");
    arts_test_fail();
    return NULL_GUID;
  }
  for (uint64_t i = 0; i < elems; i++) {
    ((uint64_t *)ptr)[i] = 0;
  }
  arts_db_release(g, DB_MODE_RW);
  return g;
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== coherence_cxl_alias_slots ===\n");

  /* ── Phase 1: one block, two RW slots of one EDT ──────────────────────── */
  void *ptr = NULL;
  arts_guid_t db = arts_db_create(&ptr, sizeof(unsigned int), ARTS_DB_CXL,
                                  ARTS_DB_PROP_NONE, NULL);
  if (db == NULL_GUID || ptr == NULL) {
    (void)fprintf(stderr, "FAIL: CXL create returned no payload\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  ((unsigned int *)ptr)[0] = 0u;
  arts_db_release(db, DB_MODE_RW);

  arts_guid_t e_alias = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t a =
      arts_edt_create(aliased_edt, 0, NULL, 2,
                      &(arts_edt_hint_t){.rank = 0,
                                         .finish_event = e_alias});
  arts_add_dependence(db, a, 0, DB_MODE_RW);
  arts_add_dependence(db, a, 1, DB_MODE_RW);
  arts_event_wait(e_alias);

  arts_guid_t e_wr = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t w =
      arts_edt_create(followup_writer, 0, NULL, 1,
                      &(arts_edt_hint_t){.rank = 0, .finish_event = e_wr});
  arts_add_dependence(db, w, 0, DB_MODE_RW);
  arts_event_wait(e_wr);

  arts_guid_t e_rd = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t r =
      arts_edt_create(checker_edt, 0, NULL, 1,
                      &(arts_edt_hint_t){.rank = 0, .finish_event = e_rd});
  arts_add_dependence(db, r, 0, DB_MODE_RO);
  arts_event_wait(e_rd);

  /* ── Phase 2: two blocks, two EDTs, opposite acquisition orders ───────── */
  arts_guid_t blk_a = make_cxl_block(ELEMS);
  arts_guid_t blk_b = make_cxl_block(ELEMS);
  if (blk_a == NULL_GUID || blk_b == NULL_GUID) {
    arts_shutdown();
    return;
  }

  /* Each worker waits on its own gate, so both are fully wired before either
   * can start and their acquisition windows overlap. */
  arts_guid_t gate_ab = arts_event_create(NULL);
  arts_guid_t gate_ba = arts_event_create(NULL);
  arts_guid_t e_ab = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t e_ba = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  if (gate_ab == NULL_GUID || gate_ba == NULL_GUID) {
    (void)fprintf(stderr, "FAIL: gate event create failed\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }

  uint64_t p_ab[2] = {0u, TAG_A};
  arts_guid_t w_ab =
      arts_edt_create(pair_worker_edt, 2, p_ab, 3,
                      &(arts_edt_hint_t){.rank = 0, .finish_event = e_ab});
  arts_add_dependence(blk_a, w_ab, 0, DB_MODE_RW);
  arts_add_dependence(blk_b, w_ab, 1, DB_MODE_RW);

  uint64_t p_ba[2] = {1u, TAG_B};
  arts_guid_t w_ba =
      arts_edt_create(pair_worker_edt, 2, p_ba, 3,
                      &(arts_edt_hint_t){.rank = 0, .finish_event = e_ba});
  arts_add_dependence(blk_b, w_ba, 0, DB_MODE_RW);
  arts_add_dependence(blk_a, w_ba, 1, DB_MODE_RW);

  arts_add_dependence(gate_ab, w_ab, 2, DB_MODE_NULL);
  arts_add_dependence(gate_ba, w_ba, 2, DB_MODE_NULL);

  /* Both workers are wired; release them together so their acquisition
   * windows overlap.  Each wait then blocks until that worker's scope
   * completes. */
  arts_event_satisfy(gate_ab, NULL_GUID);
  arts_event_satisfy(gate_ba, NULL_GUID);
  arts_event_wait(e_ab);
  arts_event_wait(e_ba);

  arts_guid_t e_pair = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t pc =
      arts_edt_create(pair_checker_edt, 0, NULL, 2,
                      &(arts_edt_hint_t){.rank = 0, .finish_event = e_pair});
  arts_add_dependence(blk_a, pc, 0, DB_MODE_RO);
  arts_add_dependence(blk_b, pc, 1, DB_MODE_RO);
  arts_event_wait(e_pair);

  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
