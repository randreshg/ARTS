/* The slab ladder.  A node's next slab is half again the size of the last
 * one MAPPED for that node, carried up onto the allocator's 32 MiB granule —
 * so from a 64 MiB base the rungs are 64, 96, 160, 256 MiB, and four slabs
 * are 576 MiB of mapped capacity.  Deriving the size from what was mapped is
 * what keeps a clamped or halved grow from skipping a rung; this test pins
 * the geometry the arena-table budget was chosen against.
 *
 * Whitebox unit test: no ARTS runtime, no ports, no config.  One NUMA node
 * so the ladder belongs to node 0 alone, and the pool's own report is the
 * observation point. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arts/memory/regpool.h"

#define SLAB_BYTES ((size_t)64 * 1024 * 1024)
#define LADDER_MIB (64 + 96 + 160 + 256)

/* Node 0's free bytes, read the way the pool reads them; SIZE_MAX when the
 * kernel does not say, which the pool takes as room. */
static size_t node0_free_bytes(void) {
  FILE *f = fopen("/sys/devices/system/node/node0/meminfo", "r");
  if (f == NULL)
    return SIZE_MAX;
  size_t r = arts_regpool_parse_node_avail(f);
  fclose(f);
  return r;
}

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
  assert(sscanf(line,
                "[REGPOOL] node 0: state=%15s arenas=%u mapped=%u grows=%u",
                state, arenas, mapped, grows) == 4);
  assert(strcmp(state, "carved") == 0);
  free(buf);
}

int main(void) {
  /* Every rung must land on node 0, and each one populated lowers what the
   * node has left for the next. */
  size_t free0 = node0_free_bytes();
  if (free0 != SIZE_MAX &&
      free0 < ARTS_REGPOOL_NODE_HEADROOM + ((size_t)LADDER_MIB << 20)) {
    printf("SKIP regpool_ladder: node 0 has %zu MiB free, below the pool's "
           "carve threshold\n",
           free0 >> 20);
    return 0;
  }
  assert(arts_regpool_init(NULL, NULL, SLAB_BYTES, 1));

  unsigned arenas = 0, mapped = 0, grows = 0;
  node0_line(&arenas, &mapped, &grows);
  assert(arenas == 1 && mapped == 64 && grows == 1);

  for (int i = 0; i < 3; i++) {
    assert(arts_regpool_grow(0) && "an explicit pre-grow must map a slab");
  }

  node0_line(&arenas, &mapped, &grows);
  assert(arenas == 4);
  assert(mapped == 64 + 96 + 160 + 256);
  assert(grows == 4);

  arts_regpool_cleanup();
  printf("PASS regpool_ladder arenas=%u mapped=%u grows=%u\n", arenas, mapped,
         grows);
  return 0;
}
