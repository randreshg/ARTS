/* An oversize payload is INTERLEAVED across the caller's node set.
 *
 * One object that every thread sweeps has no owning node, so the pool spreads
 * it: each member of the set holds a share and no single memory controller
 * serves the whole block.  Two things are asserted, because the pool's own
 * bookkeeping and the policy it hands the kernel are separate facts and
 * either can drift from the other — the report must call the mapping
 * interleaved, and the mbind the pool issues for the range must interleave
 * it over the whole set, on the mapped range before any page of it is
 * resident, with no flags and a node mask the kernel reads in full
 * (regpool_sim_policy_ok).  With one member of the set full, the set's
 * capacity is the other member's, and the mapping is placed on that member
 * instead.
 *
 * Whitebox unit test: no ARTS runtime, no ports, no config.  The machine is
 * declared (regpool_numa_sim.h): two nodes whose availability is unknown
 * (room, to the pool), so it runs on any host. */
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
#define BIG_BYTES ((size_t)64 * 1024 * 1024)

/* The report's count and MiB for the interleaved direct mappings. */
static void interleaved_counts(unsigned *out_count, unsigned *out_mib) {
  char *buf = NULL;
  size_t len = 0;
  FILE *f = open_memstream(&buf, &len);
  assert(f != NULL);
  arts_regpool_report(f);
  fclose(f);
  const char *line = strstr(buf, "[REGPOOL] direct placement:");
  assert(line != NULL);
  const char *field = strstr(line, "interleaved=");
  unsigned count = 0, mib = 0;
  if (field != NULL) {
    int got = sscanf(field, "interleaved=%u/%u", &count, &mib);
    assert(got == 2);
  }
  free(buf);
  *out_count = count;
  *out_mib = mib;
}

/* Direct mappings the report places on node `node` (count). */
static unsigned placed_on(unsigned node) {
  char *buf = NULL;
  size_t len = 0;
  FILE *f = open_memstream(&buf, &len);
  assert(f != NULL);
  arts_regpool_report(f);
  fclose(f);
  const char *line = strstr(buf, "[REGPOOL] direct placement:");
  assert(line != NULL);
  char key[16];
  snprintf(key, sizeof key, " node%u=", node);
  const char *field = strstr(line, key);
  unsigned count = 0, mib = 0;
  if (field != NULL) {
    int got = sscanf(field + strlen(key), "%u/%u", &count, &mib);
    assert(got == 2);
  }
  free(buf);
  return count;
}

int main(void) {
  regpool_sim_declare(2, REGPOOL_SIM_UNKNOWN);
  arts_regpool_set_topology(2, 0x3, NULL);
  bool inited = arts_regpool_init(NULL, NULL, SLAB_BYTES, 2);
  assert(inited);

  unsigned char *p = (unsigned char *)arts_regpool_alloc_aligned(BIG_BYTES, 64);
  assert(p != NULL);
  const arts_regpool_mr_t *m = arts_regpool_lookup(p);
  assert(m != NULL);
  assert(m->base == p && m->len == BIG_BYTES);
  /* An interleaved mapping belongs to no single node. */
  assert(m->numa_node == -1);
  unsigned count = 0, mib = 0;
  interleaved_counts(&count, &mib);
  assert(count == 1 && mib == (unsigned)(BIG_BYTES >> 20));
  const regpool_sim_mbind_t *b = regpool_sim_mbind_at(p);
  assert(b != NULL && b->mode == REGPOOL_SIM_MPOL_INTERLEAVE &&
         b->mask == 0x3ul && b->len == BIG_BYTES &&
         "the range must be interleaved over the whole set");
  assert(regpool_sim_policy_ok(b) &&
         "the policy must be set before any page of the range exists");
  p[0] = 0x5A;
  p[BIG_BYTES - 1] = 0x5A;

  /* One member full: the set's capacity is the other member's, so the
   * mapping is placed on it rather than spread over a node that cannot
   * take its share. */
  const size_t other = BIG_BYTES + SLAB_BYTES;
  arts_regpool_set_forced_full(0x2);
  unsigned char *q = (unsigned char *)arts_regpool_alloc_aligned(other, 64);
  assert(q != NULL);
  const arts_regpool_mr_t *mq = arts_regpool_lookup(q);
  assert(mq != NULL && mq->numa_node == 0);
  const regpool_sim_mbind_t *bq = regpool_sim_mbind_at(q);
  assert(bq != NULL && bq->mode == REGPOOL_SIM_MPOL_PREFERRED &&
         bq->mask == 0x1ul &&
         "with node 1 full the mapping must be placed on node 0");
  assert(regpool_sim_policy_ok(bq));
  interleaved_counts(&count, &mib);
  assert(count == 1 && placed_on(0) == 1);
  q[0] = 0x5A;
  q[other - 1] = 0x5A;

  assert(regpool_sim_all_policies_ok());
  assert(regpool_sim_consulted());

  arts_regpool_set_forced_full(0);
  arts_regpool_free(q);
  arts_regpool_free(p);
  arts_regpool_cleanup();
  printf("PASS regpool_interleave: %u MiB interleaved over nodes 0-1, then "
         "placed on node 0 with node 1 full\n",
         (unsigned)(BIG_BYTES >> 20));
  return 0;
}
