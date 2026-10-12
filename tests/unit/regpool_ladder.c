/* The slab ladder.  A node's next slab is half again the size of the last
 * one MAPPED for that node, carried up onto the allocator's 32 MiB granule —
 * so from a 64 MiB base the rungs are 64, 96, 160, 256 MiB, and four slabs
 * are 576 MiB of mapped capacity.  Deriving the size from what was mapped is
 * what keeps a clamped or halved grow from skipping a rung; this test pins
 * the geometry the arena-table budget was chosen against.
 *
 * Whitebox unit test: no ARTS runtime, no ports, no config.  A declared
 * machine of one node (regpool_numa_sim.h) with room above the pool's
 * headroom for every rung, so the ladder belongs to node 0 alone and no
 * clamp shortens a rung on any host; the pool's own report is the
 * observation point. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arts/memory/regpool.h"
#include "regpool_numa_sim.h"

#define SLAB_BYTES ((size_t)64 * 1024 * 1024)
#define LADDER_MIB (64 + 96 + 160 + 256)
#define NODE_FREE ((size_t)8 << 30)

/* Node 0's line of the report: arena count, mapped MiB, grow count. */
static void node0_line(unsigned *arenas, unsigned *mapped, unsigned *grows) {
  char *buf = NULL;
  size_t len = 0;
  FILE *f = open_memstream(&buf, &len);
  assert(f != NULL);
  arts_regpool_report(f);
  fclose(f);
  const char *line = strstr(buf, "[REGPOOL] node 0:");
  assert(line != NULL);
  char state[16];
  int got =
      sscanf(line, "[REGPOOL] node 0: state=%15s arenas=%u mapped=%u grows=%u",
             state, arenas, mapped, grows);
  assert(got == 4);
  assert(strcmp(state, "carved") == 0);
  free(buf);
}

int main(void) {
  regpool_sim_declare(1, REGPOOL_SIM_FREE);
  regpool_sim_set_node_free(0, NODE_FREE);
  regpool_sim_set_machine_available(NODE_FREE);
  _Static_assert(NODE_FREE >
                     ARTS_REGPOOL_NODE_HEADROOM + ((size_t)LADDER_MIB << 20),
                 "the declared node must hold every rung above the headroom");
  bool inited = arts_regpool_init(NULL, NULL, SLAB_BYTES, 1);
  assert(inited);

  unsigned arenas = 0, mapped = 0, grows = 0;
  node0_line(&arenas, &mapped, &grows);
  assert(arenas == 1 && mapped == 64 && grows == 1);

  for (int i = 0; i < 3; i++) {
    bool grew = arts_regpool_grow(0);
    assert(grew && "an explicit pre-grow must map a slab");
  }

  node0_line(&arenas, &mapped, &grows);
  assert(arenas == 4);
  assert(mapped == LADDER_MIB);
  assert(grows == 4);
  assert(regpool_sim_consulted());
  assert(regpool_sim_mbind_calls == 0 &&
         "a placement policy on a machine of one node places nothing");

  arts_regpool_cleanup();
  printf("PASS regpool_ladder arenas=%u mapped=%u grows=%u\n", arenas, mapped,
         grows);
  return 0;
}
