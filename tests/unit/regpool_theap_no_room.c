/* A thread with no part of its heap in an arena that has no free slice.
 *
 * A heap bound to one exclusive arena needs one slice of that arena for each
 * thread's part of it (the allocator's theap).  An arena whose slices are all
 * taken can still have free blocks, yet it has no room for a thread that
 * never drew from it.  That is ordinary exhaustion: the pool must serve the
 * thread elsewhere, never abort.  Two legs, each in a fresh pool on a
 * declared machine (regpool_numa_sim.h: one node with room, 32 MiB slabs),
 * both asserted through the pool's report:
 *
 *   first  F fills the first arena until the pool grows a second, frees one
 *          first-arena block and allocates until the sweep serves a block
 *          there again, so neither arena has a free slice.  A new thread's
 *          first allocation then finds the node's current arena, the
 *          second, with no room, and the sweep the first arena too: a
 *          third slab serves it, after two refused binds.
 *   sweep  F fills the first arena until the pool grows a second and stays
 *          alive.  A thread started later fills the second arena; its sweep
 *          finds the first arena with no room for it, and a third slab
 *          serves it, after one refused bind.
 *
 * Whitebox unit test: no ARTS runtime, no ports, no config. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "arts/memory/regpool.h"
#include "regpool_numa_sim.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLAB_BYTES ((size_t)32 << 20)
#define BLOCK ((size_t)4 << 10)
#define NODE_FREE ((size_t)4 << 30)
/* Far more blocks than three slabs of the ladder hold. */
#define MAX_FILL 200000u
#define F_KEPT (1u << 15)

static int g_failed;
#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      printf("FAIL regpool_theap_no_room: " __VA_ARGS__);                      \
      printf("\n");                                                            \
      g_failed = 1;                                                            \
    }                                                                          \
  } while (0)

/* One unsigned field of the report's total line, or ~0u when it is absent. */
static unsigned total_field(const char *key) {
  char *buf = NULL;
  size_t len = 0;
  FILE *f = open_memstream(&buf, &len);
  if (f == NULL)
    abort();
  arts_regpool_report(f);
  fclose(f);
  unsigned v = ~0u;
  const char *line = strstr(buf, "[REGPOOL] total:");
  const char *k = line != NULL ? strstr(line, key) : NULL;
  if (k == NULL || sscanf(k + strlen(key), "%u", &v) != 1)
    v = ~0u;
  free(buf);
  return v;
}

static unsigned slabs(void) { return total_field(" slabs="); }
static unsigned refusals(void) { return total_field(" theap_refusals="); }

static void *alloc_block(void) {
  void *p = arts_regpool_alloc_aligned(BLOCK, 64);
  memset(p, 0x11, 64);
  return p;
}

/* Allocate until the pool holds `n` slabs; false if it never does. */
static bool fill_until(unsigned n) {
  for (unsigned i = 0; i < MAX_FILL; i++) {
    if (slabs() >= n)
      return true;
    (void)alloc_block();
  }
  return false;
}

static pthread_barrier_t g_parked;
static pthread_barrier_t g_release;
static const void *g_first_base;
static bool g_f_ok;
static bool g_l_ok;

static void *f_first(void *arg) {
  (void)arg;
  static void *kept[F_KEPT];
  unsigned n = 0;
  bool grew = false;
  for (unsigned i = 0; i < MAX_FILL; i++) {
    if (slabs() >= 2) {
      grew = true;
      break;
    }
    void *p = alloc_block();
    if (n < F_KEPT)
      kept[n++] = p;
  }
  if (grew && n > 0) {
    g_first_base = arts_regpool_lookup(kept[0])->base;
    arts_regpool_free(kept[0]);
    for (unsigned i = 0; i < MAX_FILL; i++) {
      const arts_regpool_mr_t *m = arts_regpool_lookup(alloc_block());
      if (m != NULL && m->base == g_first_base) {
        g_f_ok = slabs() == 2;
        break;
      }
    }
  }
  pthread_barrier_wait(&g_parked);
  pthread_barrier_wait(&g_release);
  return NULL;
}

static void *l_first(void *arg) {
  (void)arg;
  const arts_regpool_mr_t *m = arts_regpool_lookup(alloc_block());
  g_l_ok = m != NULL && m->base != g_first_base;
  return NULL;
}

static void *f_sweep(void *arg) {
  (void)arg;
  g_f_ok = fill_until(2);
  pthread_barrier_wait(&g_parked);
  pthread_barrier_wait(&g_release);
  return NULL;
}

static void *l_sweep(void *arg) {
  (void)arg;
  g_l_ok = fill_until(3);
  return NULL;
}

static void fresh_pool(void) {
  regpool_sim_declare(1, REGPOOL_SIM_FREE);
  regpool_sim_set_node_free(0, NODE_FREE);
  arts_regpool_set_topology(1, 1, NULL);
  if (!arts_regpool_init(NULL, NULL, SLAB_BYTES, 1)) {
    printf("FAIL regpool_theap_no_room: pool init\n");
    exit(1);
  }
}

/* F runs until it parks, alive; then L runs to its end; then F ends. */
static void run_leg(const char *leg, void *(*f)(void *), void *(*l)(void *),
                    unsigned want_refusals) {
  fresh_pool();
  g_f_ok = false;
  g_l_ok = false;
  g_first_base = NULL;
  pthread_t ft, lt;
  pthread_create(&ft, NULL, f, NULL);
  pthread_barrier_wait(&g_parked);
  pthread_create(&lt, NULL, l, NULL);
  pthread_join(lt, NULL);
  pthread_barrier_wait(&g_release);
  pthread_join(ft, NULL);
  unsigned s = slabs(), r = refusals();
  CHECK(g_f_ok, "%s: the first thread did not reach its setup", leg);
  CHECK(g_l_ok, "%s: the later thread was not served from a new slab", leg);
  CHECK(s == 3, "%s: slabs=%u, want 3", leg, s);
  CHECK(r == want_refusals, "%s: theap_refusals=%u, want %u", leg, r,
        want_refusals);
  arts_regpool_cleanup();
}

int main(void) {
  pthread_barrier_init(&g_parked, NULL, 2);
  pthread_barrier_init(&g_release, NULL, 2);
  run_leg("first", f_first, l_first, 2);
  run_leg("sweep", f_sweep, l_sweep, 1);
  if (g_failed)
    return 1;
  printf("PASS regpool_theap_no_room: a thread with no room in a full arena "
         "is served from another slab, in both legs\n");
  return 0;
}
