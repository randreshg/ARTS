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
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/// @file dep_slot_resolved_once.c
/// @brief A slot the acquire phase handed to an arm has one resolver; a second
///        resolution of it is a Debug abort.
///
/// White-box runtime test.  Every plane hands a parked slot to one runner by
/// the RMW that takes its waiter, so the resolver
/// (`arts_db_resolve_slot_cached`, coherence.c) arbitrates nothing: it writes
/// the slot's acquire record and accounts the slot.  A Debug build checks the
/// rule at the top of every resolution and stops the run on a second one.
///
/// The test builds a parked EDT (gated on an unsatisfied event, so it never
/// enters the acquire phase on its own and is never freed), gives slot 0 the
/// state the firing frame gives a slot before the arm's handler runs (the
/// acquire record, the block's descriptor pin, the coherent subtype), biases
/// `acquire_remaining` so no resolution can schedule the EDT, and resolves
/// slot 0 once — the count drops by exactly one — and then again, which must
/// end the process with status 1 and `resolved twice`.
///
/// Debug trees only (the check is compiled out with NDEBUG), and the
/// reference tree only: the resolver runs no cell-dependent code before its
/// check.

#include "arts.h"

#include "arts/coherence/census.h"
#include "arts/coherence/coherence.h"
#include "arts/db.h"
#include "arts/edt.h"
#include "arts/gas/route_table.h"
#include "arts/runtime_types.h"
#include "arts/utils/shared.h"

#include "arts/utils/stdatomic_compat.h"
#include <stdint.h>
#include <stdio.h>

/* The parked EDT never runs; this body must never execute. */
static void never_runs(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  (void)fprintf(stderr, "FAIL: parked EDT was scheduled (should never run)\n");
  arts_abort(1);
}

/* Large enough that no resolution can drive acquire_remaining to 0. */
#define BIAS 100u

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== dep_slot_resolved_once ===\n");

  void *p = NULL;
  arts_guid_t db = arts_db_create(&p, sizeof(uint64_t), ARTS_DB,
                                  ARTS_DB_PROP_NONE, NULL);
  ((uint64_t *)p)[0] = 0xABCDu;
  arts_db_release(db, DB_MODE_RW);

  /* Slot 0 names the block (RO); slot 1 is an event that is never satisfied,
   * which keeps depc_needed above 0, so the runtime never starts the EDT's
   * acquire phase and the test drives slot 0 by hand. */
  arts_guid_t gate = arts_event_create(NULL);
  arts_guid_t eg = arts_edt_create(never_runs, 0, NULL, 2, NULL);
  arts_add_dependence(db, eg, 0, DB_MODE_RO);
  arts_add_dependence(gate, eg, 1, DB_MODE_NULL);

  arts_shared_ptr_t eh = arts_route_table_lookup_edt(eg);
  struct arts_edt_s *edt = (struct arts_edt_s *)arts_shared_get(eh);
  if (edt == NULL) {
    (void)fprintf(stderr, "FAIL: parked EDT not found in route table\n");
    arts_abort(1);
  }
  arts_edt_dep_t *dv = (arts_edt_dep_t *)arts_get_depv(edt);
  dv[0].guid = db;
  dv[0].mode = DB_MODE_RO;

  /* What the walk and the firing frame do before the arm's handler can
   * publish the slot: begin the acquire phase, then store the subtype and
   * the descriptor pin in the slot's record. */
  arts_db_acquire_prepare(edt);
  edt->dep_acq[0].subtype = ARTS_DB;
  edt->dep_acq[0].pin = arts_route_table_lookup_db(db);
  if (edt->dep_acq[0].pin == NULL) {
    (void)fprintf(stderr, "FAIL: the block is not installed\n");
    arts_abort(1);
  }
  atomic_store_explicit(&edt->acquire_remaining, BIAS, memory_order_release);

  /* A serve from what this rank holds, as a serve path declares it. */
  (void)arts_db_census_serving(false);
  unsigned int before =
      atomic_load_explicit(&edt->acquire_remaining, memory_order_acquire);
  arts_db_resolve_slot_cached(edt, 0);
  unsigned int after =
      atomic_load_explicit(&edt->acquire_remaining, memory_order_acquire);
  arts_printf("acquire_remaining: before=%u after=%u delta=%u (want 1)\n",
              before, after, before - after);
  (void)fflush(stdout);
  if (before - after != 1u) {
    (void)fprintf(stderr, "FAIL: one resolution decremented acquire_remaining "
                          "by %u (want 1)\n",
                  before - after);
    arts_abort(2);
  }

  /* A second resolution of the same slot: the Debug check ends the process
   * with status 1 before it touches the record. */
  arts_db_resolve_slot_cached(edt, 0);

  (void)fprintf(stderr, "FAIL: a second resolution of one slot went "
                        "unreported\n");
  arts_abort(2);
}

int main(int argc, char **argv) {
  /* Non-zero when a rank this process spawned ended badly: their exit status
     reaches nobody else, and a run with a dead rank did not succeed. */
  return arts_rt(argc, argv) != 0 ? 1 : 0;
}
