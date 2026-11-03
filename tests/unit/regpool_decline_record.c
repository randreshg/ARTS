/* A full node's reading is taken only by asks of a cascade it ended in.
 *
 * A grow that reads its node full (the node's free pages cannot take the
 * slab) maps nothing and records the table length it read at.  An ask made
 * at that length takes the reading instead of reading the node again only
 * when the grow ended after its cascade's first ask, so a cascade that
 * begins after the declining grow ended never takes its reading.  Checked
 * by counts:
 *
 *   (a) two separate asks at one length each read: an ask outside a cascade
 *       is a cascade of one;
 *   (b) the exhaustion cascade reads once: its second own ask, after an
 *       eviction that gives nothing back, takes the first ask's reading, and
 *       its last step, a kernel-placed grow, still maps;
 *   (c) an eviction inside the cascade that gives a retired mapping's
 *       memory back clears the reading: the second own ask reads again;
 *   (d) an ask that waits on the declining grow takes its reading;
 *   (e) a grow whose reading began before an eviction gave memory back
 *       records nothing, so the ask waiting on it reads again.
 *
 * Machine reads count the grows' sizings: nothing else in the pool reads
 * the machine's availability.  In (d) and (e) the declining grow is held
 * inside its node reading (regpool_sim_node_read_hook) while the other ask
 * starts waiting on it and, in (e), while an oversize request gives a
 * retired mapping back into the same table slot.  The machine is declared
 * (regpool_numa_sim.h): one node, with room at init and full from then on
 * by its free pages, while the machine can still give slabs.
 *
 * Whitebox unit test: no ARTS runtime, no ports, no config. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "arts/memory/regpool.h"
#include "regpool_numa_sim.h"

#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MIB ((size_t)1 << 20)
#define SLAB_BYTES (32 * MIB)
#define BLOCK MIB
#define D_BYTES (20 * MIB) /* oversize: above half the slab */
#define D2_BYTES (24 * MIB)
#define E_BYTES (22 * MIB)
#define ROOMY ((size_t)64 << 30)
#define NO_ROOM ((size_t)1 << 30) /* under the node headroom */
#define MAX_BLOCKS 512
#define BOUND_MS 30000

static int g_failed;
#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      printf("FAIL regpool_decline_record: " __VA_ARGS__);                     \
      printf("\n");                                                            \
      g_failed = 1;                                                            \
    }                                                                          \
  } while (0)

#define ABANDON(...)                                                           \
  do {                                                                         \
    printf("FAIL regpool_decline_record: " __VA_ARGS__);                       \
    printf("\n");                                                              \
    fflush(stdout);                                                            \
    _exit(1);                                                                  \
  } while (0)

typedef struct {
  unsigned grows, declines, reuses, sleeps, slabs, unplaced;
} counts_t;

static unsigned field(const char *buf, const char *line, const char *key) {
  const char *l = strstr(buf, line);
  const char *k = l != NULL ? strstr(l, key) : NULL;
  const char *nl = l != NULL ? strchr(l, '\n') : NULL;
  unsigned v;
  if (k == NULL || (nl != NULL && k > nl) ||
      sscanf(k + strlen(key), "%u", &v) != 1)
    ABANDON("the report lacks%s:\n%s", key, buf);
  return v;
}

static counts_t counts(void) {
  char *buf = NULL;
  size_t len = 0;
  FILE *f = open_memstream(&buf, &len);
  if (f == NULL)
    abort();
  arts_regpool_report(f);
  fclose(f);
  const char *n0 = "[REGPOOL] node 0:", *t = "[REGPOOL] total:";
  counts_t c = {
      field(buf, n0, " grows="),          field(buf, n0, " declines="),
      field(buf, n0, " decline_reuses="), field(buf, n0, " sleeps="),
      field(buf, t, " slabs="),           field(buf, t, " unplaced=")};
  free(buf);
  return c;
}

static unsigned machine_reads(void) {
  return atomic_load_explicit(&regpool_sim_machine_reads, memory_order_relaxed);
}

static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Blocks until the cascade grows the node once more. */
static void *g_blocks[MAX_BLOCKS];
static unsigned g_nblocks;

static void grow_by_demand(const char *what) {
  counts_t c = counts();
  unsigned grows = c.grows;
  while (g_nblocks < MAX_BLOCKS && c.grows == grows) {
    g_blocks[g_nblocks++] = arts_regpool_alloc_aligned(BLOCK, 64);
    c = counts();
  }
  if (c.grows != grows + 1)
    ABANDON("%s: %u blocks grew the node %u times", what, g_nblocks,
            c.grows - grows);
}

/* ---- a grow held inside its node reading --------------------------------- */
static __thread bool t_hold_reading;
static sem_t g_in_reading, g_reading_release;

static void hold_reading(int node) {
  (void)node;
  if (!t_hold_reading)
    return;
  t_hold_reading = false;
  sem_post(&g_in_reading);
  while (sem_wait(&g_reading_release) != 0)
    ;
}

typedef struct {
  bool hold;
  bool grew;
} asker_t;

static void *asker(void *arg) {
  asker_t *a = arg;
  regpool_sim_set_thread_node(0);
  t_hold_reading = a->hold;
  a->grew = arts_regpool_grow(0);
  return NULL;
}

static bool sem_wait_ms(sem_t *s, unsigned ms) {
  uint64_t end = now_ms() + ms;
  while (sem_trywait(s) != 0) {
    if (now_ms() >= end)
      return false;
    usleep(1000);
  }
  return true;
}

/* Runs (d) or (e): G declines while held in its reading, W waits on G; in
 * (e) an oversize request gives `retired` back meanwhile. */
static void held_decline(const char *what, void *retired, counts_t *before,
                         counts_t *after, unsigned *reads) {
  *before = counts();
  unsigned m0 = machine_reads();
  asker_t g = {.hold = true, .grew = true}, w = {.hold = false, .grew = true};
  pthread_t tg, tw;
  pthread_create(&tg, NULL, asker, &g);
  if (!sem_wait_ms(&g_in_reading, BOUND_MS))
    ABANDON("%s: the declining grow never read the node", what);
  pthread_create(&tw, NULL, asker, &w);
  uint64_t end = now_ms() + BOUND_MS;
  while (counts().sleeps == before->sleeps) {
    if (now_ms() >= end)
      ABANDON("%s: the second ask never waited on the grow", what);
    usleep(1000);
  }
  void *e = NULL;
  if (retired != NULL) {
    counts_t c = counts();
    e = arts_regpool_alloc_aligned(E_BYTES, 64);
    CHECK(counts().slabs == c.slabs,
          "%s: the oversize request moved the table from %u to %u entries; "
          "expected it to take the given-back mapping's slot",
          what, c.slabs, counts().slabs);
  }
  sem_post(&g_reading_release);
  pthread_join(tg, NULL);
  pthread_join(tw, NULL);
  CHECK(!g.grew && !w.grew, "%s: a grow on the full node returned true", what);
  if (e != NULL)
    arts_regpool_free(e);
  *after = counts();
  *reads = machine_reads() - m0;
}

int main(void) {
  regpool_sim_declare(1, REGPOOL_SIM_FREE);
  regpool_sim_set_node_free(0, ROOMY);
  regpool_sim_set_thread_node(0);
  arts_regpool_set_topology(1, 0x1, NULL);
  sem_init(&g_in_reading, 0, 0);
  sem_init(&g_reading_release, 0, 0);
  regpool_sim_node_read_hook = hold_reading;
  if (!arts_regpool_init(NULL, NULL, SLAB_BYTES, 1))
    ABANDON("init failed");
  regpool_sim_set_node_free(0, NO_ROOM);

  /* (a) */
  counts_t c0 = counts();
  unsigned m0 = machine_reads();
  bool grew1 = arts_regpool_grow(0);
  bool grew2 = arts_regpool_grow(0);
  counts_t c1 = counts();
  unsigned m1 = machine_reads();
  CHECK(!grew1 && !grew2 && c1.declines == c0.declines + 2 &&
            c1.reuses == c0.reuses && c1.slabs == c0.slabs && m1 - m0 == 2,
        "(a) two asks: grew %d %d, declines %u -> %u, reuses %u -> %u, %u "
        "machine reads; expected each ask to read and decline",
        grew1, grew2, c0.declines, c1.declines, c0.reuses, c1.reuses, m1 - m0);

  /* (b) */
  grow_by_demand("(b)");
  counts_t c2 = counts();
  unsigned m2 = machine_reads();
  CHECK(c2.unplaced > c1.unplaced && c2.declines == c1.declines + 1 &&
            c2.reuses == c1.reuses + 1 && m2 - m1 == 3,
        "(b) the cascade: kernel-placed %u -> %u MiB, declines %u -> %u, "
        "reuses %u -> %u, %u machine reads; expected one reading, taken by "
        "the second own ask, and the kernel-placed grow's two reads",
        c1.unplaced, c2.unplaced, c1.declines, c2.declines, c1.reuses,
        c2.reuses, m2 - m1);

  /* (c) A retired oversize mapping D for the cascade's eviction. */
  void *d = arts_regpool_alloc_aligned(D_BYTES, 64);
  arts_regpool_free(d);
  counts_t c3 = counts();
  unsigned m3 = machine_reads();
  grow_by_demand("(c)");
  counts_t c4 = counts();
  unsigned m4 = machine_reads();
  CHECK(c4.declines == c3.declines + 2 && c4.reuses == c3.reuses &&
            m4 - m3 == 4,
        "(c) the cascade with an eviction: declines %u -> %u, reuses %u -> "
        "%u, %u machine reads; expected the second own ask to read again "
        "after the eviction gave memory back",
        c3.declines, c4.declines, c3.reuses, c4.reuses, m4 - m3);

  /* (d) */
  counts_t d0, d1;
  unsigned dreads;
  held_decline("(d)", NULL, &d0, &d1, &dreads);
  CHECK(d1.declines == d0.declines + 1 && d1.reuses == d0.reuses + 1 &&
            dreads == 1,
        "(d) an ask waiting on the declining grow: declines %u -> %u, reuses "
        "%u -> %u, %u machine reads; expected it to take the grow's reading",
        d0.declines, d1.declines, d0.reuses, d1.reuses, dreads);

  /* (e) A retired oversize mapping D2 in D's free slot. */
  void *d2 = arts_regpool_alloc_aligned(D2_BYTES, 64);
  arts_regpool_free(d2);
  counts_t e0, e1;
  unsigned ereads;
  held_decline("(e)", d2, &e0, &e1, &ereads);
  CHECK(e1.declines == e0.declines + 2 && e1.reuses == e0.reuses && ereads == 2,
        "(e) an ask waiting on a grow whose reading predates an eviction: "
        "declines %u -> %u, reuses %u -> %u, %u machine reads; expected it to "
        "read again",
        e0.declines, e1.declines, e0.reuses, e1.reuses, ereads);
  CHECK(regpool_sim_consulted(), "the declared machine was not consulted");

  for (unsigned i = 0; i < g_nblocks; i++)
    arts_regpool_free(g_blocks[i]);
  arts_regpool_cleanup();
  if (g_failed)
    return 1;
  printf("PASS regpool_decline_record: a full node's reading is taken only "
         "by asks of a cascade it ended in, a waiting ask among them, unless "
         "memory came back during that reading\n");
  return 0;
}
