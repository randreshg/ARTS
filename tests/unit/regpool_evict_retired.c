/* Eviction of retired direct mappings, and the unplaced fallback behind it.
 *
 * A retired mapping holds memory no allocation owns, so a request that no
 * node can place must release the retired mappings before it concludes that
 * there is nowhere to put it — and, placement being a preference, it must
 * then be served by an unplaced mapping rather than refused; the arena path
 * gives them back the same way before its last resort.  The
 * force-full override makes both reachable without starving a machine: with
 * every node reporting zero availability, the placement screen refuses
 * everything by construction.  Both rules hold whatever the node count, so
 * the scenario runs on a declared machine of one node and of two
 * (regpool_numa_sim.h), each with room for the pool to carve first.
 *
 * Whitebox unit test: no ARTS runtime, no ports, no config; runs on any
 * host. */
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

#define SLAB_BYTES ((size_t)64 * 1024 * 1024)
#define FIRST_SIZE ((size_t)40 * 1024 * 1024)
#define SECOND_SIZE ((size_t)48 * 1024 * 1024)
#define NODE_FREE ((size_t)4 << 30)

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
  int got =
      sscanf(line, "[REGPOOL] direct: live=%u/%u retired=%u/%u free_slots=%u",
             &live, &live_mb, &retired, &retired_mb, &slots);
  assert(got == 5);
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
  int got =
      sscanf(line, "[REGPOOL] total: slabs=%u/%u mapped=%u unplaced=%u base=%u",
             &slabs, &max, &mapped, &unplaced, &base);
  assert(got == 5);
  free(buf);
  return unplaced;
}

static void scenario(int nodes) {
  regpool_sim_declare(nodes, REGPOOL_SIM_FREE);
  for (int n = 0; n < nodes; n++)
    regpool_sim_set_node_free(n, NODE_FREE);
  uint64_t all = ((uint64_t)1 << nodes) - 1;
  arts_regpool_set_topology((unsigned)nodes, all, NULL);
  bool inited = arts_regpool_init(NULL, NULL, SLAB_BYTES, (unsigned)nodes);
  assert(inited);

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

  /* Every node full: no placement can succeed, and what is mapped from here
   * on carries no placement policy at all. */
  arts_regpool_set_forced_full(all);
  unsigned policies = regpool_sim_mbind_calls;

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
   * slab makes it grow, no node has room for the next slab, and the kernel
   * must place it rather than the grow be refused — every block still
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
  assert(unplaced_mib() == (SLAB_BYTES >> 20) * 3 / 2 &&
         "the grown slab, the node's next rung, must be placed by the kernel");
  direct_counts(&live, &retired, &slots);
  assert(live == 0 && retired == 0 &&
         "the arena path gives retired mappings back before it maps where "
         "the kernel chooses");
  assert(regpool_sim_mbind_calls == policies &&
         "a mapping no node can place is left to the kernel, unbound");
  assert(regpool_sim_all_policies_ok());
  assert(regpool_sim_consulted());
  for (int i = 0; i < ARENA_BLOCKS; i++)
    arts_regpool_free(blocks[i]);

  arts_regpool_set_forced_full(0);
  arts_regpool_cleanup();
}

int main(void) {
  scenario(1);
  scenario(2);
  printf("PASS regpool_evict_retired: every node forced full on a declared "
         "machine of 1 and of 2 nodes, mapping unplaced\n");
  return 0;
}
