/* Giving back a retired oversize mapping, and the requests that wait for it.
 *
 * A retired oversize mapping holds memory no allocation owns.  A request that
 * finds no room gives such mappings back before anything is mapped where the
 * kernel chooses, and the teardown of each one runs outside the pool's lock.
 * Three properties follow:
 *
 *   (a) a request that reaches the arena cascade's last resort while another
 *       request is still tearing a retired mapping down waits for that
 *       teardown: it maps no kernel-placed slab meanwhile, and afterwards it
 *       takes the room the teardown gave back — a slab placed on the node
 *       that held the mapping;
 *   (b) during that teardown, an allocation that takes the pool's lock (a
 *       never-carved node's first allocation) and the free of an oversize
 *       block complete, and a lookup into the range being torn down resolves
 *       to nothing;
 *   (c) the oversize path's retry waits the same way, and then places its
 *       mapping on the node whose room came back.
 *
 * The machine is declared (regpool_numa_sim.h): two nodes, both with room by
 * their declared free memory, the calling thread on node 0.  Which node is
 * full is set through the pool's own force-full override, the one machine
 * fact the test changes while several threads run.  The retired mapping R is
 * placed on node 1, and node 1 has room again only once R's munmap has
 * returned: the test defines munmap (finding the C library's own at run
 * time), holds the request that tears R down inside it until the test has
 * observed what must hold during the teardown, and lifts node 1's override
 * after the real munmap.
 *
 * The 16 MiB arena requests meet a 32 MiB base slab: a slab that holds one
 * such block cannot hold a second (the allocator's page header and arena
 * metadata take the rest), so node 0's base arena, holding one, serves no
 * other and each such request goes down the cascade.
 *
 * Each waiting request first proves it reached its hand-back: it has a
 * retired mapping of its own to give back (D for the arena request, D2 for
 * the oversize one), and the test sees that mapping's munmap before it
 * starts watching the request.  D2 sits in a lower table slot than D and is
 * retired only after the arena request's pass over the table has gone past
 * it, so it is the oversize request's to give back.
 *
 * Threads: each declares its own node (thread-local in the sim) and reads the
 * sim through its own streams.  Which thread consults the sim once several
 * run is the scheduler's choice, so the only assertion over its counters
 * after that point is that the declared machine was consulted; the placement
 * policies are checked over the single-threaded setup.  A bounded wait only
 * limits how long a request is observed NOT to complete, or how long the test
 * waits for work that must finish; no assertion depends on how fast anything
 * runs.
 *
 * Whitebox unit test: no ARTS runtime, no ports, no config. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "arts/utils/stdatomic_compat.h"
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "arts/memory/regpool.h"
#include "regpool_numa_sim.h"

#define MIB ((size_t)1 << 20)
#define SLAB_BYTES (32 * MIB)
#define REQ (16 * MIB)     /* arena path: not above half the slab */
#define R_BYTES (18 * MIB) /* oversize: above half the slab */
#define D_BYTES (20 * MIB)
#define X_BYTES (22 * MIB)
#define D2_BYTES (24 * MIB)
#define ROOMY ((size_t)64 << 30)
#define OBSERVE_MS 250 /* how long a waiting request is watched */
#define BOUND_MS 30000 /* how long work that must finish may take */

static int g_failed;
#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      printf("FAIL regpool_evict_wait: " __VA_ARGS__);                         \
      printf("\n");                                                            \
      g_failed = 1;                                                            \
    }                                                                          \
  } while (0)

/* Fatal: the scenario cannot go on (a thread is stuck or a precondition
 * failed); exits without joining. */
#define ABANDON(...)                                                           \
  do {                                                                         \
    printf("FAIL regpool_evict_wait: " __VA_ARGS__);                           \
    printf("\n");                                                              \
    fflush(stdout);                                                            \
    _exit(1);                                                                  \
  } while (0)

/* ---- the teardown of R, held ------------------------------------------- */
static void *g_r_base;
static size_t g_r_len;
static _Atomic(bool) g_r_armed;
static sem_t g_in_teardown, g_release;
/* The witnesses: D's and D2's teardowns, seen after their real munmap. */
static void *g_d_base, *g_d2_base;
static _Atomic(bool) g_d_gone, g_d2_gone;

int munmap(void *addr, size_t len) {
  /* Resolved at the first call: initialisation order across shared objects,
   * and among equal-priority constructors, is outside the test's control, so
   * no constructor is sure to run first.  Every resolution stores the same
   * address, so the pointer publishes nothing and relaxed suffices. */
  typedef int (*munmap_fn)(void *, size_t);
  static _Atomic(munmap_fn) real_munmap;
  munmap_fn real = atomic_load_explicit(&real_munmap, memory_order_relaxed);
  if (real == NULL) {
    real = (munmap_fn)dlsym(RTLD_NEXT, "munmap");
    atomic_store_explicit(&real_munmap, real, memory_order_relaxed);
  }
  bool held = addr == g_r_base && len == g_r_len &&
              atomic_exchange_explicit(&g_r_armed, false, memory_order_acquire);
  if (held) {
    sem_post(&g_in_teardown);
    while (sem_wait(&g_release) != 0 && errno == EINTR)
      ;
  }
  int rc = real(addr, len);
  if (held)
    arts_regpool_set_forced_full(0x1); /* node 1 has R's room back */
  if (addr != NULL && addr == g_d_base && len == D_BYTES)
    atomic_store_explicit(&g_d_gone, true, memory_order_release);
  if (addr != NULL && addr == g_d2_base && len == D2_BYTES)
    atomic_store_explicit(&g_d2_gone, true, memory_order_release);
  return rc;
}

/* ---- helpers ------------------------------------------------------------- */
static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Bounded on the monotonic clock: a wall-clock step cannot cut it short. */
static bool sem_wait_ms(sem_t *s, unsigned ms) {
  uint64_t end = now_ms() + ms;
  while (sem_trywait(s) != 0) {
    if (now_ms() >= end)
      return false;
    usleep(1000);
  }
  return true;
}

/* What the report says was mapped where the kernel chose: arena slabs
 * (MiB) and oversize mappings (count). */
typedef struct {
  unsigned arena_unplaced_mib;
  unsigned direct_unplaced;
} kernel_t;

static kernel_t kernel_placed(void) {
  char *buf = NULL;
  size_t len = 0;
  FILE *f = open_memstream(&buf, &len);
  if (f == NULL)
    abort();
  arts_regpool_report(f);
  fclose(f);
  kernel_t k = {0, 0};
  const char *t = strstr(buf, "[REGPOOL] total:");
  if (t == NULL || sscanf(t,
                          "[REGPOOL] total: slabs=%*u/%*u mapped=%*u "
                          "unplaced=%u",
                          &k.arena_unplaced_mib) != 1) {
    printf("FAIL regpool_evict_wait: unparsable report:\n%s", buf);
    exit(1);
  }
  const char *p = strstr(buf, "[REGPOOL] direct placement:");
  if (p != NULL) {
    const char *eol = strchr(p, '\n');
    const char *u = strstr(p, " unplaced=");
    if (u != NULL && (eol == NULL || u < eol))
      sscanf(u, " unplaced=%u", &k.direct_unplaced);
  }
  free(buf);
  return k;
}

static int node_of(const void *p) {
  const arts_regpool_mr_t *m = arts_regpool_lookup(p);
  return m == NULL ? -2 : m->numa_node;
}

/* ---- requests ------------------------------------------------------------ */
typedef struct {
  int node;
  size_t size;
  void *p;
  _Atomic(bool) done; /* release once p is set; acquire before reading it */
} req_t;

static void *requester(void *arg) {
  req_t *r = arg;
  regpool_sim_set_thread_node(r->node);
  r->p = arts_regpool_alloc_aligned(r->size, 64);
  atomic_store_explicit(&r->done, true, memory_order_release);
  return NULL;
}

/* (b): what must complete while R's teardown is held. */
typedef struct {
  void *d;
  void *small;
  const arts_regpool_mr_t *in_r;
  _Atomic(bool) done; /* as req_t's, over small and in_r */
} during_t;

static void *during_teardown(void *arg) {
  during_t *w = arg;
  regpool_sim_set_thread_node(1); /* never carved: its grows take the lock */
  w->small = arts_regpool_alloc_aligned(MIB, 64);
  arts_regpool_free(w->d);
  w->in_r = arts_regpool_lookup((const char *)g_r_base + 4096);
  atomic_store_explicit(&w->done, true, memory_order_release);
  return NULL;
}

static bool wait_done(_Atomic(bool) *flag, unsigned ms) {
  uint64_t end = now_ms() + ms;
  while (!atomic_load_explicit(flag, memory_order_acquire)) {
    if (now_ms() >= end)
      return false;
    usleep(1000);
  }
  return true;
}

int main(void) {
  regpool_sim_declare(2, REGPOOL_SIM_FREE);
  regpool_sim_set_node_free(0, ROOMY);
  regpool_sim_set_node_free(1, ROOMY);
  regpool_sim_set_thread_node(0);
  arts_regpool_set_topology(2, 0x3, NULL);
  if (!arts_regpool_init(NULL, NULL, SLAB_BYTES, 2))
    ABANDON("init failed");

  /* R and D on node 1 (node 0 full), R retired; one 16 MiB block in node
   * 0's base arena; then both nodes full. */
  arts_regpool_set_forced_full(0x1);
  void *d2 = arts_regpool_alloc_aligned(D2_BYTES, 64);
  void *r = arts_regpool_alloc_aligned(R_BYTES, 64);
  void *d = arts_regpool_alloc_aligned(D_BYTES, 64);
  CHECK(node_of(d2) == 1 && node_of(r) == 1 && node_of(d) == 1,
        "setup: oversize blocks on nodes %d, %d and %d, expected node 1",
        node_of(d2), node_of(r), node_of(d));
  arts_regpool_free(r);
  void *mine = arts_regpool_alloc_aligned(REQ, 64);
  CHECK(node_of(mine) == 0, "setup: node 0's 16 MiB block on node %d",
        node_of(mine));
  arts_regpool_set_forced_full(0x3);
  bool setup_policies_ok = regpool_sim_all_policies_ok();
  kernel_t k0 = kernel_placed();

  sem_init(&g_in_teardown, 0, 0);
  sem_init(&g_release, 0, 0);
  g_r_base = r;
  g_r_len = R_BYTES;
  g_d_base = d;
  g_d2_base = d2;
  atomic_store_explicit(&g_r_armed, true, memory_order_release);

  /* E reaches the cascade's eviction gate and tears R down. */
  req_t e = {.node = 0, .size = REQ};
  pthread_t te;
  pthread_create(&te, NULL, requester, &e);
  if (!sem_wait_ms(&g_in_teardown, BOUND_MS))
    ABANDON("the request never reached the teardown of the retired mapping "
            "(served %s)",
            atomic_load_explicit(&e.done, memory_order_acquire)
                ? "without it"
                : "nowhere yet");

  /* (b) */
  during_t w = {.d = d};
  pthread_t tw;
  pthread_create(&tw, NULL, during_teardown, &w);
  if (!wait_done(&w.done, BOUND_MS)) {
    sem_post(&g_release);
    ABANDON("(b) an allocation, a free and a lookup did not complete while "
            "a retired mapping was being torn down: the teardown holds the "
            "pool's lock");
  }
  pthread_join(tw, NULL);
  CHECK(node_of(w.small) == 0,
        "(b) the never-carved node's first block on node %d, expected node "
        "0's arena",
        node_of(w.small));
  CHECK(w.in_r == NULL,
        "(b) a lookup into the range being torn down resolved to a record");

  /* (a) and (c): an arena request and an oversize request reach their
   * hand-back while R's teardown is held. */
  req_t a = {.node = 0, .size = REQ};
  req_t c = {.node = 0, .size = X_BYTES};
  pthread_t ta, tc;
  pthread_create(&ta, NULL, requester, &a);
  if (!wait_done(&g_d_gone, BOUND_MS)) {
    sem_post(&g_release);
    ABANDON("(a) the arena request never gave a retired mapping back");
  }
  arts_regpool_free(d2);
  pthread_create(&tc, NULL, requester, &c);
  if (!wait_done(&g_d2_gone, BOUND_MS)) {
    sem_post(&g_release);
    ABANDON("(c) the oversize request never gave a retired mapping back");
  }
  uint64_t end = now_ms() + OBSERVE_MS;
  while (now_ms() < end &&
         !(atomic_load_explicit(&a.done, memory_order_acquire) &&
           atomic_load_explicit(&c.done, memory_order_acquire)))
    usleep(1000);
  kernel_t k1 = kernel_placed();
  CHECK(!atomic_load_explicit(&a.done, memory_order_acquire),
        "(a) an arena request completed (on node %d) while a retired "
        "mapping's teardown was in flight",
        node_of(a.p));
  CHECK(!atomic_load_explicit(&c.done, memory_order_acquire),
        "(c) an oversize request completed (on node %d) while a retired "
        "mapping's teardown was in flight",
        node_of(c.p));
  CHECK(k1.arena_unplaced_mib == k0.arena_unplaced_mib &&
            k1.direct_unplaced == k0.direct_unplaced,
        "kernel-placed memory mapped during the teardown: arena %u -> %u MiB, "
        "oversize %u -> %u",
        k0.arena_unplaced_mib, k1.arena_unplaced_mib, k0.direct_unplaced,
        k1.direct_unplaced);

  sem_post(&g_release);
  if (!wait_done(&e.done, BOUND_MS) || !wait_done(&a.done, BOUND_MS) ||
      !wait_done(&c.done, BOUND_MS))
    ABANDON("a request did not complete after the teardown ended");
  pthread_join(te, NULL);
  pthread_join(ta, NULL);
  pthread_join(tc, NULL);

  kernel_t k2 = kernel_placed();
  CHECK(node_of(a.p) == 1,
        "(a) after the teardown the waiting arena request landed on node %d, "
        "expected a slab placed on node 1",
        node_of(a.p));
  CHECK(node_of(c.p) == 1,
        "(c) after the teardown the waiting oversize request landed on node "
        "%d, expected node 1",
        node_of(c.p));
  CHECK(node_of(e.p) == 1,
        "the evicting request landed on node %d, expected node 1",
        node_of(e.p));
  CHECK(k2.arena_unplaced_mib == k0.arena_unplaced_mib &&
            k2.direct_unplaced == k0.direct_unplaced,
        "kernel-placed memory mapped: arena %u -> %u MiB, oversize %u -> %u",
        k0.arena_unplaced_mib, k2.arena_unplaced_mib, k0.direct_unplaced,
        k2.direct_unplaced);
  CHECK(setup_policies_ok,
        "a placement policy was set after population or with bad arguments");
  CHECK(regpool_sim_consulted(), "the declared machine was not consulted");

  arts_regpool_free(c.p);
  arts_regpool_cleanup();
  if (g_failed)
    return 1;
  printf("PASS regpool_evict_wait: requests that reach the hand-back of "
         "retired mappings wait for a teardown in flight and take the room "
         "it frees; the teardown runs outside the pool's lock\n");
  return 0;
}
