/* SPDX-License-Identifier: Apache-2.0
 *
 * route_table_growth_mapfail — a racer whose segment mapping fails never
 * waits on another racer: it re-reads the link once and continues in a
 * published segment, or ends the process naming the failed mapping.
 *
 * route_table.c's mmap is wrapped at link time and fails the calls the test
 * arms with ENOMEM, so the real segment build runs its failure contract: the
 * header is freed, errno is kept, and nothing is registered or mapped.
 *   (i)   In process: B builds, links and returns while A is held at the
 *         entry of its build; A's mapping then fails, its one re-read finds
 *         B's segment and its claim succeeds there.  errno is ENOMEM right
 *         after A's build returns; one build, one failure, no discard, and no
 *         munmap of the failed mapping.  The process ends normally, so a leak
 *         checker judges the freed header.
 *   (ii)  In a child: B is held inside its build with the link still NULL and
 *         A's mapping fails; A ends the process with the mapping error at
 *         once, without waiting for B.
 *   (iii) In a child: every racer's mapping fails, and the first to fail ends
 *         the process with the same error.
 * Each child is forked before the test creates any thread and starts its own
 * racers; it ends through arts_abort's _exit, never on a signal.  The parent
 * forwards its output to stderr, requires the exit code and the mapping text
 * (errno text, slots, bytes, route_table_size), and the absence of a wait's
 * "never published".
 *
 * No runtime: route_table.c + guid.c + shared.c #include'd; the OoO engine is
 * stubbed.
 */

#include "arts/utils/stdatomic_compat.h"
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

unsigned int arts_global_rank_id = 0;
unsigned int arts_global_rank_count = 1;
#define ABORT_EXIT 71
void arts_abort(uint8_t code) {
  (void)code;
  fflush(stdout);
  fflush(stderr);
  _exit(ABORT_EXIT);
}
void *arts_malloc(size_t s) { return malloc(s); }
void *arts_calloc(size_t n, size_t s) { return calloc(n, s); }
void *arts_calloc_aligned(size_t n, size_t s, size_t a) {
  void *p = NULL;
  if (posix_memalign(&p, a < sizeof(void *) ? sizeof(void *) : a, n * s)) {
    return NULL;
  }
  memset(p, 0, n * s);
  return p;
}
void arts_free(void *p) { free(p); }
#ifdef ARTS_DEBUG_INJECTION
/* route_table.c's windows are scheduling-fuzz points in an injection tree. */
void arts_sched_fuzz_point(void) {}
#endif

struct arts_route_item_s;
void arts_ooo_drain(struct arts_route_item_s *s) { (void)s; }
void arts_ooo_redrive_all(struct arts_route_item_s *s) { (void)s; }
void arts_ooo_free_all(struct arts_route_item_s *s) { (void)s; }

#include "../../libs/src/core/gas/guid.c"
#include "../../libs/src/core/gas/route_table.c"
#include "../../libs/src/core/utils/shared.c"

#include "route_table_segment_probe.h"

struct arts_runtime_shared_s arts_node_info;
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;

static int g_failed;
#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      (void)fprintf(stderr, "FAIL route_table_growth_mapfail: " __VA_ARGS__);  \
      g_failed = 1;                                                            \
    }                                                                          \
  } while (0)

#define SHIFT 4u
/* The first growth of a SHIFT root maps 2^(SHIFT+1) windows of slots. */
#define GROWN_SLOTS ((uint64_t)COLLISION_RESOLVES << (SHIFT + 1))

/* ── holds ─────────────────────────────────────────────────────────────── */

/* A thread with t_hold set waits at the entry of its build until g_release;
 * g_held counts the threads waiting there. */
static __thread int t_hold;
static _Atomic(int) g_held;
static _Atomic(int) g_release;

static void hold_build(void) {
  if (!t_hold) {
    return;
  }
  atomic_fetch_add_explicit(&g_held, 1, memory_order_release);
  while (!atomic_load_explicit(&g_release, memory_order_acquire)) {
    sched_yield();
  }
}

static int await_held(int n) {
  for (int i = 0; i < 20000; i++) {
    if (atomic_load_explicit(&g_held, memory_order_acquire) >= n) {
      return 1;
    }
    struct timespec ts = {0, 1000000L};
    nanosleep(&ts, NULL);
  }
  return 0;
}

/* ── the table ─────────────────────────────────────────────────────────── */

static arts_route_table_t *g_root;
static arts_guid_t g_fill[COLLISION_RESOLVES];
static arts_guid_t g_extra[4];

static arts_guid_t db_guid(uint64_t seq) {
  return ARTS_GUID_MAKE(ARTS_GUID_DB, 0, seq);
}

/* A one-thread rank whose table is a SHIFT root with window 0 full, and keys
 * of that window left over for the racers. */
static void table_init(void) {
  arts_thread_info.thread_id = 0;
  arts_node_info.total_thread_count = 1;
  arts_guid_node_geometry_init();
  arts_node_info.route_table =
      (arts_route_table_t **)calloc(1, sizeof(arts_route_table_t *));
  for (int s = 0; s < ARTS_REMOTE_ROUTE_SHARDS; s++) {
    arts_node_info.remote_route_table[s] = arts_new_route_table(64, 6);
  }
  g_root = probe_root(1u << SHIFT, SHIFT);
  arts_node_info.route_table[0] = g_root;
  int found = 0;
  for (uint64_t seq = 1; found < COLLISION_RESOLVES + 4; seq++) {
    arts_guid_t g = db_guid(seq);
    if (get_route_table_key((uint64_t)g, SHIFT) == 0) {
      if (found < COLLISION_RESOLVES) {
        g_fill[found] = g;
      } else {
        g_extra[found - COLLISION_RESOLVES] = g;
      }
      found++;
    }
  }
  for (int i = 0; i < COLLISION_RESOLVES; i++) {
    arts_route_item_t *s = NULL;
    arts_route_table_reserve_or_lookup(g_fill[i], &s);
  }
  probe_reset();
  probe_build_hook = hold_build;
}

static void table_fini(void) {
  probe_build_hook = NULL;
  arts_delete_route_table(g_root);
  for (int s = 0; s < ARTS_REMOTE_ROUTE_SHARDS; s++) {
    arts_delete_route_table(arts_node_info.remote_route_table[s]);
  }
  free(arts_node_info.route_table);
  arts_guid_generator_cleanup();
}

typedef struct {
  arts_guid_t key;
  int hold;
  unsigned fail;
  arts_route_item_t *slot;
  int build_errno;
  pthread_t tid;
} racer_t;

static void *racer_body(void *vp) {
  racer_t *r = (racer_t *)vp;
  t_hold = r->hold;
  probe_fail_mine = r->fail;
  arts_route_table_reserve_or_lookup(r->key, &r->slot);
  r->build_errno = probe_build_errno;
  return NULL;
}

static void racer_start(racer_t *r, arts_guid_t key, int hold, unsigned fail) {
  r->key = key;
  r->hold = hold;
  r->fail = fail;
  r->slot = NULL;
  r->build_errno = 0;
  if (pthread_create(&r->tid, NULL, racer_body, r) != 0) {
    (void)fprintf(stderr, "FAIL route_table_growth_mapfail: pthread_create\n");
    _exit(1);
  }
}

/* ── (i) in process ────────────────────────────────────────────────────── */

static void case_in_process(void) {
  table_init();
  racer_t a, b;
  /* A is held at the entry of its build; its mapping is armed to fail. */
  racer_start(&a, g_extra[0], 1, 1);
  CHECK(await_held(1), "i: A never reached its build\n");
  racer_start(&b, g_extra[1], 0, 0);
  pthread_join(b.tid, NULL);
  arts_route_table_t *seg =
      atomic_load_explicit(&g_root->next, memory_order_acquire);
  CHECK(seg != NULL && b.slot != NULL && b.slot >= seg->data &&
            b.slot < seg->data + (size_t)seg->size * COLLISION_RESOLVES,
        "i: B did not claim in the segment it linked\n");
  atomic_store_explicit(&g_release, 1, memory_order_release);
  pthread_join(a.tid, NULL);
  CHECK(a.build_errno == ENOMEM,
        "i: errno after A's failed build is %d, not ENOMEM\n", a.build_errno);
  CHECK(a.slot != NULL && seg != NULL && a.slot >= seg->data &&
            a.slot < seg->data + (size_t)seg->size * COLLISION_RESOLVES &&
            atomic_load_explicit(&a.slot->key, memory_order_acquire) ==
                g_extra[0],
        "i: A's claim did not succeed in B's segment\n");
  unsigned builds = atomic_load_explicit(&probe_builds, memory_order_relaxed);
  unsigned fails =
      atomic_load_explicit(&probe_build_failures, memory_order_relaxed);
  unsigned discards =
      atomic_load_explicit(&probe_discards, memory_order_relaxed);
  unsigned unmaps =
      atomic_load_explicit(&probe_unmap_count, memory_order_relaxed);
  (void)fprintf(stderr,
                "i: %u builds, %u failed, %u discards, %u munmaps, %u mmaps "
                "failed\n",
                builds, fails, discards, unmaps,
                atomic_load_explicit(&probe_mmap_failed, memory_order_relaxed));
  CHECK(builds == 1 && fails == 1 && discards == 0,
        "i: %u builds, %u failures, %u discards; expected 1, 1, 0\n", builds,
        fails, discards);
  CHECK(unmaps == 0, "i: the failed mapping was unmapped (%u munmaps)\n",
        unmaps);
  CHECK(atomic_load_explicit(&g_root->next, memory_order_acquire) == seg &&
            atomic_load_explicit(&seg->next, memory_order_acquire) == NULL,
        "i: the chain is not root -> B's segment\n");
  table_fini();
}

/* ── (ii), (iii) in children ───────────────────────────────────────────── */

static void child_held_builder(void) {
  table_init();
  racer_t a, b;
  /* B is held inside its build with the link NULL. */
  racer_start(&b, g_extra[1], 1, 0);
  if (!await_held(1)) {
    (void)fprintf(stderr, "child: B never reached its build\n");
    _exit(2);
  }
  /* A's mapping fails: A must end the process now. */
  racer_start(&a, g_extra[0], 0, 1);
  pthread_join(a.tid, NULL);
  (void)fprintf(stderr, "child: A returned after its mapping failed\n");
  _exit(3);
}

static void child_all_fail(void) {
  table_init();
  atomic_store_explicit(&probe_fail_all, 1, memory_order_relaxed);
  racer_t r[4];
  for (int i = 0; i < 4; i++) {
    racer_start(&r[i], g_extra[i], 0, 0);
  }
  for (int i = 0; i < 4; i++) {
    pthread_join(r[i].tid, NULL);
  }
  (void)fprintf(stderr, "child: every racer returned with no mapping\n");
  _exit(3);
}

/* Runs `body` in a child forked from this single-threaded process and judges
 * how it ended; NULL when it ended with the mapping error, else why not. */
static const char *run_child(void (*body)(void), char *out, size_t cap) {
  int fds[2];
  if (pipe(fds) != 0) {
    return "pipe failed";
  }
  pid_t pid = fork();
  if (pid < 0) {
    return "fork failed";
  }
  if (pid == 0) {
    close(fds[0]);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    body();
    _exit(4);
  }
  close(fds[1]);
  size_t used = 0;
  ssize_t r;
  while (used < cap - 1 && (r = read(fds[0], out + used, cap - 1 - used)) > 0) {
    used += (size_t)r;
  }
  out[used] = '\0';
  close(fds[0]);
  int status = 0;
  if (waitpid(pid, &status, 0) != pid) {
    return "waitpid failed";
  }
  (void)fputs(out, stderr);
  if (WIFSIGNALED(status)) {
    return "the child died on a signal";
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != ABORT_EXIT) {
    return "the child did not end through arts_abort";
  }
  char want[128];
  (void)snprintf(want, sizeof(want),
                 "mapping a segment of %lu slots (%zu bytes)",
                 (unsigned long)GROWN_SLOTS,
                 (size_t)GROWN_SLOTS * sizeof(arts_route_item_t));
  if (strstr(out, want) == NULL || strstr(out, strerror(ENOMEM)) == NULL ||
      strstr(out, "route_table_size") == NULL) {
    return "the child's error does not name the failed mapping";
  }
  if (strstr(out, "never published") != NULL) {
    return "a racer waited for another's build";
  }
  return NULL;
}

int main(void) {
  /* The children first, while this process has no thread of its own. */
  static char out[8192];
  const char *why = run_child(child_held_builder, out, sizeof(out));
  CHECK(why == NULL, "ii: %s\n", why);
  why = run_child(child_all_fail, out, sizeof(out));
  CHECK(why == NULL, "iii: %s\n", why);
  case_in_process();
  if (g_failed) {
    return 1;
  }
  (void)printf("PASS route_table_growth_mapfail: a failed mapping went on in "
               "a published segment or ended the process at once, never "
               "waiting\n");
  return 0;
}
