/* Eviction of retired direct mappings, and the unplaced fallback behind it.
 *
 * A retired mapping holds memory no allocation owns, so a request that no
 * node can place must release the retired mappings before it concludes that
 * there is nowhere to put it — and, placement being a preference, it must
 * then be served by an unplaced mapping rather than refused.  The
 * force-full override makes both reachable without starving a machine: with
 * every node reporting zero availability, the placement screen refuses
 * everything by construction.
 *
 * Whitebox unit test: no ARTS runtime, no ports, no config. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arts/memory/regpool.h"

#define SLAB_BYTES ((size_t)64 * 1024 * 1024)
#define FIRST_SIZE ((size_t)40 * 1024 * 1024)
#define SECOND_SIZE ((size_t)48 * 1024 * 1024)

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

static unsigned count_nodes(void) {
  DIR *d = opendir("/sys/devices/system/node");
  if (d == NULL)
    return 1;
  unsigned n = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strncmp(e->d_name, "node", 4) == 0 &&
        isdigit((unsigned char)e->d_name[4]))
      n++;
  }
  closedir(d);
  return n ? n : 1;
}

/* The direct-mapping line of the pool's own report. */
static void direct_counts(unsigned *out_live, unsigned *out_retired,
                          unsigned *out_free_slots) {
  char *buf = NULL;
  size_t len = 0;
  FILE *f = open_memstream(&buf, &len);
  assert(f != NULL);
  arts_regpool_report(f);
  fclose(f);
  const char *line = strstr(buf, "[REGPOOL] direct:");
  assert(line != NULL);
  unsigned live = 0, live_mb = 0, retired = 0, retired_mb = 0, slots = 0;
  assert(sscanf(line,
                "[REGPOOL] direct: live=%u/%u retired=%u/%u free_slots=%u",
                &live, &live_mb, &retired, &retired_mb, &slots) == 5);
  free(buf);
  *out_live = live;
  *out_retired = retired;
  *out_free_slots = slots;
}

/* Unplaced arena capacity in MiB, read off the report's total line. */
static unsigned unplaced_mib(void) {
  char *buf = NULL;
  size_t len = 0;
  FILE *f = open_memstream(&buf, &len);
  assert(f != NULL);
  arts_regpool_report(f);
  fclose(f);
  const char *line = strstr(buf, "[REGPOOL] total:");
  assert(line != NULL);
  unsigned slabs = 0, max = 0, mapped = 0, unplaced = 0, base = 0;
  assert(sscanf(line,
                "[REGPOOL] total: slabs=%u/%u mapped=%u unplaced=%u base=%u",
                &slabs, &max, &mapped, &unplaced, &base) == 5);
  free(buf);
  return unplaced;
}

int main(void) {
  /* The pool's init must carve node 0 before anything is forced full. */
  size_t free0 = node0_free_bytes();
  if (free0 != SIZE_MAX && free0 < ARTS_REGPOOL_NODE_HEADROOM + SLAB_BYTES) {
    printf("SKIP regpool_evict_retired: node 0 has %zu MiB free, below the "
           "pool's carve threshold\n",
           free0 >> 20);
    return 0;
  }
  unsigned nodes = count_nodes();
  assert(arts_regpool_init(NULL, NULL, SLAB_BYTES, 0));

  unsigned live = 0, retired = 0, slots = 0;

  /* One retired mapping to evict later. */
  unsigned char *first =
      (unsigned char *)arts_regpool_alloc_aligned(FIRST_SIZE, 64);
  assert(first != NULL);
  memset(first, 0x5A, FIRST_SIZE);
  arts_regpool_free(first);
  direct_counts(&live, &retired, &slots);
  assert(live == 0 && retired == 1);

  /* Reuse still works, so a later eviction can only be the screen's doing. */
  unsigned char *again =
      (unsigned char *)arts_regpool_alloc_aligned(FIRST_SIZE, 64);
  assert(again == first);
  arts_regpool_free(again);
  direct_counts(&live, &retired, &slots);
  assert(live == 0 && retired == 1);

  /* Every node full: no placement can succeed. */
  uint64_t mask = nodes >= 64 ? ~(uint64_t)0 : ((uint64_t)1 << nodes) - 1;
  arts_regpool_set_forced_full(mask);

  unsigned char *unplaced =
      (unsigned char *)arts_regpool_alloc_aligned(SECOND_SIZE, 64);
  assert(unplaced != NULL);
  const arts_regpool_mr_t *m = arts_regpool_lookup(unplaced);
  assert(m != NULL);
  assert(m->numa_node == -1 &&
         "a request no node can place must be served unplaced, not refused");
  unplaced[0] = 0x5A;
  unplaced[SECOND_SIZE - 1] = 0x5A;

  /* The retired mapping was released for it, and its slot carried the new
   * one — so nothing is retired and no slot is left over. */
  direct_counts(&live, &retired, &slots);
  assert(live == 1);
  assert(retired == 0);
  assert(slots == 0);

  arts_regpool_free(unplaced);

  /* The arena path under the same condition: exhausting the node's base
   * slab makes it grow, no node can place the next slab, and the node must
   * be given an unplaced base slab rather than a refusal — every block still
   * resolves, and the pool's report shows the unplaced capacity. */
  enum { ARENA_BLOCKS = 72 };
  static void *blocks[ARENA_BLOCKS];
  for (int i = 0; i < ARENA_BLOCKS; i++) {
    blocks[i] = arts_regpool_alloc_aligned((size_t)1 << 20, 64);
    assert(blocks[i] != NULL &&
           "an arena request must be served when no node can place a slab");
    assert(arts_regpool_lookup(blocks[i]) != NULL);
    memset(blocks[i], 0xA5, (size_t)1 << 20);
  }
  assert(unplaced_mib() >= SLAB_BYTES >> 20 &&
         "the grown slab must be an unplaced base slab");
  for (int i = 0; i < ARENA_BLOCKS; i++)
    arts_regpool_free(blocks[i]);

  arts_regpool_set_forced_full(0);
  arts_regpool_cleanup();
  printf("PASS regpool_evict_retired (%u nodes forced full, mapping unplaced)\n",
         nodes);
  return 0;
}
