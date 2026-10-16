/* A full node's demand and the end of the allocator's arena table.
 *
 * The machine is declared (regpool_numa_sim.h): two nodes with room, the
 * calling thread on node 0, and node 0 forced full, so the test runs and
 * asserts the same on any host.  Asserted through the pool's own report and
 * lookup:
 *
 *   (i)   the full node takes no slab at all — no placed one, and no
 *         kernel-placed one — and its demand is served from the other
 *         node's arena, whose slab the pool placed on that node, moved there
 *         once and then on the fast path;
 *   (ii)  once the arena table refuses a slab, no later grow maps again
 *         (the table never gives a slot back, so every such map would be
 *         refused in turn), whether asked for explicitly or by allocations
 *         from a node that has room again;
 *   (iii) after the refusal, an allocation another node's arena can serve
 *         is served, and the node keeps drawing from that arena on the fast
 *         path rather than re-entering the exhaustion cascade;
 *   (iv)  an allocation no arena can serve then ends in the pool's fatal
 *         error at once (asserted in a child process), and leaves a retired
 *         oversize mapping in place: giving it back cannot let a grow map.
 *
 * The table is filled with uncommitted exclusive reservations, so the test
 * holds a few hundred MiB of resident memory at most.  It links the pool's
 * object and the allocator directly, so both share one arena table.
 *
 * Whitebox unit test: no ARTS runtime, no ports, no config. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <mimalloc.h>

#include "arts/memory/regpool.h"
#include "arts/runtime_state.h"
#include "arts/system/identity.h"
#include "regpool_numa_sim.h"

/* What the pool's log and fatal paths reference. */
unsigned int arts_global_rank_id = 0;
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;
#define FATAL_STATUS 3
/* The fatal path also reports how many placement policies the process set,
 * so a process that dies on it can still be checked for maps it made. */
void arts_abort(uint8_t code) {
  (void)code;
  fprintf(stderr, "SIM-MBIND-CALLS=%u\n",
          atomic_load_explicit(&regpool_sim_mbind_calls, memory_order_relaxed));
  fflush(stdout);
  fflush(stderr);
  _exit(FATAL_STATUS);
}

#define SLAB_BYTES ((size_t)32 * 1024 * 1024)
#define BLOCK ((size_t)1 << 20)
#define OVERSIZE ((size_t)20 << 20) /* above half the slab: a mapping */
#define NODE_FREE ((size_t)4 << 30)

static int g_failed;
#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      printf("FAIL regpool_spill_and_latch: " __VA_ARGS__);                    \
      printf("\n");                                                            \
      g_failed = 1;                                                            \
    }                                                                          \
  } while (0)

typedef struct {
  char state[16];
  unsigned arenas, mapped, grows, spills;
} node_line_t;

typedef struct {
  unsigned slabs, mapped, unplaced, slots, refusals;
} total_line_t;

static char *report(void) {
  char *buf = NULL;
  size_t len = 0;
  FILE *f = open_memstream(&buf, &len);
  if (f == NULL)
    abort();
  arts_regpool_report(f);
  fclose(f);
  return buf;
}

static bool parse_node(const char *buf, int node, node_line_t *out) {
  char key[32];
  snprintf(key, sizeof key, "[REGPOOL] node %d:", node);
  const char *line = strstr(buf, key);
  return line != NULL &&
         sscanf(line,
                "[REGPOOL] node %*u: state=%15s arenas=%u mapped=%u grows=%u "
                "grow_ms=%*u waits=%*u wait_ms=%*u spills=%u",
                out->state, &out->arenas, &out->mapped, &out->grows,
                &out->spills) == 5;
}

static bool parse_total(const char *buf, total_line_t *out) {
  const char *line = strstr(buf, "[REGPOOL] total:");
  return line != NULL &&
         sscanf(line,
                "[REGPOOL] total: slabs=%u/%*u mapped=%u unplaced=%u base=%*u "
                "arena_slots=%u/%*u table_refusals=%u",
                &out->slabs, &out->mapped, &out->unplaced, &out->slots,
                &out->refusals) == 5;
}

static void snapshot(int a, int b, node_line_t *na, node_line_t *nb,
                     total_line_t *t) {
  char *buf = report();
  if (!parse_node(buf, a, na) || !parse_node(buf, b, nb) ||
      !parse_total(buf, t)) {
    printf("FAIL regpool_spill_and_latch: unparsable report:\n%s", buf);
    free(buf);
    exit(1);
  }
  free(buf);
}

/* Allocate `n` blocks from the calling thread; every one must resolve to a
 * slab placed on `want_node`. */
static void alloc_on(unsigned n, int want_node, const char *phase) {
  for (unsigned i = 0; i < n; i++) {
    void *p = arts_regpool_alloc_aligned(BLOCK, 64);
    const arts_regpool_mr_t *m = arts_regpool_lookup(p);
    CHECK(m != NULL, "%s: block %u escaped every registered slab", phase, i);
    if (m == NULL)
      return;
    CHECK(m->numa_node == want_node,
          "%s: block %u served from node %d, expected node %d", phase, i,
          m->numa_node, want_node);
    memset(p, 0x5A, BLOCK);
  }
}

int main(void) {
  /* The calling thread's node (a) is forced full; the other (b) serves it. */
  const int a = 0, b = 1;
  regpool_sim_declare(2, REGPOOL_SIM_FREE);
  regpool_sim_set_node_free(a, NODE_FREE);
  regpool_sim_set_node_free(b, NODE_FREE);
  regpool_sim_set_thread_node(a);
  setenv("ARTS_REGPOOL_FORCE_FULL_NODES", "0", 1);
  arts_regpool_set_topology(2, 0x3, NULL);
  if (!arts_regpool_init(NULL, NULL, SLAB_BYTES, 2)) {
    printf("FAIL regpool_spill_and_latch: init failed with node %d free\n", b);
    return 1;
  }

  node_line_t na, nb;
  total_line_t t, t0;

  /* (i) Init could not carve the full node and carved the other one. */
  snapshot(a, b, &na, &nb, &t);
  CHECK(strcmp(na.state, "refused") == 0 && na.arenas == 0,
        "full node %d: state=%s arenas=%u, expected refused with no arena", a,
        na.state, na.arenas);
  CHECK(strcmp(nb.state, "carved") == 0 && nb.arenas == 1,
        "node %d: state=%s arenas=%u, expected carved with one arena", b,
        nb.state, nb.arenas);
  CHECK(t.unplaced == 0,
        "a kernel-placed slab exists (%u MiB) while node %d "
        "has room",
        t.unplaced, b);

  /* The full node's demand lands on the other node: moved there by its
   * first allocation, then served on the fast path. */
  alloc_on(8, b, "spill");
  snapshot(a, b, &na, &nb, &t);
  CHECK(na.arenas == 0 && t.unplaced == 0,
        "the full node took a slab (arenas=%u, unplaced=%u MiB)", na.arenas,
        t.unplaced);
  CHECK(na.spills == 1,
        "node %d spilled %u times for 8 blocks one arena "
        "holds, expected once",
        a, na.spills);
  /* And the slab serving it was placed on the other node by the policy the
   * pool handed the kernel, before any of its pages existed. */
  {
    void *p = arts_regpool_alloc_aligned(BLOCK, 64);
    const arts_regpool_mr_t *m = arts_regpool_lookup(p);
    const regpool_sim_mbind_t *pol =
        (m != NULL) ? regpool_sim_mbind_at(m->base) : NULL;
    CHECK(pol != NULL && pol->mode == REGPOOL_SIM_MPOL_PREFERRED &&
              pol->mask == (1ul << b) && regpool_sim_policy_ok(pol),
          "the slab serving node %d was not placed on node %d", a, b);
    arts_regpool_free(p);
  }

  /* (ii) One more slab on the serving node, then a full table. */
  bool pregrew = arts_regpool_grow(b);
  CHECK(pregrew, "pre-growing node %d failed", b);
  unsigned burned = 0;
  for (;;) {
    mi_arena_id_t id;
    if (mi_reserve_os_memory_ex((size_t)32 << 20, /*commit=*/false,
                                /*allow_large=*/false, /*exclusive=*/true,
                                &id) != 0)
      break;
    burned++;
  }
  snapshot(a, b, &na, &nb, &t0);
  CHECK(t0.refusals == 0, "refusals before the table filled: %u", t0.refusals);
  /* Every slab mapped here is placed on a declared node, so every map sets a
   * policy: the policy count is the map count.  The refused grow maps one
   * slab (the table is what refuses it); after it, nothing may be mapped. */
  unsigned maps0 =
      atomic_load_explicit(&regpool_sim_mbind_calls, memory_order_relaxed);
  bool grew = arts_regpool_grow(b);
  CHECK(!grew, "a grow succeeded against a full table");
  unsigned maps_latched =
      atomic_load_explicit(&regpool_sim_mbind_calls, memory_order_relaxed);
  CHECK(maps_latched == maps0 + 1,
        "the refused grow mapped %u slabs, expected the one the table "
        "refused",
        maps_latched - maps0);
  snapshot(a, b, &na, &nb, &t);
  CHECK(t.refusals == 1, "table refusals %u after the first refused grow",
        t.refusals);
  CHECK(t.slabs == t0.slabs && t.mapped == t0.mapped,
        "a refused grow left a slab (slabs %u -> %u)", t0.slabs, t.slabs);
  /* Node a has its room back first, so its grow below is the table's to
   * refuse, not the node's. */
  arts_regpool_set_forced_full(0);
  bool grew_b = arts_regpool_grow(b);
  bool grew_a = arts_regpool_grow(a);
  CHECK(!grew_b && !grew_a, "a grow succeeded after the table refused one");
  snapshot(a, b, &na, &nb, &t);
  CHECK(t.refusals == 1, "a grow mapped again after the refusal (%u refusals)",
        t.refusals);
  CHECK(atomic_load_explicit(&regpool_sim_mbind_calls, memory_order_relaxed) ==
            maps_latched,
        "%u slabs mapped by grows after the refusal",
        atomic_load_explicit(&regpool_sim_mbind_calls, memory_order_relaxed) -
            maps_latched);

  /* (iii) Node a, its room back, allocates: its own grow is the table's to
   * refuse, so an unlatched pool would map again on its behalf.  40 blocks
   * overrun the first serving arena (32 MiB) into the second, so the node
   * moves exactly once more and is otherwise served on the fast path. */
  unsigned spills0 = na.spills;
  alloc_on(40, b, "after the refusal");
  snapshot(a, b, &na, &nb, &t);
  CHECK(t.refusals == 1,
        "allocations mapped again after the refusal (%u "
        "refusals)",
        t.refusals);
  CHECK(t.slabs == t0.slabs && t.mapped == t0.mapped && t.unplaced == 0,
        "the pool grew after the refusal (slabs %u -> %u)", t0.slabs, t.slabs);
  CHECK(na.spills == spills0 + 1,
        "node %d spilled %u times for 40 blocks over two arenas, expected "
        "once",
        a, na.spills - spills0);
  CHECK(atomic_load_explicit(&regpool_sim_mbind_calls, memory_order_relaxed) ==
            maps_latched,
        "%u slabs mapped by allocations after the refusal",
        atomic_load_explicit(&regpool_sim_mbind_calls, memory_order_relaxed) -
            maps_latched);

  /* An oversize block, freed: oversize requests still map after the
   * refusal, and its retired mapping must survive the arena path's last
   * resort, which can map nothing any more — giving it back would only lose
   * it to the oversize requests that reuse it. */
  void *big = arts_regpool_alloc_aligned(OVERSIZE, 64);
  CHECK(big != NULL && arts_regpool_lookup(big) != NULL,
        "an oversize block was not served after the refusal");
  arts_regpool_free(big);
  total_line_t tr;
  snapshot(a, b, &na, &nb, &tr);
  unsigned maps_retired =
      atomic_load_explicit(&regpool_sim_mbind_calls, memory_order_relaxed);

  /* (iv) Demand past every arena: the pool's fatal error, promptly and with
   * nothing mapped on the way.  The child's report precedes the error. */
  fflush(stdout);
  fflush(stderr);
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    printf("FAIL regpool_spill_and_latch: pipe: %s\n", strerror(errno));
    return 1;
  }
  pid_t pid = fork();
  if (pid < 0) {
    printf("FAIL regpool_spill_and_latch: fork: %s\n", strerror(errno));
    return 1;
  }
  if (pid == 0) {
    alarm(30); /* a cascade that never ends must not hang the test */
    close(pipefd[0]);
    dup2(pipefd[1], STDERR_FILENO);
    for (unsigned i = 0; i < 4096; i++)
      (void)arts_regpool_alloc_aligned(BLOCK, 64);
    _exit(0);
  }
  close(pipefd[1]);
  /* Drained to EOF so the child never blocks on a full pipe; what does not
   * fit is dropped (the report is a handful of lines). */
  size_t cap = 1 << 16, len = 0;
  char *out = malloc(cap + 1);
  if (out == NULL) {
    printf("FAIL regpool_spill_and_latch: no memory for the child's output\n");
    return 1;
  }
  char sink[4096];
  ssize_t r;
  for (;;) {
    char *dst = len < cap ? out + len : sink;
    size_t room = len < cap ? cap - len : sizeof sink;
    r = read(pipefd[0], dst, room);
    if (r < 0 && errno == EINTR)
      continue;
    if (r <= 0)
      break;
    if (dst != sink)
      len += (size_t)r;
  }
  out[len] = '\0';
  close(pipefd[0]);
  int status = 0;
  pid_t waited;
  do {
    waited = waitpid(pid, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited != pid) {
    printf("FAIL regpool_spill_and_latch: waitpid: %s\n", strerror(errno));
    return 1;
  }
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == FATAL_STATUS,
        "demand past every arena did not end in the pool's fatal error "
        "(status 0x%x)",
        status);
  total_line_t tc;
  CHECK(parse_total(out, &tc), "no pool report before the fatal error");
  if (parse_total(out, &tc))
    CHECK(tc.refusals == 1 && tc.slabs == tr.slabs,
          "the exhausted pool mapped on its way to the fatal error "
          "(refusals %u, slabs %u -> %u)",
          tc.refusals, tr.slabs, tc.slabs);
  const char *direct = strstr(out, "[REGPOOL] direct:");
  unsigned retired = 0;
  CHECK(direct != NULL &&
            sscanf(direct, "[REGPOOL] direct: live=%*u/%*u retired=%u",
                   &retired) == 1 &&
            retired == 1,
        "the arena path gave a retired mapping back where no grow could map "
        "(retired %u, expected 1)",
        retired);
  const char *maps_line = strstr(out, "SIM-MBIND-CALLS=");
  unsigned child_maps = 0;
  CHECK(maps_line != NULL &&
            sscanf(maps_line, "SIM-MBIND-CALLS=%u", &child_maps) == 1 &&
            child_maps == maps_retired,
        "the exhausted pool mapped %u slabs on its way to the fatal error",
        child_maps - maps_retired);
  free(out);
  CHECK(regpool_sim_all_policies_ok(),
        "a placement policy was set after population or with bad arguments");
  CHECK(regpool_sim_consulted(), "the declared machine was not consulted");

  arts_regpool_cleanup();
  if (g_failed)
    return 1;
  printf("PASS regpool_spill_and_latch: node %d full, served from node %d; "
         "%u table slots filled, 1 refusal, no map after it\n",
         a, b, burned);
  return 0;
}
