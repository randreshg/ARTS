/* SPDX-License-Identifier: Apache-2.0
 *
 * scheduler_loop_variants — verifies the scheduler_loop[] dispatch table layout
 * and that the configured loop is the one the runtime actually installs.
 *
 * scheduler_loop[] (scheduler.c) is a function-pointer dispatch table indexed
 * by the `scheduler` config key.  Every build carries the three CPU loops at
 * the same indices — {default, network_before_steal, network_first} at 0..2 —
 * and a GPU build appends its three loops (gpu, gpu_backoff, gpu_demand) at
 * 3..5, so a numeric `scheduler=N` below 3 selects the same loop in every
 * build.
 *
 * Asserted at runtime:
 *   1. scheduler_loop[0..2] are default, network_before_steal and
 *      network_first, in that order, and the table holds at least those three.
 *   2. The three CPU loop entry points are three DISTINCT function pointers —
 *      the table is a real dispatch table, not aliased entries.
 *   3. The runtime installs the configured loop: arts_node_info.scheduler ==
 *      scheduler_loop[0] under the default config (the test cfg sets no
 *      `scheduler` key, so it defaults to 0).
 *
 * Runs in every build: the checks are layout invariants, not behavior that
 * varies by coherence protocol.  Verification is synchronous in main_edt; any
 * failed check aborts immediately.
 */

#include <stdint.h>
#include <stdio.h>

#include "arts.h"
#include "arts/runtime.h"       /* scheduler_loop[], loop fn externs */
#include "arts/runtime_state.h" /* arts_node_info.scheduler */

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== scheduler_loop_variants ===\n");

  scheduler_t s_default = (scheduler_t)arts_default_scheduler_loop;
  scheduler_t s_nfirst = (scheduler_t)arts_network_first_scheduler_loop;
  scheduler_t s_nbefore = (scheduler_t)arts_network_before_steal_scheduler_loop;

  /* Invariant 1: the three CPU loops sit at 0..2 in every build. */
  if (scheduler_loop_count < 3u) {
    (void)fprintf(stderr, "FAIL: scheduler_loop holds %u loops, fewer than 3\n",
                  scheduler_loop_count);
    arts_abort(1);
  }
  if (scheduler_loop[0] != s_default || scheduler_loop[1] != s_nbefore ||
      scheduler_loop[2] != s_nfirst) {
    (void)fprintf(stderr,
                  "FAIL: scheduler_loop[0..2]={%p,%p,%p}, expected "
                  "{default=%p, network_before_steal=%p, network_first=%p}\n",
                  (void *)scheduler_loop[0], (void *)scheduler_loop[1],
                  (void *)scheduler_loop[2], (void *)s_default,
                  (void *)s_nbefore, (void *)s_nfirst);
    arts_abort(1);
  }

  /* Invariant 2: the three CPU loops are distinct dispatch targets. */
  if (s_default == s_nfirst || s_default == s_nbefore ||
      s_nfirst == s_nbefore) {
    (void)fprintf(stderr,
                  "FAIL: CPU scheduler loops are not distinct "
                  "(default=%p network_first=%p network_before_steal=%p)\n",
                  (void *)s_default, (void *)s_nfirst, (void *)s_nbefore);
    arts_abort(1);
  }

  /* Invariant 3: the runtime installed the configured (default) loop.  The
   * test config selects no scheduler index, so it defaults to 0 and the
   * active loop must be scheduler_loop[0]. */
  if (arts_node_info.scheduler != scheduler_loop[0]) {
    (void)fprintf(stderr,
                  "FAIL: active scheduler %p != scheduler_loop[0] %p — "
                  "configured loop not installed\n",
                  (void *)arts_node_info.scheduler, (void *)scheduler_loop[0]);
    arts_abort(1);
  }
  if (arts_node_info.scheduler != s_default) {
    (void)fprintf(stderr,
                  "FAIL: active scheduler %p != arts_default_scheduler_loop %p "
                  "under default config\n",
                  (void *)arts_node_info.scheduler, (void *)s_default);
    arts_abort(1);
  }

  printf("scheduler_loop_variants: CPU loops at 0..2, 3 distinct CPU loops, "
         "active loop = configured (index 0) — PASS\n");
  arts_shutdown();
}

int main(int argc, char **argv) {
  /* Non-zero when a rank this process spawned ended badly: their exit status
     reaches nobody else, and a run with a dead rank did not succeed. */
  return arts_rt(argc, argv) != 0 ? 1 : 0;
}
