/******************************************************************************
** This material was prepared as an account of work sponsored by an agency   **
** of the United States Government.  Neither the United States Government    **
** nor the United States Department of Energy, nor Battelle Memorial         **
** Institute, nor any of their employees, nor any jurisdiction or            **
** organization that has cooperated in the development of these materials,   **
** makes any warranty, express or implied, or assumes any legal liability    **
** or responsibility for the accuracy, completeness, or usefulness or any    **
** information, apparatus, product, software, or process disclosed, or       **
** represents that its use would not infringe privately owned rights.        **
******************************************************************************/

/// @file numa_steal_policy.c
/// @brief Validates the scheduler's NUMA-local worker selection helpers.

#include <stdio.h>

#include "arts/runtime_state.h"

static int expect_equal(const char *label, unsigned int got,
                        unsigned int expected) {
  if (got != expected) {
    fprintf(stderr, "FAIL: %s expected=%u got=%u\n", label, expected, got);
    return 1;
  }
  return 0;
}

int main(void) {
  unsigned int balanced_numa_ids[] = {0, 0, 1, 1};
  unsigned int skewed_numa_ids[] = {0, 1, 1};
  int failed = 0;

  failed += expect_equal("ready_worker_prefers_requested_numa",
                         arts_pick_worker_for_numa(1, balanced_numa_ids, 4, 0),
                         2);
  failed += expect_equal("ready_worker_spreads_within_requested_numa",
                         arts_pick_worker_for_numa(0, balanced_numa_ids, 4, 3),
                         1);
  failed += expect_equal("ready_worker_falls_back_when_numa_absent",
                         arts_pick_worker_for_numa(2, skewed_numa_ids, 3, 1),
                         1);

  failed += expect_equal("steal_prefers_same_numa",
                         arts_pick_worker_steal_victim(
                             0, 0, balanced_numa_ids, 4, 2, true),
                         1);
  failed += expect_equal("steal_uses_local_peer_when_available",
                         arts_pick_worker_steal_victim(
                             2, 1, balanced_numa_ids, 4, 0, false),
                         3);
  failed += expect_equal("steal_blocks_cross_numa_until_enabled",
                         arts_pick_worker_steal_victim(
                             0, 0, skewed_numa_ids, 3, 0, false),
                         ARTS_INVALID_WORKER_ID);
  failed += expect_equal("steal_falls_back_cross_numa_when_enabled",
                         arts_pick_worker_steal_victim(
                             0, 0, skewed_numa_ids, 3, 0, true),
                         1);

  if (failed)
    return 1;

  printf("PASS: numa_steal_policy\n");
  return 0;
}
