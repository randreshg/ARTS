/* SPDX-License-Identifier: Apache-2.0
 *
 * route_table_placement — a key's route table is a pure function of its
 * 48-bit key and of the rank's node-init geometry: with S keys per rank slice
 * and kpt per thread block, r = key / S, t = (key mod S) / kpt,
 * v = (key mod S) mod kpt and table = (t + r + v / 2^(size + 2)) mod T.  The
 * oracle below computes that with plain division, independently of the
 * runtime's reciprocal multiplies.
 *
 *   1. Formula and bound: over rank counts, thread counts and table sizes,
 *      every key class (block edges and chunk edges of every sampled rank and
 *      thread, slack keys, the top of the key space, data-block keys of every
 *      sampled size hint and creator slice and the never-minted seqs, keys 0
 *      and 2^48 - 1, every kind and several home fields) resolves to the
 *      oracle's table, which is below T.
 *   2. Stagger: chunk j of rank r's thread-t block lands in (t + r + j) mod T
 *      at geometries where dividing the key by kpt alone would not.
 *   3. Thread agreement: eight threads with distinct thread ids resolve the
 *      same keys to the oracle's tables.
 *   4. Rank-id independence: the geometry and the placement rebuilt under two
 *      rank ids resolve every key alike.
 *   5. Entry-point agreement, T = 4 real tables, for a flat key this rank
 *      minted, a foreign flat key and a data-block key: a fresh install on
 *      one thread is found from another by lookup, typed lookup and
 *      install_or_get; a retire run while the installed handle has no home yet
 *      finds the slot by search; an OoO park on a third thread is seen by the
 *      installer's drain; every slot lies in the oracle's table only.
 *   6. The reciprocal division equals hardware division over the divisors a
 *      rank uses.
 *   7. A large single-source set spreads: a range of 8 chunks fills 8 tables
 *      with one run each and no table grows; a thread's 4 chunks of single
 *      mints take 4 consecutive tables.
 *   8. Node init builds the tables and node cleanup clears the placement; in
 *      a Debug build a placement before node init stops on its assert.
 *
 * No runtime: guid.c, route_table.c and shared.c are #include'd; the OoO
 * engine is a stub whose drain records the chain it takes.
 */

#include "arts/system/shutdown.h"
#include "arts/utils/stdatomic_compat.h"
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

unsigned int arts_global_rank_id = 0;
unsigned int arts_global_rank_count = 1;
_Noreturn void arts_exit(int rc, enum arts_exit_origin origin) {
  (void)origin;
  _exit(rc ? rc : 70);
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

/* Item 5's home-less retire: the install of g_hook_key hands its handle to
 * another thread between the install CAS and the home store. */
static void after_install_cas(unsigned long key);
#define ROUTE_TABLE_AFTER_INSTALL_CAS(key)                                     \
  after_install_cas((unsigned long)(key))

#include "../../libs/src/core/gas/guid.c"
#include "../../libs/src/core/gas/route_table.c"
#include "../../libs/src/core/utils/shared.c"

#include "counter_stubs.h"

struct arts_runtime_shared_s arts_node_info;
ARTS_THREAD_LOCAL_IE struct arts_runtime_private_s arts_thread_info;

static unsigned long g_failures;
#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      if (g_failures++ < 20) {                                                 \
        (void)fprintf(stderr, "FAIL route_table_placement: " __VA_ARGS__);     \
      }                                                                        \
    }                                                                          \
  } while (0)

/* ── OoO stubs: the drain records a chain it takes ───────────────────────── */

static arts_route_item_t *g_drain_slot;
static arts_lf_link_t *g_drained;
void arts_ooo_drain_on(struct arts_route_item_s *slot,
                       arts_shared_ptr_t installed) {
  (void)installed;
  arts_lf_link_t *l = arts_lf_stack_drain(&slot->ooo_list);
  if (l != NULL) {
    g_drain_slot = slot;
    g_drained = l;
  }
}
void arts_ooo_redrive_all(struct arts_route_item_s *s) { (void)s; }
void arts_ooo_free_all(struct arts_route_item_s *s) { (void)s; }

/* ── geometry and oracle ─────────────────────────────────────────────────── */

#define MAXT 64u
static uint64_t g_S, g_kpt, g_T, g_c;

static void free_keys(void) {
  if (arts_node_info.keys != NULL) {
    for (unsigned t = 0; t < arts_node_info.total_thread_count; t++) {
      free(arts_node_info.keys[t]);
    }
    free(arts_node_info.keys);
    arts_node_info.keys = NULL;
  }
}

/* The runtime's node init of rank `rank` of `nrank` with `threads` threads:
 * the geometry, then (with `counters`) every thread's key counters. */
static void geometry(unsigned nrank, unsigned rank, unsigned threads,
                     unsigned size, int counters) {
  free_keys();
  arts_guid_generator_cleanup();
  arts_global_rank_count = nrank;
  arts_global_rank_id = rank;
  arts_node_info.total_thread_count = threads;
  arts_guid_node_geometry_init();
  if (counters) {
    arts_node_info.keys = (uint64_t **)calloc(threads, sizeof(uint64_t *));
    for (unsigned t = 0; t < threads; t++) {
      arts_thread_info.thread_id = t;
      arts_guid_key_generator_init();
    }
    arts_thread_info.thread_id = 0;
  }
  g_S = ((uint64_t)1 << 48) / nrank;
  g_kpt = g_S / threads;
  g_T = threads;
  g_c = size + 2;
}

static unsigned oracle(arts_guid_t g) {
  uint64_t k = ARTS_GUID_GET_KEY(g);
  uint64_t r = k / g_S, off = k % g_S, t = off / g_kpt, v = off % g_kpt;
  return (unsigned)((t + r + (v >> g_c)) % g_T);
}

/* Placeholder tables: placement only selects one. */
static arts_route_table_t g_ph[MAXT];
static arts_route_table_t *g_ph_slots[MAXT];

static void place_on_placeholders(unsigned size) {
  route_place_init(size);
  arts_node_info.route_table = g_ph_slots;
}

static unsigned g_check_kind;
static void check_key(uint64_t key, unsigned nrank) {
  static const unsigned homes[4] = {0u, 1u, 0x3FFDu, 0x3FFEu};
  unsigned kind = g_check_kind++ & 3u;
  (void)nrank;
  arts_guid_t g = ARTS_GUID_MAKE(kind, homes[(g_check_kind >> 2) & 3u], key);
  unsigned want = oracle(g);
  long got = arts_get_route_table(g) - g_ph;
  CHECK(want < g_T && got == (long)want,
        "1: key %#llx (S=%llu kpt=%llu T=%llu) took table %ld, want %u\n",
        (unsigned long long)key, (unsigned long long)g_S,
        (unsigned long long)g_kpt, (unsigned long long)g_T, got, want);
}

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return g_rng;
}

/* Sampled ranks of `nrank`: all of a small count, else the edges and some. */
static unsigned sample_ranks(unsigned nrank, unsigned *out) {
  unsigned n = 0;
  if (nrank <= 16) {
    for (unsigned r = 0; r < nrank; r++) {
      out[n++] = r;
    }
    return n;
  }
  out[n++] = 0;
  out[n++] = 1;
  out[n++] = nrank / 2;
  out[n++] = nrank - 2;
  out[n++] = nrank - 1;
  for (int i = 0; i < 8; i++) {
    out[n++] = (unsigned)(rnd() % nrank);
  }
  return n;
}

/* ── 1. formula and bound ───────────────────────────────────────────────── */

static void item_formula(void) {
  static const unsigned ns[] = {1, 2, 3, 4, 7, 64, 1000, 16382};
  static const unsigned ts[] = {1, 2, 3, 5, 8, 10, 20, 64};
  static const unsigned sizes[] = {1, 4, 14, 16, 31};
  const uint64_t top = ((uint64_t)1 << 48) - 1;
  for (unsigned a = 0; a < sizeof(ns) / sizeof(ns[0]); a++) {
    for (unsigned b = 0; b < sizeof(ts) / sizeof(ts[0]); b++) {
      for (unsigned c = 0; c < sizeof(sizes) / sizeof(sizes[0]); c++) {
        unsigned nrank = ns[a], threads = ts[b];
        geometry(nrank, 0, threads, sizes[c], 0);
        place_on_placeholders(sizes[c]);
        check_key(0, nrank);
        check_key(1, nrank);
        check_key(top, nrank);
        unsigned ranks[16];
        unsigned nr = sample_ranks(nrank, ranks);
        for (unsigned i = 0; i < nr; i++) {
          uint64_t base = g_S * ranks[i];
          for (unsigned t = 0; t < threads; t++) {
            uint64_t lo = base + g_kpt * t;
            check_key(lo, nrank);
            check_key(lo + 1, nrank);
            check_key(lo + g_kpt - 1, nrank);
            check_key(lo + g_kpt - 2, nrank);
            for (uint64_t j = 1; j <= 3; j++) {
              if ((j << g_c) < g_kpt) {
                check_key(lo + (j << g_c) - 1, nrank);
                check_key(lo + (j << g_c), nrank);
              }
            }
            check_key(lo + rnd() % g_kpt, nrank);
          }
          for (uint64_t k = base + g_kpt * threads; k < base + g_S; k++) {
            check_key(k, nrank); /* slack: never minted */
          }
          if (base > 0) {
            check_key(base - 1, nrank);
          }
        }
        for (uint64_t k = g_S * nrank, n = 0; k <= top && n < 64; k++, n++) {
          check_key(k, nrank); /* the top of the key space */
        }
        uint64_t budget = (ARTS_GUID_DB_SEQ_MASK + 1) / nrank;
        for (uint64_t h = 0; h < 1024; h++) {
          if (!(h < 3 || h == 31 || h == 32 || h == 127 || h == 128 ||
                h >= 1022 || h % 37 == 0)) {
            continue;
          }
          for (unsigned i = 0; i < nr; i++) {
            uint64_t s0 = budget * ranks[i];
            check_key(ARTS_GUID_DB_KEY(h, s0 + 1), nrank);
            check_key(ARTS_GUID_DB_KEY(h, s0 + 1 + rnd() % budget), nrank);
            check_key(ARTS_GUID_DB_KEY(h, s0 + budget - 1), nrank);
          }
          check_key(ARTS_GUID_DB_KEY(h, budget * nrank), nrank);
          check_key(ARTS_GUID_DB_KEY(h, ARTS_GUID_DB_SEQ_MASK), nrank);
        }
      }
    }
  }
}

/* ── 2. stagger ─────────────────────────────────────────────────────────── */

static void item_stagger(void) {
  static const unsigned geo[][3] = {
      {16382, 20, 4}, {16382, 20, 14}, {4240, 63, 16}, {8, 11, 4}, {3, 5, 1}};
  for (unsigned gi = 0; gi < sizeof(geo) / sizeof(geo[0]); gi++) {
    unsigned nrank = geo[gi][0], threads = geo[gi][1], size = geo[gi][2];
    geometry(nrank, 0, threads, size, 0);
    place_on_placeholders(size);
    unsigned ranks[16];
    unsigned nr = sample_ranks(nrank, ranks);
    unsigned shortcut_off = 0;
    for (unsigned i = 0; i < nr; i++) {
      uint64_t r = ranks[i];
      for (uint64_t t = 0; t < threads; t++) {
        uint64_t lo = g_S * r + g_kpt * t;
        for (uint64_t j = 0; j < 6 && ((j + 1) << g_c) <= g_kpt; j++) {
          unsigned want = (unsigned)((t + r + j) % threads);
          uint64_t first = lo + (j << g_c), last = lo + ((j + 1) << g_c) - 1;
          long a =
              arts_get_route_table(ARTS_GUID_MAKE(ARTS_GUID_EVENT, 0, first)) -
              g_ph;
          long b =
              arts_get_route_table(ARTS_GUID_MAKE(ARTS_GUID_EDT, 0, last)) -
              g_ph;
          CHECK(a == (long)want && b == (long)want,
                "2: N=%u T=%u size=%u rank %llu thread %llu chunk %llu took "
                "%ld/%ld, want %u\n",
                nrank, threads, size, (unsigned long long)r,
                (unsigned long long)t, (unsigned long long)j, a, b, want);
          uint64_t blk = first / g_kpt;
          uint64_t ck = (first - blk * g_kpt) >> g_c;
          shortcut_off += (blk + blk / threads + ck) % threads != want;
        }
      }
    }
    if (nrank > 1000) {
      CHECK(shortcut_off > 0,
            "2: N=%u T=%u size=%u does not separate the exact block from "
            "key / kpt\n",
            nrank, threads, size);
    }
  }
}

/* ── 3. thread agreement ────────────────────────────────────────────────── */

#define AGREE_KEYS 65536u
#define AGREE_THREADS 8u
static arts_guid_t g_agree_keys[AGREE_KEYS];
static unsigned char g_agree_got[AGREE_THREADS][AGREE_KEYS];

static void *agree_worker(void *vp) {
  unsigned id = (unsigned)(uintptr_t)vp;
  arts_thread_info.thread_id = id;
  for (unsigned i = 0; i < AGREE_KEYS; i++) {
    g_agree_got[id][i] =
        (unsigned char)(arts_get_route_table(g_agree_keys[i]) - g_ph);
  }
  return NULL;
}

static void item_threads(void) {
  geometry(3, 0, 8, 4, 0);
  place_on_placeholders(4);
  for (unsigned i = 0; i < AGREE_KEYS; i++) {
    uint64_t r = i % 3, t = (i / 3) % 8, v = 1 + (i / 24) * 5 + rnd() % 3;
    g_agree_keys[i] = ARTS_GUID_MAKE(1 + i % 3, 0, g_S * r + g_kpt * t + v);
  }
  pthread_t tid[AGREE_THREADS];
  for (unsigned id = 0; id < AGREE_THREADS; id++) {
    pthread_create(&tid[id], NULL, agree_worker, (void *)(uintptr_t)id);
  }
  for (unsigned id = 0; id < AGREE_THREADS; id++) {
    pthread_join(tid[id], NULL);
  }
  for (unsigned id = 0; id < AGREE_THREADS; id++) {
    for (unsigned i = 0; i < AGREE_KEYS; i++) {
      unsigned want = oracle(g_agree_keys[i]);
      CHECK(g_agree_got[id][i] == want,
            "3: thread %u took table %u for key %u, want %u\n", id,
            g_agree_got[id][i], i, want);
    }
  }
}

/* ── 4. rank-id independence ────────────────────────────────────────────── */

static void item_rank_ids(void) {
  static const unsigned rank_ids[2] = {0, 3};
  uint64_t bases[2];
  for (unsigned ri = 0; ri < 2; ri++) {
    geometry(4, rank_ids[ri], 5, 10, 0);
    place_on_placeholders(10);
    bases[ri] = arts_guid_flat_base;
    for (uint64_t r = 0; r < 4; r++) {
      for (uint64_t t = 0; t < 5; t++) {
        for (uint64_t j = 0; j < 8; j++) {
          uint64_t key = g_S * r + g_kpt * t + (j << g_c) + 1 + j;
          arts_guid_t g = ARTS_GUID_MAKE(ARTS_GUID_EDT, r, key);
          long got = arts_get_route_table(g) - g_ph;
          CHECK(got == (long)oracle(g),
                "4: rank id %u took table %ld for rank %llu thread %llu, "
                "want %u\n",
                rank_ids[ri], got, (unsigned long long)r, (unsigned long long)t,
                oracle(g));
        }
      }
    }
  }
  CHECK(bases[0] != bases[1], "4: the two rank ids share a flat base\n");
}

/* ── 5. entry-point agreement ───────────────────────────────────────────── */

static int table_of_slot(const arts_route_item_t *s) {
  int found = -1, hits = 0;
  for (unsigned i = 0; i < g_T; i++) {
    for (arts_route_table_t *t = arts_node_info.route_table[i]; t != NULL;
         t = atomic_load_explicit(&t->next, memory_order_acquire)) {
      if (s >= t->data &&
          s < t->data + (uint64_t)t->size * COLLISION_RESOLVES) {
        found = (int)i;
        hits++;
      }
    }
  }
  return hits == 1 ? found : -1;
}

typedef void (*thread_fn_t)(void *);
typedef struct {
  unsigned id;
  thread_fn_t fn;
  void *arg;
} run_ctx_t;

static void *run_trampoline(void *vp) {
  run_ctx_t *c = (run_ctx_t *)vp;
  arts_thread_info.thread_id = c->id;
  c->fn(c->arg);
  return NULL;
}

/* Runs fn(arg) on a thread of its own, as runtime thread `id`. */
static void run_on(unsigned id, thread_fn_t fn, void *arg) {
  run_ctx_t c = {id, fn, arg};
  pthread_t tid;
  pthread_create(&tid, NULL, run_trampoline, &c);
  pthread_join(tid, NULL);
}

typedef struct {
  arts_guid_t key;
  arts_shared_ptr_t cb; /* the installer's own reference */
  arts_route_item_t *slot;
  int ok;
} ep_t;

static int g_objs[16];

static void ep_install_fresh(void *vp) {
  ep_t *e = (ep_t *)vp;
  arts_shared_ptr_t cb = arts_route_table_make_handle(&g_objs[0], e->key);
  e->cb = arts_shared_copy(cb);
  e->ok = arts_route_table_install_handle_fresh(cb, e->key);
}

static void ep_install(void *vp) {
  ep_t *e = (ep_t *)vp;
  arts_shared_ptr_t cb = arts_route_table_make_handle(&g_objs[1], e->key);
  e->cb = arts_shared_copy(cb);
  e->ok = arts_route_table_install_handle_if_absent(cb, e->key);
}

static arts_shared_ptr_t typed_lookup(arts_guid_t g) {
  switch (arts_guid_get_kind(g)) {
  case ARTS_GUID_EVENT:
    return arts_route_table_lookup_event(g);
  case ARTS_GUID_EDT:
    return arts_route_table_lookup_edt(g);
  default:
    return arts_route_table_lookup_db(g);
  }
}

/* Thread b: lookup, typed lookup and install_or_get all find k1's object. */
static void ep_find(void *vp) {
  ep_t *e = (ep_t *)vp;
  arts_shared_ptr_t h = arts_route_table_lookup(e->key);
  arts_shared_ptr_t ht = typed_lookup(e->key);
  arts_route_item_t *slot = NULL;
  arts_route_table_reserve_or_lookup(e->key, &slot);
  arts_shared_ptr_t other = arts_route_table_make_handle(&g_objs[2], e->key);
  arts_shared_ptr_t hg = arts_route_table_install_or_get(slot, other, e->key);
  e->ok = h == e->cb && ht == e->cb && hg == e->cb;
  e->slot = slot;
  arts_shared_abandon(&other);
  arts_shared_release(&h);
  arts_shared_release(&ht);
  arts_shared_release(&hg);
}

static arts_guid_t g_hook_key;
static arts_shared_ptr_t g_hook_cb;
static int g_hook_home_null, g_hook_retired;

static void ep_retire_homeless(void *vp) {
  (void)vp;
  g_hook_home_null = arts_shared_home(g_hook_cb) == NULL;
  g_hook_retired = arts_route_table_set_destroyed_if(g_hook_key, g_hook_cb);
}

static void after_install_cas(unsigned long key) {
  if (g_hook_key != 0 && key == (unsigned long)g_hook_key) {
    /* The installed cb: the slot holds it, its home is not stored yet. */
    arts_route_item_t *slot = NULL;
    arts_route_table_reserve_or_lookup(g_hook_key, &slot);
    g_hook_cb = arts_shared_slot_load(&slot->value);
    run_on(1, ep_retire_homeless, NULL);
    arts_shared_release(&g_hook_cb);
    g_hook_key = 0;
  }
}

typedef struct {
  arts_lf_link_t link;
} park_node_t;
static park_node_t g_park;

static void ep_park(void *vp) {
  ep_t *e = (ep_t *)vp;
  arts_route_table_reserve_or_lookup(e->key, &e->slot);
  arts_lf_stack_push(&e->slot->ooo_list, &g_park.link);
}

static void ep_retire(void *vp) {
  ep_t *e = (ep_t *)vp;
  e->ok = arts_route_table_set_destroyed_if(e->key, e->cb);
  arts_shared_release(&e->cb);
}

static void item_entry_points(void) {
  geometry(2, 0, 4, 6, 1);
  arts_route_table_node_init(1u << 6, 6);
  arts_thread_info.thread_id = 0;
  arts_guid_t own = arts_guid_create_for_rank(0, ARTS_GUID_EVENT);
  uint64_t budget = (ARTS_GUID_DB_SEQ_MASK + 1) / 2;
  const arts_guid_t classes[3] = {
      own, ARTS_GUID_MAKE(ARTS_GUID_EDT, 0, g_S + 2 * g_kpt + 7),
      ARTS_GUID_MAKE(ARTS_GUID_DB, 0, ARTS_GUID_DB_KEY(37, budget + 5))};
  for (unsigned ci = 0; ci < 3; ci++) {
    ep_t k1 = {classes[ci], NULL, NULL, 0};
    ep_t k2 = {classes[ci] + 1, NULL, NULL, 0};
    ep_t k3 = {classes[ci] + 2, NULL, NULL, 0};

    run_on(0, ep_install_fresh, &k1);
    CHECK(k1.ok, "5: class %u: the fresh install lost\n", ci);
    ep_t f = k1;
    run_on(1, ep_find, &f);
    CHECK(f.ok, "5: class %u: another thread did not find the object\n", ci);
    CHECK(f.slot == arts_shared_home(k1.cb) &&
              table_of_slot(f.slot) == (int)oracle(k1.key),
          "5: class %u: k1's slot is in table %d, want %u only\n", ci,
          table_of_slot(f.slot), oracle(k1.key));

    g_hook_key = k2.key;
    g_hook_home_null = g_hook_retired = 0;
    run_on(0, ep_install, &k2);
    CHECK(k2.ok && g_hook_home_null && g_hook_retired,
          "5: class %u: install %d, home unset %d, home-less retire %d\n", ci,
          k2.ok, g_hook_home_null, g_hook_retired);
    CHECK(table_of_slot(arts_shared_home(k2.cb)) == (int)oracle(k2.key),
          "5: class %u: k2's slot is in table %d, want %u only\n", ci,
          table_of_slot(arts_shared_home(k2.cb)), oracle(k2.key));
    arts_shared_release(&k2.cb);

    g_drain_slot = NULL;
    g_drained = NULL;
    run_on(2, ep_park, &k3);
    run_on(0, ep_install, &k3);
    CHECK(k3.ok && g_drained == &g_park.link && g_drain_slot == k3.slot &&
              arts_shared_home(k3.cb) == k3.slot,
          "5: class %u: the drain did not take the park on its slot\n", ci);
    CHECK(table_of_slot(k3.slot) == (int)oracle(k3.key),
          "5: class %u: k3's slot is in table %d, want %u only\n", ci,
          table_of_slot(k3.slot), oracle(k3.key));

    run_on(3, ep_retire, &k1);
    run_on(3, ep_retire, &k3);
    CHECK(k1.ok && k3.ok, "5: class %u: a retire failed\n", ci);
  }
  arts_route_table_node_cleanup();
}

/* ── 6. reciprocal division ─────────────────────────────────────────────── */

static unsigned long g_div_bad;
static void div_check(uint64_t n, uint64_t d) {
  uint64_t mul;
  uint8_t shift;
  route_div_make(d, &mul, &shift);
  if (route_div(n, mul, shift) != n / d && g_div_bad++ < 10) {
    (void)fprintf(stderr,
                  "FAIL route_table_placement: 6: %llu / %llu gave %llu\n",
                  (unsigned long long)n, (unsigned long long)d,
                  (unsigned long long)route_div(n, mul, shift));
  }
}

static void div_sweep(uint64_t d, uint64_t nmax) {
  uint64_t kmax = nmax / d;
  div_check(0, d);
  div_check(nmax, d);
  for (uint64_t k = 1; k <= 4 && k <= kmax; k++) {
    div_check(k * d - 1, d);
    div_check(k * d, d);
  }
  for (uint64_t k = kmax > 4 ? kmax - 4 : 1; k <= kmax; k++) {
    div_check(k * d - 1, d);
    div_check(k * d, d);
  }
  for (int i = 0; i < 4; i++) {
    div_check(rnd() % (nmax + 1), d);
  }
}

static void item_reciprocal(void) {
  const uint64_t n49 = ((uint64_t)1 << 49) - 1;
  for (uint64_t t = 1; t <= 65536; t++) {
    div_sweep(t, n49);
  }
  static const uint64_t ts[] = {1, 3, 20, 64};
  for (uint64_t nrank = 1; nrank <= 16382; nrank++) {
    uint64_t slice = ((uint64_t)1 << 48) / nrank;
    for (unsigned i = 0; i < sizeof(ts) / sizeof(ts[0]); i++) {
      uint64_t kpt = slice / ts[i];
      div_check(slice - 1, kpt);
      div_check(rnd() & (((uint64_t)1 << 48) - 1), kpt);
      div_check(rnd() & (((uint64_t)1 << 48) - 1), slice);
    }
  }
  for (int i = 0; i < 20000; i++) {
    uint64_t d = 1 + (rnd() >> (1 + rnd() % 63));
    div_sweep(d, ((uint64_t)1 << 62) - 1);
  }
  CHECK(g_div_bad == 0, "6: %lu reciprocal divisions were wrong\n", g_div_bad);
}

/* ── 7. a large single-source set spreads ───────────────────────────────── */

static unsigned keyed_slots(const arts_route_table_t *t) {
  unsigned n = 0;
  for (uint64_t i = 0; i < (uint64_t)t->size * COLLISION_RESOLVES; i++) {
    arts_guid_t k = atomic_load_explicit(&t->data[i].key, memory_order_relaxed);
    n += k != 0;
  }
  return n;
}

static void item_spread(void) {
  enum { T = 8, SIZE = 7, CHUNK = 1 << (SIZE + 2) };
  geometry(1, 0, T, SIZE, 1);
  arts_route_table_node_init(1u << SIZE, SIZE);
  arts_thread_info.thread_id = 0;
  *arts_guid_generator_get_key(0, ARTS_GUID_EVENT) = CHUNK;
  arts_guid_t range = arts_guid_reserve_range(ARTS_GUID_EVENT, T * CHUNK, 0);
  for (unsigned i = 0; i < T * CHUNK; i++) {
    arts_route_item_t *slot = NULL;
    arts_guid_t g = arts_guid_from_index(range, i);
    arts_route_table_reserve_or_lookup(g, &slot);
    unsigned want = (1 + i / CHUNK) % T;
    CHECK(table_of_slot(slot) == (int)want && oracle(g) == want,
          "7: member %u is in table %d, want %u\n", i, table_of_slot(slot),
          want);
  }
  for (unsigned i = 0; i < T; i++) {
    arts_route_table_t *t = arts_node_info.route_table[i];
    CHECK(atomic_load_explicit(&t->next, memory_order_acquire) == NULL &&
              keyed_slots(t) == CHUNK,
          "7: table %u holds %u keys over %s segment\n", i, keyed_slots(t),
          atomic_load_explicit(&t->next, memory_order_acquire) ? "a grown"
                                                               : "one");
  }
  /* Thread 3's single mints from a chunk edge: tables 3 + 1 .. 3 + 4. */
  arts_thread_info.thread_id = 3;
  *arts_guid_generator_get_key(0, ARTS_GUID_EDT) = CHUNK;
  unsigned per_table[T] = {0};
  for (unsigned i = 0; i < 4 * CHUNK; i++) {
    arts_route_table_t *got =
        arts_get_route_table(arts_guid_create_for_rank(0, ARTS_GUID_EDT));
    for (unsigned t = 0; t < T; t++) {
      per_table[t] += got == arts_node_info.route_table[t];
    }
  }
  for (unsigned t = 0; t < T; t++) {
    unsigned want = (t >= 4) ? CHUNK : 0;
    CHECK(per_table[t] == want, "7: table %u took %u single mints, want %u\n",
          t, per_table[t], want);
  }
  arts_thread_info.thread_id = 0;
  arts_route_table_node_cleanup();
}

/* ── 8. node init and cleanup ───────────────────────────────────────────── */

static void item_node_init(void) {
  geometry(2, 1, 5, 9, 0);
  arts_route_table_node_init(1u << 9, 9);
  for (unsigned i = 0; i < 5; i++) {
    arts_route_table_t *t = arts_node_info.route_table[i];
    CHECK(t != NULL && t->size == (1u << 9) && t->shift == 9,
          "8: table %u was not built as a root of 2^9 windows\n", i);
  }
  arts_route_table_node_cleanup();
  CHECK(arts_node_info.route_table == NULL && route_place.tables == 0 &&
            route_place.slice_mul == 0,
        "8: node cleanup left the placement set\n");
}

#ifndef NDEBUG
/* A placement before node init stops on its assert: run first, in a child
 * forked before any node init or thread. */
static int assert_before_node_init(void) {
  int fds[2];
  if (pipe(fds) != 0) {
    perror("pipe");
    return 0;
  }
  pid_t pid = fork();
  if (pid == 0) {
    struct rlimit no_core = {0, 0};
    (void)setrlimit(RLIMIT_CORE, &no_core);
    close(fds[0]);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    (void)arts_get_route_table(ARTS_GUID_MAKE(ARTS_GUID_EVENT, 0, 5));
    _exit(0);
  }
  close(fds[1]);
  char out[4096];
  size_t len = 0;
  ssize_t n;
  while ((n = read(fds[0], out + len, sizeof(out) - 1 - len)) > 0) {
    len += (size_t)n;
    if (len == sizeof(out) - 1) {
      break;
    }
  }
  out[len] = '\0';
  close(fds[0]);
  int status = 0;
  if (pid < 0 || waitpid(pid, &status, 0) != pid) {
    return 0;
  }
  int ok = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT &&
           strstr(out, "i < route_place.tables") != NULL;
  if (!ok) {
    (void)fprintf(stderr,
                  "FAIL route_table_placement: 8: a placement before node "
                  "init ended with status %#x (child stderr: %s)\n",
                  status, out);
  }
  return ok;
}
#endif

int main(void) {
#ifndef NDEBUG
  if (!assert_before_node_init()) {
    return 1;
  }
#endif
  for (unsigned i = 0; i < MAXT; i++) {
    g_ph_slots[i] = &g_ph[i];
  }
  item_formula();
  item_stagger();
  item_threads();
  item_rank_ids();
  item_entry_points();
  item_reciprocal();
  item_spread();
  item_node_init();
  free_keys();
  arts_guid_generator_cleanup();
  if (g_failures != 0) {
    (void)fprintf(stderr, "route_table_placement: %lu failures\n", g_failures);
    return 1;
  }
  printf("PASS route_table_placement: the oracle's table for every key "
         "class, the stagger, thread and rank-id agreement, entry-point "
         "agreement, exact reciprocals, a spread single-source set, node "
         "init and cleanup\n");
  return 0;
}
