/* SPDX-License-Identifier: Apache-2.0
 *
 * worker_init_deque_size sizes every thread's work-stealing deque, which
 * indexes its ring modulo that size, so config load refuses 0 (ARTS_ERROR)
 * and accepts 1, the smallest ring the deque can grow from.
 *
 * compute_derived also derives thread counts; table_length<=1 keeps it on the
 * single-node path, which touches no ports.
 */

#include "../../libs/src/core/system/config.c"
#include "config_test_common.h"

#include <stdio.h>
#include <string.h>

/* 1 when compute_derived refused the size, 0 when it accepted it. */
static int refused(unsigned int deque_size) {
  static struct arts_config_s c;
  memset(&c, 0, sizeof(c));
  c.table_length = 1;
  c.worker_thread_count = 4;
  c.route_table_size = 16;
  c.gpu_route_table_size = 12;
  c.deque_size = deque_size;
  g_abort_armed = 1;
  if (setjmp(g_abort_jmp) == 0) {
    config_compute_derived(&c);
    g_abort_armed = 0;
    return 0;
  }
  return 1;
}

int main(void) {
  static const struct {
    unsigned int size;
    int refused;
  } cases[] = {{0, 1}, {1, 0}, {4096, 0}};
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    int r = refused(cases[i].size);
    if (r != cases[i].refused) {
      printf("FAIL config_deque_size_range: worker_init_deque_size=%u was %s\n",
             cases[i].size, r ? "refused" : "accepted");
      return 1;
    }
  }
  printf("PASS config_deque_size_range\n");
  return 0;
}
