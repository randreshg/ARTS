/* What serves a full node's demand, in order.
 *
 * A node whose free memory cannot take its next slab is full, and its own
 * grow maps nothing.  Its demand is then served from memory that exists
 * before any is mapped where the kernel chooses:
 *
 *   (i)   other nodes' arenas, while they have space — however full their
 *         nodes are — before any new slab;
 *   (ii)  then a slab placed on the nearest node that can take one, grown
 *         on the full node's behalf; such a node maps a placed slab or
 *         nothing, never one the kernel places;
 *   (iii) the caller's own set before any node outside it, and a node
 *         outside it before the kernel;
 *   (iv)  and only when every arena and every node declined, a slab the
 *         kernel places, the requesting node's own, sized by its own rung.
 *
 * The machine is declared (regpool_numa_sim.h) so every leg runs and
 * asserts the same on any host.  The declared machine's availability
 * (MACHINE) keeps the slot share far above every rung used here whatever
 * the allocator's arena-table size, so each node's floor is its next rung:
 * 32 MiB for a node with no slab, 64 MiB for a node whose one slab is the
 * 32 MiB base.  A node's usable room is its free memory above the pool's
 * headroom; a 32 MiB arena holds fewer than BLOCKS 1 MiB blocks.
 *
 * Whitebox unit test: no ARTS runtime, no ports, no config. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arts/memory/regpool.h"
#include "regpool_numa_sim.h"

#define MIB ((size_t)1 << 20)
#define SLAB_BYTES (32 * MIB)
#define BLOCK MIB
#define BLOCKS 40 /* past a 32 MiB arena, so the cascade must go on */
#define ROOMY ((size_t)64 << 30)
#define MACHINE ((size_t)1 << 40)
/* Usable room for a 32 MiB slab but not a 64 MiB one. */
#define BETWEEN (ARTS_REGPOOL_NODE_HEADROOM + 48 * MIB)
#define NO_ROOM ARTS_REGPOOL_NODE_HEADROOM
#define MAX_NODES 3

static int g_failed;
#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      printf("FAIL regpool_spill_target: " __VA_ARGS__);                       \
      printf("\n");                                                            \
      g_failed = 1;                                                            \
    }                                                                          \
  } while (0)

typedef struct {
  char state[16];
  unsigned arenas, grows, spills;
} node_line_t;

typedef struct {
  node_line_t node[MAX_NODES];
  unsigned slabs, unplaced;
} shape_t;

static shape_t shape(int nodes, const char *leg) {
  char *buf = NULL;
  size_t len = 0;
  FILE *f = open_memstream(&buf, &len);
  if (f == NULL)
    abort();
  arts_regpool_report(f);
  fclose(f);
  shape_t s;
  bool ok = true;
  for (int n = 0; n < nodes && ok; n++) {
    char key[32];
    snprintf(key, sizeof key, "[REGPOOL] node %d:", n);
    const char *line = strstr(buf, key);
    int got = line == NULL ? 0
                           : sscanf(line,
                                    "[REGPOOL] node %*u: state=%15s arenas=%u "
                                    "mapped=%*u grows=%u grow_ms=%*u waits=%*u "
                                    "wait_ms=%*u spills=%u",
                                    s.node[n].state, &s.node[n].arenas,
                                    &s.node[n].grows, &s.node[n].spills);
    ok = got == 4;
  }
  if (ok) {
    const char *line = strstr(buf, "[REGPOOL] total:");
    int got = line == NULL ? 0
                           : sscanf(line,
                                    "[REGPOOL] total: slabs=%u/%*u mapped=%*u "
                                    "unplaced=%u",
                                    &s.slabs, &s.unplaced);
    ok = got == 2;
  }
  if (!ok) {
    printf("FAIL regpool_spill_target: %s: unparsable report:\n%s", leg, buf);
    exit(1);
  }
  free(buf);
  return s;
}

/* A declared machine of `nodes` nodes, every one roomy, on which the pool is
 * initialized by a thread on `init_node`; `set` is the caller's node set. */
static void start(int nodes, uint64_t set, int init_node, const char *leg) {
  regpool_sim_declare(nodes, REGPOOL_SIM_FREE);
  regpool_sim_set_machine_available(MACHINE);
  for (int n = 0; n < nodes; n++)
    regpool_sim_set_node_free(n, ROOMY);
  regpool_sim_set_thread_node(init_node);
  arts_regpool_set_topology((unsigned)nodes, set, NULL);
  if (!arts_regpool_init(NULL, NULL, SLAB_BYTES, (unsigned)nodes)) {
    printf("FAIL regpool_spill_target: %s: init failed\n", leg);
    exit(1);
  }
}

static void finish(const char *leg) {
  CHECK(regpool_sim_all_policies_ok(),
        "%s: a placement policy was set after population or with bad "
        "arguments",
        leg);
  CHECK(regpool_sim_consulted(), "%s: the declared machine was not consulted",
        leg);
  arts_regpool_cleanup();
}

/* Allocate `n` blocks from the calling thread, recording where each landed
 * in `where` (a node, or -1 for a kernel-placed slab). */
static void alloc_blocks(unsigned n, int *where, const char *leg) {
  for (unsigned i = 0; i < n; i++) {
    void *p = arts_regpool_alloc_aligned(BLOCK, 64);
    const arts_regpool_mr_t *m = arts_regpool_lookup(p);
    CHECK(m != NULL, "%s: block %u escaped every registered slab", leg, i);
    if (m == NULL) {
      where[i] = -2;
      continue;
    }
    where[i] = m->numa_node;
    memset(p, 0x5A, BLOCK);
  }
}

/* (ii, iv) Node 0 is full to everyone; node 1 is full to its own grow and
 * roomy to every other reader.  Node 1, grown on node 0's behalf, finds
 * itself full and must decline: every slab is node 0's own kernel-placed
 * one — at init, and again in the exhaustion cascade. */
static void leg_peer_declines(void) {
  const char *leg = "peer";
  regpool_sim_declare(2, REGPOOL_SIM_FREE);
  regpool_sim_set_machine_available(MACHINE);
  regpool_sim_set_node_mode(1, REGPOOL_SIM_SELF_FULL);
  regpool_sim_set_node_free(0, NO_ROOM);
  regpool_sim_set_thread_node(0);
  arts_regpool_set_topology(2, 0x3, NULL);
  if (!arts_regpool_init(NULL, NULL, SLAB_BYTES, 2)) {
    printf("FAIL regpool_spill_target: %s: init failed\n", leg);
    exit(1);
  }
  shape_t s = shape(2, leg);
  CHECK(s.node[1].grows == 0 && s.node[0].grows == 1 &&
            s.unplaced == SLAB_BYTES / MIB,
        "%s: init: node 0 grew %u, node 1 grew %u, %u MiB kernel-placed; "
        "expected one %zu MiB kernel-placed slab, node 0's",
        leg, s.node[0].grows, s.node[1].grows, s.unplaced, SLAB_BYTES / MIB);
  CHECK(atomic_load_explicit(&regpool_sim_self_reads[1],
                             memory_order_relaxed) >= 1,
        "%s: init never grew node 1 on node 0's behalf", leg);
  unsigned self1 =
      atomic_load_explicit(&regpool_sim_self_reads[1], memory_order_relaxed);

  int where[BLOCKS];
  alloc_blocks(BLOCKS, where, leg);
  for (unsigned i = 0; i < BLOCKS; i++)
    CHECK(where[i] == -1,
          "%s: block %u served from node %d, expected a kernel-placed slab",
          leg, i, where[i]);
  s = shape(2, leg);
  CHECK(s.node[1].grows == 0 && s.node[0].grows == 2,
        "%s: cascade: node 0 grew %u, node 1 grew %u; expected node 0's "
        "second kernel-placed slab and nothing from node 1",
        leg, s.node[0].grows, s.node[1].grows);
  CHECK(atomic_load_explicit(&regpool_sim_self_reads[1], memory_order_relaxed) >
            self1,
        "%s: the cascade never grew node 1 on node 0's behalf", leg);
  CHECK(atomic_load_explicit(&regpool_sim_mbind_calls, memory_order_relaxed) ==
            0,
        "%s: %u placement policies set where every slab is the kernel's", leg,
        atomic_load_explicit(&regpool_sim_mbind_calls, memory_order_relaxed));
  finish(leg);
}

/* (i, iv) Node 1 has one slab and its node is then full, as is node 0, which
 * has none.  Node 0's demand must take node 1's arena first — moved there
 * once — and map node 0's own kernel-placed slab only once that arena is
 * spent; node 1 grows nothing. */
static void leg_existing_arena_first(void) {
  const char *leg = "existing-arena";
  start(2, 0x3, 1, leg);
  shape_t s0 = shape(2, leg);
  CHECK(s0.node[1].arenas == 1 && s0.node[1].grows == 1 &&
            s0.node[0].grows == 0,
        "%s: init carved node 1 arenas=%u grows=%u, node 0 grows=%u; "
        "expected node 1 only",
        leg, s0.node[1].arenas, s0.node[1].grows, s0.node[0].grows);
  regpool_sim_set_node_free(0, NO_ROOM);
  regpool_sim_set_node_free(1, NO_ROOM);
  regpool_sim_set_thread_node(0);
  int where[BLOCKS];
  alloc_blocks(BLOCKS, where, leg);
  unsigned on1 = 0;
  while (on1 < BLOCKS && where[on1] == 1)
    on1++;
  unsigned rest = on1;
  while (rest < BLOCKS && where[rest] == -1)
    rest++;
  CHECK(on1 > 0 && rest == BLOCKS,
        "%s: %u blocks on node 1's arena, then block %u on node %d; expected "
        "node 1's arena first, then node 0's kernel-placed slab",
        leg, on1, rest, rest < BLOCKS ? where[rest] : -1);
  shape_t s = shape(2, leg);
  CHECK(s.node[0].grows == 1 && s.node[0].spills == 1 &&
            s.unplaced == SLAB_BYTES / MIB,
        "%s: node 0 grew %u, spilled %u, %u MiB kernel-placed; expected one "
        "move onto node 1's arena and then one %zu MiB kernel-placed slab",
        leg, s.node[0].grows, s.node[0].spills, s.unplaced, SLAB_BYTES / MIB);
  CHECK(s.node[1].grows == 1, "%s: node 1 grew %u -> %u; expected nothing", leg,
        s0.node[1].grows, s.node[1].grows);
  finish(leg);
}

/* (ii) Node 0 has one slab and is full; node 1 has none and room for its
 * own 32 MiB floor (not for node 0's 64 MiB one).  Node 0's own grow maps
 * nothing, and its demand past its arena is served by a slab placed on node
 * 1 — moved there once — with no kernel-placed slab. */
static void leg_peer_placed(void) {
  const char *leg = "peer-placed";
  start(2, 0x3, 0, leg);
  shape_t s0 = shape(2, leg);
  CHECK(s0.node[0].arenas == 1 && s0.node[1].grows == 0,
        "%s: init carved node 0 arenas=%u, node 1 grows=%u; expected node 0 "
        "only",
        leg, s0.node[0].arenas, s0.node[1].grows);
  regpool_sim_set_node_free(0, NO_ROOM);
  regpool_sim_set_node_free(1, BETWEEN);
  unsigned maps0 =
      atomic_load_explicit(&regpool_sim_mbind_calls, memory_order_relaxed);
  bool grew = arts_regpool_grow(0);
  shape_t s = shape(2, leg);
  CHECK(!grew && s.node[0].grows == s0.node[0].grows && s.unplaced == 0 &&
            atomic_load_explicit(&regpool_sim_mbind_calls,
                                 memory_order_relaxed) == maps0,
        "%s: node 0's grow returned %d, grew %u -> %u, %u MiB kernel-placed; "
        "expected nothing mapped",
        leg, grew, s0.node[0].grows, s.node[0].grows, s.unplaced);

  int where[BLOCKS];
  alloc_blocks(BLOCKS, where, leg);
  unsigned on0 = 0;
  while (on0 < BLOCKS && where[on0] == 0)
    on0++;
  unsigned rest = on0;
  while (rest < BLOCKS && where[rest] == 1)
    rest++;
  CHECK(on0 > 0 && on0 < BLOCKS && rest == BLOCKS,
        "%s: %u blocks on node 0, then block %u on node %d; expected node "
        "0's arena, then a slab placed on node 1",
        leg, on0, rest, rest < BLOCKS ? where[rest] : -1);
  s = shape(2, leg);
  CHECK(s.node[1].grows == 1 && s.node[0].spills == 1 && s.unplaced == 0,
        "%s: node 1 grew %u, node 0 spilled %u, %u MiB kernel-placed; "
        "expected one slab placed on node 1 and one move onto it",
        leg, s.node[1].grows, s.node[0].spills, s.unplaced);
  finish(leg);
}

/* (iii) Three nodes, the caller's set {0, 1}; node 0 is full, node 1 has
 * room for its 32 MiB floor and node 2, outside the set, far more.  Node 1
 * takes the slab: the set comes first, whatever the free memory says. */
static void leg_set_first(void) {
  const char *leg = "set-first";
  start(3, 0x3, 0, leg);
  regpool_sim_set_node_free(0, NO_ROOM);
  regpool_sim_set_node_free(1, BETWEEN);
  regpool_sim_set_node_free(2, ROOMY);
  shape_t s0 = shape(3, leg);
  /* Node 0's arena (carved at init) is spent first, then the cascade grows
   * another node. */
  int where[BLOCKS];
  alloc_blocks(BLOCKS, where, leg);
  unsigned on0 = 0;
  while (on0 < BLOCKS && where[on0] == 0)
    on0++;
  unsigned rest = on0;
  while (rest < BLOCKS && where[rest] == 1)
    rest++;
  CHECK(on0 > 0 && on0 < BLOCKS && rest == BLOCKS,
        "%s: %u blocks on node 0, then block %u on node %d; expected node "
        "0's arena, then a slab placed on node 1",
        leg, on0, rest, rest < BLOCKS ? where[rest] : -1);
  shape_t s = shape(3, leg);
  CHECK(s.node[1].grows == s0.node[1].grows + 1 &&
            s.node[2].grows == s0.node[2].grows && s.unplaced == 0,
        "%s: node 1 grew %u -> %u, node 2 grew %u -> %u, %u MiB "
        "kernel-placed; expected one slab placed on node 1",
        leg, s0.node[1].grows, s.node[1].grows, s0.node[2].grows,
        s.node[2].grows, s.unplaced);
  finish(leg);
}

/* (iii) Three nodes, the caller's set {0, 1}; node 0 is full, node 1 full
 * to its own grow; node 2, outside the set, has room.  Init carves node 2 —
 * after node 1 was asked and declined, and before any kernel-placed slab —
 * and node 0's demand is served there. */
static void leg_outside_set_before_kernel(void) {
  const char *leg = "outside-set";
  regpool_sim_declare(3, REGPOOL_SIM_FREE);
  regpool_sim_set_machine_available(MACHINE);
  regpool_sim_set_node_mode(1, REGPOOL_SIM_SELF_FULL);
  regpool_sim_set_node_free(0, NO_ROOM);
  regpool_sim_set_node_free(2, ROOMY);
  regpool_sim_set_thread_node(0);
  arts_regpool_set_topology(3, 0x3, NULL);
  if (!arts_regpool_init(NULL, NULL, SLAB_BYTES, 3)) {
    printf("FAIL regpool_spill_target: %s: init failed\n", leg);
    exit(1);
  }
  CHECK(atomic_load_explicit(&regpool_sim_self_reads[1],
                             memory_order_relaxed) >= 1,
        "%s: node 1 was never asked before node 2", leg);
  int where[4];
  alloc_blocks(4, where, leg);
  for (unsigned i = 0; i < 4; i++)
    CHECK(where[i] == 2, "%s: block %u served from node %d, expected node 2",
          leg, i, where[i]);
  shape_t s = shape(3, leg);
  CHECK(s.node[2].grows == 1 && s.node[0].grows == 0 && s.node[1].grows == 0 &&
            s.unplaced == 0,
        "%s: grows %u/%u/%u, %u MiB kernel-placed; expected one slab placed "
        "on node 2",
        leg, s.node[0].grows, s.node[1].grows, s.node[2].grows, s.unplaced);
  finish(leg);
}

int main(void) {
  leg_peer_declines();
  leg_existing_arena_first();
  leg_peer_placed();
  leg_set_first();
  leg_outside_set_before_kernel();
  if (g_failed)
    return 1;
  printf("PASS regpool_spill_target: a full node's demand takes existing "
         "arenas, then placed slabs on other nodes, then the kernel's\n");
  return 0;
}
