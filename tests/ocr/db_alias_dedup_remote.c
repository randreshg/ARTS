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

/// @file db_alias_dedup_remote.c
/// @brief The alias dedup where the acquiring rank is NOT the block's home.
///
/// One EDT acquires each distinct block ONCE: a later slot naming a block an
/// earlier slot of the same EDT already secured is an alias on that single
/// acquisition — one coherence acquire, one release.  db_alias_dedup states the
/// rule with the aliasing EDT on the block's home, where the rank's own copy of
/// the block is the answer to every way of asking for it, so a second
/// acquisition is indistinguishable from the first.
///
/// Off the home it is distinguishable, and what every arm owes here is that the
/// HOME sees the aliased write.  Two copies of one block inside one EDT mean
/// two write-backs, and the copy the EDT never wrote through can be the one
/// that lands last: the EDT's whole write to the block disappears with no error
/// anywhere.
///
/// Whatever modes the slots ask for, they carry the SAME pointer: the engine
/// classifies an EDT's whole dependence vector before anything fires, so one
/// block is one acquisition on every arm and every other slot naming it is
/// handed that acquisition's payload.  A store through the write slot is
/// therefore readable through the read ones with no ordering in between.
///
/// The one acquisition an EDT gets for a block carries the STRONGEST mode any
/// of its slots asks for, so a write slot owns it whatever its index.  Let a
/// read own it instead and the EDT's stores go out through a read release:
/// nothing is exclusive, and on an arm whose release is what carries the bytes
/// home they are dropped outright.  Three EDTs state it: read-then-write,
/// read-write-read, and a mid-body release that must release the WRITE
/// acquisition and not the read slot that names the same block.
///
/// The phases are event-ordered rather than left to the runtime, so the test
/// states the same thing under every memory model.

#include "arts.h"
#include "../test_failure_status.h"

#include <stdint.h>

#define MAGIC 0xA11A5u
#define MAGIC2 0xA11A6u
#define MAGIC3 0xA11A7u
#define MAGIC4 0xA11A8u

/// The aliasing EDT, placed away from the block's home: slot 0 and slot 1 name
/// one block, both RW.
void aliased_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  if (depc < 2) {
    arts_printf("FAIL: db_alias_dedup_remote expected depc>=2 got %u\n", depc);
    arts_test_fail();
    return;
  }
  unsigned int *a = (unsigned int *)depv[0].ptr;
  unsigned int *b = (unsigned int *)depv[1].ptr;
  if (a == NULL || b == NULL) {
    arts_printf("FAIL: db_alias_dedup_remote NULL alias ptr (%p,%p)\n",
                (void *)a, (void *)b);
    arts_test_fail();
    return;
  }
  if (a != b) {
    arts_printf("FAIL: db_alias_dedup_remote alias slots are not the same "
                "payload (%p,%p) on rank %u\n",
                (void *)a, (void *)b, arts_get_current_rank());
    arts_test_fail();
    return;
  }
  a[0] = MAGIC;
  if (b[0] != MAGIC) {
    arts_printf("FAIL: db_alias_dedup_remote alias slots not same DB "
                "(b=0x%x)\n",
                b[0]);
    arts_test_fail();
  }
}

/// A READ slot and a WRITE slot on one block, declared read-first.  The write
/// must own the one acquisition, so the store reaches the home.
void mixed_alias_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  if (depc < 2) {
    arts_printf("FAIL: db_alias_dedup_remote mixed expected depc>=2 got %u\n",
                depc);
    arts_test_fail();
    return;
  }
  unsigned int *ro = (unsigned int *)depv[0].ptr;
  unsigned int *rw = (unsigned int *)depv[1].ptr;
  if (ro == NULL || rw == NULL) {
    arts_printf("FAIL: db_alias_dedup_remote mixed NULL slot ptr (%p,%p)\n",
                (void *)ro, (void *)rw);
    arts_test_fail();
    return;
  }
  if (ro != rw) {
    arts_printf("FAIL: db_alias_dedup_remote mixed slots are not the same "
                "payload (%p,%p) on rank %u\n",
                (void *)ro, (void *)rw, arts_get_current_rank());
    arts_test_fail();
    return;
  }
  rw[0] = MAGIC2;
  /* One acquisition: the store is readable through the read slot with no
   * ordering of any kind in between. */
  if (ro[0] != MAGIC2) {
    arts_printf("FAIL: db_alias_dedup_remote mixed read slot did not see the "
                "write (0x%x)\n",
                ro[0]);
    arts_test_fail();
  }
}

/// Three slots on one block — read, write, read, declared in that order.  The
/// write in the middle owns the acquisition and the two reads alias it.
void triple_alias_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  if (depc < 3) {
    arts_printf("FAIL: db_alias_dedup_remote triple expected depc>=3 got %u\n",
                depc);
    arts_test_fail();
    return;
  }
  unsigned int *ro0 = (unsigned int *)depv[0].ptr;
  unsigned int *rw = (unsigned int *)depv[1].ptr;
  unsigned int *ro2 = (unsigned int *)depv[2].ptr;
  if (ro0 == NULL || rw == NULL || ro2 == NULL) {
    arts_printf("FAIL: db_alias_dedup_remote triple NULL slot ptr "
                "(%p,%p,%p)\n",
                (void *)ro0, (void *)rw, (void *)ro2);
    arts_test_fail();
    return;
  }
  if (ro0 != rw || ro2 != rw) {
    arts_printf("FAIL: db_alias_dedup_remote triple slots are not the same "
                "payload (%p,%p,%p) on rank %u\n",
                (void *)ro0, (void *)rw, (void *)ro2, arts_get_current_rank());
    arts_test_fail();
    return;
  }
  rw[0] = MAGIC3;
  if (ro0[0] != MAGIC3 || ro2[0] != MAGIC3) {
    arts_printf("FAIL: db_alias_dedup_remote triple read slots did not see the "
                "write (0x%x,0x%x)\n",
                ro0[0], ro2[0]);
    arts_test_fail();
  }
}

/// Releases the block in the middle of its body and then satisfies an event a
/// reader depends on.  The release names the WRITE acquisition, so by the time
/// the event fires the block's bytes are wherever a reader will look for them;
/// releasing the read slot that names the same block instead would leave the
/// write held until the EDT's epilogue, after the reader was let go.
/// paramv = { the event to satisfy }.
void mid_release_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  if (depc < 2) {
    arts_printf("FAIL: db_alias_dedup_remote midrel expected depc>=2 got %u\n",
                depc);
    arts_test_fail();
    return;
  }
  unsigned int *rw = (unsigned int *)depv[1].ptr;
  arts_guid_t db = depv[1].guid;
  if (rw == NULL) {
    arts_printf("FAIL: db_alias_dedup_remote midrel NULL write slot\n");
    arts_test_fail();
    return;
  }
  rw[0] = MAGIC4;
  arts_db_release(db, DB_MODE_RW);
  arts_event_satisfy((arts_guid_t)paramv[0], NULL_GUID);
}

/// Reads the block at its home after an aliasing EDT released it.
/// paramv = { value the home must hold, non-zero to announce the verdict }.
void home_checker_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  unsigned int want = (unsigned int)paramv[0];
  unsigned int *d = (unsigned int *)depv[0].ptr;
  if (d == NULL || d[0] != want) {
    arts_printf("FAIL: db_alias_dedup_remote home lost the aliased write, "
                "got 0x%x want 0x%x\n",
                d ? d[0] : 0u, want);
    arts_test_fail();
    return;
  }
  if (paramv[1] != 0u && arts_test_status() == 0) {
    /* The one verdict line, and only if nothing this rank ran has failed: a
     * phase that failed earlier has already printed its FAIL, and a run that
     * announced both would read as passing. */
    arts_printf("PASS: db_alias_dedup_remote\n");
  }
}

/* Runs one aliasing EDT on `writer_rank`, then a home-side check of `want`. */
static void phase(arts_edt_t body, arts_guid_t db, unsigned int writer_rank,
                  unsigned int slots, const arts_db_access_mode_t *modes,
                  unsigned int want, unsigned int announce) {
  arts_guid_t e_w = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t w = arts_edt_create(
      body, 0, NULL, slots,
      &(arts_edt_hint_t){.rank = writer_rank, .finish_event = e_w});
  for (unsigned int i = 0; i < slots; i++) {
    arts_add_dependence(db, w, i, modes[i]);
  }
  arts_event_wait(e_w);

  uint64_t want_v[2] = {(uint64_t)want, (uint64_t)announce};
  arts_guid_t e_rd = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t r =
      arts_edt_create(home_checker_edt, 2, want_v, 1,
                      &(arts_edt_hint_t){.rank = 0, .finish_event = e_rd});
  arts_add_dependence(db, r, 0, DB_MODE_RO);
  arts_event_wait(e_rd);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== db_alias_dedup_remote ===\n");

  /* The block is homed on rank 0 and the aliasing EDT runs elsewhere, so its
   * acquisitions are the ones that travel.  A single-rank run has nowhere
   * else to put it and degenerates to db_alias_dedup. */
  unsigned int nranks = arts_get_total_ranks();
  unsigned int writer_rank = (nranks > 1u) ? 1u : 0u;

  void *ptr = NULL;
  arts_guid_t db =
      arts_db_create(&ptr, sizeof(unsigned int), ARTS_DB, ARTS_DB_PROP_NONE,
                     &(arts_db_hint_t){.rank = 0});
  ((unsigned int *)ptr)[0] = 0u;
  arts_db_release(db, DB_MODE_RW);

  const arts_db_access_mode_t rw_rw[2] = {DB_MODE_RW, DB_MODE_RW};
  const arts_db_access_mode_t ro_rw[2] = {DB_MODE_RO, DB_MODE_RW};
  const arts_db_access_mode_t ro_rw_ro[3] = {DB_MODE_RO, DB_MODE_RW,
                                             DB_MODE_RO};
  phase(aliased_edt, db, writer_rank, 2, rw_rw, MAGIC, 0u);
  /* Read slot first, write slot second: the write must still own the one
   * acquisition. */
  phase(mixed_alias_edt, db, writer_rank, 2, ro_rw, MAGIC2, 0u);
  phase(triple_alias_edt, db, writer_rank, 3, ro_rw_ro, MAGIC3, 0u);

  /* The mid-body release.  The reader is wired before the releasing EDT
   * exists, so its dependence on the gate is registered before anything can
   * satisfy it. */
  arts_guid_t gate = arts_event_create(&ARTS_EVENT_HINT_ONCE);
  uint64_t want_v[2] = {(uint64_t)MAGIC4, 1u};
  arts_guid_t e_rd = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t r =
      arts_edt_create(home_checker_edt, 2, want_v, 2,
                      &(arts_edt_hint_t){.rank = 0, .finish_event = e_rd});
  arts_add_dependence(db, r, 0, DB_MODE_RO);
  arts_add_dependence(gate, r, 1, DB_MODE_NULL);

  uint64_t gate_v[1] = {(uint64_t)gate};
  arts_guid_t e_mr = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t mr = arts_edt_create(
      mid_release_edt, 1, gate_v, 2,
      &(arts_edt_hint_t){.rank = writer_rank, .finish_event = e_mr});
  arts_add_dependence(db, mr, 0, DB_MODE_RO);
  arts_add_dependence(db, mr, 1, DB_MODE_RW);
  arts_event_wait(e_mr);
  arts_event_wait(e_rd);

  arts_shutdown();
}

int main(int argc, char **argv) {
  /* Non-zero when a rank this process spawned ended badly (their own status
     reaches nobody else), and non-zero when a check on THIS rank failed. */
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
