/* SPDX-License-Identifier: Apache-2.0
 *
 * route_table_fresh — the reservation of a freshly minted GUID publishes it
 * at the first free slot of its probe window with one CAS and never waits.
 *
 *   (a) The fresh walk passes another key's BUSY word, a live claim of
 *       another key held after its fence, and a slot held RETIRING by a
 *       retire stopped inside its window, and takes the first LOST word:
 *       it returns while the retire is still held (the no-wait witness), and
 *       the slot holds the GUID with its claim version untouched.
 *   (b) A general reserver of another key is held after its claim in the
 *       same window: the fresh reservation takes the next free slot, and
 *       the general reserver, released, publishes its own.
 *   (c) A fresh reservation of a GUID already published (the first slot of
 *       its window, which the walk meets before any free one): a Debug build
 *       ends the process with "already published" (checked in a forked
 *       child); a Release build returns the published slot.
 *   (d) Debug only: a general reservation of the same GUID is held after its
 *       claim, which the test sees before it starts the fresh reservation;
 *       the fresh reservation's check ends the process with "another
 *       reservation holds it" (in a forked child).
 * Each child is forked before the test creates any thread, ends through
 * arts_abort's _exit and has its output forwarded to stderr.
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
/* route_table.c's other windows are scheduling-fuzz points there. */
void arts_sched_fuzz_point(void) {}
#endif

struct arts_route_item_s;
void arts_ooo_drain(struct arts_route_item_s *s) { (void)s; }
void arts_ooo_redrive_all(struct arts_route_item_s *s) { (void)s; }
void arts_ooo_free_all(struct arts_route_item_s *s) { (void)s; }

typedef struct {
  _Atomic(int) arrived;
  _Atomic(int) go;
} gate_t;

static __thread gate_t *t_after_claim;
static __thread gate_t *t_after_retire;

static void hook_hold(gate_t **slot) {
  gate_t *g = *slot;
  if (g == NULL) {
    return;
  }
  *slot = NULL;
  atomic_store_explicit(&g->arrived, 1, memory_order_release);
  while (!atomic_load_explicit(&g->go, memory_order_acquire)) {
    sched_yield();
  }
}

#include "../../libs/src/core/gas/guid.c"
#define ROUTE_TABLE_AFTER_CLAIM(key) ((void)(key), hook_hold(&t_after_claim))
#define ROUTE_TABLE_AFTER_RETIRE_CLAIM(key)                                    \
  ((void)(key), hook_hold(&t_after_retire))
#include "../../libs/src/core/gas/route_table.c"
#include "../../libs/src/core/utils/shared.c"

struct arts_runtime_shared_s arts_node_info;
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;

static int g_failed;
#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      (void)fprintf(stderr, "FAIL route_table_fresh: " __VA_ARGS__);           \
      g_failed = 1;                                                            \
    }                                                                          \
  } while (0)

static void gate_init(gate_t *g) {
  atomic_init(&g->arrived, 0);
  atomic_init(&g->go, 0);
}

static int gate_await(gate_t *g) {
  for (int i = 0; i < 20000; i++) {
    if (atomic_load_explicit(&g->arrived, memory_order_acquire)) {
      return 1;
    }
    struct timespec ts = {0, 1000000L};
    nanosleep(&ts, NULL);
  }
  return 0;
}

static void gate_open(gate_t *g) {
  atomic_store_explicit(&g->go, 1, memory_order_release);
}

/* ── the table ─────────────────────────────────────────────────────────── */

#define SHIFT 10u
static arts_route_table_t *g_table;

static void table_init(void) {
  arts_thread_info.thread_id = 0;
  arts_node_info.total_thread_count = 1;
  arts_guid_node_geometry_init();
  arts_node_info.route_table =
      (arts_route_table_t **)calloc(1, sizeof(arts_route_table_t *));
  for (int s = 0; s < ARTS_REMOTE_ROUTE_SHARDS; s++) {
    arts_node_info.remote_route_table[s] = arts_new_route_table(64, 6);
  }
  g_table = arts_new_route_table(1u << SHIFT, SHIFT);
  arts_node_info.route_table[0] = g_table;
}

static void table_fini(void) {
  arts_delete_route_table(g_table);
  for (int s = 0; s < ARTS_REMOTE_ROUTE_SHARDS; s++) {
    arts_delete_route_table(arts_node_info.remote_route_table[s]);
  }
  free(arts_node_info.route_table);
  arts_guid_generator_cleanup();
}

static arts_guid_t db_guid(uint64_t seq) {
  return ARTS_GUID_MAKE(ARTS_GUID_DB, 0, seq);
}

static uint64_t g_seq = 5000;

/* `n` keys sharing one window no earlier draw used. */
static void same_window(arts_guid_t *out, int n) {
  out[0] = db_guid(g_seq++);
  uint64_t w = get_route_table_key((uint64_t)out[0], SHIFT);
  int found = 1;
  for (uint64_t seq = g_seq; found < n; seq++) {
    arts_guid_t g = db_guid(seq);
    if (get_route_table_key((uint64_t)g, SHIFT) == w) {
      out[found++] = g;
    }
  }
  g_seq += 1u << 20;
}

static arts_route_item_t *window(arts_guid_t k) {
  return &g_table->data[get_route_table_key((uint64_t)k, SHIFT)];
}

static arts_guid_t word(arts_route_item_t *s) {
  return atomic_load_explicit(&s->key, memory_order_acquire);
}

static void put(arts_route_item_t *s, arts_guid_t w) {
  atomic_store_explicit(&s->key, w, memory_order_release);
}

typedef struct {
  arts_guid_t key;
  gate_t *after_claim;
  arts_route_item_t *slot;
  pthread_t tid;
} reserver_t;

static void *reserve_body(void *vp) {
  reserver_t *r = (reserver_t *)vp;
  t_after_claim = r->after_claim;
  arts_route_table_reserve_or_lookup(r->key, &r->slot);
  return NULL;
}

typedef struct {
  arts_guid_t key;
  arts_shared_ptr_t cb;
  gate_t *after;
  int ret;
  pthread_t tid;
} retirer_t;

static void *retire_body(void *vp) {
  retirer_t *r = (retirer_t *)vp;
  t_after_retire = r->after;
  r->ret = arts_route_table_set_destroyed_if(r->key, r->cb);
  return NULL;
}

static int g_obj;

/* ── (a) ───────────────────────────────────────────────────────────────── */

static void case_a(void) {
  arts_guid_t k[5];
  same_window(k, 5);
  arts_guid_t fresh = k[0], kclaim = k[1], kret = k[2];
  arts_route_item_t *s0 = window(fresh);
  /* s2: an installed key whose retire is held inside its window. */
  put(s0, k[3]);
  put(s0 + 1, k[4]);
  arts_shared_ptr_t cb = arts_route_table_install_if_absent(&g_obj, kret);
  CHECK(cb != NULL && word(s0 + 2) == kret, "a: setup of the retiring slot\n");
  gate_t gr;
  gate_init(&gr);
  retirer_t r = {kret, arts_shared_copy(cb), &gr, -1, 0};
  pthread_create(&r.tid, NULL, retire_body, &r);
  CHECK(gate_await(&gr) && word(s0 + 2) == ARTS_ROUTE_KEY_RETIRING,
        "a: the retire is not held RETIRING\n");
  /* s3: a lost claim of version 5. */
  (s0 + 3)->claim_ver = 5;
  put(s0 + 3, ROUTE_KEY_LOST(5));
  /* s1: a live claim of another key, held after its fence. */
  put(s0 + 1, 0);
  gate_t gc;
  gate_init(&gc);
  reserver_t c = {kclaim, &gc, NULL, 0};
  pthread_create(&c.tid, NULL, reserve_body, &c);
  CHECK(gate_await(&gc) && ROUTE_KEY_IS_CLAIM(word(s0 + 1)),
        "a: the other key's claim is not in the second slot\n");
  /* s0: another claimer's BUSY word. */
  put(s0, ROUTE_KEY_BUSY);

  arts_route_item_t *got = NULL;
  arts_route_table_reserve_fresh(fresh, &got);
  CHECK(got == s0 + 3 && word(s0 + 3) == fresh,
        "a: the fresh GUID did not take the first LOST word\n");
  CHECK((s0 + 3)->claim_ver == 5, "a: the fresh publish moved the version\n");
  CHECK(word(s0) == ROUTE_KEY_BUSY && ROUTE_KEY_IS_CLAIM(word(s0 + 1)) &&
            word(s0 + 2) == ARTS_ROUTE_KEY_RETIRING,
        "a: the fresh walk wrote a word it passed\n");

  /* Release the others: the claim waits the retire out, then publishes. */
  put(s0, k[3]);
  gate_open(&gc);
  gate_open(&gr);
  pthread_join(r.tid, NULL);
  pthread_join(c.tid, NULL);
  CHECK(r.ret == 1 && word(s0 + 2) == 0, "a: the held retire did not land\n");
  CHECK(c.slot == s0 + 1 && word(s0 + 1) == kclaim,
        "a: the other key did not publish its claim\n");
  arts_shared_release(&r.cb);
  arts_shared_release(&cb);
}

/* ── (b) ───────────────────────────────────────────────────────────────── */

static void case_b(void) {
  arts_guid_t k[2];
  same_window(k, 2);
  arts_guid_t fresh = k[0], kclaim = k[1];
  arts_route_item_t *s0 = window(fresh);
  gate_t gc;
  gate_init(&gc);
  reserver_t c = {kclaim, &gc, NULL, 0};
  pthread_create(&c.tid, NULL, reserve_body, &c);
  CHECK(gate_await(&gc) && ROUTE_KEY_IS_CLAIM(word(s0)),
        "b: the general claim is not in the first slot\n");
  arts_route_item_t *got = NULL;
  arts_route_table_reserve_fresh(fresh, &got);
  CHECK(got == s0 + 1 && word(s0 + 1) == fresh,
        "b: the fresh GUID did not take the next free slot\n");
  gate_open(&gc);
  pthread_join(c.tid, NULL);
  CHECK(c.slot == s0 && word(s0) == kclaim,
        "b: the general reserver did not publish its own slot\n");
  CHECK(word(s0 + 2) == 0, "b: a third slot was written\n");
}

/* ── (c), (d) ──────────────────────────────────────────────────────────── */

#ifdef NDEBUG
static void case_c_release(void) {
  arts_guid_t k[1];
  same_window(k, 1);
  arts_route_item_t *s0 = window(k[0]);
  arts_route_item_t *first = NULL;
  arts_route_table_reserve_or_lookup(k[0], &first);
  CHECK(first == s0, "c: the general reservation is not in the first slot\n");
  arts_route_item_t *got = NULL;
  arts_route_table_reserve_fresh(k[0], &got);
  CHECK(got == s0 && word(s0 + 1) == 0,
        "c: Release did not return the published slot\n");
}
#else
static void child_published(void) {
  table_init();
  arts_guid_t k[1];
  same_window(k, 1);
  arts_route_item_t *first = NULL;
  arts_route_table_reserve_or_lookup(k[0], &first);
  if (first != window(k[0])) {
    (void)fprintf(stderr, "child: the published slot is not the first\n");
    _exit(2);
  }
  arts_route_item_t *got = NULL;
  arts_route_table_reserve_fresh(k[0], &got);
  (void)fprintf(stderr, "child: the fresh reservation returned\n");
  _exit(3);
}

static void child_claimed(void) {
  table_init();
  arts_guid_t k[1];
  same_window(k, 1);
  gate_t gc;
  gate_init(&gc);
  reserver_t c = {k[0], &gc, NULL, 0};
  pthread_create(&c.tid, NULL, reserve_body, &c);
  if (!gate_await(&gc) || !ROUTE_KEY_IS_CLAIM(word(window(k[0])))) {
    (void)fprintf(stderr, "child: the general claim was not seen\n");
    _exit(2);
  }
  arts_route_item_t *got = NULL;
  arts_route_table_reserve_fresh(k[0], &got);
  (void)fprintf(stderr, "child: the fresh reservation returned\n");
  _exit(3);
}

static const char *run_child(void (*body)(void), const char *want, char *out,
                             size_t cap) {
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
  if (strstr(out, want) == NULL) {
    return "the child's error does not name the misuse";
  }
  return NULL;
}
#endif

int main(void) {
#ifndef NDEBUG
  /* The children first, while this process has no thread of its own. */
  static char out[8192];
  const char *why =
      run_child(child_published, "is already published", out, sizeof(out));
  CHECK(why == NULL, "c: %s\n", why);
  why = run_child(child_claimed, "another reservation holds it", out,
                  sizeof(out));
  CHECK(why == NULL, "d: %s\n", why);
#endif
  table_init();
  case_a();
  case_b();
#ifdef NDEBUG
  case_c_release();
#endif
  table_fini();
  if (g_failed) {
    return 1;
  }
  (void)printf("PASS route_table_fresh: the fresh walk passed claims and a "
               "held retire without waiting and took the first free word; a "
               "fresh reservation of a named GUID is caught in Debug\n");
  return 0;
}
