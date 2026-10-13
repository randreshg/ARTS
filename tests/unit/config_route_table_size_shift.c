/* SPDX-License-Identifier: Apache-2.0
 *
 * config_compute_derived route_table_entries = 1U << route_table_size.
 *
 * compute_derived sets:
 *     config->route_table_entries     = 1U << config->route_table_size;
 *     config->gpu_route_table_entries = 1U << config->gpu_route_table_size;
 * with NO clamp on the shift count.  Properties / boundaries:
 *   size=20  -> 1<<20 = 1048576           (normal)
 *   size=31  -> 1<<31 = 0x80000000        (high bit set, still defined)
 * Both are pinned as computed, which documents the absence of a clamp: a
 * config typo route_table_size=31 silently asks for a 2-billion-entry table.
 * A size of 32 or more shifts a 32-bit unsigned by its width, which is
 * undefined, so there is no value to assert for it.
 *
 * compute_derived also derives thread counts; we keep table_length<=1 so it
 * takes the simple single-node reclaim path and does not touch ports.
 */

#include "../../libs/src/core/system/config.c"
#include "config_test_common.h"

#include <stdio.h>
#include <string.h>

static void minimal(struct arts_config_s *c) {
  memset(c, 0, sizeof(*c));
  c->table_length = 1; /* single-node path */
  c->worker_thread_count = 4;
  c->gpu_route_table_size = 12; /* keep gpu shift well-defined */
}

int main(void) {
  /* size = 20 -> 1<<20 */
  {
    struct arts_config_s c;
    minimal(&c);
    c.route_table_size = 20;
    config_compute_derived(&c);
    if (c.route_table_entries != (1u << 20)) {
      fprintf(stderr, "FAIL shift: size 20 -> %u (want %u)\n",
              c.route_table_entries, 1u << 20);
      return 1;
    }
    if (c.gpu_route_table_entries != (1u << 12)) {
      fprintf(stderr, "FAIL shift: gpu size 12 -> %u\n",
              c.gpu_route_table_entries);
      return 1;
    }
  }

  /* size = 31 -> high bit set, still defined; PIN the unclamped huge value. */
  {
    struct arts_config_s c;
    minimal(&c);
    c.route_table_size = 31;
    config_compute_derived(&c);
    if (c.route_table_entries != 0x80000000u) {
      fprintf(stderr, "FAIL shift: size 31 -> 0x%x (want 0x80000000)\n",
              c.route_table_entries);
      return 1;
    }
  }

  printf("PASS config_route_table_size_shift: 20/31 unclamped pinned\n");
  return 0;
}
