/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
/// @file runtime_barrier_counts.c
/// @brief Asserts the four startup/teardown barrier counters
///        (ready_to_push / ready_to_parallel_start / ready_to_inspect /
///         ready_to_clean) were each decremented exactly total_thread_count
///        times during bring-up.
///
/// Each barrier counter is initialised to total_thread_count in
/// arts_runtime_node_init and every thread (thread-0 in
/// arts_thread_zero_node_start, non-zero threads in arts_runtime_private_init)
/// decrements it exactly once then spins until it reaches zero.  An off-by-one
/// (double-decrement or a skipped decrement) would either hang startup or leave
/// a barrier counter non-zero.  Once main_edt runs, all three startup barriers
/// (push/parallel_start/inspect) have necessarily reached zero (no thread runs
/// an EDT before every thread passed the inspect rendezvous), so this test
/// asserts they read zero exactly.  ready_to_clean is the teardown barrier
/// and is still at its initial value (total_thread_count) at main_edt time,
/// which we also assert to pin the init-count invariant.
///
/// White-box: reads arts_node_info via runtime_state.h, mirroring
/// the internal-state inspection idiom of the other whitebox tests.
///
/// Single-node, every configuration (protocol-agnostic).

#include "arts.h"
#include "arts/runtime_state.h"

#include "../test_failure_status.h"

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== runtime_barrier_counts ===\n");

  /* Relaxed: this thread passed every startup barrier, and no thread
   * arrives at the teardown barrier before this EDT ends. */
  unsigned int tc = arts_node_info.total_thread_count;
  unsigned int push =
      atomic_load_explicit(&arts_node_info.ready_to_push, memory_order_relaxed);
  unsigned int par = atomic_load_explicit(
      &arts_node_info.ready_to_parallel_start, memory_order_relaxed);
  unsigned int insp = atomic_load_explicit(&arts_node_info.ready_to_inspect,
                                           memory_order_relaxed);
  unsigned int clean = atomic_load_explicit(&arts_node_info.ready_to_clean,
                                            memory_order_relaxed);

  bool ok = true;

  /* All three startup barriers must have drained to zero by the time main_edt
   * runs: no thread runs an EDT before every thread passed the inspect
   * rendezvous. */
  if (push != 0) {
    arts_printf("  FAIL: ready_to_push=%u (expected 0)\n", push);
    arts_test_fail();
    ok = false;
  }
  if (par != 0) {
    arts_printf("  FAIL: ready_to_parallel_start=%u (expected 0)\n", par);
    arts_test_fail();
    ok = false;
  }
  if (insp != 0) {
    arts_printf("  FAIL: ready_to_inspect=%u (expected 0)\n", insp);
    arts_test_fail();
    ok = false;
  }

  /* ready_to_clean is the teardown barrier; at main_edt time no thread has
   * reached private_cleanup yet, so it must still equal its init value tc.
   * This pins the "init = total_thread_count" invariant for the count. */
  if (tc == 0 || clean != tc) {
    arts_printf("  FAIL: ready_to_clean=%u (expected tc=%u)\n", clean, tc);
    arts_test_fail();
    ok = false;
  }

  if (ok) {
    arts_printf("PASS runtime_barrier_counts: tc=%u all startup barriers 0, "
                "clean=%u\n",
                tc, clean);
  }

  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
