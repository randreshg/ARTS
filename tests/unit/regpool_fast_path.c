/* The arena path's two steady-state shortcuts, read against the pool's own
 * state: a per-thread uniform binding that stands in for every request
 * class's binding while no class of the thread has moved off the node's memo
 * value, and a confinement guard that accepts a block inside the slab of the
 * binding that drew it before it scans the slab table.
 *
 * The test includes the pool's source, so it reads the calling thread's
 * record (t_uniform, t_moved_seen), its classes and the slab table directly;
 * it links only the allocator and the fabric library.  Each leg runs on a
 * machine it declares (regpool_numa_sim.h: one node with room, or two), in a
 * fresh pool with 32 MiB base slabs, on a fresh thread.  "With the table
 * hidden" means with the published table length held at zero, so the scan
 * accepts nothing and only a binding's range can.
 *
 *   uniform  a class-path draw sets the record to its class's binding of the
 *            memo; requests of classes the thread never drew are then served
 *            from it, and no class is made for them; a public draw through
 *            the record passes with the table hidden.
 *   moved    1 KiB room in A1, then 2 KiB blocks until A1 is spent for them:
 *            the cascade that grows A2 moves the 2 KiB class under A1 and
 *            clears the record, and one class-path draw under A2 sets it
 *            again.  2 KiB room in A2, then 1 KiB blocks until A2 is spent
 *            for them: the cascade serves the 1 KiB class from A1 under A2
 *            and clears the record; from then on every class takes its own
 *            path under A2 (the 1 KiB class only ever from A1, with no
 *            cascade, a class never drawn before gets an entry) and the
 *            record is never set again; a public draw of the moved class
 *            passes with the table hidden, by the range of its binding to A1.
 *   nodes    two nodes, the second with no memory: 1 KiB room in A0, then V
 *            grown on node 0 with 2 KiB room in it, and node 1's first draw
 *            names V its memo, so the record is (0, V) on node 0 and (1, V)
 *            on node 1.  1 KiB blocks on node 1 until V is spent for them:
 *            one cascade serves them from A0, the move is recorded for node
 *            1 alone and clears the record; back on node 0 a 2 KiB draw sets
 *            (0, V) with no cascade; on node 1 the 1 KiB class keeps drawing
 *            from A0 with no cascade while the record holds (0, V).
 *   exit     a draw a thread-exit hook makes after the pool's own exit hook
 *            ran is refused by the pool (forked child).
 *   confine  two nodes with room.  A binding carries its arena slab's range,
 *            also when the slab is not the newest; a block of that slab
 *            passes by range alone with the table hidden; a block of another
 *            slab passes by the table scan; the range is [base, base + len);
 *            an address outside every slab fails; a block that an escaping
 *            heap draws through the record is refused (forked child).
 *
 * Whitebox unit test: no ARTS runtime, no ports, no config. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <mimalloc.h>

#include "arts/runtime_state.h"
#include "arts/system/identity.h"
#include "regpool_numa_sim.h"

/* What the pool's log and fatal paths and its per-thread tables reference. */
unsigned int arts_global_rank_id = 0;
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;
void *arts_calloc(size_t nmemb, size_t size) { return mi_calloc(nmemb, size); }
void arts_free(void *ptr) { mi_free(ptr); }
#define FATAL_STATUS 3
void arts_abort(uint8_t code) {
  (void)code;
  fflush(NULL);
  _exit(FATAL_STATUS);
}

#include "../../libs/src/core/memory/regpool.c"

#define SLAB_BYTES ((size_t)32 << 20)
#define NODE_FREE ((size_t)4 << 30)
#define SIZE_A ((size_t)1 << 10)
#define SIZE_B ((size_t)2 << 10)
#define SIZE_C ((size_t)3 << 10)
#define SIZE_D ((size_t)5 << 10)
#define ALIGN ((size_t)64)
#define KEEP 512u
#define K 16
/* More blocks than the moved leg holds: the first two slabs of the ladder
 * (32 and 48 MiB) in 1 KiB blocks. */
#define MAX_BLOCKS (1u << 17)

static int g_failed;
#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      printf("FAIL regpool_fast_path: " __VA_ARGS__);                          \
      printf("\n");                                                            \
      g_failed = 1;                                                            \
    }                                                                          \
  } while (0)

/* Node 0 has room; every other node has `other_free` bytes. */
static void fresh_pool(int nodes, size_t other_free) {
  regpool_sim_declare(nodes, REGPOOL_SIM_FREE);
  regpool_sim_set_node_free(0, NODE_FREE);
  for (int n = 1; n < nodes; n++)
    regpool_sim_set_node_free(n, other_free);
  arts_regpool_set_topology((unsigned)nodes, ((uint64_t)1 << nodes) - 1, NULL);
  if (!arts_regpool_init(NULL, NULL, SLAB_BYTES, (unsigned)nodes)) {
    printf("FAIL regpool_fast_path: pool init\n");
    exit(1);
  }
}

typedef struct {
  void (*fn)(void);
} job_t;

static void *job_main(void *arg) {
  ((const job_t *)arg)->fn();
  return NULL;
}

static void on_thread(void (*fn)(void)) {
  job_t job = {fn};
  pthread_t t;
  if (pthread_create(&t, NULL, job_main, &job) != 0)
    abort();
  pthread_join(t, NULL);
}

static const regpool_slab_t *slab_of(const void *p) {
  const arts_regpool_mr_t *m = arts_regpool_lookup(p);
  if (m == NULL) {
    printf("FAIL regpool_fast_path: block %p escaped every slab\n", p);
    exit(1);
  }
  return (const regpool_slab_t *)((const char *)m -
                                  offsetof(regpool_slab_t, mr));
}

static mi_arena_id_t memo_of(int node) {
  return atomic_load_explicit(&g_node_arena[node], memory_order_acquire);
}

static mi_arena_id_t memo(void) { return memo_of(0); }

/* The published table length, held at zero until show_table: the scan then
 * accepts nothing.  Only the calling thread uses the pool meanwhile, and only
 * on draws that need no table. */
static size_t hide_table(void) {
  size_t n = atomic_load_explicit(&g_slab_count, memory_order_relaxed);
  atomic_store_explicit(&g_slab_count, 0, memory_order_release);
  return n;
}

static void show_table(size_t n) {
  atomic_store_explicit(&g_slab_count, n, memory_order_release);
}

/* The calling thread's entry for a class, or NULL: never makes one. */
static const regpool_class_t *class_peek_aligned(size_t size, size_t align) {
  regpool_classes_t *t = t_classes;
  if (t == NULL || t == &g_classes_gone)
    return NULL;
  uint64_t key = arts_regpool_class_key(size, align);
  const regpool_class_t *c = regpool_class_slot(t, key);
  return c->key == key ? c : NULL;
}

static const regpool_class_t *class_peek(size_t size) {
  return class_peek_aligned(size, ALIGN);
}

static bool record_is(mi_arena_id_t seen, const regpool_class_t *c) {
  return t_uniform.node == 0 && t_uniform.seen == seen && c != NULL &&
         c->bound.arena == seen && t_uniform.bound.heap == c->bound.heap &&
         t_uniform.bound.base == c->bound.base &&
         t_uniform.bound.len == c->bound.len;
}

static bool binding_spans(const regpool_binding_t *b, const regpool_slab_t *s) {
  return b->base == (uintptr_t)s->mr.base && b->len == s->mr.len &&
         b->arena == s->arena;
}

/* ---- the drawing thread's blocks ---------------------------------------- */

static void *g_blocks[MAX_BLOCKS];
static unsigned g_n;
static const regpool_slab_t *g_last;

static void *draw_aligned(size_t size, size_t align) {
  void *p = arts_regpool_alloc_aligned(size, align);
  memset(p, 0x5A, 64);
  g_last = slab_of(p);
  if (g_n >= MAX_BLOCKS) {
    printf("FAIL regpool_fast_path: the leg holds %u blocks\n", g_n);
    exit(1);
  }
  g_blocks[g_n++] = p;
  return p;
}

static void *draw(size_t size) { return draw_aligned(size, ALIGN); }

/* KEEP blocks, every other one freed: room for the size in the arena they
 * came from that only this thread's heap can use. */
static void room(size_t size) {
  unsigned first = g_n;
  for (unsigned i = 0; i < KEEP; i++)
    (void)draw(size);
  for (unsigned i = first + 1; i < g_n; i += 2) {
    arts_regpool_free(g_blocks[i]);
    g_blocks[i] = NULL;
  }
}

static void until_other(size_t size, const regpool_slab_t *from) {
  do
    (void)draw(size);
  while (g_last == from);
}

static void until_from(size_t size, const regpool_slab_t *from) {
  do
    (void)draw(size);
  while (g_last != from);
}

static void free_all(void) {
  for (unsigned i = 0; i < g_n; i++) {
    if (g_blocks[i] != NULL)
      arts_regpool_free(g_blocks[i]);
    g_blocks[i] = NULL;
  }
  g_n = 0;
}

static unsigned cascades_of(int node) {
  return atomic_load_explicit(&g_node_cascades[node], memory_order_relaxed);
}

static unsigned cascades(void) { return cascades_of(0); }

/* ---- legs ---------------------------------------------------------------- */

static void leg_uniform(void) {
  CHECK(t_uniform.node == -1, "uniform: a fresh thread starts with a record");
  (void)draw(SIZE_A);
  const regpool_slab_t *a1 = g_last;
  CHECK(record_is(memo(), class_peek(SIZE_A)) && a1->arena == memo(),
        "uniform: the first draw did not set the record to its class's "
        "binding of the memo");
  CHECK(binding_spans(&t_uniform.bound, a1),
        "uniform: the record does not carry the memo slab's range");
  regpool_uniform_t r = t_uniform;
  size_t fresh[] = {SIZE_B, SIZE_C, SIZE_D, (size_t)192, (size_t)24 << 10};
  for (unsigned i = 0; i < sizeof fresh / sizeof fresh[0]; i++) {
    (void)draw(fresh[i]);
    CHECK(class_peek(fresh[i]) == NULL,
          "uniform: a %zu-byte request made a class while the record held",
          fresh[i]);
    CHECK(g_last == a1, "uniform: a %zu-byte request left the memo slab",
          fresh[i]);
  }
  CHECK(t_uniform.node == r.node && t_uniform.seen == r.seen &&
            t_uniform.bound.heap == r.bound.heap,
        "uniform: draws from the record changed it");
  size_t n = hide_table();
  void *q = arts_regpool_alloc_aligned(SIZE_C, ALIGN);
  show_table(n);
  CHECK(slab_of(q) == a1 && t_uniform.node == 0,
        "uniform: the draw through the record left the memo slab");
  arts_regpool_free(q);
  CHECK(cascades() == 0, "uniform: %u cascades, want 0", cascades());
  free_all();
}

static void leg_moved(void) {
  room(SIZE_A); /* 1 KiB room in A1 */
  const regpool_slab_t *a1 = g_last;
  CHECK(record_is(a1->arena, class_peek(SIZE_A)),
        "moved: 1 KiB draws did not set the record to A1");
  CHECK(class_peek(SIZE_B) == NULL, "moved: a 2 KiB class before any draw");

  until_other(SIZE_B, a1); /* A1 spent for 2 KiB; A2 grown */
  const regpool_slab_t *a2 = g_last;
  const regpool_class_t *cb = class_peek(SIZE_B);
  CHECK(a2 != a1 && a2->arena == memo(), "moved: the grow did not make A2");
  CHECK(cb != NULL && cb->bound.arena == a2->arena &&
            cb->memo_seen == a1->arena,
        "moved: the 2 KiB class is not bound to A2 under A1");
  CHECK(t_moved_seen[0] == a1->arena,
        "moved: the 2 KiB class's move under A1 was not recorded");
  CHECK(t_uniform.node == -1, "moved: the record outlived a move under A1");

  (void)draw(SIZE_B);
  CHECK(g_last == a2 && record_is(a2->arena, class_peek(SIZE_B)),
        "moved: one class-path draw under A2 did not set the record to A2");
  CHECK(binding_spans(&t_uniform.bound, a2),
        "moved: the record does not carry A2's range");

  room(SIZE_B); /* 2 KiB room in A2 */
  unsigned c0 = cascades();
  until_from(SIZE_A, a1); /* A2 spent for 1 KiB: back to A1 */
  const regpool_class_t *ca = class_peek(SIZE_A);
  CHECK(cascades() == c0 + 1,
        "moved: %u cascades to serve 1 KiB from A1, want 1", cascades() - c0);
  CHECK(ca != NULL && ca->bound.arena == a1->arena &&
            ca->memo_seen == a2->arena,
        "moved: the 1 KiB class is not bound to A1 under A2");
  CHECK(t_moved_seen[0] == a2->arena,
        "moved: the 1 KiB class's move under A2 was not recorded");
  CHECK(t_uniform.node == -1, "moved: the record outlived a move under A2");
  CHECK(ca != NULL && binding_spans(&ca->bound, a1),
        "moved: the 1 KiB class's binding does not carry A1's range");
  size_t n = hide_table();
  void *q = arts_regpool_alloc_aligned(SIZE_A, ALIGN);
  show_table(n);
  CHECK(slab_of(q) == a1 && t_uniform.node == -1,
        "moved: the moved class's draw left A1 or set the record");
  arts_regpool_free(q);

  c0 = cascades();
  bool where = true, unset = true;
  for (int r = 0; r < K; r++) {
    (void)draw(SIZE_A);
    where &= g_last == a1;
    unset &= t_uniform.node == -1;
    (void)draw(SIZE_B);
    where &= g_last == a2;
    unset &= t_uniform.node == -1;
  }
  CHECK(where, "moved: a block came from a slab without room for its size");
  CHECK(unset, "moved: the record was set again under A2");
  /* Another class of the 2 KiB block size: its room is the 2 KiB class's. */
  CHECK(class_peek_aligned(SIZE_B, 2 * ALIGN) == NULL,
        "moved: a 128-aligned class before any draw");
  (void)draw_aligned(SIZE_B, 2 * ALIGN);
  CHECK(class_peek_aligned(SIZE_B, 2 * ALIGN) != NULL && g_last == a2,
        "moved: a new class under A2 did not take its own path to A2");
  CHECK(t_uniform.node == -1, "moved: a new class set the record under A2");
  CHECK(cascades() == c0, "moved: %u cascades after the move, want 0",
        cascades() - c0);
  free_all();
}

static void leg_nodes(void) {
  regpool_sim_set_thread_node(0);
  room(SIZE_A); /* 1 KiB room in A0 */
  const regpool_slab_t *a0 = g_last;
  if (!arts_regpool_grow(0)) {
    printf("FAIL regpool_fast_path: nodes: the grow of node 0 failed\n");
    exit(1);
  }
  mi_arena_id_t v = memo_of(0);
  CHECK(v != a0->arena, "nodes: the grow did not make V");
  room(SIZE_B); /* 2 KiB room in V */
  CHECK(g_last->arena == v && t_uniform.node == 0 && t_uniform.seen == v,
        "nodes: the record is not (0, V)");

  regpool_sim_set_thread_node(1);
  unsigned c1 = cascades_of(1);
  (void)draw(SIZE_C); /* node 1 has no arena: served from V, its memo now */
  CHECK(memo_of(1) == v && g_last->arena == v && cascades_of(1) == c1 + 1,
        "nodes: node 1's first draw did not name V its memo");
  (void)draw(SIZE_C);
  CHECK(g_last->arena == v && t_uniform.node == 1 && t_uniform.seen == v,
        "nodes: the record is not (1, V)");

  c1 = cascades_of(1);
  until_from(SIZE_A, a0); /* V spent for 1 KiB on node 1: back to A0 */
  const regpool_class_t *ca = class_peek(SIZE_A);
  CHECK(cascades_of(1) == c1 + 1,
        "nodes: %u cascades to serve 1 KiB from A0 on node 1, want 1",
        cascades_of(1) - c1);
  CHECK(ca != NULL && ca->bound.arena == a0->arena && ca->memo_node == 1 &&
            ca->memo_seen == v,
        "nodes: the 1 KiB class is not bound to A0 under (1, V)");
  CHECK(t_moved_seen[1] == v && t_moved_seen[0] == NULL,
        "nodes: the move under (1, V) was not recorded for node 1 alone");
  CHECK(t_uniform.node == -1, "nodes: the record outlived a move under (1, V)");

  regpool_sim_set_thread_node(0);
  unsigned c0 = cascades_of(0);
  (void)draw(SIZE_B);
  CHECK(g_last->arena == v && cascades_of(0) == c0,
        "nodes: a 2 KiB draw on node 0 left V");
  CHECK(t_uniform.node == 0 && t_uniform.seen == v,
        "nodes: no record under (0, V), where no class moved");

  regpool_sim_set_thread_node(1);
  c1 = cascades_of(1);
  bool where = true;
  for (int r = 0; r < K; r++) {
    (void)draw(SIZE_A);
    where &= g_last == a0;
  }
  CHECK(where, "nodes: the moved class left A0 on node 1");
  CHECK(cascades_of(1) == c1,
        "nodes: %u cascades for the moved class on node 1, want 0",
        cascades_of(1) - c1);
  CHECK(t_uniform.node == 0 && t_uniform.seen == v,
        "nodes: node 1's draws changed the record (0, V)");
  free_all();
}

static void leg_confine(void) {
  void *p1 = draw(SIZE_A);
  const regpool_slab_t *a1 = g_last;
  CHECK(binding_spans(&t_uniform.bound, a1) &&
            binding_spans(&class_peek(SIZE_A)->bound, a1),
        "confine: the binding does not carry its arena slab's range");

  if (!arts_regpool_grow(1)) {
    printf("FAIL regpool_fast_path: confine: the grow of node 1 failed\n");
    exit(1);
  }
  regpool_binding_t old;
  CHECK(regpool_thread_bind(a1->arena, 0, &old) == REGPOOL_BOUND &&
            binding_spans(&old, a1),
        "confine: a bind of an arena below a newer one took another range");

  size_t n = hide_table();
  bool scan = arts_regpool_lookup(p1) != NULL;
  bool range = regpool_in_slab(p1, &t_uniform.bound);
  show_table(n);
  CHECK(!scan && range,
        "confine: a block of the bound slab did not pass by range alone");

  char buf[4096];
  regpool_binding_t span = {NULL, NULL, (uintptr_t)buf, sizeof buf};
  CHECK(regpool_in_slab(buf, &span) &&
            regpool_in_slab(buf + sizeof buf - 1, &span) &&
            !regpool_in_slab(buf + sizeof buf, &span) &&
            !regpool_in_slab((const void *)((uintptr_t)buf - 1), &span),
        "confine: the range a binding passes is not [base, base + len)");
  const void *end = (const void *)(t_uniform.bound.base + t_uniform.bound.len);
  CHECK(regpool_in_slab(end, &t_uniform.bound) ==
            (arts_regpool_lookup(end) != NULL),
        "confine: the end of the bound slab passed apart from the scan");

  if (!arts_regpool_grow(0)) {
    printf("FAIL regpool_fast_path: confine: the explicit grow failed\n");
    exit(1);
  }
  (void)draw(SIZE_A);
  const regpool_slab_t *a2 = g_last;
  CHECK(a2 != a1 && a2->arena == memo() && binding_spans(&t_uniform.bound, a2),
        "confine: the draw after the grow did not bind A2 with its range");
  const regpool_binding_t *b = &t_uniform.bound;
  CHECK((uintptr_t)p1 - b->base >= b->len && regpool_in_slab(p1, b),
        "confine: a block of another slab did not pass by the table scan");

  int local = 0;
  void *foreign = mi_malloc(SIZE_A);
  CHECK(!regpool_in_slab(&local, b) && !regpool_in_slab(foreign, b) &&
            !regpool_in_slab(foreign, &regpool_unbound),
        "confine: an address outside every slab passed");
  mi_free(foreign);
  free_all();
}

/* ---- forked children --------------------------------------------------- */

static pthread_key_t g_late_key;

/* Runs after the pool's exit hook: its key is made after the pool's. */
static void late_hook(void *v) {
  (void)v;
  if (t_uniform.node != -1 || t_classes != &g_classes_gone)
    printf("BAD: the pool's exit hook left the record or the classes\n");
  (void)arts_regpool_alloc_aligned(SIZE_A, ALIGN);
  printf("BAD: drew after the pool's exit hook\n");
}

static void child_exit_thread(void) {
  (void)arts_regpool_alloc_aligned(SIZE_A, ALIGN);
  if (t_uniform.node != 0)
    printf("BAD: the draw did not set the record\n");
  if (pthread_key_create(&g_late_key, late_hook) != 0 ||
      pthread_setspecific(g_late_key, (void *)1) != 0)
    _exit(2);
}

static void child_escape_thread(void) {
  (void)arts_regpool_alloc_aligned(SIZE_A, ALIGN);
  mi_heap_t *h = mi_heap_new();
  if (t_uniform.node != 0 || h == NULL)
    _exit(2);
  t_uniform.bound.heap = h;
  (void)arts_regpool_alloc_aligned(SIZE_A, ALIGN);
  printf("BAD: an escaping block was returned\n");
}

typedef struct {
  const char *name;
  void (*fn)(void);
  const char *expect;
} child_t;

/* Runs one child; true when it ended in the pool's fatal error with the
 * expected text and reported nothing BAD before it. */
static bool run_child(const child_t *c) {
  int fds[2];
  if (pipe(fds) != 0)
    abort();
  fflush(NULL);
  pid_t pid = fork();
  if (pid < 0)
    abort();
  if (pid == 0) {
    close(fds[0]);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    struct rlimit core;
    if (getrlimit(RLIMIT_CORE, &core) == 0) {
      core.rlim_cur = 0;
      (void)setrlimit(RLIMIT_CORE, &core);
    }
    fresh_pool(1, 0);
    on_thread(c->fn);
    fflush(NULL);
    _exit(0);
  }
  close(fds[1]);
  size_t cap = 1 << 16, len = 0;
  char *text = malloc(cap);
  for (;;) {
    if (len + 4096 + 1 > cap) {
      cap *= 2;
      text = realloc(text, cap);
    }
    ssize_t n = read(fds[0], text + len, cap - len - 1);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    len += (size_t)n;
  }
  text[len] = '\0';
  close(fds[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  bool ok = WIFEXITED(status) && WEXITSTATUS(status) == FATAL_STATUS &&
            strstr(text, c->expect) != NULL && strstr(text, "BAD:") == NULL;
  if (!ok) {
    printf("FAIL regpool_fast_path: %s: status 0x%x; the child wrote:\n%s",
           c->name, (unsigned)status, text);
  } else if (strstr(text, "WARNING: ThreadSanitizer") != NULL ||
             strstr(text, "ERROR: AddressSanitizer") != NULL ||
             strstr(text, "runtime error:") != NULL) {
    fputs(text, stderr);
    ok = false;
  }
  free(text);
  return ok;
}

typedef struct {
  const char *name;
  void (*fn)(void);
  int nodes;
  size_t other_free;
} leg_t;

int main(void) {
  const leg_t legs[] = {
      {"uniform", leg_uniform, 1, 0},
      {"moved", leg_moved, 1, 0},
      {"nodes", leg_nodes, 2, 0},
      {"confine", leg_confine, 2, NODE_FREE},
  };
  for (unsigned i = 0; i < sizeof legs / sizeof legs[0]; i++) {
    fresh_pool(legs[i].nodes, legs[i].other_free);
    on_thread(legs[i].fn);
    arts_regpool_cleanup();
    printf("%s: done\n", legs[i].name);
  }
  const child_t children[] = {
      {"exit", child_exit_thread,
       "allocation on a thread whose exit hooks have run"},
      {"escape", child_escape_thread, "escaped all registered slabs"},
  };
  for (unsigned i = 0; i < sizeof children / sizeof children[0]; i++) {
    if (!run_child(&children[i]))
      g_failed = 1;
    else
      printf("%s: refused\n", children[i].name);
  }
  if (g_failed)
    return 1;
  printf("PASS regpool_fast_path: the uniform binding and the slab-range "
         "guard, in all six legs\n");
  return 0;
}
