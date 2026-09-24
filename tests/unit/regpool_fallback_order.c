/* The order in which the pool tries other nodes when one cannot take a
 * mapping.  Ascending node index says nothing about a machine; the order the
 * pool uses is the requesting node first, then the nodes of the caller's own
 * set before any outside it, nearer before farther, free memory breaking ties.
 *
 * Both keys that can be asserted without depending on the machine's own
 * distances are checked against a supplied table: distance monotonicity from
 * every requesting node, and a near node OUTSIDE the set losing to a far node
 * inside it.
 *
 * Whitebox unit test: no ARTS runtime, no ports, no config. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "arts/memory/regpool.h"

#define SLAB_BYTES ((size_t)64 * 1024 * 1024)
#define NODES 4u

/* Two sockets of two nodes: 10 to itself, 12 within a socket, 32 across. */
static uint32_t dist[NODES * NODES];

static uint32_t at(unsigned from, unsigned to) {
  return dist[from * NODES + to];
}

int main(void) {
  for (unsigned i = 0; i < NODES; i++) {
    for (unsigned j = 0; j < NODES; j++) {
      dist[i * NODES + j] = (i == j) ? 10u : ((i / 2 == j / 2) ? 12u : 32u);
    }
  }

  /* Every node in the set: the order is then distance alone (free memory
   * only separates equals, which cannot break monotonicity). */
  arts_regpool_set_topology(NODES, /*node_set=*/0xFu, dist);
  assert(arts_regpool_init(NULL, NULL, SLAB_BYTES, 0));
  for (unsigned k = 0; k < NODES; k++) {
    int order[NODES];
    unsigned n = arts_regpool_node_order((int)k, order, NODES);
    assert(n == NODES);
    assert(order[0] == (int)k && "the requesting node is tried first");
    uint64_t seen = 0;
    for (unsigned i = 0; i < n; i++) {
      assert(order[i] >= 0 && order[i] < (int)NODES);
      assert((seen & ((uint64_t)1 << order[i])) == 0 && "no node twice");
      seen |= (uint64_t)1 << order[i];
      if (i > 0) {
        assert(at(k, (unsigned)order[i - 1]) <= at(k, (unsigned)order[i]) &&
               "every same-distance node comes before any farther one");
      }
    }
  }
  arts_regpool_cleanup();

  /* A set of {0, 2}: from node 0, node 2 is the far one (32) and node 1 the
   * near one (12), but node 1 is not this caller's — a mapping the caller's
   * own nodes cannot serve is the last resort, however near it looks. */
  arts_regpool_set_topology(NODES, /*node_set=*/0x5u, dist);
  assert(arts_regpool_init(NULL, NULL, SLAB_BYTES, 0));
  {
    int order[NODES];
    unsigned n = arts_regpool_node_order(0, order, NODES);
    assert(n == NODES);
    assert(order[0] == 0);
    assert(order[1] == 2 && "an in-set node precedes every out-of-set one");
    /* Among the nodes outside the set, distance still decides. */
    assert(order[2] == 1 && order[3] == 3);
  }
  arts_regpool_cleanup();

  printf("PASS regpool_fallback_order\n");
  return 0;
}
