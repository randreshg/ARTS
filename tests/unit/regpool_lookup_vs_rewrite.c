/* Lock-free lookups while the pool rewrites a table entry under them.
 *
 * A lookup scans every entry of the pool's table without its lock, and the
 * arena sweep reads every live entry's kind the same way, while an oversize
 * request may give a retired mapping back and rewrite the freed entry for a
 * new mapping.  Neither reader may race the rewrite, and a pointer a live
 * allocation owns must resolve to its own record whatever is rewritten
 * around it:
 *
 *   - a writer cycles oversize mappings of two shapes with every node forced
 *     full, so each request evicts the other shape's retired mapping and
 *     rewrites its entry, and checks that its own block resolves to it.
 *     The first half of its cycles follow one another at once, and only that
 *     half lets a race detector report a range read racing the rewrite: with
 *     every block held, such a race goes unreported.  In the second half it
 *     holds each block until the sweeper has finished a round, so the sweeps
 *     read the entry it rewrites next;
 *   - two readers resolve a live arena block and a live oversize block for as
 *     long as the writer runs; the oversize block's entry lies past the one
 *     the writer rewrites, so every lookup of it reads that entry's range;
 *   - a sweeper keeps a window of arena blocks live and replaces the oldest
 *     few each round, so the arena its heap draws from keeps running out and
 *     the allocation cascade sweeps the table, and gives retired mappings
 *     back, concurrently.
 *
 * Under ThreadSanitizer this is the oracle for the entries' publication; in
 * every tree it checks that a live block always resolves.  The machine is
 * declared (regpool_numa_sim.h): one node, with room by its declared free
 * memory.
 *
 * Whitebox unit test: no ARTS runtime, no ports, no config. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "arts/utils/stdatomic_compat.h"
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arts/memory/regpool.h"
#include "regpool_numa_sim.h"

#define MIB ((size_t)1 << 20)
#define SLAB_BYTES (32 * MIB)
#define NODE_FREE ((size_t)4 << 30)
#define LIVE_DIRECT (36 * MIB)
#define SHAPE_A (40 * MIB)
#define SHAPE_B (48 * MIB)
#define WRITER_CYCLES 120
#define SWEEP_BLOCKS 16
#define SWEEP_TURNOVER 4
#define SWEEP_BLOCK (4 * MIB)
#define READERS 2
#define WAIT_SPINS 100000

/* Set by the writer once its cycles are done: the readers' and the
 * sweeper's stop flag.  Only its own value: each thread reports through its
 * own result, read after the join. */
static _Atomic(bool) g_stop;
/* Sweeper rounds finished, polled by the writer.  Only its own value: an
 * order here would order the sweeps before the rewrites they must not
 * race. */
static _Atomic(unsigned long) g_rounds;
static unsigned char *g_arena_block;
static unsigned char *g_direct_block;

struct reader_result {
  unsigned long lookups;
  bool failed;
};

static void *reader(void *arg) {
  struct reader_result *res = (struct reader_result *)arg;
  while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
    const arts_regpool_mr_t *a = arts_regpool_lookup(g_arena_block + 4096);
    const arts_regpool_mr_t *d = arts_regpool_lookup(g_direct_block + 4096);
    if (a == NULL || (const unsigned char *)a->base > g_arena_block ||
        g_arena_block >= (const unsigned char *)a->base + a->len) {
      res->failed = true;
    }
    if (d == NULL || d->base != g_direct_block || d->len < LIVE_DIRECT) {
      res->failed = true;
    }
    res->lookups++;
  }
  return NULL;
}

struct sweeper_result {
  unsigned long rounds;
  bool failed;
};

static void *sweeper(void *arg) {
  struct sweeper_result *res = (struct sweeper_result *)arg;
  void *blocks[SWEEP_BLOCKS];
  for (int i = 0; i < SWEEP_BLOCKS; i++) {
    blocks[i] = arts_regpool_alloc_aligned(SWEEP_BLOCK, 64);
  }
  unsigned next = 0;
  while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
    for (int k = 0; k < SWEEP_TURNOVER; k++) {
      arts_regpool_free(blocks[next]);
      blocks[next] = arts_regpool_alloc_aligned(SWEEP_BLOCK, 64);
      if (blocks[next] == NULL || arts_regpool_lookup(blocks[next]) == NULL) {
        res->failed = true;
      }
      next = (next + 1) % SWEEP_BLOCKS;
    }
    res->rounds++;
    atomic_fetch_add_explicit(&g_rounds, 1, memory_order_relaxed);
  }
  for (int i = 0; i < SWEEP_BLOCKS; i++) {
    arts_regpool_free(blocks[i]);
  }
  return NULL;
}

int main(void) {
  regpool_sim_declare(1, REGPOOL_SIM_FREE);
  regpool_sim_set_node_free(0, NODE_FREE);
  arts_regpool_set_topology(1, 1, NULL);
  if (!arts_regpool_init(NULL, NULL, SLAB_BYTES, 1)) {
    fprintf(stderr, "FAIL regpool_lookup_vs_rewrite: pool init\n");
    return 1;
  }
  g_arena_block = (unsigned char *)arts_regpool_alloc_aligned(MIB, 64);
  /* The entry the writer's cycles rewrite, retired before the live oversize
   * block takes the entry after it. */
  void *first = arts_regpool_alloc_aligned(SHAPE_A, 64);
  arts_regpool_free(first);
  g_direct_block = (unsigned char *)arts_regpool_alloc_aligned(LIVE_DIRECT, 64);
  if (g_arena_block == NULL || first == NULL || g_direct_block == NULL) {
    fprintf(stderr, "FAIL regpool_lookup_vs_rewrite: setup allocation\n");
    return 1;
  }
  /* No placement can succeed from here on: every oversize request of a new
   * shape gives the retired mapping back and reuses its entry. */
  arts_regpool_set_forced_full(1);

  pthread_t readers[READERS];
  struct reader_result rres[READERS];
  memset(rres, 0, sizeof(rres));
  pthread_t sweep;
  struct sweeper_result sres;
  memset(&sres, 0, sizeof(sres));
  for (int i = 0; i < READERS; i++) {
    if (pthread_create(&readers[i], NULL, reader, &rres[i]) != 0) {
      fprintf(stderr, "FAIL regpool_lookup_vs_rewrite: pthread_create\n");
      return 1;
    }
  }
  if (pthread_create(&sweep, NULL, sweeper, &sres) != 0) {
    fprintf(stderr, "FAIL regpool_lookup_vs_rewrite: pthread_create\n");
    return 1;
  }

  int bad_own = 0;
  for (int c = 0; c < WRITER_CYCLES; c++) {
    size_t size = (c % 2 == 0) ? SHAPE_A : SHAPE_B;
    unsigned char *p = (unsigned char *)arts_regpool_alloc_aligned(size, 64);
    const arts_regpool_mr_t *m = p != NULL ? arts_regpool_lookup(p) : NULL;
    if (m == NULL || m->base != p || m->len < size) {
      bad_own++;
    }
    unsigned long r0 = atomic_load_explicit(&g_rounds, memory_order_relaxed);
    for (int spin = 0;
         c >= WRITER_CYCLES / 2 && spin < WAIT_SPINS &&
         atomic_load_explicit(&g_rounds, memory_order_relaxed) == r0;
         spin++) {
      sched_yield();
    }
    if (p != NULL) {
      p[0] = (unsigned char)c;
      p[size - 1] = (unsigned char)c;
      arts_regpool_free(p);
    }
  }
  atomic_store_explicit(&g_stop, true, memory_order_relaxed);
  for (int i = 0; i < READERS; i++) {
    pthread_join(readers[i], NULL);
  }
  pthread_join(sweep, NULL);

  int rc = 0;
  if (bad_own != 0) {
    fprintf(stderr,
            "FAIL regpool_lookup_vs_rewrite: %d of %d oversize blocks did not "
            "resolve to their own record\n",
            bad_own, WRITER_CYCLES);
    rc = 1;
  }
  for (int i = 0; i < READERS; i++) {
    if (rres[i].failed || rres[i].lookups == 0) {
      fprintf(stderr,
              "FAIL regpool_lookup_vs_rewrite: reader %d (%lu lookups) saw a "
              "live block resolve elsewhere or nowhere\n",
              i, rres[i].lookups);
      rc = 1;
    }
  }
  if (sres.failed) {
    fprintf(stderr,
            "FAIL regpool_lookup_vs_rewrite: an arena block was refused or "
            "did not resolve\n");
    rc = 1;
  }
  arts_regpool_free(g_arena_block);
  arts_regpool_free(g_direct_block);
  arts_regpool_set_forced_full(0);
  arts_regpool_cleanup();
  if (rc == 0) {
    printf("PASS regpool_lookup_vs_rewrite: %d oversize rewrites under %d "
           "readers and %lu arena sweep rounds\n",
           WRITER_CYCLES, READERS, sres.rounds);
  }
  return rc;
}
