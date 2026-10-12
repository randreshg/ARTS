/* Lazy carving: a node's memory is mapped when a thread on that node first
 * allocates, not at init.  Init owes only the proof that the pool can map at
 * all, so exactly one node — the initializing thread's — is carved; a node
 * that no thread has allocated on is UNTRIED, which is memory that exists
 * and has simply not been asked for, and a thread moved onto such a node
 * must get ITS node's memory rather than another node's arena (the fallback
 * path is for a node the pool tried and could not carve).
 *
 * Whitebox unit test: no ARTS runtime, no ports, no config.  The machine is
 * declared (regpool_numa_sim.h) with every node's availability unknown (room,
 * to the pool), and the thread is moved between nodes by declaration, so it
 * runs on any host; each slab's placement is checked against the policy the
 * pool asked the kernel for.  Two machines: two nodes, and the widest node
 * set the pool takes (64 nodes), whose last node is carved too — its bit is
 * the last of the mask word, which the kernel reads only when maxnode counts
 * one past it. */
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

static unsigned carved_nodes(void) {
  char *buf = NULL;
  size_t len = 0;
  FILE *f = open_memstream(&buf, &len);
  assert(f != NULL);
  arts_regpool_report(f);
  fclose(f);
  unsigned n = 0;
  for (const char *p = buf; (p = strstr(p, "state=carved")) != NULL;
       p += strlen("state=carved"))
    n++;
  free(buf);
  return n;
}

static void two_nodes(void) {
  regpool_sim_declare(2, REGPOOL_SIM_UNKNOWN);
  regpool_sim_set_thread_node(0);
  arts_regpool_set_topology(2, 0x3, NULL);
  bool inited = arts_regpool_init(NULL, NULL, SLAB_BYTES, 2);
  assert(inited);
  assert(carved_nodes() == 1 &&
         "init must carve the initializing thread's node and no other");

  void *p0 = arts_regpool_alloc_aligned(1 << 20, 64);
  const arts_regpool_mr_t *m0 = arts_regpool_lookup(p0);
  assert(m0 != NULL && m0->numa_node == 0);
  const regpool_sim_mbind_t *b0 = regpool_sim_mbind_at(m0->base);
  assert(b0 != NULL && b0->mode == REGPOOL_SIM_MPOL_PREFERRED &&
         b0->mask == 0x1ul);
  assert(regpool_sim_policy_ok(b0));

  regpool_sim_set_thread_node(1);
  void *p = arts_regpool_alloc_aligned(1 << 20, 64);
  assert(p != NULL);
  const arts_regpool_mr_t *m = arts_regpool_lookup(p);
  assert(m != NULL);
  assert(m->numa_node == 1 &&
         "a thread on an uncarved node must be served from that node");
  const regpool_sim_mbind_t *b = regpool_sim_mbind_at(m->base);
  assert(b != NULL && b->mode == REGPOOL_SIM_MPOL_PREFERRED &&
         b->mask == 0x2ul && "the slab carved for node 1 is placed there");
  assert(regpool_sim_policy_ok(b));
  assert(carved_nodes() == 2);
  assert(regpool_sim_all_policies_ok());
  assert(regpool_sim_consulted());
  memset(p, 0xA5, 1 << 20);
  arts_regpool_free(p);
  arts_regpool_free(p0);

  arts_regpool_cleanup();
}

/* Init on node 0 of a 64-node machine, then the first allocation of a thread
 * on node 63 carves node 63, placed there. */
static void highest_node(void) {
  const int last = 63;
  regpool_sim_declare(last + 1, REGPOOL_SIM_UNKNOWN);
  regpool_sim_set_thread_node(0);
  arts_regpool_set_topology((unsigned)last + 1, ~(uint64_t)0, NULL);
  bool inited = arts_regpool_init(NULL, NULL, SLAB_BYTES, (unsigned)last + 1);
  assert(inited);
  assert(carved_nodes() == 1);

  regpool_sim_set_thread_node(last);
  void *p = arts_regpool_alloc_aligned(1 << 20, 64);
  assert(p != NULL);
  const arts_regpool_mr_t *m = arts_regpool_lookup(p);
  assert(m != NULL && m->numa_node == last &&
         "a thread on the last node must be served from that node");
  const regpool_sim_mbind_t *b = regpool_sim_mbind_at(m->base);
  assert(b != NULL && b->mode == REGPOOL_SIM_MPOL_PREFERRED &&
         b->mask == 1ul << last &&
         "the slab carved for the last node is placed there");
  assert(b->maxnode - 1 > (unsigned long)last &&
         "the kernel must read the mask as far as the last node's bit");
  assert(regpool_sim_policy_ok(b));
  assert(carved_nodes() == 2);
  assert(regpool_sim_all_policies_ok());
  assert(regpool_sim_consulted());
  memset(p, 0xA5, 1 << 20);
  arts_regpool_free(p);

  regpool_sim_set_thread_node(0);
  arts_regpool_cleanup();
}

int main(void) {
  two_nodes();
  highest_node();
  printf("PASS regpool_lazy_carve: init carved node 0, node 1 of 2 and node "
         "63 of 64 carved by their first allocation\n");
  return 0;
}
