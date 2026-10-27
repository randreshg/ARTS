/* Room in an exclusive arena is per thread and per request class.
 *
 * An arena's free slices are shared, but each of its pages belongs to one
 * thread's heap and serves one block size, so an arena can refuse one
 * thread, or one size of a thread, while it still has room for another.  A
 * refusal must then move only the refused (thread, class) and leave the
 * node's other threads and classes where they have room: otherwise two of
 * them whose room lies in different arenas send each other back through the
 * exhaustion cascade at every request.
 *
 * Four legs, each on its own declared machine (regpool_numa_sim.h) and a
 * fresh pool with 32 MiB base slabs; workers are threads driven one command
 * at a time, and a block's slab is read back through arts_regpool_lookup.
 *
 *   theap  one node.  U has room only in arena A1 and no part of its heap in
 *          A2; T has room only in A2; neither arena has a free slice.
 *   page   as theap, but U also has a part of its heap in A2, whose pages it
 *          filled.
 *   spill  two nodes; node 0 cannot grow and has no arena.  U and T run on
 *          node 0, W on node 1, which has B1 and B2, neither with a free
 *          slice.  U has room only in B1; T only in B2, and no part of its
 *          heap in B1.
 *   sizes  one node, one thread T: 1 KiB blocks have room only in A1, 2 KiB
 *          blocks only in A2.
 *
 * Then K lock-step rounds (U one block, then T one block; in sizes, one
 * 1 KiB block, then one 2 KiB block) and, in the first three legs, K
 * allocations of each thread at once.  Exact counts from the pool's report:
 * in theap the first round's two cascades (U refused by A2's theap, T by its
 * spent A1 binding) and one refused bind, and nothing else anywhere; no slab
 * added; every block from the slab where its thread and size have room.
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
#define SIZE_A ((size_t)1 << 10)
#define SIZE_B ((size_t)2 << 10)
#define K 16
#define KEEP 256
/* More blocks than a thread holds in any leg: the first two slabs of the
 * ladder (32 and 48 MiB) in 1 KiB blocks. */
#define MAX_BLOCKS (1u << 17)

static int g_failed;
#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      printf("FAIL regpool_room_per_thread: " __VA_ARGS__);                    \
      printf("\n");                                                            \
      g_failed = 1;                                                            \
    }                                                                          \
  } while (0)

/* ---- the pool's report ------------------------------------------------- */

typedef struct {
  unsigned cascades, spills, refusals, slabs;
} counts_t;

/* One unsigned field of the report line that starts with `line`, or ~0u. */
static unsigned field(const char *buf, const char *line, const char *key) {
  const char *l = strstr(buf, line);
  const char *k = l != NULL ? strstr(l, key) : NULL;
  const char *nl = l != NULL ? strchr(l, '\n') : NULL;
  unsigned v = ~0u;
  if (k == NULL || (nl != NULL && k > nl) ||
      sscanf(k + strlen(key), "%u", &v) != 1)
    v = ~0u;
  return v;
}

static counts_t counts(int node) {
  char *buf = NULL;
  size_t len = 0;
  FILE *f = open_memstream(&buf, &len);
  if (f == NULL)
    abort();
  arts_regpool_report(f);
  fclose(f);
  char line[32];
  snprintf(line, sizeof line, "[REGPOOL] node %d:", node);
  counts_t c = {field(buf, line, " cascades="), field(buf, line, " spills="),
                field(buf, "[REGPOOL] total:", " theap_refusals="),
                field(buf, "[REGPOOL] total:", " slabs=")};
  free(buf);
  if (c.cascades == ~0u || c.spills == ~0u || c.refusals == ~0u ||
      c.slabs == ~0u) {
    printf("FAIL regpool_room_per_thread: the report lacks a field\n");
    exit(1);
  }
  return c;
}

static const void *slab_of(const void *p) {
  const arts_regpool_mr_t *m = arts_regpool_lookup(p);
  if (m == NULL) {
    printf("FAIL regpool_room_per_thread: block %p escaped every slab\n", p);
    exit(1);
  }
  return m->base;
}

/* ---- workers, driven one command at a time ----------------------------- */

typedef struct worker {
  pthread_t th;
  int node;
  pthread_mutex_t mu;
  pthread_cond_t cv;
  void (*cmd)(struct worker *);
  bool busy;
  void *blocks[MAX_BLOCKS];
  unsigned n;
  const void *last_slab;
} worker_t;

static void *worker_main(void *arg) {
  worker_t *w = arg;
  regpool_sim_set_thread_node(w->node);
  pthread_mutex_lock(&w->mu);
  for (;;) {
    while (!w->busy)
      pthread_cond_wait(&w->cv, &w->mu);
    void (*cmd)(worker_t *) = w->cmd;
    if (cmd == NULL) {
      w->busy = false;
      pthread_cond_broadcast(&w->cv);
      pthread_mutex_unlock(&w->mu);
      return NULL;
    }
    pthread_mutex_unlock(&w->mu);
    cmd(w);
    pthread_mutex_lock(&w->mu);
    w->busy = false;
    pthread_cond_broadcast(&w->cv);
  }
}

static void post(worker_t *w, void (*cmd)(worker_t *)) {
  pthread_mutex_lock(&w->mu);
  w->cmd = cmd;
  w->busy = true;
  pthread_cond_broadcast(&w->cv);
  pthread_mutex_unlock(&w->mu);
}

static void wait_idle(worker_t *w) {
  pthread_mutex_lock(&w->mu);
  while (w->busy)
    pthread_cond_wait(&w->cv, &w->mu);
  pthread_mutex_unlock(&w->mu);
}

static void run(worker_t *w, void (*cmd)(worker_t *)) {
  post(w, cmd);
  wait_idle(w);
}

static void start(worker_t *w, int node) {
  memset(w, 0, sizeof *w);
  w->node = node;
  pthread_mutex_init(&w->mu, NULL);
  pthread_cond_init(&w->cv, NULL);
  if (pthread_create(&w->th, NULL, worker_main, w) != 0)
    abort();
}

static void stop(worker_t *w) {
  run(w, NULL);
  pthread_join(w->th, NULL);
  pthread_mutex_destroy(&w->mu);
  pthread_cond_destroy(&w->cv);
}

/* ---- commands ------------------------------------------------------------ */

/* Read by the commands; written by main only while every worker is idle. */
static size_t g_size = BLOCK;
static const void *g_from;
static unsigned g_target_slabs;

static void cmd_one(worker_t *w) {
  void *p = arts_regpool_alloc_aligned(g_size, 64);
  memset(p, 0x5A, 64);
  w->last_slab = slab_of(p);
  if (w->n >= MAX_BLOCKS) {
    printf("FAIL regpool_room_per_thread: a worker holds %u blocks\n", w->n);
    exit(1);
  }
  w->blocks[w->n++] = p;
}

/* KEEP blocks of 4 KiB, or 2 * KEEP of a smaller size, every other one
 * freed: room in the arena they came from that only this thread's heap can
 * use, with no page of them left empty. */
static void cmd_room(worker_t *w) {
  unsigned first = w->n;
  unsigned count = g_size == BLOCK ? KEEP : 2 * KEEP;
  for (unsigned i = 0; i < count; i++)
    cmd_one(w);
  for (unsigned i = first + 1; i < w->n; i += 2) {
    arts_regpool_free(w->blocks[i]);
    w->blocks[i] = NULL;
  }
}

/* Allocate until the pool has g_target_slabs slabs. */
static void cmd_fill_until_slabs(worker_t *w) {
  while (counts(0).slabs < g_target_slabs)
    cmd_one(w);
}

/* Allocate until a block comes from g_from. */
static void cmd_until_from(worker_t *w) {
  do
    cmd_one(w);
  while (w->last_slab != g_from);
}

/* Allocate until a block comes from another slab than g_from. */
static void cmd_until_other(worker_t *w) {
  do
    cmd_one(w);
  while (w->last_slab == g_from);
}

/* Free one of this thread's blocks in g_from, then allocate until a block
 * comes from g_from: the arena it was drawing from is then spent for it. */
static void cmd_spend_current(worker_t *w) {
  for (unsigned i = 0; i < w->n; i++) {
    if (w->blocks[i] != NULL && slab_of(w->blocks[i]) == g_from) {
      arts_regpool_free(w->blocks[i]);
      w->blocks[i] = NULL;
      break;
    }
  }
  cmd_until_from(w);
}

/* Free every other block this thread holds in g_from. */
static void cmd_halve(worker_t *w) {
  bool odd = false;
  for (unsigned i = 0; i < w->n; i++) {
    if (w->blocks[i] == NULL || slab_of(w->blocks[i]) != g_from)
      continue;
    if (odd) {
      arts_regpool_free(w->blocks[i]);
      w->blocks[i] = NULL;
    }
    odd = !odd;
  }
}

static void cmd_k(worker_t *w) {
  for (int i = 0; i < K; i++)
    cmd_one(w);
}

/* ---- the rounds ---------------------------------------------------------- */

typedef struct {
  unsigned cascades, cascades_later, refusals, spills;
} want_t;

static bool came_from(const worker_t *w, unsigned last, const void *slab) {
  bool ok = true;
  for (unsigned i = w->n - last; i < w->n; i++)
    ok &= w->blocks[i] != NULL && slab_of(w->blocks[i]) == slab;
  return ok;
}

/* K lock-step rounds of U then T, then K allocations of each at once. */
static void rounds(const char *leg, worker_t *u, const void *u_slab,
                   worker_t *t, const void *t_slab, want_t want) {
  counts_t c0 = counts(0), c1 = c0;
  bool where = true;
  for (int r = 0; r < K; r++) {
    if (r == 1)
      c1 = counts(0);
    run(u, cmd_one);
    where &= u->last_slab == u_slab;
    run(t, cmd_one);
    where &= t->last_slab == t_slab;
  }
  counts_t c2 = counts(0);
  CHECK(c2.cascades - c0.cascades == want.cascades,
        "%s: %u cascades in %d lock-step rounds, want %u", leg,
        c2.cascades - c0.cascades, K, want.cascades);
  CHECK(c2.cascades - c1.cascades == want.cascades_later,
        "%s: %u cascades in rounds 2..%d, want %u", leg,
        c2.cascades - c1.cascades, K, want.cascades_later);
  CHECK(c2.refusals - c0.refusals == want.refusals,
        "%s: %u refused binds in the rounds, want %u", leg,
        c2.refusals - c0.refusals, want.refusals);
  CHECK(c2.spills - c0.spills == want.spills,
        "%s: node 0 spilled %u times in the rounds, want %u", leg,
        c2.spills - c0.spills, want.spills);
  CHECK(c2.slabs == c0.slabs, "%s: the rounds grew the pool (%u -> %u slabs)",
        leg, c0.slabs, c2.slabs);
  CHECK(where, "%s: a lock-step block came from a slab without its room", leg);

  post(u, cmd_k);
  post(t, cmd_k);
  wait_idle(u);
  wait_idle(t);
  counts_t c3 = counts(0);
  CHECK(c3.cascades == c2.cascades && c3.refusals == c2.refusals &&
            c3.spills == c2.spills && c3.slabs == c2.slabs,
        "%s: concurrent allocations left the fast path (cascades +%u, "
        "refused binds +%u, spills +%u, slabs +%u)",
        leg, c3.cascades - c2.cascades, c3.refusals - c2.refusals,
        c3.spills - c2.spills, c3.slabs - c2.slabs);
  CHECK(came_from(u, K, u_slab) && came_from(t, K, t_slab),
        "%s: a concurrent block came from a slab without its room", leg);
  printf("%s: %d rounds: cascades %u (rounds 2..%d: %u) refused binds %u "
         "spills %u slabs %u -> %u; concurrent: cascades %u refused binds %u "
         "spills %u slabs %u\n",
         leg, K, c2.cascades - c0.cascades, K, c2.cascades - c1.cascades,
         c2.refusals - c0.refusals, c2.spills - c0.spills, c0.slabs, c2.slabs,
         c3.cascades - c2.cascades, c3.refusals - c2.refusals,
         c3.spills - c2.spills, c3.slabs - c2.slabs);
}

static void fresh_pool_one_node(void) {
  regpool_sim_declare(1, REGPOOL_SIM_FREE);
  regpool_sim_set_node_free(0, (size_t)4 << 30);
  arts_regpool_set_topology(1, 1, NULL);
  if (!arts_regpool_init(NULL, NULL, SLAB_BYTES, 1)) {
    printf("FAIL regpool_room_per_thread: pool init\n");
    exit(1);
  }
}

static worker_t g_u, g_t, g_w;

static void leg_theap(void) {
  fresh_pool_one_node();
  g_size = BLOCK;
  start(&g_u, 0);
  start(&g_t, 0);
  run(&g_u, cmd_room); /* U: its heap's part and room in A1 */
  const void *a1 = g_u.last_slab;
  g_target_slabs = 2;
  run(&g_t, cmd_fill_until_slabs); /* A1 spent for T; A2 grown */
  const void *a2 = g_t.last_slab;
  run(&g_t, cmd_room); /* T: room in A2 */
  g_from = a1;
  run(&g_t, cmd_spend_current); /* A2 spent for T: no free slice left */
  g_from = a2;
  run(&g_t, cmd_halve); /* T: room in A2 again */
  CHECK(a1 != a2 && counts(0).slabs == 2, "theap: setup gave %u slabs",
        counts(0).slabs);
  rounds("theap", &g_u, a1, &g_t, a2, (want_t){2, 0, 1, 0});
  stop(&g_u);
  stop(&g_t);
}

static void leg_page(void) {
  fresh_pool_one_node();
  g_size = BLOCK;
  start(&g_u, 0);
  start(&g_t, 0);
  run(&g_u, cmd_room); /* U: room in A1 */
  const void *a1 = g_u.last_slab;
  g_target_slabs = 2;
  run(&g_t, cmd_fill_until_slabs); /* A1 spent for T; A2 grown */
  const void *a2 = g_t.last_slab;
  run(&g_t, cmd_room); /* T: room in A2 */
  g_from = a1;
  run(&g_u, cmd_until_from); /* U fills A2 until it is spent for U */
  g_from = a2;
  run(&g_t, cmd_halve); /* T: room in A2 */
  CHECK(a1 != a2 && counts(0).slabs == 2, "page: setup gave %u slabs",
        counts(0).slabs);
  rounds("page", &g_u, a1, &g_t, a2, (want_t){0, 0, 0, 0});
  stop(&g_u);
  stop(&g_t);
}

static void leg_spill(void) {
  regpool_sim_declare(2, REGPOOL_SIM_FREE);
  regpool_sim_set_machine_available((size_t)1 << 40);
  regpool_sim_set_node_free(0, ARTS_REGPOOL_NODE_HEADROOM);
  regpool_sim_set_node_free(1, (size_t)64 << 30);
  regpool_sim_set_thread_node(1);
  arts_regpool_set_topology(2, 0x3, NULL);
  if (!arts_regpool_init(NULL, NULL, SLAB_BYTES, 2)) {
    printf("FAIL regpool_room_per_thread: pool init\n");
    exit(1);
  }
  g_size = BLOCK;
  start(&g_u, 0);
  start(&g_t, 0);
  start(&g_w, 1);
  run(&g_u, cmd_room); /* U: served from B1, room there */
  const void *b1 = g_u.last_slab;
  g_target_slabs = counts(0).slabs + 1;
  run(&g_w, cmd_fill_until_slabs); /* W: B1 spent; B2 grown */
  const void *b2 = g_w.last_slab;
  run(&g_t, cmd_room); /* T: no part of its heap fits B1; served from B2 */
  CHECK(g_t.last_slab == b2 && b1 != b2,
        "spill: T was served from another slab than B2");
  g_from = b1;
  run(&g_w, cmd_spend_current); /* B2 has no free slice */
  g_from = b2;
  run(&g_t, cmd_halve); /* T: room in B2 */
  CHECK(counts(0).slabs == 2, "spill: setup gave %u slabs", counts(0).slabs);
  rounds("spill", &g_u, b1, &g_t, b2, (want_t){0, 0, 0, 0});
  stop(&g_u);
  stop(&g_t);
  stop(&g_w);
}

static void leg_sizes(void) {
  fresh_pool_one_node();
  start(&g_t, 0);
  g_size = SIZE_A;
  run(&g_t, cmd_room); /* 1 KiB room in A1 */
  const void *a1 = g_t.last_slab;
  g_size = SIZE_B;
  g_from = a1;
  run(&g_t, cmd_until_other); /* A1 spent for 2 KiB; A2 grown */
  const void *a2 = g_t.last_slab;
  run(&g_t, cmd_room); /* 2 KiB room in A2 */
  g_size = SIZE_A;
  g_from = a1;
  run(&g_t, cmd_until_from); /* A2 spent for 1 KiB: back to A1 */
  CHECK(a1 != a2 && counts(0).slabs == 2, "sizes: setup gave %u slabs",
        counts(0).slabs);
  counts_t c0 = counts(0), c1 = c0;
  bool where = true;
  for (int r = 0; r < K; r++) {
    if (r == 1)
      c1 = counts(0);
    g_size = SIZE_A;
    run(&g_t, cmd_one);
    where &= g_t.last_slab == a1;
    g_size = SIZE_B;
    run(&g_t, cmd_one);
    where &= g_t.last_slab == a2;
  }
  counts_t c2 = counts(0);
  CHECK(c2.cascades == c0.cascades && c2.cascades == c1.cascades,
        "sizes: %u cascades in %d rounds of two sizes, want 0",
        c2.cascades - c0.cascades, K);
  CHECK(c2.refusals == c0.refusals, "sizes: %u refused binds, want 0",
        c2.refusals - c0.refusals);
  CHECK(c2.slabs == c0.slabs, "sizes: the rounds grew the pool (%u -> %u)",
        c0.slabs, c2.slabs);
  CHECK(where, "sizes: a block came from a slab without room for its size");
  printf("sizes: %d rounds: cascades %u (rounds 2..%d: %u) refused binds %u "
         "slabs %u -> %u\n",
         K, c2.cascades - c0.cascades, K, c2.cascades - c1.cascades,
         c2.refusals - c0.refusals, c0.slabs, c2.slabs);
  stop(&g_t);
}

int main(void) {
  void (*legs[])(void) = {leg_theap, leg_page, leg_spill, leg_sizes};
  for (unsigned i = 0; i < sizeof legs / sizeof legs[0]; i++) {
    legs[i]();
    arts_regpool_cleanup();
  }
  if (g_failed)
    return 1;
  printf("PASS regpool_room_per_thread: a refusal moved only its own thread "
         "and size class, in all four legs\n");
  return 0;
}
