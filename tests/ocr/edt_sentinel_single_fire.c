/* SPDX-License-Identifier: Apache-2.0
 *
 * EDT pre-reserved-GUID sentinel single-fire protocol.
 *
 * Property under test
 * -------------------
 * The EDT create body sets depc_needed = depc + 1 (the sentinel) BEFORE
 * installing the EDT, replays any out-of-order satisfies queued against the
 * still-RESERVED GUID, then removes the sentinel (-1).  The
 * invariant is: exactly ONE party observes the depc_needed==0 transition and
 * fires the EDT — never zero (lost wakeup) and never two (double dispatch).
 *
 * Five scenarios per run, each driving a distinct branch of the protocol:
 *
 *   A) fresh GUID, its one slot satisfied after create: fires exactly once.
 *
 *   B) pre-reserved GUID, all deps satisfied BEFORE create: the satisfies are
 *      queued on the RESERVED slot (OoO).  The sentinel keeps depc_needed >= 1
 *      across the install+replay, so the sentinel-removal at the end of create
 *      observes 0 and fires.  Must fire exactly once.
 *
 *   C) pre-reserved GUID, last real satisfy lands AFTER create: create's
 *      sentinel removal sees depc_needed > 0 (does not fire); the trailing
 *      satisfy drives it to 0 and fires.  Must fire exactly once.
 *
 *   D) a satisfy naming a slot past depc, delivered between the real slots,
 *      is ignored: the EDT still fires exactly once, with every slot written.
 *
 *   E) a satisfy naming a slot past depc is not one of the EDT's dependences,
 *      so it does not count toward readiness: an EDT with one real slot of two
 *      satisfied and one out-of-range satisfy is still pending, and destroying
 *      it ends it unrun.  Counted, the out-of-range satisfy would make the EDT
 *      runnable, the destroy would be the no-op it is on a runnable EDT, and
 *      the body would run.  Must never fire.
 *
 * Each fired EDT increments its own per-scenario atomic counter.  A collector
 * EDT (gated on a finish event that all scenarios join) verifies every counter
 * of A-D is EXACTLY 1 and E's is 0.  A stranded EDT (counter 0) or a double
 * fire (counter 2) is a FAIL; a deadlock is caught by the ctest TIMEOUT.
 *
 * Config-agnostic: pure single-rank EDT lifecycle, no coherence transfer.
 */

#include "arts.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

/* Counter DB: one atomic per scenario fire-count, plus D's premature fires. */
typedef struct {
  _Atomic unsigned int fired_a;
  _Atomic unsigned int fired_b;
  _Atomic unsigned int fired_c;
  _Atomic unsigned int fired_d;
  _Atomic unsigned int premature_d;
  _Atomic unsigned int fired_e;
} counters_t;

#define D_LAST_VALUE ((arts_guid_t)0x5678u)

#define PV_CTR 0 /* paramv[0] = counters DB guid */

static void bump(arts_guid_t ctr_guid, _Atomic unsigned int *which) {
  (void)ctr_guid;
  atomic_fetch_add_explicit(which, 1u, memory_order_relaxed);
}

/* Scenario A body: depv[0]=counters(RW). */
void fire_a(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
            arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  counters_t *c = (counters_t *)depv[0].ptr;
  bump((arts_guid_t)paramv[PV_CTR], &c->fired_a);
}

/* Scenario B body: pre-reserved, deps queued before create. depv[0]=counters
 * (RW), depv[1..2] are the pre-queued VAL slots that drove readiness. */
void fire_b(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
            arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  counters_t *c = (counters_t *)depv[0].ptr;
  bump((arts_guid_t)paramv[PV_CTR], &c->fired_b);
}

/* Scenario C body: pre-reserved, last satisfy after create. depv[0]=counters
 * (RW), depv[1] is the trailing VAL slot. */
void fire_c(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
            arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  counters_t *c = (counters_t *)depv[0].ptr;
  bump((arts_guid_t)paramv[PV_CTR], &c->fired_c);
}

/* Scenario D body: depv[0]=counters (RW), depv[1] = the value satisfied
 * last. */
void fire_d(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
            arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  counters_t *c = (counters_t *)depv[0].ptr;
  if (depv[1].guid != D_LAST_VALUE) {
    atomic_fetch_add_explicit(&c->premature_d, 1u, memory_order_relaxed);
  }
  bump((arts_guid_t)paramv[PV_CTR], &c->fired_d);
}

/* Scenario E body: must never run -- its slot 1 is never satisfied. */
void fire_e(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
            arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  counters_t *c = (counters_t *)depv[0].ptr;
  bump((arts_guid_t)paramv[PV_CTR], &c->fired_e);
}

/* Collector: reads the counters RO after all scenarios drained. */
void collector(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  /* depv[0] = finish event (NULL payload), depv[1] = counters DB (RO). */
  counters_t *c = (counters_t *)depv[1].ptr;
  unsigned int a = atomic_load_explicit(&c->fired_a, memory_order_relaxed);
  unsigned int b = atomic_load_explicit(&c->fired_b, memory_order_relaxed);
  unsigned int cc = atomic_load_explicit(&c->fired_c, memory_order_relaxed);
  unsigned int d = atomic_load_explicit(&c->fired_d, memory_order_relaxed);
  unsigned int e = atomic_load_explicit(&c->fired_e, memory_order_relaxed);
  unsigned int early =
      atomic_load_explicit(&c->premature_d, memory_order_relaxed);
  arts_printf("edt_sentinel_single_fire: A=%u B=%u C=%u D=%u (fired before "
              "its last slot: %u)\n",
              a, b, cc, d, early);
  arts_printf("edt_sentinel_single_fire: E=%u\n", e);
  if (a == 1u && b == 1u && cc == 1u && d == 1u && early == 0u && e == 0u) {
    arts_printf("PASS edt_sentinel_single_fire\n");
  } else {
    arts_printf("FAIL edt_sentinel_single_fire: A-D must each fire exactly "
                "once, after all of their slots, and E never\n");
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

  arts_printf("=== edt_sentinel_single_fire ===\n");

  void *cptr = NULL;
  arts_guid_t ctr =
      arts_db_create(&cptr, sizeof(counters_t), ARTS_DB, ARTS_DB_PROP_NONE,
                     &(arts_db_hint_t){.rank = 0});
  counters_t *c = (counters_t *)cptr;
  atomic_init(&c->fired_a, 0u);
  atomic_init(&c->fired_b, 0u);
  atomic_init(&c->fired_c, 0u);
  atomic_init(&c->fired_d, 0u);
  atomic_init(&c->premature_d, 0u);
  atomic_init(&c->fired_e, 0u);
  arts_db_release(ctr, DB_MODE_RW);

  uint64_t pv[1] = {(uint64_t)ctr};

  /* Finish scope: the collector waits until every scenario drains (a fired
   * EDT's completion and a destroyed one's destroy balance its INCR). */
  arts_guid_t fe = arts_event_create(&ARTS_EVENT_HINT_FINISH);

  /* --- Scenario A: fresh GUID; the counters dep is its only dependency. --- */
  {
    arts_guid_t a = arts_edt_create(
        fire_a, 1, pv, 1, &(arts_edt_hint_t){.rank = 0, .finish_event = fe});
    arts_add_dependence(ctr, a, 0, DB_MODE_RW);
  }

  /* --- Scenario B: pre-reserved GUID with all real deps satisfied BEFORE the
   * EDT is created, so they queue on the RESERVED slot (OoO). depc = 3 (the
   * counters RW + two VAL slots). We satisfy the two VAL slots and the RW dep
   * first, then create the EDT — its sentinel-removal must be the sole firer.
   * --- */
  {
    arts_guid_t b = arts_guid_reserve(ARTS_GUID_EDT, 0);
    /* Pre-queue the two VAL satisfies and the RW dep against the RESERVED b. */
    arts_edt_satisfy_slot(b, 1, NULL_GUID, DB_MODE_NULL);
    arts_edt_satisfy_slot(b, 2, NULL_GUID, DB_MODE_NULL);
    arts_add_dependence(ctr, b, 0, DB_MODE_RW);
    /* Now create with the pre-reserved GUID; depc=3. */
    arts_edt_create(fire_b, 1, pv, 3,
                    &(arts_edt_hint_t){.guid = b, .finish_event = fe});
  }

  /* --- Scenario C: pre-reserved GUID where the LAST real satisfy lands after
   * create. We pre-queue only the RW dep, create the EDT (depc=2), then deliver
   * the trailing VAL satisfy. Create's sentinel removal must see depc_needed>0;
   * the trailing satisfy fires it. --- */
  {
    arts_guid_t cg = arts_guid_reserve(ARTS_GUID_EDT, 0);
    arts_add_dependence(ctr, cg, 0, DB_MODE_RW);
    arts_edt_create(fire_c, 1, pv, 2,
                    &(arts_edt_hint_t){.guid = cg, .finish_event = fe});
    /* Trailing satisfy that drives readiness to 0 after the sentinel removal.
     */
    arts_edt_satisfy_slot(cg, 1, NULL_GUID, DB_MODE_NULL);
  }

  /* --- Scenario D: slot 0, then a slot past depc, then the last slot. --- */
  {
    arts_guid_t d = arts_edt_create(
        fire_d, 1, pv, 2, &(arts_edt_hint_t){.rank = 0, .finish_event = fe});
    arts_add_dependence(ctr, d, 0, DB_MODE_RW);
    arts_edt_satisfy_slot(d, 5, NULL_GUID, DB_MODE_NULL);
    arts_edt_satisfy_slot(d, 1, D_LAST_VALUE, DB_MODE_NULL);
  }

  /* --- Scenario E: slot 0, a slot past depc, then a destroy while slot 1 is
   * still missing. --- */
  {
    arts_guid_t e = arts_edt_create(
        fire_e, 1, pv, 2, &(arts_edt_hint_t){.rank = 0, .finish_event = fe});
    arts_add_dependence(ctr, e, 0, DB_MODE_RW);
    arts_edt_satisfy_slot(e, 5, NULL_GUID, DB_MODE_NULL);
    arts_edt_destroy(e);
  }

  /* Collector gated on the finish scope: created last, depends on fe firing. */
  arts_guid_t coll =
      arts_edt_create(collector, 1, pv, 2, &(arts_edt_hint_t){.rank = 0});
  arts_add_dependence(fe, coll, 0, DB_MODE_NULL);
  arts_add_dependence(ctr, coll, 1, DB_MODE_RO);
}

int main(int argc, char **argv) {
  /* Non-zero when a rank this process spawned ended badly: their exit status
     reaches nobody else, and a run with a dead rank did not succeed. */
  return arts_rt(argc, argv) != 0 ? 1 : 0;
}
