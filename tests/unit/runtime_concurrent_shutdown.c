/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
/// @file runtime_concurrent_shutdown.c
/// @brief Drives multiple concurrent shutdown initiators (many EDTs each call
///        arts_shutdown): exactly one of them begins the rank's end, with no
///        deadlock and a clean end.
///
/// arts_shutdown -> arts_enter_shutdown_state(ARTS_EXIT_APP) gates on a
/// compare-exchange of shutdown_state from 0 to 1: exactly one caller wins
/// the 0->1 transition and performs the post (MN) + stop_workers; all others
/// observe a non-zero state and no-op.  Here we stress N worker EDTs racing
/// into arts_shutdown simultaneously: the run must end with status 0 (ctest
/// TIMEOUT reaps a hang), and where the tree writes counter files the
/// registration's record check reads the rank's end from its file: origin
/// APP, status 0.
///
/// We fan out N independent EDTs that all call arts_shutdown.  A win counter in
/// a DB lets each EDT observe how many got to run before the runtime stopped;
/// the property under test is liveness (no deadlock) and one clean end.
///
/// The initiators acquire one block RW with no order among them, so the
/// program lies outside DB-WRF.

#include "arts.h"

#include "arts/utils/stdatomic_compat.h"

#define N_INITIATORS 8

/// Each initiator EDT bumps a shared counter then calls arts_shutdown.  Because
/// stop_workers only retires worker threads, in-flight EDTs may still be torn
/// down; the test does not require all N to run, only that the runtime stops.
void initiator_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  _Atomic(unsigned int) *ctr = (_Atomic(unsigned int) *)depv[0].ptr;
  if (ctr) {
    atomic_fetch_add_explicit(ctr, 1u, memory_order_relaxed);
  }
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== runtime_concurrent_shutdown ===\n");

  /* Shared counter DB acquired RW by every initiator (forces them to race on
   * the same DB while all also race into arts_shutdown). */
  void *ctr_ptr = NULL;
  arts_guid_t ctr_db = arts_db_create(&ctr_ptr, sizeof(_Atomic(unsigned int)),
                                      ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  atomic_init((_Atomic(unsigned int) *)ctr_ptr, 0u);
  arts_db_release(ctr_db);

  for (int i = 0; i < N_INITIATORS; i++) {
    arts_guid_t e = arts_edt_create(initiator_edt, 0, NULL, 1,
                                    &(arts_edt_hint_t){.rank = 0});
    arts_add_dependence(ctr_db, e, 0, DB_MODE_RW);
  }

  arts_printf("PASS runtime_concurrent_shutdown: launched %d initiators\n",
              N_INITIATORS);
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
