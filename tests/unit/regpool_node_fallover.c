/* Init on a node the placement screen refuses.
 *
 * The injected exhaustion (ARTS_REGPOOL_FORCE_FULL_NODES) marks the CALLING
 * thread's node as full before init, so this exercises deterministically —
 * with no real memory pressure — the path a genuinely starved node takes:
 * placement is a preference, so init must still succeed and the node's
 * threads must still be served — from another node's arena, since another
 * node has room, and never from a slab of their own, placed or not — while
 * every pointer still resolves through arts_regpool_lookup (confinement
 * holds regardless of placement).
 *
 * The machine is declared (regpool_numa_sim.h): two nodes, both in the
 * caller's set, the calling thread on node 0 and node 1's availability
 * unknown (room, to the pool), so it runs on any host and also checks the
 * placement the pool asked the kernel for.  Registered only for
 * arena-allocator builds. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arts/memory/regpool.h"
#include "regpool_numa_sim.h"

/* Node `node`'s placed-arena count and spill count, and the pool's unplaced
 * arena capacity in MiB, read off the pool's report. */
static void report_counts(unsigned node, unsigned *out_arenas,
                          unsigned *out_spills, unsigned *out_unplaced) {
  char *buf = NULL;
  size_t len = 0;
  FILE *f = open_memstream(&buf, &len);
  assert(f != NULL);
  arts_regpool_report(f);
  fclose(f);
  char key[32];
  snprintf(key, sizeof key, "[REGPOOL] node %u:", node);
  const char *line = strstr(buf, key);
  assert(line != NULL);
  char state[16];
  int got = sscanf(line,
                   "[REGPOOL] node %*u: state=%15s arenas=%u mapped=%*u "
                   "grows=%*u grow_ms=%*u waits=%*u wait_ms=%*u spills=%u",
                   state, out_arenas, out_spills);
  assert(got == 3);
  line = strstr(buf, "[REGPOOL] total:");
  assert(line != NULL);
  got = sscanf(line, "[REGPOOL] total: slabs=%*u/%*u mapped=%*u unplaced=%u",
               out_unplaced);
  assert(got == 1);
  free(buf);
}

int main(void) {
  regpool_sim_declare(2, REGPOOL_SIM_UNKNOWN);
  int set_rc = setenv("ARTS_REGPOOL_FORCE_FULL_NODES", "0", 1);
  assert(set_rc == 0);
  arts_regpool_set_topology(2, 0x3, NULL);

  /* Init must survive the node the screen refuses. */
  bool inited = arts_regpool_init(NULL, NULL, (size_t)64 * 1024 * 1024, 2);
  assert(inited);

  /* An allocation from that node's own thread must succeed AND stay
   * confined to a registered slab — one placed on the other node, or the
   * override did nothing and this test is passing vacuously. */
  void *p = arts_regpool_alloc_aligned(1 << 20, 64);
  assert(p != NULL);
  const arts_regpool_mr_t *m = arts_regpool_lookup(p);
  assert(m != NULL);
  assert(m->numa_node == 1 &&
         "a node the screen refuses is served from the other node's arena");
  const regpool_sim_mbind_t *b = regpool_sim_mbind_at(m->base);
  assert(b != NULL && b->mode == REGPOOL_SIM_MPOL_PREFERRED &&
         b->mask == 0x2ul &&
         "the serving slab must have been placed on node 1");
  assert(regpool_sim_policy_ok(b));
  memset(p, 0xA5, 1 << 20);
  arts_regpool_free(p);

  /* A second allocation is served from the same arena on the fast path:
   * the node was moved onto it once, and took no slab of its own. */
  void *q = arts_regpool_alloc_aligned(1 << 16, 64);
  assert(q != NULL);
  const arts_regpool_mr_t *mq = arts_regpool_lookup(q);
  assert(mq != NULL && mq->numa_node == 1);
  arts_regpool_free(q);
  unsigned arenas = 0, spills = 0, unplaced = 0;
  report_counts(0, &arenas, &spills, &unplaced);
  assert(arenas == 0 && "the full node must not take a slab of its own");
  assert(unplaced == 0 && "no slab is left to the kernel while a node has "
                          "room");
  assert(spills == 1 && "the node is moved onto the serving arena once");
  assert(regpool_sim_all_policies_ok());
  assert(regpool_sim_consulted());

  arts_regpool_cleanup();
  printf("PASS regpool_node_fallover: node 0 refused by the screen, served "
         "from node 1's arena\n");
  return 0;
}
