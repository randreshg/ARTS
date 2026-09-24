/* SPDX-License-Identifier: Apache-2.0
 *
 * T136 — remote pre-reserved EDT create with its satisfies reordered ahead
 *        (arts_handler_edt_create sentinel; OoO replay on the home rank).
 *
 * Property under test (arts_handler_edt_create)
 * ---------------------------------------------
 * When an EDT is created with a pre-reserved GUID homed on a REMOTE rank, a
 * satisfy or dependence that reaches the home BEFORE the create queues OoO on
 * the still-reserved slot.  The RX handler arts_handler_edt_create:
 *   - bumps a sentinel (depc_needed += 1) before the EDT becomes visible,
 *   - installs the EDT, replaying the queued satisfies under the sentinel,
 *   - removes the sentinel; exactly one party observes the 0 transition, so
 *     the EDT fires exactly once (no double dispatch, no lost fire),
 *   - chains the EDT's finish scope to the remote parent through a proxy
 *     latch, so the parent scope drains once the EDT has run.
 *
 * Scenario (deterministic, single driver rank)
 * --------------------------------------------
 * Reserve an EDT GUID homed on rank W (remote when nranks>1, else rank 0).
 *   1) Pre-satisfy the EDT's one real dep (a VAL) against the still-RESERVED
 *      remote GUID, and wire its counter dep — both reach W ahead of the
 *      create and queue OoO there.
 *   2) Create that GUID, targeting W under a finish scope.  On W the install
 *      replays both, and the EDT fires exactly once under the sentinel.
 * A finish event gates a collector; main_edt waits on it.  The member bumps a
 * shared counter exactly once.  A lost fire → the scope never drains →
 * TIMEOUT; a double fire → counter == 2 → FAIL.
 *
 * A second create of a live GUID parks until the occupant is destroyed, so
 * this test drives ONE create.  Requires >1 rank to exercise the remote path;
 * SKIPs cleanly to a trivial single-rank check on 1n.
 */

#include "arts.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
  _Atomic unsigned int fired; /* member fire count; must be exactly 1 */
} ctr_t;

/* The migrated member EDT.  depv[0] = VAL (reordered-ahead satisfy),
 * depv[1] = counter DB (RW). */
void member(uint32_t pc, const uint64_t *pv, uint32_t dc, arts_edt_dep_t dv[]) {
  (void)pc;
  (void)pv;
  (void)dc;
  ctr_t *c = (ctr_t *)dv[1].ptr;
  if (c) {
    atomic_fetch_add_explicit(&c->fired, 1u, memory_order_relaxed);
  }
}

void collector(uint32_t pc, const uint64_t *pv, uint32_t dc,
               arts_edt_dep_t dv[]) {
  (void)pc;
  (void)pv;
  (void)dc;
  ctr_t *c = (ctr_t *)dv[1].ptr;
  unsigned int f = atomic_load_explicit(&c->fired, memory_order_relaxed);
  arts_printf("edt_remote_create_reordered_satisfy: member fired=%u\n", f);
  if (f == 1u) {
    arts_printf("PASS edt_remote_create_reordered_satisfy\n");
  } else {
    arts_printf("FAIL edt_remote_create_reordered_satisfy: member fired %u times (want 1)\n",
                f);
    arts_abort(1);
  }
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== edt_remote_create_reordered_satisfy ===\n");

  unsigned int nranks = arts_get_total_ranks();
  if (nranks < 2) {
    /* The reorder is a REMOTE-create property: with one rank the pre-reserved
     * create is local and nothing travels ahead of it.  Skip cleanly. */
    arts_printf("SKIP edt_remote_create_reordered_satisfy: requires 2+ ranks (got %u)\n",
                nranks);
    arts_shutdown();
    return;
  }
  unsigned int W = 1u;

  void *cp = NULL;
  arts_guid_t cdb =
      arts_db_create(&cp, sizeof(ctr_t), ARTS_DB, ARTS_DB_PROP_NONE,
                     &(arts_db_hint_t){.rank = 0});
  ctr_t *c = (ctr_t *)cp;
  atomic_init(&c->fired, 0u);
  arts_db_release(cdb, DB_MODE_RW);

  uint64_t pv[1] = {(uint64_t)cdb};

  arts_guid_t fe = arts_event_create(&ARTS_EVENT_HINT_FINISH);

  /* Reserve the member EDT GUID on the (remote) home rank W. */
  arts_guid_t mguid = arts_guid_reserve(ARTS_GUID_EDT, W);

  /* (1) Reordered-ahead remote satisfy: deliver the VAL dep before the create.
   * It queues OoO on W's RESERVED slot. */
  arts_edt_satisfy_slot(mguid, 0, NULL_GUID, DB_MODE_NULL);
  /* Wire the counter dep (slot 1, RW) — delivered when the EDT installs. */
  arts_add_dependence(cdb, mguid, 1, DB_MODE_RW);

  /* (2) Create the migrated GUID, targeting rank W under the finish scope.
   * On W the install replays both queued satisfies, and the EDT fires once
   * under the sentinel. */
  arts_edt_create(member, 1, pv, 2,
                  &(arts_edt_hint_t){.guid = mguid, .finish_event = fe});

  /* Collector gated on the finish scope. */
  arts_guid_t coll =
      arts_edt_create(collector, 1, pv, 2, &(arts_edt_hint_t){.rank = 0});
  arts_add_dependence(fe, coll, 0, DB_MODE_NULL);
  arts_add_dependence(cdb, coll, 1, DB_MODE_RO);
}

int main(int argc, char **argv) {
  /* Non-zero when a rank this process spawned ended badly: their exit status
     reaches nobody else, and a run with a dead rank did not succeed. */
  return arts_rt(argc, argv) != 0 ? 1 : 0;
}
