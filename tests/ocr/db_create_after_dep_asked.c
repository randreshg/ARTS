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

/// @file db_create_after_dep_asked.c
/// @brief A create on a rank where a DEPENDENCE on the same labeled GUID has
///        already touched the DB.
///
/// A create makes the block: the object at its home, which every operation on
/// the label is ordered against.  A cache is a different thing — the landing
/// and coherence state a rank keeps for a DB, which any rank makes when it
/// first touches one, with or without a create.  So a dependence dispatched
/// before its label's create leaves a cache on its own rank and a deferred
/// request at the home, and the create that then runs on that rank is still
/// the block's create: it records the creator's hold in the cache already
/// there, gives the block its first image, announces it, and the deferred
/// request is drained against the block that now exists.
///
/// Both request kinds get a lane, because they ask through different state: a
/// remote RW dependence opens an ownership request, a remote RO dependence a
/// fetch or a snapshot, and on the exclusion arm both live in the one word
/// the create's hold goes in.  Each lane, on a non-home rank: the dependence
/// is dispatched first (it touches the DB and asks the home), then the
/// label's single create on that rank, then a writer ORDERED AFTER that
/// dependence (its output event), and last a reader ordered after the
/// writer.  The write chain has
/// to be ordered end to end for the read to have one legal value — unordered
/// writers of one block may land in either order.
///
/// What it catches: a create that read the cache as a block that exists and
/// so created nothing (its pointer is NULL), a deferred request the create
/// never releases (the ctest TIMEOUT), a first user handed no storage, and a
/// value chain that loses the ordered writer's bytes.  Only the writer writes:
/// the create is not ordered against it (the read the create may wake is what
/// gates the writer, and the create can still be running then), so a value
/// from the create would give the final read two legal answers.  Which of the two
/// orders the runtime picks — the dependence first, or the create first — is
/// not enforceable from inside the program, and the assertions hold in both:
/// the create is the block's creator either way, and the ordered chain pins
/// the value.  Every arm records the hold in its own state (the grant word,
/// or the exclusion arm's cache word), so this runs everywhere.  Needs >= 2
/// ranks; SKIPs cleanly otherwise.

#include "arts.h"

#include <stdint.h>
#include <stdio.h>

#include "../test_failure_status.h"

#define SENTINEL 0x5A5A5A5Au
#define LANES 2u /* one lane asks RW first, one asks RO first */

static void fail(const char *what) {
  arts_printf("FAIL: db_create_after_dep_asked: %s (rank %u)\n", what,
              arts_get_current_rank());
  arts_test_fail();
  arts_shutdown();
}

/* The dependence that touches the DB FIRST: it asks for a block that does not
 * exist yet, which is what leaves the cache the create then finds.  Its own
 * value is unordered against the create, so nothing is asserted about it —
 * a NULL pointer for a sized block IS a failure, because every acquire of a
 * block that exists is owed storage.  Its output event orders the writer
 * after it, so the sentinel is the last write and the final read has one
 * legal value. */
void early_rw_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  if (depv[0].ptr == NULL) {
    fail("an RW dependence that asked before the create got no storage");
    return;
  }
  /* No write: the ordered writer below is the only one, so the final read has
   * one legal answer whichever way the create and this task interleave. */
}

void early_ro_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  if (depv[0].ptr == NULL) {
    fail("an RO dependence issued before the create got no storage");
    return;
  }
  /* Any value is legal here (the read is unordered against every write), so
   * the load is only for its side effect of touching the copy. */
  volatile uint32_t sink = *(const uint32_t *)depv[0].ptr;
  (void)sink;
}

/* The create on that same rank.  It is the label's only create, so it makes
 * the block and is handed its pointer — whether it runs before the dependence
 * (making the cache itself) or after it (recording its hold in the cache that
 * dependence left).  A NULL here means the create read that cache as a block
 * that already exists, which for a label created once is a defect. */
void late_creator_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t child = (arts_guid_t)paramv[0];
  void *p = arts_db_create_with_guid(child, sizeof(uint32_t), ARTS_DB,
                                     ARTS_DB_PROP_NONE, NULL);
  if (p == NULL) {
    fail("the block's only create was handed no pointer");
    return;
  }
  /* Deliberately no write.  Nothing orders this task against the writer
   * below — the read it may have woken is what gates that writer, and this
   * task can still be running then — so a value written here would race the
   * one the program does order, and the final read would have two legal
   * answers.  The pointer is what this task is here to assert. */
  arts_db_release(child, DB_MODE_RW);
}

/* The write the program DOES order: after the early acquirer (slot 0 is that
 * acquirer's output event) and before the reader (its own output event). */
void writer_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  if (depv[1].ptr == NULL) {
    fail("the ordered writer got no storage");
    return;
  }
  *(uint32_t *)depv[1].ptr = SENTINEL;
}

/* paramv = { done event }.  depv[0] = the writer's output event, depv[1] =
 * the block, RO — ordered after the write, so the VALUE is asserted. */
void ordered_reader_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  arts_guid_t done = (arts_guid_t)paramv[0];
  const uint32_t *p = (const uint32_t *)depv[1].ptr;
  if (p == NULL) {
    fail("the ordered reader got no storage");
    return;
  }
  if (*p != SENTINEL) {
    arts_printf("FAIL: db_create_after_dep_asked: ordered read 0x%X, expected "
                "the writer's 0x%X (rank %u)\n",
                *p, SENTINEL, arts_get_current_rank());
    arts_test_fail();
    arts_shutdown();
    return;
  }
  arts_event_satisfy(done, NULL_GUID);
}

void done_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("PASS: db_create_after_dep_asked %u lanes\n", LANES);
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== db_create_after_dep_asked ===\n");

  unsigned int nranks = arts_get_total_ranks();
  if (nranks < 2) {
    arts_printf("SKIP db_create_after_dep_asked: needs >= 2 ranks (have %u)\n",
                nranks);
    arts_shutdown();
    return;
  }

  /* Two labeled children homed on rank 0, so every lane's rank is remote to
   * the home and its dependence really does ask over the wire. */
  arts_guid_t range = arts_guid_reserve_range(ARTS_GUID_DB, LANES, 0);
  arts_guid_t done = arts_event_create(&ARTS_EVENT_HINT_LATCH(LANES));
  arts_guid_t tail = arts_edt_create(done_edt, 0, NULL, 1,
                                     &(arts_edt_hint_t){.rank = 0});
  arts_add_dependence(done, tail, 0, DB_MODE_NULL);

  for (unsigned int lane = 0; lane < LANES; lane++) {
    arts_guid_t child = arts_guid_from_index(range, lane);
    unsigned int r = 1u + (lane % (nranks - 1u));
    uint64_t cpv[1] = {(uint64_t)child};

    /* The dependence that asks first — RW on lane 0, RO on lane 1, because
     * the two ask through different state — and the single create of that
     * label on the same rank, dispatched after it. */
    arts_guid_t asked = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
    arts_guid_t early =
        arts_edt_create(lane == 0 ? early_rw_edt : early_ro_edt, 0, NULL, 1,
                        &(arts_edt_hint_t){.rank = r, .output_event = asked});
    arts_add_dependence(child, early, 0,
                        lane == 0 ? DB_MODE_RW : DB_MODE_RO);
    arts_edt_create(late_creator_edt, 1, cpv, 0,
                    &(arts_edt_hint_t){.rank = r});

    /* The ordered write — after the early acquirer, so the two writes of this
     * block are ordered and the sentinel is the one that stands — and the
     * read that must observe it. */
    arts_guid_t written = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
    arts_guid_t w =
        arts_edt_create(writer_edt, 0, NULL, 2,
                        &(arts_edt_hint_t){.rank = r, .output_event = written});
    arts_add_dependence(asked, w, 0, DB_MODE_NULL);
    arts_add_dependence(child, w, 1, DB_MODE_RW);

    uint64_t rpv[1] = {(uint64_t)done};
    arts_guid_t rd = arts_edt_create(ordered_reader_edt, 1, rpv, 2,
                                     &(arts_edt_hint_t){.rank = 0});
    arts_add_dependence(written, rd, 0, DB_MODE_NULL);
    arts_add_dependence(child, rd, 1, DB_MODE_RO);
  }
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
