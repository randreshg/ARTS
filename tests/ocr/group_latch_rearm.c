/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* group_latch_rearm.
 *
 * Target: simple.latch INCR (a nested group's chain) racing the child DECR —
 * the re-arm-after-fire edge ordering on a group latch (event.c).
 *
 * A group is a simple latch=1, the opener's count.  When a member of G1 opens
 * a child group G_child and chains it into its current group, it INCRs G1 and
 * wires G_child->DECR G1.  G1 must not fire until every INCR has a matching
 * DECR.  The race of interest: many child groups are chained (INCR G1)
 * concurrently while earlier children complete (DECR G1).  If an INCR were
 * ever ordered after the matching child population so the latch transiently
 * hit 0, G1 would fire-and-not-re-fire and the successor would run before all
 * work completed (or never).
 *
 * Design: ORCHS orchestrator EDTs run in the outer group G1.  Each opens its
 * own child group G_child, chains it into G1 (INCR G1) and spawns LEAVES leaf
 * EDTs joined to G_child, then DECRs G_child; each leaf marks a unique slot,
 * and so does each orchestrator and each of OUTER leaves joined to G1
 * directly, so G1's own members are counted beside its child groups.
 * main_edt DECRs G1 once it has created every member and wired the
 * successor.  Because every child INCR happens-before the child's own leaves
 * can complete (the orchestrator chains the child before creating its
 * leaves, and the child fires only after its leaves finish and its DECR),
 * G1 fires exactly once after ALL leaves ran.
 *
 * The successor (dep on G1) asserts: (a) every slot is set (no premature
 * fire), and (b) it ran exactly once (re-arm would either deadlock — caught by
 * ctest TIMEOUT — or double-fire — caught by the fire-count slot).
 *
 * PASS criterion: every slot set, successor fired once.  No EDT
 * busy-waits; a stranded G1 is reaped by the ctest TIMEOUT.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#include "arts.h"

#define ORCHS 8
#define LEAVES 8
#define OUTER 3
/* Slots: the child groups' leaves, then G1's direct leaves, then one per
 * orchestrator. */
#define OUTER_BASE (ORCHS * LEAVES)
#define ORCH_BASE (OUTER_BASE + OUTER)
#define N_SLOTS (ORCH_BASE + ORCHS)

/* leaf_edt: mark slot. depv[0] = counter DB (RW). paramv[0] = slot. */
static void leaf_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  _Atomic(int) *c = (_Atomic(int) *)depv[0].ptr;
  if (c == NULL) {
    (void)fprintf(stderr, "FAIL: leaf NULL counter DB\n");
    arts_abort(1);
  }
  atomic_store_explicit(&c[(int)paramv[0]], 1, memory_order_release);
}

/* orchestrator: runs in G1.  Opens a child group chained into G1 (INCR G1)
 * and LEAVES leaves joined to it.  depv[0] = counter DB (RW). paramv[0] =
 * base. */
static void orchestrator(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                         arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  arts_guid_t cdb = depv[0].guid;
  uint64_t base = paramv[0];
  _Atomic(int) *c = (_Atomic(int) *)depv[0].ptr;
  if (c == NULL) {
    (void)fprintf(stderr, "FAIL: orchestrator NULL counter DB\n");
    arts_abort(1);
  }
  atomic_store_explicit(&c[ORCH_BASE + (int)(base / LEAVES)], 1,
                        memory_order_release);

  arts_event_hint_t gh = ARTS_EVENT_HINT_LATCH(1);
  gh.auto_destroy = true;
  arts_guid_t cg = arts_event_create(&gh);
  if (cg == NULL_GUID) {
    (void)fprintf(stderr, "FAIL: child group create\n");
    arts_abort(1);
  }
  arts_guid_t g1 = arts_current_group();
  arts_event_satisfy_slot(g1, NULL_GUID, ARTS_EVENT_LATCH_INCR_SLOT);
  arts_add_dependence(cg, g1, ARTS_EVENT_LATCH_DECR_SLOT, DB_MODE_NULL);
  arts_edt_hint_t ih = ARTS_EDT_HINT_DEFAULTS;
  ih.group = cg;
  for (int i = 0; i < LEAVES; i++) {
    uint64_t slot = base + (uint64_t)i;
    arts_guid_t l = arts_edt_create(leaf_edt, 1, &slot, 1, &ih);
    arts_add_dependence(cdb, l, 0, DB_MODE_RW);
  }
  /* cg fires when its leaves finish; its fire DECRs G1. */
  arts_event_satisfy_slot(cg, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
}

/* successor: dep[0] = G1 (NULL), dep[1] = counter DB (RO), dep[2] = fire DB
 * (RW, counts successor fires). */
static void successor(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  _Atomic(int) *c = (_Atomic(int) *)depv[1].ptr;
  _Atomic(int) *fire = (_Atomic(int) *)depv[2].ptr;
  if (c == NULL || fire == NULL) {
    (void)fprintf(stderr, "FAIL: successor NULL DB\n");
    arts_abort(1);
  }
  int prevfire = atomic_fetch_add_explicit(fire, 1, memory_order_acq_rel);
  if (prevfire != 0) {
    (void)fprintf(stderr, "FAIL: successor fired %d times (latch re-armed)\n",
                  prevfire + 1);
    arts_abort(1);
  }
  int set = 0;
  for (int i = 0; i < N_SLOTS; i++) {
    set += atomic_load_explicit(&c[i], memory_order_acquire);
  }
  if (set != N_SLOTS) {
    (void)fprintf(stderr,
                  "FAIL: %d/%d slots set when G1 fired (premature "
                  "fire / re-arm)\n",
                  set, N_SLOTS);
    arts_abort(1);
  }
  printf("group_latch_rearm: %d orchestrators x %d leaves and %d direct "
         "leaves all done before single G1 successor — PASS\n",
         ORCHS, LEAVES, OUTER);
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  _Atomic(int) *c = NULL;
  arts_guid_t cdb = arts_db_create((void **)&c, sizeof(_Atomic(int)) * N_SLOTS,
                                   ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  for (int i = 0; i < N_SLOTS; i++) {
    atomic_init(&c[i], 0);
  }
  arts_db_release(cdb);

  _Atomic(int) *fire = NULL;
  arts_guid_t fdb = arts_db_create((void **)&fire, sizeof(_Atomic(int)),
                                   ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  atomic_init(fire, 0);
  arts_db_release(fdb);

  arts_event_hint_t gh = ARTS_EVENT_HINT_LATCH(1);
  gh.auto_destroy = true;
  arts_guid_t g1 = arts_event_create(&gh);

  arts_edt_hint_t oh = ARTS_EDT_HINT_DEFAULTS;
  oh.group = g1;
  for (int o = 0; o < ORCHS; o++) {
    uint64_t base = (uint64_t)(o * LEAVES);
    arts_guid_t orch = arts_edt_create(orchestrator, 1, &base, 1, &oh);
    arts_add_dependence(cdb, orch, 0, DB_MODE_RW);
  }
  for (int i = 0; i < OUTER; i++) {
    uint64_t slot = (uint64_t)(OUTER_BASE + i);
    arts_guid_t l = arts_edt_create(leaf_edt, 1, &slot, 1, &oh);
    arts_add_dependence(cdb, l, 0, DB_MODE_RW);
  }

  arts_guid_t s = arts_edt_create(successor, 0, NULL, 3, NULL);
  arts_add_dependence(g1, s, 0, DB_MODE_NULL);
  arts_add_dependence(cdb, s, 1, DB_MODE_RO);
  arts_add_dependence(fdb, s, 2, DB_MODE_RW);
  arts_event_satisfy_slot(g1, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
}

int main(int argc, char **argv) {
  /* Non-zero when a rank this process spawned ended badly: their exit status
     reaches nobody else, and a run with a dead rank did not succeed. */
  return arts_rt(argc, argv) != 0 ? 1 : 0;
}
