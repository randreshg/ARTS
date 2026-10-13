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

/// @file await_publish_ack_shutdown.c
/// @brief A release waiting for its publish or write-back to be acknowledged
///        escapes the wait when the shutdown overtakes the acknowledgement.
///
/// A release that publishes, or writes back, synchronously blocks until the
/// home answers.  A publish flight's acknowledgement completes the flight and
/// wakes every waiter whose version it covers; the clear-to-send that names a
/// landing in the home's copy to a releasing rank that holds none, FLUSH's
/// acknowledgement and a PURGE grant return name their waiter by pointer.
/// Once teardown starts the progress threads stop draining, so that answer may
/// never arrive: the waiter must escape on its shutdown re-check (a timed wait
/// and a shutdown-state poll), or the blocked releaser is stranded and the
/// shutdown hangs.  The wait an escape abandons stays allocated, so an answer
/// that still arrives posts into live memory.
///
/// First a warmup of completed cross-rank write hand-offs of one block, every
/// hop gated on the previous hop's output event, in which each release off
/// the home that waits for the home's answer gets it.  Then, waited for by
/// nothing, while an EDT on the home starts the shutdown:
///   - a burst of blocks homed on rank 0, each written once on another rank
///     that has already written it once.  Under write-through publishing (VAL
///     x WT and INV x WT, either release policy) its release flies on the
///     landing the earlier write's acknowledgement left; under FLUSH the burst
///     write's own fetch brings one; under EXCL x PURGE the grant carries it
///     and nothing acknowledges the release; under INV x WB the release
///     publishes control only, with no landing;
///   - creators on the other ranks, each creating a block homed on rank 0,
///     writing it and releasing it: unless the create's answer has brought
///     one, the create's hold has no landing in the home's copy, so a release
///     that carries its payload to the home asks for one and waits for it.
/// Every write is ordered before the next write of its block, so the program
/// is valid under DB-WRF as well.
///
/// With the argument "contended" the burst is instead every writer of the
/// warmup's block at once, several on each rank, so a release finds its
/// rank's publish of the block already in flight and joins it.  Writers of
/// one block with no order among them are outside DB-WRF; that form runs only
/// where the OCR model holds.
///
/// Correct behavior is a clean teardown (every blocked releaser escapes); a
/// stranded releaser keeps its rank from exiting.

#include "arts.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define WARMUP 100u
#define BURST 64u

static void rw_bump_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  unsigned int *d = (unsigned int *)depv[0].ptr;
  if (d != NULL) {
    d[0] = d[0] + 1u;
  }
}

/* Creates a block homed on rank 0 from the rank it runs on, writes it under
 * the create's hold and releases it. */
static void away_creator_edt(uint32_t paramc, const uint64_t *paramv,
                             uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  void *ptr = NULL;
  arts_guid_t db =
      arts_db_create(&ptr, sizeof(unsigned int), ARTS_DB, ARTS_DB_PROP_NONE,
                     &(arts_db_hint_t){.rank = 0});
  ((unsigned int *)ptr)[0] = 1u;
  arts_db_release(db, DB_MODE_RW);
}

static void shutdown_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                         arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  /* Trigger teardown.  Releases of the burst may still be waiting for their
   * answers; each must escape via the shutdown re-check. */
  arts_printf("PASS: await_publish_ack_shutdown reached shutdown\n");
  arts_shutdown();
}

static arts_guid_t create_counter(void) {
  void *ptr = NULL;
  arts_guid_t db =
      arts_db_create(&ptr, sizeof(unsigned int), ARTS_DB, ARTS_DB_PROP_NONE,
                     &(arts_db_hint_t){.rank = 0});
  ((unsigned int *)ptr)[0] = 0u;
  arts_db_release(db, DB_MODE_RW);
  return db;
}

/* The ranks other than the home, in turn (the home itself on one rank). */
static unsigned int away_rank(unsigned int h, unsigned int nranks) {
  return (nranks > 1u) ? 1u + h % (nranks - 1u) : 0u;
}

static void write_once(arts_guid_t db, unsigned int rank, arts_guid_t scope) {
  arts_guid_t e =
      arts_edt_create(rw_bump_edt, 0, NULL, 1,
                      &(arts_edt_hint_t){.rank = rank, .finish_event = scope});
  arts_add_dependence(db, e, 0, DB_MODE_RW);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;

  int argc = (int)paramv[0];
  char **argv = (char **)paramv[1];
  bool contended = argc > 1 && strcmp(argv[1], "contended") == 0;

  arts_printf("=== await_publish_ack_shutdown%s ===\n",
              contended ? " contended" : "");

  unsigned int nranks = arts_get_total_ranks();
  arts_guid_t db = create_counter();

  /* Warmup: completed write hand-offs around the ranks, one at a time.  A
   * release off the home that waits for the home's answer gets it here,
   * before the shutdown, and where an acknowledgement leaves a landing every
   * rank keeps the one its last acknowledgement left. */
  for (unsigned int it = 0; it < WARMUP; it++) {
    arts_guid_t fe = arts_event_create(&ARTS_EVENT_HINT_FINISH);
    arts_guid_t prev = NULL_GUID;
    for (unsigned int h = 0; h < 2u * nranks; h++) {
      arts_guid_t oe = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
      arts_guid_t e = arts_edt_create(
          rw_bump_edt, 0, NULL, (prev == NULL_GUID) ? 1u : 2u,
          &(arts_edt_hint_t){
              .rank = h % nranks, .finish_event = fe, .output_event = oe});
      arts_add_dependence(db, e, 0, DB_MODE_RW);
      if (prev != NULL_GUID) {
        arts_add_dependence(prev, e, 1, DB_MODE_NULL);
      }
      prev = oe;
    }
    arts_event_wait(fe);
  }

  if (contended) {
    /* Every writer of the warmup's block at once, several per rank.  No
     * finish scope: shutdown_edt is its own. */
    for (unsigned int h = 0; h < BURST; h++) {
      write_once(db, h % nranks, NULL_GUID);
    }
  } else {
    /* Each burst block is written once, ahead of the burst, on the rank that
     * writes it again in the burst. */
    arts_guid_t blocks[BURST];
    arts_guid_t fe = arts_event_create(&ARTS_EVENT_HINT_FINISH);
    for (unsigned int h = 0; h < BURST; h++) {
      blocks[h] = create_counter();
      write_once(blocks[h], away_rank(h, nranks), fe);
    }
    arts_event_wait(fe);

    /* The burst: one more write per block, off its home, interleaved with
     * the away creators, so both kinds of release are in flight when the
     * shutdown comes.  No finish scope: shutdown_edt is its own. */
    for (unsigned int h = 0; h < BURST; h++) {
      write_once(blocks[h], away_rank(h, nranks), NULL_GUID);
      arts_edt_create(away_creator_edt, 0, NULL, 0,
                      &(arts_edt_hint_t){.rank = away_rank(h, nranks)});
    }
  }

  /* The shutdown EDT runs on the home; by the time it is scheduled the
   * burst's releases are in flight. */
  arts_edt_create(shutdown_edt, 0, NULL, 0, &(arts_edt_hint_t){.rank = 0});
}

int main(int argc, char **argv) {
  /* Non-zero when a rank this process spawned ended badly: their exit status
     reaches nobody else, and a run with a dead rank did not succeed. */
  return arts_rt(argc, argv) != 0 ? 1 : 0;
}
