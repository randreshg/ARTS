/* SPDX-License-Identifier: Apache-2.0
 *
 * route_table_segment_grow — force the lazy segment-chain GROWTH in
 * search_for_empty: band overflow, concurrent growth, convergence, and the
 * largest segment.
 *
 * A route_table band is COLLISION_RESOLVES (8) wide.  When >8 DISTINCT GUIDs
 * hash to the same band on the root segment, the 9th claim cannot fit and
 * search_for_empty lazily grows the chain: the one thread whose CAS takes
 * `->next` from NULL to GROWING builds the 2x / shift+1 segment and publishes
 * it; every other thread at the chain's end waits for that publication.
 *
 * We hand-pick >8 GUIDs that all hash to band base 0 at the root (size=16,
 * shift=4), forcing at least one growth, then:
 *
 *   1. Serial: install all colliding keys → the chain grew (root->next != NULL)
 *      and every key has exactly one canonical slot; lookups return the right
 *      objects; no slot aliasing.
 *
 *   2. Concurrent grow race: many threads simultaneously search_for_empty the
 *      same overflowing key-set → all converge on one slot per key, with no
 *      lost or duplicate slot.
 *
 *   3. Growth always helps: search_for_empty extends the chain whenever
 *      `next == NULL`, so an arbitrarily large colliding set is absorbed and
 *      the impossible-state ARTS_ERROR after its loop is never reached.  We
 *      install far more than one band's worth of colliding keys and assert NO
 *      abort occurs.
 *
 *   4. Except past the largest segment: a segment of 2^31 windows cannot be
 *      doubled in its unsigned count, so a key whose window is full there ends
 *      the process with an error naming route_table_size, never a segment of
 *      a wrapped size (checked in a forked child).
 *
 * No runtime: route_table.c + guid.c + shared.c #include'd; OoO drain no-op.
 */

#include "arts/utils/stdatomic_compat.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

unsigned int arts_global_rank_id = 0;
unsigned int arts_global_rank_count = 1;
void arts_abort(uint8_t code) { _exit(code ? code : 70); }
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

struct arts_route_item_s;
void arts_ooo_drain(struct arts_route_item_s *s) { (void)s; }
void arts_ooo_redrive_all(struct arts_route_item_s *s) { (void)s; }
void arts_ooo_free_all(struct arts_route_item_s *s) { (void)s; }

#include "../../libs/src/core/gas/guid.c"
#include "../../libs/src/core/gas/route_table.c"
#include "../../libs/src/core/utils/shared.c"

struct arts_runtime_shared_s arts_node_info;
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;

#define FAIL(...)                                                              \
  do {                                                                         \
    (void)fprintf(stderr, "FAIL route_table_segment_grow: " __VA_ARGS__);      \
    return 1;                                                                  \
  } while (0)

/* A standalone root table so we control size/shift (the grow path keys off
 * the table's own shift, not the global generator).
 *
 * INVARIANT (load-bearing): a route_table's `size` MUST equal 2^`shift` —
 * get_route_table_key returns `(hash >> (64-shift)) * COLLISION_RESOLVES`, a
 * band base in [0, 2^shift * 8), and the slot array has exactly size*8 slots.
 * arts_new_route_table asserts it of every table it builds, and the grow
 * path's newFunc(2*size, shift+1) preserves it (2*2^shift == 2^(shift+1)).
 * We pick a SMALL but invariant-respecting root: shift=4 → size=16 (128 slots),
 * so growth is reached quickly while every grown band base stays in range. */
static arts_route_table_t *g_root;
static const unsigned ROOT_SHIFT = 4;
static const unsigned ROOT_SIZE = 1u << 4; /* == 2^ROOT_SHIFT */

/* Collect N distinct GUID keys that hash to band base 0 at the root. */
#define NCOLLIDE 40
static arts_guid_t g_keys[NCOLLIDE];

static int collect_colliding_keys(void) {
  int found = 0;
  for (uint64_t k = 1; k < 5000000 && found < NCOLLIDE; k++) {
    arts_guid_t g = ARTS_GUID_MAKE(ARTS_GUID_DB, 0, k);
    if (get_route_table_key((uint64_t)g, ROOT_SHIFT) == 0) {
      g_keys[found++] = g;
    }
  }
  return found;
}

static unsigned chain_len(arts_route_table_t *t) {
  unsigned n = 0;
  for (arts_route_table_t *c = t; c;
       c = atomic_load_explicit(&c->next, memory_order_acquire)) {
    n++;
  }
  return n;
}

/* ── concurrent grow race ──────────────────────────────────────────────── */
#define GROW_THREADS 8

typedef struct {
  atomic_int *gate;
  arts_route_item_t **observed; /* [NCOLLIDE] per-thread slot view */
} grow_ctx_t;

static void *grow_worker(void *vp) {
  grow_ctx_t *c = (grow_ctx_t *)vp;
  while (atomic_load_explicit(c->gate, memory_order_acquire) == 0) {
  }
  for (int rep = 0; rep < 200; rep++) {
    for (int i = 0; i < NCOLLIDE; i++) {
      arts_route_item_t *item =
          arts_route_table_search_for_empty(g_root, g_keys[i]);
      if (!item) {
        return (void *)1; /* never NULL */
      }
      if (c->observed[i] == NULL) {
        c->observed[i] = item;
      } else if (c->observed[i] != item) {
        return (void *)2; /* slot diverged within a thread */
      }
    }
  }
  return NULL;
}

/* ── growth past the largest segment ───────────────────────────────────── */

/* A child fills one window of a segment that counts 2^31 windows, then claims
 * one more key there.  The segment probes with shift 1, so its keys reach only
 * the slots of the two windows mapped for it; only its count is the largest.
 * Returns NULL when the child ended with an error naming route_table_size and
 * the segment's count, else what it did instead. */
static const char *largest_segment_refuses_growth(char *out, size_t cap) {
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
    arts_route_table_t *last = arts_new_route_table(2, 1);
    last->size = 1u << 31;
    int claimed = 0;
    for (uint64_t k = 1; claimed <= COLLISION_RESOLVES; k++) {
      arts_guid_t g = ARTS_GUID_MAKE(ARTS_GUID_DB, 0, k);
      if (get_route_table_key((uint64_t)g, last->shift) == 0) {
        (void)arts_route_table_search_for_empty(last, g);
        claimed++;
      }
    }
    _exit(0);
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
  if (WIFSIGNALED(status)) {
    return "the child died on a signal";
  }
  if (WEXITSTATUS(status) == 0) {
    return "the claim past the largest segment succeeded";
  }
  if (strstr(out, "route_table_size") == NULL ||
      strstr(out, "2147483648 windows") == NULL) {
    return "the child failed without naming route_table_size and the count";
  }
  return NULL;
}

int main(void) {
  int n = collect_colliding_keys();
  if (n < NCOLLIDE) {
    FAIL("only found %d colliding keys (need %d)\n", n, NCOLLIDE);
  }

  /* ---- 1. Serial growth + canonical slots. ---- */
  g_root = arts_new_route_table(ROOT_SIZE, ROOT_SHIFT);
  arts_route_item_t *slots[NCOLLIDE];
  for (int i = 0; i < NCOLLIDE; i++) {
    slots[i] = arts_route_table_search_for_empty(g_root, g_keys[i]);
    if (!slots[i]) {
      FAIL("1: search_for_empty returned NULL for key %d\n", i);
    }
    if (atomic_load_explicit(&slots[i]->key, memory_order_acquire) !=
        g_keys[i]) {
      FAIL("1: slot %d claimed wrong key\n", i);
    }
  }
  /* The chain MUST have grown past the root (one band of 8 can't hold 40). */
  if (chain_len(g_root) < 2) {
    FAIL("1: chain did not grow (len=%u) despite %d colliding keys\n",
         chain_len(g_root), NCOLLIDE);
  }
  /* Every key resolves back to exactly its slot; no aliasing. */
  for (int i = 0; i < NCOLLIDE; i++) {
    arts_route_item_t *f = arts_route_table_search_for_key(g_root, g_keys[i]);
    if (f != slots[i]) {
      FAIL("1: key %d search_for_key %p != claimed %p\n", i, (void *)f,
           (void *)slots[i]);
    }
    for (int j = 0; j < i; j++) {
      if (slots[i] == slots[j]) {
        FAIL("1: keys %d and %d aliased the same slot\n", i, j);
      }
    }
  }
  arts_delete_route_table(g_root);

  /* ---- 2. Concurrent grow race. ---- */
  g_root = arts_new_route_table(ROOT_SIZE, ROOT_SHIFT);
  pthread_t tids[GROW_THREADS];
  grow_ctx_t ctx[GROW_THREADS];
  atomic_int gate;
  atomic_init(&gate, 0);
  arts_route_item_t **storage = (arts_route_item_t **)calloc(
      (size_t)GROW_THREADS * NCOLLIDE, sizeof(arts_route_item_t *));
  for (int t = 0; t < GROW_THREADS; t++) {
    ctx[t].gate = &gate;
    ctx[t].observed = storage + (size_t)t * NCOLLIDE;
    if (pthread_create(&tids[t], NULL, grow_worker, &ctx[t]) != 0) {
      FAIL("2: pthread_create %d\n", t);
    }
  }
  atomic_store_explicit(&gate, 1, memory_order_release);
  for (int t = 0; t < GROW_THREADS; t++) {
    void *rc = NULL;
    pthread_join(tids[t], &rc);
    if (rc) {
      FAIL("2: worker %d returned error code %ld\n", t, (long)rc);
    }
  }
  /* Cross-thread: all threads must agree on the slot pointer for each key. */
  for (int i = 0; i < NCOLLIDE; i++) {
    arts_route_item_t *winner = NULL;
    for (int t = 0; t < GROW_THREADS; t++) {
      arts_route_item_t *p = storage[(size_t)t * NCOLLIDE + i];
      if (!p) {
        continue;
      }
      if (!winner) {
        winner = p;
      } else if (winner != p) {
        FAIL("2: key %d slot diverged across threads (%p vs %p)\n", i,
             (void *)winner, (void *)p);
      }
    }
    if (!winner) {
      FAIL("2: key %d never observed\n", i);
    }
  }
  free(storage);
  arts_delete_route_table(g_root);

  /* ---- 3. A large colliding set is always absorbed. ---------------------- *
   * NCOLLIDE (40) >> one band (8); if growth could ever fail to help, the
   * serial install above would have aborted (ARTS_ERROR → _exit non-zero).
   * Reaching here shows the chain always extends. */

  /* ---- 4. The largest segment cannot grow, and says so. ---- */
  char out[4096] = "";
  const char *why = largest_segment_refuses_growth(out, sizeof(out));
  if (why != NULL) {
    FAIL("4: %s; child output:\n%s\n", why, out);
  }

  printf("PASS route_table_segment_grow: forced chain growth absorbed %d "
         "colliding keys, concurrent grow converges, no impossible-state "
         "abort, growth past the largest segment refused by name\n",
         NCOLLIDE);
  return 0;
}
