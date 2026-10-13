/* SPDX-License-Identifier: Apache-2.0
 *
 * config_compute_derived turns route_table_size and gpu_route_table_size into
 * entry counts (1U << size).  A size whose table cannot exist is refused at
 * load (ARTS_ERROR), never shifted:
 *   route_table_size      4..31  (the remote shards hash with shift size - 3,
 *                                  which must be at least 1; 1U << 32 does not
 *                                  exist in an unsigned int)
 *   gpu_route_table_size  1..28  (shift at least 1; 8 slots per entry counted
 *                                  in an unsigned int)
 * Both ends of both ranges are accepted with the exact entry count, and the
 * first value past each end is refused.
 *
 * compute_derived also derives thread counts; table_length<=1 keeps it on the
 * single-node path, which touches no ports.
 */

#include "../../libs/src/core/system/config.c"
#include "config_test_common.h"

#include <stdio.h>
#include <string.h>

static void minimal(struct arts_config_s *c, unsigned int size,
                    unsigned int gpu_size) {
  memset(c, 0, sizeof(*c));
  c->table_length = 1;
  c->worker_thread_count = 4;
  c->deque_size = 4096;
  c->route_table_size = size;
  c->gpu_route_table_size = gpu_size;
}

/* 1 when compute_derived refused the pair, 0 when it accepted it. */
static int refused(unsigned int size, unsigned int gpu_size,
                   struct arts_config_s *out) {
  static struct arts_config_s c;
  minimal(&c, size, gpu_size);
  g_abort_fired = 0;
  g_abort_armed = 1;
  if (setjmp(g_abort_jmp) == 0) {
    config_compute_derived(&c);
    g_abort_armed = 0;
    *out = c;
    return 0;
  }
  return 1;
}

int main(void) {
  static const struct {
    unsigned int size, gpu_size;
    int refused;
    unsigned int entries, gpu_entries;
  } cases[] = {
      {4, 12, 0, 1u << 4, 1u << 12},
      {16, 12, 0, 1u << 16, 1u << 12},
      {31, 12, 0, 0x80000000u, 1u << 12},
      {16, 1, 0, 1u << 16, 1u << 1},
      {16, 28, 0, 1u << 16, 1u << 28},
      {3, 12, 1, 0, 0},
      {0, 12, 1, 0, 0},
      {32, 12, 1, 0, 0},
      {16, 0, 1, 0, 0},
      {16, 29, 1, 0, 0},
      {16, 32, 1, 0, 0},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    struct arts_config_s c;
    int r = refused(cases[i].size, cases[i].gpu_size, &c);
    if (r != cases[i].refused) {
      printf("FAIL config_route_table_size_shift: route_table_size=%u "
             "gpu_route_table_size=%u was %s\n",
             cases[i].size, cases[i].gpu_size, r ? "refused" : "accepted");
      return 1;
    }
    if (!r && (c.route_table_entries != cases[i].entries ||
               c.gpu_route_table_entries != cases[i].gpu_entries)) {
      printf("FAIL config_route_table_size_shift: sizes %u/%u gave %u/%u "
             "entries (want %u/%u)\n",
             cases[i].size, cases[i].gpu_size, c.route_table_entries,
             c.gpu_route_table_entries, cases[i].entries, cases[i].gpu_entries);
      return 1;
    }
  }
  printf("PASS config_route_table_size_shift\n");
  return 0;
}
