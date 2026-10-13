/* Oversize (direct-slab) allocations: a request above the arena's largest
 * chunk object takes a dedicated mapping instead of the arena, and its free
 * retires that mapping — the memory stays mapped and registered, owned by no
 * allocation, so the next request of the same length and alignment gets it
 * back while the freed pointer stops resolving.  Exercised here at several
 * sizes with the pool's confinement lookup on every pointer, plus the
 * placement, checked through the policy the pool hands the kernel and through
 * the pool's own report, either of which can drift from the other: an
 * oversize mapping is interleaved over the caller's whole node set rather
 * than placed on one node, on the mapped range before any page of it is
 * resident; with one member of a two-node set full the set's capacity is the
 * other member's and the mapping is placed there; and when the set is one node
 * that is full the mapping is placed on another node rather than refused.  A
 * small request aligned past what an arena honours takes a mapping of its own
 * too.  The two legs with a full node are driven by the pool's injected
 * exhaustion and run only where ARTS_DEBUG_INJECTION compiles it.
 *
 * Whitebox unit test: no ARTS runtime, no ports, no config.  The machine is
 * declared (regpool_numa_sim.h): two nodes, each with room for every mapping
 * here above the pool's headroom, so it runs and asserts the same on any
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
#define NODE_FREE ((size_t)4 << 30)

static void declare_machine(void) {
  regpool_sim_declare(2, REGPOOL_SIM_FREE);
  regpool_sim_set_node_free(0, NODE_FREE);
  regpool_sim_set_node_free(1, NODE_FREE);
}

/* Live and retired direct-mapping counts, read off the pool's own report. */
static void direct_counts(unsigned *out_live, unsigned *out_retired) {
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
}

/* The report's direct placement line: the interleaved mappings (count and
 * MiB), and the count placed on `node`. */
static void placement_counts(unsigned node, unsigned *out_interleaved,
                             unsigned *out_mib, unsigned *out_on_node) {
  char *buf = NULL;
  size_t len = 0;
  FILE *f = open_memstream(&buf, &len);
  assert(f != NULL);
  arts_regpool_report(f);
  fclose(f);
  const char *line = strstr(buf, "[REGPOOL] direct placement:");
  unsigned count = 0, mib = 0, on_node = 0, on_node_mib = 0;
  if (line != NULL) {
    const char *field = strstr(line, "interleaved=");
    if (field != NULL) {
      int got = sscanf(field, "interleaved=%u/%u", &count, &mib);
      assert(got == 2);
    }
    char key[16];
    snprintf(key, sizeof key, " node%u=", node);
    field = strstr(line, key);
    if (field != NULL) {
      int got = sscanf(field + strlen(key), "%u/%u", &on_node, &on_node_mib);
      assert(got == 2);
    }
  }
  free(buf);
  *out_interleaved = count;
  *out_mib = mib;
  *out_on_node = on_node;
}

static unsigned interleaved_count(void) {
  unsigned count = 0, mib = 0, on_node = 0;
  placement_counts(0, &count, &mib, &on_node);
  return count;
}

/* The mapping at `m` was interleaved over both declared nodes. */
static void assert_spread(const arts_regpool_mr_t *m) {
  assert(m->numa_node == -1);
  const regpool_sim_mbind_t *b = regpool_sim_mbind_at(m->base);
  assert(b != NULL && b->mode == REGPOOL_SIM_MPOL_INTERLEAVE &&
         b->mask == 0x3ul && b->len == m->len &&
         "an oversize mapping is interleaved over the caller's node set");
  assert(regpool_sim_policy_ok(b));
}

static void check_one(size_t size) {
  unsigned char *p = (unsigned char *)arts_regpool_alloc_aligned(size, 64);
  assert(p != NULL);
  const arts_regpool_mr_t *m = arts_regpool_lookup(p);
  assert(m != NULL);
  assert((unsigned char *)m->base == p && m->len == size &&
         "an oversize block is served by a mapping of exactly its length");
  assert_spread(m);
  p[0] = 0x5A;
  p[size / 2] = 0x5A;
  p[size - 1] = 0x5A;
  assert(p[0] == 0x5A && p[size / 2] == 0x5A && p[size - 1] == 0x5A);
  arts_regpool_free(p);
  /* The free hands the mapping back to the pool: the pointer must no longer
   * resolve, whether the mapping was kept or torn down. */
  assert(arts_regpool_lookup(p) == NULL);
}

int main(void) {
  /* 64 MiB slab: anything above the arena's 32 MiB chunk object is direct. */
  declare_machine();
  arts_regpool_set_topology(2, 0x3, NULL);
  bool inited = arts_regpool_init(NULL, NULL, SLAB_BYTES, 2);
  assert(inited);

  check_one((size_t)40 * 1024 * 1024);
  check_one((size_t)100 * 1024 * 1024);
  /* A third distinct length: each shape gets a mapping of its own, and none
   * of the earlier ones is handed out for a length it does not match. */
  check_one((size_t)48 * 1024 * 1024);

  /* A small request aligned past what an arena honours takes a mapping of
   * its own, never an arena grow that could not serve it. */
  {
    const size_t align = (size_t)128 * 1024;
    unsigned live0 = 0, retired0 = 0;
    direct_counts(&live0, &retired0);
    void *q = arts_regpool_alloc_aligned(4096, align);
    assert(q != NULL && ((uintptr_t)q & (align - 1)) == 0);
    const arts_regpool_mr_t *m = arts_regpool_lookup(q);
    assert(m != NULL && m->base == q);
    unsigned live1 = 0, retired1 = 0;
    direct_counts(&live1, &retired1);
    assert(live1 == live0 + 1 &&
           "a request aligned past an arena's limit takes a mapping of its "
           "own");
    arts_regpool_free(q);
  }
  assert(regpool_sim_all_policies_ok());
  assert(regpool_sim_consulted());
  arts_regpool_cleanup();

  /* Reuse, and the bound on what reuse may hand out.  A fresh pool,
   * so the report's counts are exactly this leg's. */
  declare_machine();
  arts_regpool_set_topology(2, 0x3, NULL);
  inited = arts_regpool_init(NULL, NULL, SLAB_BYTES, 2);
  assert(inited);
  const size_t big = (size_t)64 * 1024 * 1024;
  unsigned char *first = (unsigned char *)arts_regpool_alloc_aligned(big, 64);
  assert(first != NULL);
  assert_spread(arts_regpool_lookup(first));
  memset(first, 0x5A, big);
  arts_regpool_free(first);

  /* Retained: the same shape gets the same mapping back. */
  unsigned char *again = (unsigned char *)arts_regpool_alloc_aligned(big, 64);
  assert(again == first);

  arts_regpool_free(again);

  /* A different length must not be served from the retired mapping, which
   * stays retired and available for its own shape — the nodes have room
   * for the new mapping, so nothing is released for it. */
  unsigned char *other =
      (unsigned char *)arts_regpool_alloc_aligned(big + SLAB_BYTES, 64);
  assert(other != NULL);
  assert(other != first);
  unsigned live = 0, retired = 0;
  direct_counts(&live, &retired);
  assert(live == 1);
  assert(retired == 1);
  assert_spread(arts_regpool_lookup(other));
  assert(regpool_sim_all_policies_ok());
  assert(regpool_sim_consulted());
  arts_regpool_free(other);
  arts_regpool_cleanup();

#ifdef ARTS_DEBUG_INJECTION
  /* A two-node set with one member full.  The machine's availability is
   * unknown here, so the forced-full member is the only thing that says
   * either node lacks room. */
  regpool_sim_declare(2, REGPOOL_SIM_UNKNOWN);
  arts_regpool_set_topology(2, 0x3, NULL);
  inited = arts_regpool_init(NULL, NULL, SLAB_BYTES, 2);
  assert(inited);
  {
    unsigned char *spread =
        (unsigned char *)arts_regpool_alloc_aligned(big, 64);
    assert(spread != NULL);
    assert_spread(arts_regpool_lookup(spread));
    unsigned count = 0, mib = 0, on0 = 0;
    placement_counts(0, &count, &mib, &on0);
    assert(count == 1 && mib == (unsigned)(big >> 20) &&
           "the report must call the spread mapping interleaved");
    spread[0] = 0x5A;
    spread[big - 1] = 0x5A;

    const size_t wider = big + SLAB_BYTES;
    arts_regpool_set_forced_full(0x2);
    unsigned char *q = (unsigned char *)arts_regpool_alloc_aligned(wider, 64);
    assert(q != NULL);
    const arts_regpool_mr_t *mq = arts_regpool_lookup(q);
    assert(mq != NULL && mq->numa_node == 0);
    const regpool_sim_mbind_t *bq = regpool_sim_mbind_at(q);
    assert(bq != NULL && bq->mode == REGPOOL_SIM_MPOL_PREFERRED &&
           bq->mask == 0x1ul &&
           "with node 1 full the mapping must be placed on node 0");
    assert(regpool_sim_policy_ok(bq));
    placement_counts(0, &count, &mib, &on0);
    assert(count == 1 && on0 == 1);
    q[0] = 0x5A;
    q[wider - 1] = 0x5A;
    assert(regpool_sim_all_policies_ok());
    assert(regpool_sim_consulted());
    arts_regpool_set_forced_full(0);
    arts_regpool_free(q);
    arts_regpool_free(spread);
  }
  arts_regpool_cleanup();

  /* Placement fallover.  The caller's node set is the one node its thread
   * runs on, and that node is forced full — so the set cannot absorb the
   * mapping, there is nothing to spread it over, and the pool must place it
   * on the other node rather than refuse it. */
  declare_machine();
  int set_rc = setenv("ARTS_REGPOOL_FORCE_FULL_NODES", "0", 1);
  assert(set_rc == 0);
  arts_regpool_set_topology(2, 0x1, NULL);
  inited = arts_regpool_init(NULL, NULL, SLAB_BYTES, 2);
  assert(inited);
  unsigned char *p =
      (unsigned char *)arts_regpool_alloc_aligned((size_t)48 * 1024 * 1024, 64);
  assert(p != NULL);
  const arts_regpool_mr_t *m = arts_regpool_lookup(p);
  assert(m != NULL);
  assert(m->numa_node != 0 &&
         "a request the caller's node cannot take must relocate, not fail");
  assert(m->numa_node == 1);
  const regpool_sim_mbind_t *b = regpool_sim_mbind_at(p);
  assert(b != NULL && b->mode == REGPOOL_SIM_MPOL_PREFERRED &&
         b->mask == 0x2ul && "the relocated mapping is placed on node 1");
  assert(regpool_sim_policy_ok(b));
  assert(interleaved_count() == 0 &&
         "a set of one full node has nothing to spread the mapping over");
  assert(regpool_sim_all_policies_ok());
  assert(regpool_sim_consulted());
  int relocated = m->numa_node;
  p[0] = 0x5A;
  p[(size_t)48 * 1024 * 1024 - 1] = 0x5A;
  arts_regpool_free(p);
  arts_regpool_cleanup();
  printf("PASS regpool_direct_oversize (relocated off full node 0 to %d)\n",
         relocated);
#else
  printf("PASS regpool_direct_oversize (no injected exhaustion: the full-node "
         "legs are not compiled)\n");
#endif
  return 0;
}
