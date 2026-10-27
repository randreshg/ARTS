/* SPDX-License-Identifier: Apache-2.0
 *
 * route_table_claim_assist — reservations of one key decide among their
 * claims without waiting on each other.
 *
 * A claim is a versioned CLAIM word in a slot of the key's probe window; the
 * earliest live claim in probe order wins, a later one is killed, and a
 * reserver that finds an earlier one helps it to publication.  Every case is
 * driven deterministically through route_table.c's hooks: a reserver is held
 * just after its claim's fence (AFTER_CLAIM), a helper between its read of
 * the claim it helps and the help (BEFORE_HELP), a retire before and after
 * its claim of RETIRING.  Each case asserts the slots' final words, that one
 * slot publishes the key and that no claim word is left.
 *   (a) A claims a later slot and is held; B claims an earlier slot, kills
 *       A's claim and publishes; A resumes, frees its slot and returns B's.
 *   (b) A claims the earlier slot and is held; B claims a later one, helps
 *       A's claim to publication and returns it while A is still held (the
 *       no-wait witness); A resumes and returns the same slot.
 *   (c) Three claimers: the earliest kills both later ones; and, all three
 *       held, the latest helps the earliest and kills the middle one.
 *   (d) A helper held after reading a claim while that claim is killed,
 *       freed and its slot claimed again for another key: the helper's
 *       publish CAS names the old version and fails.
 *   (e) The key a claim is reserving is published by a helper, installed,
 *       retired and its slot published by another key while the claim's
 *       owner is held: the owner's CASes fail, it writes nothing into the
 *       other key's slot and reserves the key anew.
 *   (e') As (e), but a duplicate retire holds the slot RETIRING across the
 *       owner's release and hands the key back: the owner waits that window
 *       out and resolves to the slot holding the key's next generation.
 *   (f) A killed claim's LOST word is taken by a claimer of another key
 *       before its owner frees it: the owner's free CAS fails and the other
 *       key's publication stays.
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
#include <time.h>
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
#ifdef ARTS_DEBUG_INJECTION
/* route_table.c's other windows are scheduling-fuzz points there. */
void arts_sched_fuzz_point(void) {}
#endif

struct arts_route_item_s;
void arts_ooo_drain(struct arts_route_item_s *s) { (void)s; }
void arts_ooo_redrive_all(struct arts_route_item_s *s) { (void)s; }
void arts_ooo_free_all(struct arts_route_item_s *s) { (void)s; }

/* A one-shot hold: the thread that reaches a hooked point with its gate set
 * announces itself and waits until the test opens the gate. */
typedef struct {
  _Atomic(int) arrived;
  _Atomic(int) go;
} gate_t;

static __thread gate_t *t_after_claim;
static __thread gate_t *t_before_help;
static __thread gate_t *t_before_retire;
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
#define ROUTE_TABLE_BEFORE_HELP(key) ((void)(key), hook_hold(&t_before_help))
#define ROUTE_TABLE_BEFORE_RETIRE_CLAIM(key)                                   \
  ((void)(key), hook_hold(&t_before_retire))
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
      (void)fprintf(stderr, "FAIL route_table_claim_assist: " __VA_ARGS__);    \
      g_failed = 1;                                                            \
    }                                                                          \
  } while (0)

static void gate_init(gate_t *g) {
  atomic_init(&g->arrived, 0);
  atomic_init(&g->go, 0);
}

/* Waits until a thread holds at `g`; 0 if none does within the bound. */
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

static void pause_ms(long ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

/* ── one table per case ─────────────────────────────────────────────────── */

#define SHIFT 10u
static arts_route_table_t *g_table;

/* Node init of a one-thread rank: every DB GUID of this rank's slice lives in
 * route_table[0]. */
static void rt_init(void) {
  arts_thread_info.thread_id = 0;
  arts_node_info.total_thread_count = 1;
  arts_guid_node_geometry_init();
  arts_node_info.route_table =
      (arts_route_table_t **)calloc(1, sizeof(arts_route_table_t *));
  for (int s = 0; s < ARTS_REMOTE_ROUTE_SHARDS; s++) {
    arts_node_info.remote_route_table[s] = arts_new_route_table(64, 6);
  }
}

static void case_begin(void) {
  g_table = arts_new_route_table(1u << SHIFT, SHIFT);
  arts_node_info.route_table[0] = g_table;
}

static void case_end(void) {
  arts_delete_route_table(g_table);
  arts_node_info.route_table[0] = NULL;
}

static arts_guid_t db_guid(uint64_t seq) {
  return ARTS_GUID_MAKE(ARTS_GUID_DB, 0, seq);
}

static uint64_t g_next_seq = 1000;

/* A key whose window no earlier case used, and `n` more keys sharing its
 * window. */
static arts_guid_t pick_keys(arts_guid_t *others, int n) {
  arts_guid_t k = db_guid(g_next_seq++);
  uint64_t w = get_route_table_key((uint64_t)k, SHIFT);
  int found = 0;
  for (uint64_t seq = g_next_seq; found < n; seq++) {
    arts_guid_t g = db_guid(seq);
    if (get_route_table_key((uint64_t)g, SHIFT) == w) {
      others[found++] = g;
    }
  }
  g_next_seq += 1u << 20; /* past every key drawn, so windows stay apart */
  return k;
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

/* Slots of k's window publishing `key`, and claim words in the window. */
static unsigned published(arts_guid_t k, arts_guid_t key) {
  unsigned n = 0;
  for (int i = 0; i < COLLISION_RESOLVES; i++) {
    n += word(window(k) + i) == key;
  }
  return n;
}

static unsigned claim_words(arts_guid_t k) {
  unsigned n = 0;
  for (int i = 0; i < COLLISION_RESOLVES; i++) {
    arts_guid_t w = word(window(k) + i);
    n += w == ROUTE_KEY_BUSY || ROUTE_KEY_IS_CLAIM(w) || ROUTE_KEY_IS_LOST(w);
  }
  return n;
}

/* ── a reserver on its own thread ──────────────────────────────────────── */

typedef struct {
  arts_guid_t key;
  gate_t *after_claim;
  gate_t *before_help;
  arts_route_item_t *slot;
  _Atomic(int) done;
  pthread_t tid;
} reserver_t;

static void *reserve_body(void *vp) {
  reserver_t *r = (reserver_t *)vp;
  t_after_claim = r->after_claim;
  t_before_help = r->before_help;
  arts_route_table_reserve_or_lookup(r->key, &r->slot);
  atomic_store_explicit(&r->done, 1, memory_order_release);
  return NULL;
}

static void reserver_start(reserver_t *r, arts_guid_t key, gate_t *after_claim,
                           gate_t *before_help) {
  r->key = key;
  r->after_claim = after_claim;
  r->before_help = before_help;
  r->slot = NULL;
  atomic_init(&r->done, 0);
  if (pthread_create(&r->tid, NULL, reserve_body, r) != 0) {
    (void)fprintf(stderr, "FAIL route_table_claim_assist: pthread_create\n");
    exit(1);
  }
}

static int is_done(reserver_t *r) {
  return atomic_load_explicit(&r->done, memory_order_acquire);
}

static arts_route_item_t *reserve_here(arts_guid_t key) {
  arts_route_item_t *s = NULL;
  arts_route_table_reserve_or_lookup(key, &s);
  return s;
}

/* A stranger GUID that holds a slot, and a retire's return of it. */
static arts_guid_t stranger(int i) {
  return db_guid((1ull << 37) + (uint64_t)i);
}

/* ── (a) an earlier claim kills a held later one ───────────────────────── */

static void case_a(void) {
  case_begin();
  arts_guid_t unused[1];
  arts_guid_t k = pick_keys(unused, 0);
  arts_route_item_t *s0 = window(k), *s1 = s0 + 1;
  put(s0, stranger(0));
  gate_t ga;
  gate_init(&ga);
  reserver_t a;
  reserver_start(&a, k, &ga, NULL);
  CHECK(gate_await(&ga), "a: A never claimed\n");
  CHECK(ROUTE_KEY_IS_CLAIM(word(s1)),
        "a: A's claim is not in the later slot\n");
  put(s0, 0); /* the stranger is retired and its slot returned */
  arts_route_item_t *b = reserve_here(k);
  CHECK(b == s0 && word(s0) == k, "a: B did not publish the earlier slot\n");
  CHECK(ROUTE_KEY_IS_LOST(word(s1)), "a: B did not kill A's later claim\n");
  gate_open(&ga);
  pthread_join(a.tid, NULL);
  CHECK(a.slot == s0, "a: A did not resolve to B's slot\n");
  CHECK(word(s1) == 0, "a: A did not free its lost claim\n");
  CHECK(published(k, k) == 1 && claim_words(k) == 0,
        "a: %u slots publish the key, %u claim words left\n", published(k, k),
        claim_words(k));
  case_end();
}

/* ── (b) a later claimer helps a held earlier claim ────────────────────── */

static void case_b(void) {
  case_begin();
  arts_guid_t unused[1];
  arts_guid_t k = pick_keys(unused, 0);
  arts_route_item_t *s0 = window(k), *s1 = s0 + 1;
  gate_t ga;
  gate_init(&ga);
  reserver_t a;
  reserver_start(&a, k, &ga, NULL);
  CHECK(gate_await(&ga), "b: A never claimed\n");
  CHECK(ROUTE_KEY_IS_CLAIM(word(s0)),
        "b: A's claim is not in the first slot\n");
  arts_route_item_t *b = reserve_here(k);
  CHECK(!is_done(&a), "b: A returned while held\n");
  CHECK(b == s0 && word(s0) == k,
        "b: B did not publish A's claim while A was held\n");
  CHECK(word(s1) == 0, "b: B did not free its own lost claim\n");
  gate_open(&ga);
  pthread_join(a.tid, NULL);
  CHECK(a.slot == s0, "b: A did not resolve to its own published slot\n");
  CHECK(published(k, k) == 1 && claim_words(k) == 0,
        "b: %u slots publish the key, %u claim words left\n", published(k, k),
        claim_words(k));
  case_end();
}

/* ── (c) three claimers ────────────────────────────────────────────────── */

static void case_c(void) {
  /* The earliest kills both later ones. */
  case_begin();
  arts_guid_t unused[1];
  arts_guid_t k = pick_keys(unused, 0);
  arts_route_item_t *s0 = window(k), *s1 = s0 + 1, *s2 = s0 + 2;
  put(s0, stranger(0));
  put(s1, stranger(1));
  gate_t ga, gb;
  gate_init(&ga);
  gate_init(&gb);
  reserver_t a, b;
  reserver_start(&a, k, &ga, NULL);
  CHECK(gate_await(&ga) && ROUTE_KEY_IS_CLAIM(word(s2)),
        "c: A did not claim the third slot\n");
  put(s1, 0);
  reserver_start(&b, k, &gb, NULL);
  CHECK(gate_await(&gb) && ROUTE_KEY_IS_CLAIM(word(s1)),
        "c: B did not claim the second slot\n");
  put(s0, 0);
  arts_route_item_t *c = reserve_here(k);
  CHECK(c == s0 && word(s0) == k, "c: C did not publish the first slot\n");
  CHECK(ROUTE_KEY_IS_LOST(word(s1)) && ROUTE_KEY_IS_LOST(word(s2)),
        "c: C did not kill both later claims\n");
  gate_open(&gb);
  pthread_join(b.tid, NULL);
  gate_open(&ga);
  pthread_join(a.tid, NULL);
  CHECK(a.slot == s0 && b.slot == s0,
        "c: A or B did not resolve to C's slot\n");
  CHECK(published(k, k) == 1 && claim_words(k) == 0,
        "c: %u slots publish the key, %u claim words left\n", published(k, k),
        claim_words(k));
  case_end();

  /* All three held: the latest helps the earliest and kills the middle. */
  case_begin();
  k = pick_keys(unused, 0);
  s0 = window(k);
  s1 = s0 + 1;
  s2 = s0 + 2;
  put(s0, stranger(0));
  put(s1, stranger(1));
  gate_t g3[3];
  reserver_t r[3];
  for (int i = 0; i < 3; i++) {
    gate_init(&g3[i]);
  }
  reserver_start(&r[2], k, &g3[2], NULL);
  CHECK(gate_await(&g3[2]) && ROUTE_KEY_IS_CLAIM(word(s2)),
        "c2: the latest claim is not in the third slot\n");
  put(s1, 0);
  reserver_start(&r[1], k, &g3[1], NULL);
  CHECK(gate_await(&g3[1]) && ROUTE_KEY_IS_CLAIM(word(s1)),
        "c2: the middle claim is not in the second slot\n");
  put(s0, 0);
  reserver_start(&r[0], k, &g3[0], NULL);
  CHECK(gate_await(&g3[0]) && ROUTE_KEY_IS_CLAIM(word(s0)),
        "c2: the earliest claim is not in the first slot\n");
  gate_open(&g3[2]);
  pthread_join(r[2].tid, NULL);
  CHECK(!is_done(&r[0]) && !is_done(&r[1]), "c2: a held claimer returned\n");
  CHECK(r[2].slot == s0 && word(s0) == k,
        "c2: the latest did not publish the earliest claim\n");
  CHECK(ROUTE_KEY_IS_LOST(word(s1)) && word(s2) == 0,
        "c2: the middle claim was not killed or the latest's not freed\n");
  gate_open(&g3[1]);
  pthread_join(r[1].tid, NULL);
  gate_open(&g3[0]);
  pthread_join(r[0].tid, NULL);
  CHECK(r[0].slot == s0 && r[1].slot == s0,
        "c2: the held claimers did not resolve to the earliest slot\n");
  CHECK(published(k, k) == 1 && claim_words(k) == 0,
        "c2: %u slots publish the key, %u claim words left\n", published(k, k),
        claim_words(k));
  case_end();
}

/* ── (d) a stale help names the old version ────────────────────────────── */

static void case_d(void) {
  case_begin();
  arts_guid_t other[1];
  arts_guid_t k = pick_keys(other, 1);
  arts_guid_t k2 = other[0];
  arts_route_item_t *s0 = window(k), *s1 = s0 + 1, *s2 = s0 + 2;
  put(s0, stranger(0));
  gate_t ga, gh, gd;
  gate_init(&ga);
  gate_init(&gh);
  gate_init(&gd);
  /* A claims the second slot, held. */
  reserver_t a, h, d;
  reserver_start(&a, k, &ga, NULL);
  CHECK(gate_await(&ga) && ROUTE_KEY_IS_CLAIM(word(s1)),
        "d: A's claim is not in the second slot\n");
  uint64_t va = ROUTE_KEY_VERSION(word(s1));
  /* H claims the third, reads A's claim and is held before helping it. */
  reserver_start(&h, k, NULL, &gh);
  CHECK(gate_await(&gh), "d: H never reached its help\n");
  /* C publishes the first slot and kills A's claim; A frees it. */
  put(s0, 0);
  arts_route_item_t *c = reserve_here(k);
  CHECK(c == s0 && ROUTE_KEY_IS_LOST(word(s1)),
        "d: C did not publish the first slot and kill A's claim\n");
  gate_open(&ga);
  pthread_join(a.tid, NULL);
  CHECK(a.slot == s0 && word(s1) == 0, "d: A did not free its lost claim\n");
  /* D claims A's old slot for another key, held: the slot holds a live
   * claim again, of a later version.  Then K's publication is returned. */
  reserver_start(&d, k2, &gd, NULL);
  CHECK(gate_await(&gd), "d: D never claimed\n");
  arts_guid_t dw = word(s1);
  CHECK(ROUTE_KEY_IS_CLAIM(dw) && ROUTE_KEY_VERSION(dw) == va + 1 &&
            atomic_load_explicit(&s1->claim_key, memory_order_relaxed) == k2,
        "d: D's claim is not version %lu of A's old slot\n",
        (unsigned long)(va + 1));
  put(s0, 0);
  /* H helps the claim it read: its CAS names version va and fails. */
  gate_open(&gh);
  pthread_join(h.tid, NULL);
  CHECK(word(s1) == dw, "d: the stale help wrote over D's claim\n");
  CHECK(h.slot == s0 && word(s0) == k,
        "d: H did not reserve the key anew in the free slot\n");
  CHECK(word(s2) == 0, "d: H did not free its own lost claim\n");
  gate_open(&gd);
  pthread_join(d.tid, NULL);
  CHECK(d.slot == s1 && word(s1) == k2, "d: D did not publish its claim\n");
  CHECK(published(k, k) == 1 && published(k, k2) == 1 && claim_words(k) == 0,
        "d: the keys are not published once each, or claim words are left\n");
  case_end();
}

/* ── (e) the reserved key is retired while its claim's owner is held ───── */

static int g_obj_k = 1, g_obj_c = 2, g_obj_k2 = 3;

static void case_e(void) {
  case_begin();
  arts_guid_t other[1];
  arts_guid_t k = pick_keys(other, 1);
  arts_guid_t kc = other[0];
  arts_route_item_t *s0 = window(k), *s1 = s0 + 1;
  gate_t ga;
  gate_init(&ga);
  reserver_t a;
  reserver_start(&a, k, &ga, NULL);
  CHECK(gate_await(&ga) && ROUTE_KEY_IS_CLAIM(word(s0)),
        "e: A's claim is not in the first slot\n");
  /* B helps A's claim to publication. */
  CHECK(reserve_here(k) == s0 && word(s0) == k, "e: B did not publish s\n");
  /* B installs K's object and retires it by identity: s is returned. */
  arts_shared_ptr_t cb = arts_route_table_install_if_absent(&g_obj_k, k);
  CHECK(cb != NULL, "e: K's object did not install\n");
  CHECK(arts_route_table_set_destroyed_if(k, cb), "e: K's retire failed\n");
  arts_shared_release(&cb);
  CHECK(word(s0) == 0, "e: K's retire did not return s\n");
  /* C, of the same window, publishes s and installs its object. */
  arts_shared_ptr_t cc = arts_route_table_install_if_absent(&g_obj_c, kc);
  CHECK(cc != NULL && word(s0) == kc, "e: C did not publish s\n");
  gate_open(&ga);
  pthread_join(a.tid, NULL);
  arts_shared_ptr_t hc = arts_route_table_lookup(kc);
  CHECK(word(s0) == kc && arts_shared_get(hc) == &g_obj_c,
        "e: C's slot was written by A\n");
  arts_shared_release(&hc);
  CHECK(a.slot != NULL && a.slot != s0 && word(a.slot) == k,
        "e: A does not end on a slot publishing K\n");
  CHECK(a.slot == s1, "e: A did not reserve K anew in the next free slot\n");
  CHECK(published(k, k) == 1 && published(k, kc) == 1 && claim_words(k) == 0,
        "e: the keys are not published once each, or claim words are left\n");
  CHECK(arts_route_table_set_destroyed_if(kc, cc), "e: C's retire failed\n");
  arts_shared_release(&cc);
  case_end();
}

/* (e') A duplicate retire holds s RETIRING across A's release. */

typedef struct {
  arts_guid_t key;
  arts_shared_ptr_t cb;
  gate_t *before, *after;
  int ret;
  pthread_t tid;
} retirer_t;

static void *retire_body(void *vp) {
  retirer_t *r = (retirer_t *)vp;
  t_before_retire = r->before;
  t_after_retire = r->after;
  r->ret = arts_route_table_set_destroyed_if(r->key, r->cb);
  return NULL;
}

static void case_e2(void) {
  case_begin();
  arts_guid_t unused[1];
  arts_guid_t k = pick_keys(unused, 0);
  arts_route_item_t *s0 = window(k);
  gate_t ga, gbefore, gafter;
  gate_init(&ga);
  gate_init(&gbefore);
  gate_init(&gafter);
  reserver_t a;
  reserver_start(&a, k, &ga, NULL);
  CHECK(gate_await(&ga) && ROUTE_KEY_IS_CLAIM(word(s0)),
        "e': A's claim is not in the first slot\n");
  /* B helps and publishes s, then installs object k1. */
  arts_shared_ptr_t k1 = arts_route_table_install_if_absent(&g_obj_k, k);
  CHECK(k1 != NULL && word(s0) == k, "e': k1 is not installed in s\n");
  /* R2, a duplicate retire of k1, peeks it and is held before its claim. */
  retirer_t r2 = {k, arts_shared_copy(k1), &gbefore, &gafter, -1, 0};
  pthread_create(&r2.tid, NULL, retire_body, &r2);
  CHECK(gate_await(&gbefore), "e': R2 never reached its claim\n");
  /* R1 retires k1 in full; K's next generation k2 is created into s. */
  CHECK(arts_route_table_set_destroyed_if(k, k1), "e': R1's retire failed\n");
  arts_shared_release(&k1);
  arts_shared_ptr_t k2 = arts_route_table_install_if_absent(&g_obj_k2, k);
  CHECK(k2 != NULL && word(s0) == k, "e': k2 is not installed in s\n");
  /* R2 claims RETIRING over k2's publication and is held there. */
  gate_open(&gbefore);
  CHECK(gate_await(&gafter) && word(s0) == ARTS_ROUTE_KEY_RETIRING,
        "e': R2 did not hold s RETIRING\n");
  /* A's CASes fail on RETIRING, and its new reservation waits the window
   * out. */
  gate_open(&ga);
  pause_ms(100);
  CHECK(!is_done(&a), "e': A did not wait out the RETIRING window\n");
  CHECK(word(s0) == ARTS_ROUTE_KEY_RETIRING, "e': A wrote into s\n");
  /* R2's value CAS fails: it hands the key back, and A resolves to s. */
  gate_open(&gafter);
  pthread_join(r2.tid, NULL);
  pthread_join(a.tid, NULL);
  arts_shared_release(&r2.cb);
  CHECK(r2.ret == 0, "e': the duplicate retire claimed a retire\n");
  arts_shared_ptr_t h = arts_route_table_lookup(k);
  CHECK(word(s0) == k && arts_shared_get(h) == &g_obj_k2,
        "e': s does not publish K with k2\n");
  arts_shared_release(&h);
  CHECK(a.slot == s0, "e': A did not resolve to s\n");
  CHECK(published(k, k) == 1 && claim_words(k) == 0,
        "e': %u slots publish the key, %u claim words left\n", published(k, k),
        claim_words(k));
  CHECK(arts_route_table_set_destroyed_if(k, k2), "e': k2's retire failed\n");
  arts_shared_release(&k2);
  case_end();
}

/* ── (f) a LOST word taken before its owner frees it ───────────────────── */

static void case_f(void) {
  case_begin();
  arts_guid_t other[1];
  arts_guid_t k = pick_keys(other, 1);
  arts_guid_t k2 = other[0];
  arts_route_item_t *s0 = window(k), *s1 = s0 + 1;
  put(s0, stranger(0));
  gate_t ga;
  gate_init(&ga);
  reserver_t a;
  reserver_start(&a, k, &ga, NULL);
  CHECK(gate_await(&ga) && ROUTE_KEY_IS_CLAIM(word(s1)),
        "f: A's claim is not in the second slot\n");
  uint64_t va = ROUTE_KEY_VERSION(word(s1));
  put(s0, 0);
  CHECK(reserve_here(k) == s0 && word(s1) == ROUTE_KEY_LOST(va),
        "f: B did not publish the first slot and kill A's claim\n");
  /* A claimer of another key takes A's LOST word and publishes there. */
  CHECK(reserve_here(k2) == s1 && word(s1) == k2,
        "f: the other key did not take A's lost slot\n");
  CHECK(s1->claim_ver == va + 1, "f: the take did not bump the version\n");
  gate_open(&ga);
  pthread_join(a.tid, NULL);
  CHECK(a.slot == s0, "f: A did not resolve to B's slot\n");
  CHECK(word(s1) == k2, "f: A's free wrote over the other key's slot\n");
  CHECK(published(k, k) == 1 && published(k, k2) == 1 && claim_words(k) == 0,
        "f: the keys are not published once each, or claim words are left\n");
  case_end();
}

int main(void) {
  rt_init();
  case_a();
  case_b();
  case_c();
  case_d();
  case_e();
  case_e2();
  case_f();
  for (int s = 0; s < ARTS_REMOTE_ROUTE_SHARDS; s++) {
    arts_delete_route_table(arts_node_info.remote_route_table[s]);
  }
  free(arts_node_info.route_table);
  arts_guid_generator_cleanup();
  if (g_failed) {
    return 1;
  }
  (void)printf("PASS route_table_claim_assist: a later claim was killed, an "
               "earlier one helped while its owner was held, a stale help "
               "failed, a retired key's owner wrote nothing, a RETIRING "
               "window was waited out, a taken LOST word stayed taken\n");
  return 0;
}
