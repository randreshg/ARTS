/* Every node full to its own grow: the pool must still grow.
 *
 * A full node's own grow maps nothing, and a node grown on its behalf maps
 * only a slab placed on itself.  Availability is read per decision, and the
 * readings disagree whenever it moves between them: a node can look full to
 * itself and roomy to everyone else, so the node asked in a full node's
 * stead finds itself full in turn.  The pool must not turn that into a
 * failed allocation (or a failed init) while the machine still has memory:
 * once every other node has declined, the requesting node grows a slab the
 * kernel places.
 *
 * The declared machine (regpool_numa_sim.h, SELF_FULL) makes that
 * disagreement permanent and deterministic: each node reads as empty to the
 * grow sizing a slab for it and as plentiful to every other reader.  Every
 * slab mapped here therefore comes from the last-resort grow, at init and
 * in the exhaustion cascade.
 *
 * Whitebox unit test: no ARTS runtime, no ports, no config; runs on any
 * host. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arts/memory/regpool.h"
#include "regpool_numa_sim.h"

#define SLAB_BYTES ((size_t)32 * 1024 * 1024)
#define BLOCK ((size_t)1 << 20)
#define BLOCKS 80 /* past the first slab, so the cascade must grow */

static int g_failed;
#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      printf("FAIL regpool_spill_cycle: " __VA_ARGS__);                        \
      printf("\n");                                                            \
      g_failed = 1;                                                            \
    }                                                                          \
  } while (0)

typedef struct {
  char state[16];
  unsigned arenas;
} node_line_t;

static void report_lines(node_line_t *n0, node_line_t *n1, unsigned *slabs,
                         unsigned *unplaced, unsigned *refusals) {
  char *buf = NULL;
  size_t len = 0;
  FILE *f = open_memstream(&buf, &len);
  if (f == NULL)
    abort();
  arts_regpool_report(f);
  fclose(f);
  node_line_t *out[2] = {n0, n1};
  for (int n = 0; n < 2; n++) {
    char key[32];
    snprintf(key, sizeof key, "[REGPOOL] node %d:", n);
    const char *line = strstr(buf, key);
    if (line == NULL || sscanf(line, "[REGPOOL] node %*u: state=%15s arenas=%u",
                               out[n]->state, &out[n]->arenas) != 2) {
      printf("FAIL regpool_spill_cycle: unparsable report:\n%s", buf);
      exit(1);
    }
  }
  const char *line = strstr(buf, "[REGPOOL] total:");
  if (line == NULL ||
      sscanf(line,
             "[REGPOOL] total: slabs=%u/%*u mapped=%*u unplaced=%u base=%*u "
             "arena_slots=%*u/%*u table_refusals=%u",
             slabs, unplaced, refusals) != 3) {
    printf("FAIL regpool_spill_cycle: unparsable report:\n%s", buf);
    exit(1);
  }
  free(buf);
}

int main(void) {
  regpool_sim_declare(2, REGPOOL_SIM_SELF_FULL);
  arts_regpool_set_topology(2, 0x3, NULL);

  /* Init: node 0 is full, node 1 asked in its stead finds itself full, and
   * init must still carve one of them. */
  if (!arts_regpool_init(NULL, NULL, SLAB_BYTES, 2)) {
    printf("FAIL regpool_spill_cycle: init failed with memory on the "
           "machine — each node found itself full and the other roomy\n");
    return 1;
  }
  node_line_t n0, n1;
  unsigned slabs = 0, unplaced = 0, refusals = 0;
  report_lines(&n0, &n1, &slabs, &unplaced, &refusals);
  CHECK(strcmp(n0.state, "carved") == 0 && n0.arenas == 0 &&
            unplaced >= (unsigned)(SLAB_BYTES >> 20) && slabs == 1,
        "init: node 0 state=%s placed arenas=%u, unplaced %u MiB, slabs %u; "
        "expected one kernel-placed slab for node 0",
        n0.state, n0.arenas, unplaced, slabs);
  CHECK(strcmp(n1.state, "refused") == 0,
        "init: node 1 state=%s, expected refused (it found itself full)",
        n1.state);
  /* Node 1 was grown on node 0's behalf and found itself full; node 0 then
   * sized its own slab. */
  CHECK(atomic_load_explicit(&regpool_sim_self_reads[1],
                             memory_order_relaxed) >= 1 &&
            atomic_load_explicit(&regpool_sim_self_reads[0],
                                 memory_order_relaxed) >= 1,
        "init: the declared machine was not consulted as intended (node 1 "
        "self %u, node 0 self %u)",
        atomic_load_explicit(&regpool_sim_self_reads[1], memory_order_relaxed),
        atomic_load_explicit(&regpool_sim_self_reads[0], memory_order_relaxed));
  unsigned self0 =
      atomic_load_explicit(&regpool_sim_self_reads[0], memory_order_relaxed);

  /* Allocation past the first slab: the cascade's own grow maps nothing and
   * the other node finds itself full, and the cascade must grow anyway. */
  for (unsigned i = 0; i < BLOCKS; i++) {
    void *p = arts_regpool_alloc_aligned(BLOCK, 64);
    const arts_regpool_mr_t *m = arts_regpool_lookup(p);
    CHECK(m != NULL && m->numa_node == -1, "block %u: %s", i,
          m == NULL ? "escaped every registered slab"
                    : "not on a kernel-placed slab");
    if (m == NULL)
      break;
    memset(p, 0x5A, BLOCK);
  }
  report_lines(&n0, &n1, &slabs, &unplaced, &refusals);
  CHECK(slabs >= 2 && n0.arenas == 0 && n1.arenas == 0 && refusals == 0,
        "after %u blocks: slabs %u (node 0 placed %u, node 1 placed %u), "
        "refusals %u; expected kernel-placed slabs only",
        BLOCKS, slabs, n0.arenas, n1.arenas, refusals);
  CHECK(atomic_load_explicit(&regpool_sim_self_reads[0], memory_order_relaxed) >
            self0,
        "the cascade never sized a slab for node 0 again");
  CHECK(atomic_load_explicit(&regpool_sim_mbind_calls, memory_order_relaxed) ==
            0,
        "%u placement policies set where every slab is the kernel's to place",
        atomic_load_explicit(&regpool_sim_mbind_calls, memory_order_relaxed));
  CHECK(regpool_sim_consulted(), "the declared machine was not consulted");

  arts_regpool_cleanup();
  if (g_failed)
    return 1;
  printf("PASS regpool_spill_cycle: %u kernel-placed slabs (%u MiB) mapped "
         "while each node found itself full and the other roomy\n",
         slabs, unplaced);
  return 0;
}
