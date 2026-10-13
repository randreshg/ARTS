/* SPDX-License-Identifier: Apache-2.0
 *
 * config_compute_derived worker-count derivation.  With a pre-set
 * thread_count the worker count is
 *     worker = thread_count - progress_thread_count,
 * an unsigned subtraction that must never wrap.  A progress_thread_count
 * above a pre-set thread_count is refused at load, so the boundary the
 * derivation itself must survive is progress == thread_count.
 *
 * This test drives the multi-node branch (table_length>1, table==NULL so the
 * port loop is skipped) with the boundary case (thread=2, progress=2) and
 * asserts the SANE result: worker a small in-range value (0 here),
 * thread_count consistent — NOT an astronomical wrap.
 */

#include "../../libs/src/core/system/config.c"
#include "config_test_common.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  struct arts_config_s c;
  memset(&c, 0, sizeof(c));
  c.table_length = 2; /* multi-node branch */
  c.table = NULL;     /* port-population loop is guarded on table != NULL */
  /* The multi-node branch also resolves the port block, which needs a launcher
     to know who is allowed to choose it. */
  c.launcher = arts_config_make_new_var("local");
  c.route_table_size = 16;
  c.gpu_route_table_size = 12;

  c.thread_count = 2; /* pre-set (as if from SLURM_CPUS_PER_TASK) */
  c.progress_thread_count = 2; /* boundary: progress == thread_count */
  c.worker_thread_count = 0;

  config_compute_derived(&c);

  /* worker = thread - progress = 2 - 2 = 0 (in range, not a ~4e9 wrap). */
  if (c.worker_thread_count > c.thread_count) {
    fprintf(stderr,
            "FAIL config_thread_count_underflow: worker=%u underflowed "
            "(thread_count=%u)\n",
            c.worker_thread_count, c.thread_count);
    return 1;
  }
  if (c.worker_thread_count != 0) {
    fprintf(stderr,
            "FAIL config_thread_count_underflow: worker=%u, expected 0 "
            "(thread=2, progress=2)\n",
            c.worker_thread_count);
    return 1;
  }

  printf("PASS config_thread_count_underflow: worker=%u (no underflow; "
         "thread_count=%u)\n",
         c.worker_thread_count, c.thread_count);
  /* compute_derived allocated the port block (multi-node, none named). */
  arts_config_destroy(&c);
  return 0;
}
