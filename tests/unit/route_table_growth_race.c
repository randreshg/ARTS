/* SPDX-License-Identifier: Apache-2.0
 *
 * route_table_growth_race — reservations of one key race each other, the
 * growth of their chain and retires of the keys being reserved, round after
 * round, and the chain ends consistent every time.
 *
 * Every round starts from a fresh root of 16 windows (shift 4).  Its keys are
 * a colliding family, 48 keys whose window is 0 at shift 9 and so at every
 * shift 4-9 (below shift 10 the window is the top bits of one product, so the
 * family fills window 0 of up to six segments, and beyond shift 9 no key
 * collides at every shift), and a spread family drawn afresh each round over
 * every window.  Twelve reservers reserve every key twice, from two different
 * threads, in a shuffled order, so same-key claims race beside growth.  One
 * key in four is installed by its first reserver and announced to a retirer
 * thread, which retires it by identity as soon as it is announced: the
 * second reserver of that key may still be in flight (a retire of the key a
 * claim is reserving), and the slots it returns in early segments race the
 * claims made in new ones.  After each round, with every thread joined at a
 * barrier, the chain is walked single-threaded:
 *   - a key no retire touched is published in exactly one slot, the one both
 *     of its reservers got; a retired key is published in at most one;
 *   - no BUSY, CLAIM, LOST or RETIRING word is left;
 *   - the chain grew, and builds - discards == segments grown;
 *   - every discarded segment was unmapped at exactly its built range (the
 *     segment ops are counted and route_table.c's munmap is wrapped).
 * ThreadSanitizer judges the publication edges.  ARTS_SCHED_FUZZ widens the
 * build->link, claim->scan and read->help windows in an injection tree.
 *
 * No runtime: route_table.c + guid.c + shared.c #include'd, the scheduling
 * fuzz and the thread RNG linked; the OoO engine is stubbed.
 */

#include "arts/utils/stdatomic_compat.h"
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#include "arts/utils/random.h"
#include "route_table_segment_probe.h"

struct arts_runtime_shared_s arts_node_info;
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;

#define ROUNDS 200
#define RESERVERS 12
#define ROOT_SHIFT 4u
#define COLLIDE 48 /* 6 segments (shifts 4-9) x COLLISION_RESOLVES */
#define SPREAD 64
#define NKEYS (COLLIDE + SPREAD)
#define NRES (2 * NKEYS)

static int g_failed;
#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      (void)fprintf(stderr, "FAIL route_table_growth_race: " __VA_ARGS__);     \
      g_failed = 1;                                                            \
    }                                                                          \
  } while (0)

/* Per round, written by main before the start barrier and read by every
 * thread after it; the per-reservation results are written by one reserver
 * each and read by main after the end barrier. */
static arts_guid_t g_keys[NKEYS];
static int g_order[NRES];
static arts_route_item_t *g_got[NRES];
static int g_obj[NKEYS];

/* Announced keys, in a fixed array: a reserver draws an index with the
 * cursor's fetch_add, fills the entry and publishes it with its ready flag. */
typedef struct {
  arts_guid_t key;
  arts_shared_ptr_t cb;
  _Atomic(int) ready;
} announce_t;
static announce_t g_ann[NKEYS];
static _Atomic(unsigned) g_ann_cursor;
static _Atomic(unsigned) g_reservers_done;
static _Atomic(unsigned) g_retired;
static _Atomic(unsigned) g_retire_failed;
static int g_was_retired[NKEYS];

static pthread_barrier_t g_start, g_end;

static int retired_key(int k) { return k % 4 == 0; }

static void *reserver(void *vp) {
  unsigned id = (unsigned)(uintptr_t)vp;
  arts_thread_info.thread_id = 0;
  arts_thread_random_init(0, id + 1);
  for (int round = 0; round < ROUNDS; round++) {
    pthread_barrier_wait(&g_start);
    for (int j = 0; j < NRES; j++) {
      int r = g_order[j];
      if ((unsigned)(r % RESERVERS) != id) {
        continue;
      }
      int k = r / 2;
      arts_route_item_t *slot = NULL;
      arts_route_table_reserve_or_lookup(g_keys[k], &slot);
      g_got[r] = slot;
      if (retired_key(k) && r % 2 == 0) {
        arts_shared_ptr_t cb =
            arts_route_table_make_handle(&g_obj[k], g_keys[k]);
        arts_shared_ptr_t mine = arts_shared_copy(cb);
        if (arts_route_table_install_handle_if_absent(cb, g_keys[k])) {
          unsigned i = atomic_fetch_add_explicit(&g_ann_cursor, 1u,
                                                 memory_order_relaxed);
          g_ann[i].key = g_keys[k];
          g_ann[i].cb = mine;
          atomic_store_explicit(&g_ann[i].ready, 1, memory_order_release);
        } else {
          arts_shared_release(&mine);
          arts_shared_abandon(&cb);
          atomic_fetch_add_explicit(&g_retire_failed, 1u, memory_order_relaxed);
        }
      }
    }
    atomic_fetch_add_explicit(&g_reservers_done, 1u, memory_order_release);
    pthread_barrier_wait(&g_end);
  }
  return NULL;
}

static void *retirer(void *vp) {
  (void)vp;
  arts_thread_info.thread_id = 0;
  arts_thread_random_init(0, RESERVERS + 1);
  for (int round = 0; round < ROUNDS; round++) {
    pthread_barrier_wait(&g_start);
    unsigned next = 0;
    for (;;) {
      if (next < NKEYS &&
          atomic_load_explicit(&g_ann[next].ready, memory_order_acquire)) {
        announce_t *a = &g_ann[next++];
        if (arts_route_table_set_destroyed_if(a->key, a->cb)) {
          atomic_fetch_add_explicit(&g_retired, 1u, memory_order_relaxed);
        } else {
          atomic_fetch_add_explicit(&g_retire_failed, 1u, memory_order_relaxed);
        }
        arts_shared_release(&a->cb);
        continue;
      }
      if (atomic_load_explicit(&g_reservers_done, memory_order_acquire) ==
              RESERVERS &&
          next == atomic_load_explicit(&g_ann_cursor, memory_order_relaxed)) {
        break;
      }
      sched_yield();
    }
    pthread_barrier_wait(&g_end);
  }
  return NULL;
}

/* xorshift for the test's own draws (main thread only). */
static uint64_t g_rng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return g_rng;
}

static arts_guid_t db_guid(uint64_t seq) {
  return ARTS_GUID_MAKE(ARTS_GUID_DB, 0, seq);
}

static arts_guid_t g_collide[COLLIDE];

static void prepare_round(arts_route_table_t *root) {
  arts_node_info.route_table[0] = root;
  for (int i = 0; i < COLLIDE; i++) {
    g_keys[i] = g_collide[i];
  }
  for (int i = COLLIDE; i < NKEYS;) {
    /* A spread key: any window, any key not already drawn. */
    arts_guid_t g = db_guid(1 + (rnd() & ((1ull << 36) - 1)));
    int dup = 0;
    for (int j = 0; j < i; j++) {
      dup |= g_keys[j] == g;
    }
    if (!dup) {
      g_keys[i++] = g;
    }
  }
  for (int i = 0; i < NRES; i++) {
    g_order[i] = i;
    g_got[i] = NULL;
  }
  for (int i = NRES - 1; i > 0; i--) {
    int j = (int)(rnd() % (uint64_t)(i + 1));
    int t = g_order[i];
    g_order[i] = g_order[j];
    g_order[j] = t;
  }
  for (int i = 0; i < NKEYS; i++) {
    g_ann[i].key = 0;
    g_ann[i].cb = NULL;
    atomic_store_explicit(&g_ann[i].ready, 0, memory_order_relaxed);
  }
  atomic_store_explicit(&g_ann_cursor, 0u, memory_order_relaxed);
  atomic_store_explicit(&g_reservers_done, 0u, memory_order_relaxed);
  probe_reset();
}

static unsigned slots_publishing(arts_route_table_t *root, arts_guid_t key,
                                 arts_route_item_t **where) {
  unsigned n = 0;
  for (arts_route_table_t *t = root; t != NULL;
       t = atomic_load_explicit(&t->next, memory_order_acquire)) {
    uint64_t pos = get_route_table_key((uint64_t)key, t->shift);
    for (int i = 0; i < COLLISION_RESOLVES; i++) {
      if (atomic_load_explicit(&t->data[pos + i].key, memory_order_acquire) ==
          key) {
        *where = &t->data[pos + i];
        n++;
      }
    }
  }
  return n;
}

static int check_round(int round, arts_route_table_t *root) {
  int bad = 0;
  for (int k = 0; k < NKEYS; k++) {
    g_was_retired[k] = 0;
  }
  unsigned n = atomic_load_explicit(&g_ann_cursor, memory_order_relaxed);
  for (unsigned i = 0; i < n; i++) {
    for (int k = 0; k < NKEYS; k++) {
      g_was_retired[k] |= g_keys[k] == g_ann[i].key;
    }
  }
  for (int k = 0; k < NKEYS; k++) {
    arts_route_item_t *where = NULL;
    unsigned c = slots_publishing(root, g_keys[k], &where);
    if (g_was_retired[k]) {
      if (c > 1) {
        CHECK(0, "round %d: retired key %d is published in %u slots\n", round,
              k, c);
        bad = 1;
      }
      continue;
    }
    if (c != 1 || g_got[2 * k] != where || g_got[2 * k + 1] != where) {
      CHECK(0,
            "round %d: key %d is published in %u slots; its reservers got "
            "%p and %p, published %p\n",
            round, k, c, (void *)g_got[2 * k], (void *)g_got[2 * k + 1],
            (void *)where);
      bad = 1;
    }
  }
  unsigned claims = 0, published = 0;
  probe_scan_chain(root, 0, &claims, &published);
  if (claims != 0) {
    CHECK(0, "round %d: %u claim or retiring words left\n", round, claims);
    bad = 1;
  }
  unsigned segs = 0;
  for (arts_route_table_t *t = root; t != NULL;
       t = atomic_load_explicit(&t->next, memory_order_acquire)) {
    segs++;
  }
  unsigned builds = atomic_load_explicit(&probe_builds, memory_order_relaxed);
  unsigned discards =
      atomic_load_explicit(&probe_discards, memory_order_relaxed);
  if (segs < 2 || builds - discards != segs - 1) {
    CHECK(0, "round %d: %u segments from %u builds and %u discards\n", round,
          segs, builds, discards);
    bad = 1;
  }
  if (probe_discards_not_unmapped(0) != 0) {
    CHECK(0, "round %d: %u discarded segments were not unmapped exactly\n",
          round, probe_discards_not_unmapped(0));
    bad = 1;
  }
  return bad;
}

int main(void) {
  arts_thread_info.thread_id = 0;
  arts_node_info.total_thread_count = 1;
  arts_guid_node_geometry_init();
  arts_thread_random_init(0, 0);
  arts_node_info.route_table =
      (arts_route_table_t **)calloc(1, sizeof(arts_route_table_t *));
  for (int s = 0; s < ARTS_REMOTE_ROUTE_SHARDS; s++) {
    arts_node_info.remote_route_table[s] = arts_new_route_table(64, 6);
  }
  int found = 0;
  for (uint64_t seq = 1; found < COLLIDE && seq < (1ull << 36); seq++) {
    arts_guid_t g = db_guid(seq);
    if (get_route_table_key((uint64_t)g, 9) == 0) {
      g_collide[found++] = g;
    }
  }
  if (found < COLLIDE) {
    (void)fprintf(stderr,
                  "FAIL route_table_growth_race: only %d colliding "
                  "keys\n",
                  found);
    return 1;
  }

  pthread_barrier_init(&g_start, NULL, RESERVERS + 2);
  pthread_barrier_init(&g_end, NULL, RESERVERS + 2);
  pthread_t tid[RESERVERS + 1];
  arts_route_table_t *root = probe_root(1u << ROOT_SHIFT, ROOT_SHIFT);
  prepare_round(root);
  for (unsigned i = 0; i < RESERVERS; i++) {
    pthread_create(&tid[i], NULL, reserver, (void *)(uintptr_t)i);
  }
  pthread_create(&tid[RESERVERS], NULL, retirer, NULL);
  unsigned rounds_bad = 0, total_discards = 0, total_segs = 0;
  for (int round = 0; round < ROUNDS; round++) {
    pthread_barrier_wait(&g_start);
    pthread_barrier_wait(&g_end);
    rounds_bad += (unsigned)check_round(round, root);
    total_discards +=
        atomic_load_explicit(&probe_discards, memory_order_relaxed);
    for (arts_route_table_t *t = root; t != NULL;
         t = atomic_load_explicit(&t->next, memory_order_acquire)) {
      total_segs++;
    }
    arts_delete_route_table(root);
    if (round + 1 < ROUNDS) {
      root = probe_root(1u << ROOT_SHIFT, ROOT_SHIFT);
      prepare_round(root);
    }
  }
  for (unsigned i = 0; i <= RESERVERS; i++) {
    pthread_join(tid[i], NULL);
  }
  pthread_barrier_destroy(&g_start);
  pthread_barrier_destroy(&g_end);
  for (int s = 0; s < ARTS_REMOTE_ROUTE_SHARDS; s++) {
    arts_delete_route_table(arts_node_info.remote_route_table[s]);
  }
  free(arts_node_info.route_table);
  arts_guid_generator_cleanup();
  unsigned retired = atomic_load_explicit(&g_retired, memory_order_relaxed);
  unsigned rfail = atomic_load_explicit(&g_retire_failed, memory_order_relaxed);
  (void)fprintf(stderr,
                "%d rounds: %u segments, %u lost builds discarded, %u keys "
                "retired, %u installs or retires refused\n",
                ROUNDS, total_segs, total_discards, retired, rfail);
  CHECK(rfail == 0, "%u installs or retires of a published key failed\n",
        rfail);
  CHECK(retired == (unsigned)ROUNDS * ((NKEYS + 3) / 4),
        "%u keys retired, expected %u\n", retired,
        (unsigned)ROUNDS * ((NKEYS + 3) / 4));
  if (g_failed || rounds_bad) {
    return 1;
  }
  (void)printf("PASS route_table_growth_race: %d rounds of racing growth, "
               "claims and retires\n",
               ROUNDS);
  return 0;
}
