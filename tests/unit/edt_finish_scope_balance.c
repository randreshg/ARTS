/* SPDX-License-Identifier: Apache-2.0
 *
 * T135 — EDT finish-scope INCR/DECR balance (local + remote-create proxy).
 *
 * Property under test (create_core join INCR + unset DECR; remote proxy)
 * --------------------------------------------------------------------
 * Every EDT that joins a finish scope emits exactly one INCR at create
 * (arts_edt_create_core) and exactly one DECR at completion
 * (arts_unset_thread_local_edt_info).  The scope (a LATCH(1) finish event with
 * a creator-token) fires when its counter returns to 0 — i.e. when the creator
 * releases its token (arts_event_wait) AND every joined EDT has completed.
 *
 * For a REMOTE EDT the INCR is emitted on the source rank inside create_core
 * (against the parent finish event there), and the create body at the EDT's
 * home, finding the parent homed on another rank, installs a local proxy
 * LATCH(1) wired to forward a DECR to the parent when the proxy drains.  So a
 * remote member still contributes a clean +1/-1 to the parent scope.  If any INCR or DECR is dropped (or doubled) the
 * scope never drains and arts_event_wait hangs forever (caught by ctest
 * TIMEOUT); an over-DECR would fire the scope early before members ran, which
 * the per-member tally would expose.
 *
 * Scenario
 * --------
 * main_edt creates a finish event, then M member EDTs joined to it spread
 * round-robin across all ranks (local + remote when nranks>1), each marking a
 * block of its own.  main_edt then arts_event_wait(fe): this releases the
 * creator-token and blocks until every member completes.  After the wait
 * returns, every member must have run exactly once — proving the scope drained
 * precisely when (and only when) all members finished.  Then shut down.
 *
 * One block per member: no two write acquisitions of a block ever overlap, so
 * the tally is exact under every memory model.  A shared counter would make
 * the oracle depend on the runtime ordering the members' writes, which the
 * DB-WRF model leaves to the program.
 *
 * Single-node: all members local (pure INCR/DECR balance).  Multinode: members
 * on remote ranks exercise the proxy-LATCH forward path.
 */

#include "arts.h"

#include <stdint.h>
#include <stdio.h>

#define MEMBERS 24

typedef struct {
  unsigned int ran; /* how many times this member executed */
} mark_t;

/* Member body: mark its own block.  depv[0] = this member's block (RW). */
void member(uint32_t pc, const uint64_t *pv, uint32_t dc, arts_edt_dep_t dv[]) {
  (void)pc;
  (void)pv;
  (void)dc;
  mark_t *m = (mark_t *)dv[0].ptr;
  if (m) {
    m->ran++;
  }
}

/* Verifier body: RO-acquire every member's block AFTER every member completed,
 * so coherence delivers the final marks (the creator cannot read them through
 * its raw create-time pointers — remote RW members migrate/replace the home
 * buffers, leaving those pointers stale/freed).  depv[i] = member i's block
 * (RO). */
void verifier(uint32_t pc, const uint64_t *pv, uint32_t dc,
              arts_edt_dep_t dv[]) {
  (void)pc;
  (void)pv;
  unsigned int once = 0u; /* members whose block reads exactly one run */
  unsigned int runs = 0u; /* runs summed over every block */
  for (uint32_t i = 0; i < dc; i++) {
    const mark_t *m = (const mark_t *)dv[i].ptr;
    unsigned int n = m ? m->ran : 0u;
    runs += n;
    once += (n == 1u);
  }
  if (dc == (uint32_t)MEMBERS && once == (unsigned int)MEMBERS) {
    arts_printf("PASS edt_finish_scope_balance: %d members, scope balanced\n",
                MEMBERS);
  } else {
    arts_printf("FAIL edt_finish_scope_balance: scope drained with %u members "
                "run once, %u runs in all (want %d) — INCR/DECR imbalance\n",
                once, runs, MEMBERS);
    arts_abort(1);
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== edt_finish_scope_balance ===\n");

  unsigned int nranks = arts_get_total_ranks();

  /* One block per member, zeroed and released before any member depends on
   * it: the create's hold is the block's first write acquisition, the
   * member's its second, and the two never overlap. */
  arts_guid_t blocks[MEMBERS];
  for (int i = 0; i < MEMBERS; i++) {
    void *p = NULL;
    blocks[i] = arts_db_create(&p, sizeof(mark_t), ARTS_DB, ARTS_DB_PROP_NONE,
                               &(arts_db_hint_t){.rank = 0});
    if (p == NULL) {
      arts_printf("FAIL edt_finish_scope_balance: create %d handed no "
                  "storage\n", i);
      arts_abort(1);
    }
    ((mark_t *)p)->ran = 0u;
    arts_db_release(blocks[i], DB_MODE_RW);
  }

  arts_guid_t fe = arts_event_create(&ARTS_EVENT_HINT_FINISH);

  /* M members joined to the finish scope, round-robin across all ranks, each
   * writing its own block. */
  for (int i = 0; i < MEMBERS; i++) {
    unsigned int rank = (unsigned int)i % nranks;
    arts_guid_t m = arts_edt_create(
        member, 0, NULL, 1, &(arts_edt_hint_t){.rank = rank, .finish_event = fe});
    arts_add_dependence(blocks[i], m, 0, DB_MODE_RW);
  }

  /* Release the creator-token and block until the scope drains (all members
   * completed).  If any INCR/DECR is dropped the scope never reaches 0 and this
   * hangs — caught by the ctest TIMEOUT. */
  arts_event_wait(fe);

  /* The wait returned ⇒ the scope drained ⇒ every member's DECR landed, which
   * happens only after each member body ran.  Read the marks back through a
   * verifier EDT (RO acquires) rather than the stale create-time pointers,
   * then shut down once that second scope drains. */
  arts_guid_t fe2 = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t v = arts_edt_create(
      verifier, 0, NULL, MEMBERS,
      &(arts_edt_hint_t){.rank = 0, .finish_event = fe2});
  for (int i = 0; i < MEMBERS; i++) {
    arts_add_dependence(blocks[i], v, (uint32_t)i, DB_MODE_RO);
  }
  arts_event_wait(fe2);
  arts_shutdown();
}

int main(int argc, char **argv) {
  /* Non-zero when a rank this process spawned ended badly: their exit status
     reaches nobody else, and a run with a dead rank did not succeed. */
  return arts_rt(argc, argv) != 0 ? 1 : 0;
}
