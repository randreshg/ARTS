/* The order in which the pool tries other nodes when one cannot take a
 * mapping.  Ascending node index says nothing about a machine; the order the
 * pool uses is the requesting node first, then the nodes of the caller's own
 * set before any outside it, nearer before farther, free memory breaking ties.
 *
 * Every key is checked against a supplied distance table on a declared
 * machine (regpool_numa_sim.h) whose nodes' free memory is fixed, so none
 * depends on the machine the test runs on: distance monotonicity from every
 * requesting node, free memory ordering two equidistant nodes, a near
 * node OUTSIDE the set losing to a far node inside it, and a table that
 * describes more nodes than init counts, or fewer, or more than the pool
 * can hold.
 *
 * Whitebox unit test: no ARTS runtime, no ports, no config. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "arts/memory/regpool.h"
#include "regpool_numa_sim.h"

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

  /* Node 3 has more free memory than every other node, which have equal
   * amounts. */
  regpool_sim_declare(NODES, REGPOOL_SIM_FREE);
  for (unsigned i = 0; i < NODES; i++)
    regpool_sim_set_node_free((int)i, (size_t)4 << 30);
  regpool_sim_set_node_free(3, (size_t)8 << 30);

  /* Every node in the set: the order is then distance, with free memory
   * separating only equals, which cannot break monotonicity. */
  arts_regpool_set_topology(NODES, /*node_set=*/0xFu, dist);
  bool inited = arts_regpool_init(NULL, NULL, SLAB_BYTES, 0);
  assert(inited);
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
  {
    /* From node 0, nodes 2 and 3 are equidistant; node 3 has more free. */
    int order[NODES];
    unsigned n = arts_regpool_node_order(0, order, NODES);
    assert(n == NODES);
    assert(order[0] == 0 && order[1] == 1);
    assert(order[2] == 3 && order[3] == 2 &&
           "free memory orders equidistant nodes, more free first");
  }
  assert(regpool_sim_all_policies_ok());
  assert(regpool_sim_consulted());
  arts_regpool_cleanup();

  /* A set of {0, 2}: from node 0, node 2 is the far one (32) and node 1 the
   * near one (12), but node 1 is not this caller's — a mapping the caller's
   * own nodes cannot serve is the last resort, however near it looks. */
  arts_regpool_set_topology(NODES, /*node_set=*/0x5u, dist);
  inited = arts_regpool_init(NULL, NULL, SLAB_BYTES, 0);
  assert(inited);
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

  /* The table describes four nodes and init counts three: each pair keeps
   * the distance the table gives it.  From node 1, node 0 (12) precedes
   * node 2 (32). */
  arts_regpool_set_topology(NODES, /*node_set=*/0x7u, dist);
  inited = arts_regpool_init(NULL, NULL, SLAB_BYTES, NODES - 1);
  assert(inited);
  {
    int order[NODES];
    unsigned n = arts_regpool_node_order(1, order, NODES);
    assert(n == NODES - 1);
    assert(order[0] == 1 && order[1] == 0 && order[2] == 2 &&
           "a table of more nodes than init counts keeps each pair's "
           "distance");
  }
  arts_regpool_cleanup();

  /* The table describes two nodes and init counts three: node 2, which the
   * table does not describe, comes after node 1, which it places far, even
   * with more free memory. */
  {
    const uint32_t two[4] = {10, 40, 40, 10};
    regpool_sim_set_node_free(2, (size_t)8 << 30);
    arts_regpool_set_topology(2, /*node_set=*/0x7u, two);
    inited = arts_regpool_init(NULL, NULL, SLAB_BYTES, 3);
    assert(inited);
    int order[NODES];
    unsigned n = arts_regpool_node_order(0, order, NODES);
    assert(n == 3);
    assert(order[0] == 0 && order[1] == 1 && order[2] == 2 &&
           "a pair the table does not describe is farther than every pair it "
           "does");
    arts_regpool_cleanup();
  }

  /* A table of more nodes than the pool can hold (65) is read row by row at
   * the caller's own width: from node 1, node 0 (12) precedes node 2 (40). */
  {
    enum { WIDE = 65 };
    static uint32_t wide[WIDE * WIDE];
    for (unsigned i = 0; i < WIDE; i++)
      for (unsigned j = 0; j < WIDE; j++)
        wide[i * WIDE + j] = (i == j) ? 10u : 30u;
    wide[1 * WIDE + 0] = 12u;
    wide[1 * WIDE + 2] = 40u;
    wide[0 * WIDE + 64] = 40u; /* where a row read at the pool's width lands */
    arts_regpool_set_topology(WIDE, /*node_set=*/0x7u, wide);
    inited = arts_regpool_init(NULL, NULL, SLAB_BYTES, 3);
    assert(inited);
    int order[NODES];
    unsigned n = arts_regpool_node_order(1, order, NODES);
    assert(n == 3);
    assert(order[0] == 1 && order[1] == 0 && order[2] == 2 &&
           "a table wider than the pool is read at the caller's width");
    arts_regpool_cleanup();
  }

  printf("PASS regpool_fallback_order\n");
  return 0;
}
