/* SPDX-License-Identifier: Apache-2.0
 *
 * route_table_install_if_absent — the install primitive's promise and the
 * retire forms' single flight.
 *
 * arts_route_table_install_if_absent installs into an empty slot only and
 * hands the winner a ref it releases: of concurrent installs of one key
 * exactly ONE wins; a loser abandons its cb (the abandon must NOT run the
 * deleter — the loser keeps owning its object).  What a losing create does
 * next (the engine parks it) is not this primitive's.
 * Every retire form is single-flight (of concurrent retires of one object
 * exactly one returns true) and RETURNS the slot: the key is zeroed after
 * the cb is released, so the slot is claimable again.
 *
 * Scenarios:
 *
 *   A. install_if_absent storm on ONE empty slot: exactly one handle returned; the
 *      slot holds the winner's object; the deleter has NOT run for any loser
 *      object (losers keep theirs); winner freed exactly once at teardown.
 *
 *   B. retire single-flight: N threads race a retire (the object form) on a
 *      populated slot — exactly one returns true; the object's deleter runs
 *      exactly once; the slot's key is returned to 0.
 *
 *   C. create→destroy churn on one GUID: every round installs into the
 *      returned slot and one retire takes it; no double-free / no leak (ASan);
 *      the slot comes back every round.
 *
 *   D. an identity retire takes only its own generation.
 *
 *   E. a retire that pinned generation 1 and runs after generation 1 was
 *      retired and generation 2 installed takes nothing: the retire is bound
 *      to the object it saw.
 *
 *   F. a handle-less retire (the object form, pinning whatever the key
 *      currently publishes) that arrives while another retire of the
 *      previous generation holds the slot RETIRING and then hands the key
 *      back (it named generation 1, the slot holds generation 2) retires
 *      generation 2: it looks through the RETIRING slot instead of missing
 *      it.
 *
 *   G. a handle-less retire that arrives while a real retire of generation 1
 *      holds the slot RETIRING waits it out and returns false; generation 2,
 *      installed right after, stays.
 *
 * F and G park the retire inside set_destroyed_if through the file's
 * retire-claim hooks, so the interleavings are forced, not sampled.
 *
 * No runtime: route_table.c + guid.c + shared.c #include'd; OoO drain is a
 * no-op (no OoO payloads pushed).
 */

#include <pthread.h>
#include <stdatomic.h>
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

/* Wait-free counter primitives for the DB seq allocator (libc-free unit
 * pattern: mirror the atomics.c definitions verbatim). */
uint64_t arts_atomic_fetch_add_u64(volatile uint64_t *d, uint64_t v) {
  return __sync_fetch_and_add(d, v);
}
uint64_t arts_atomic_cswap_u64(volatile uint64_t *d, uint64_t o, uint64_t n) {
  return __sync_val_compare_and_swap(d, o, n);
}
uint64_t arts_atomic_read_u64(const volatile uint64_t *d) {
  return __atomic_load_n(d, __ATOMIC_ACQUIRE);
}

#include "../../libs/src/core/gas/guid.c"
/* Retire-claim hooks: a thread with a hook armed runs it in its identity
 * retire, before the key claim (BEFORE) or after a won claim (AFTER). */
static __thread void (*t_before_claim)(void);
static __thread void (*t_after_claim)(void);
static void hook_before_claim(void) {
  void (*f)(void) = t_before_claim;
  t_before_claim = NULL;
  if (f != NULL) {
    f();
  }
}
static void hook_after_claim(void) {
  void (*f)(void) = t_after_claim;
  t_after_claim = NULL;
  if (f != NULL) {
    f();
  }
}
#define ROUTE_TABLE_BEFORE_RETIRE_CLAIM(key) ((void)(key), hook_before_claim())
#define ROUTE_TABLE_AFTER_RETIRE_CLAIM(key) ((void)(key), hook_after_claim())

#include "../../libs/src/core/gas/route_table.c"
#include "../../libs/src/core/utils/shared.c"

struct arts_runtime_shared_s arts_node_info;
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;

static _Atomic int g_deletes;
static void test_deleter(void *obj) {
  atomic_fetch_add_explicit(&g_deletes, 1, memory_order_relaxed);
  free(obj);
}

#define FAIL(...)                                                              \
  do {                                                                         \
    (void)fprintf(stderr, "FAIL route_table_install_if_absent: " __VA_ARGS__); \
    return 1;                                                                  \
  } while (0)

static void rt_init(void) {
  arts_thread_info.thread_id = 0;
  arts_node_info.total_thread_count = 1;
  arts_node_info.gpu = 0;
  arts_node_info.keys = (uint64_t **)calloc(1, sizeof(uint64_t *));
  arts_node_info.keys[0] = (uint64_t *)calloc(
      (size_t)ARTS_GUID_LAST * arts_global_rank_count, sizeof(uint64_t));
  for (unsigned i = 0; i < ARTS_GUID_LAST * arts_global_rank_count; i++) {
    arts_node_info.keys[0][i] = 1;
  }
  arts_node_info.global_guid_thread_id =
      (uint64_t *)calloc(1, sizeof(uint64_t));
  num_tables = 1;
  min_global_guid_thread = 0;
  max_global_guid_thread = 1;
  keys_per_thread = 1u << 20;
  global_guid_on = 0;

  /* DB seq allocator (mirrors set_guid_generator_after_parallel_start). */
  arts_db_seq_budget =
      ((ARTS_GUID_DB_SEQ_MASK + 1) - ARTS_GUID_DB_STARTUP_RESERVE) /
      arts_global_rank_count;
  db_seq_creator_base = arts_db_seq_budget * arts_global_rank_id;
  if (db_seq_next) {
    free((void *)db_seq_next);
  }
  db_seq_next = (volatile uint64_t *)malloc(sizeof(uint64_t) *
                                            arts_global_rank_count);
  for (unsigned r = 0; r < arts_global_rank_count; r++) {
    db_seq_next[r] = db_seq_creator_base + 1;
  }
  free(t_db_cursor);
  t_db_cursor = NULL;
  arts_node_info.route_table =
      (arts_route_table_t **)calloc(1, sizeof(arts_route_table_t *));
  arts_node_info.route_table[0] = arts_new_route_table(1024, 10);
  for (int s = 0; s < ARTS_REMOTE_ROUTE_SHARDS; s++) {
    arts_node_info.remote_route_table[s] = arts_new_route_table(64, 10);
  }
}

/* ── Scenario A: install_if_absent storm ───────────────────────────────── */
#define A_THREADS 12

typedef struct {
  arts_guid_t guid;
  atomic_int *gate;
  int *obj; /* this thread's candidate object */
  int won;  /* did this thread win the CAS? */
} a_ctx_t;

static void *a_worker(void *vp) {
  a_ctx_t *c = (a_ctx_t *)vp;
  while (atomic_load_explicit(c->gate, memory_order_acquire) == 0) {
  }
  arts_shared_ptr_t h =
      arts_route_table_install_if_absent(c->obj, c->guid, 0, false);
  c->won = h != NULL ? 1 : 0;
  arts_shared_release(&h);
  return NULL;
}

/* ── Scenario B: set_destroyed single-flight ───────────────────────────── */
#define B_THREADS 12

typedef struct {
  arts_guid_t guid;
  void *obj;
  atomic_int *gate;
  int won;
} b_ctx_t;

static void *b_worker(void *vp) {
  b_ctx_t *c = (b_ctx_t *)vp;
  while (atomic_load_explicit(c->gate, memory_order_acquire) == 0) {
  }
  c->won = arts_route_table_set_destroyed_object(c->guid, c->obj) ? 1 : 0;
  return NULL;
}

/* ── Scenarios F and G: a handle-less retire meets a retire in flight ───── */
static arts_guid_t g_fg_guid;
static arts_shared_ptr_t g_fg_gen1;
static int *g_fg_obj2;
static atomic_int g_fg_parked;  /* the identity retire holds RETIRING */
static atomic_int g_fg_release; /* let it go on */

static void fg_install_gen2(void) {
  g_fg_obj2 = (int *)malloc(sizeof(int));
  arts_shared_ptr_t h =
      arts_route_table_install_if_absent(g_fg_obj2, g_fg_guid, 0, false);
  if (h == NULL) {
    (void)fprintf(stderr, "FAIL route_table_install_if_absent: F/G "
                          "generation 2 install lost\n");
    _exit(1);
  }
  arts_shared_release(&h);
}

/* F, before the claim: another retire takes generation 1 and generation 2
 * installs, so the parked retire's claim lands on generation 2's slot. */
static void f_before(void) {
  void (*after)(void) = t_after_claim;
  t_after_claim = NULL;
  bool inner = arts_route_table_set_destroyed_if(g_fg_guid, g_fg_gen1);
  t_after_claim = after;
  if (!inner) {
    (void)fprintf(stderr, "FAIL route_table_install_if_absent: F inner "
                          "retire of generation 1 failed\n");
    _exit(1);
  }
  fg_install_gen2();
}

static void park_after_claim(void) {
  atomic_store(&g_fg_parked, 1);
  while (atomic_load(&g_fg_release) == 0) {
    sched_yield();
  }
}

static void *fg_retire_worker(void *vp) {
  if (vp != NULL) {
    t_before_claim = f_before;
  }
  t_after_claim = park_after_claim;
  return (void *)(uintptr_t)arts_route_table_set_destroyed_if(g_fg_guid,
                                                               g_fg_gen1);
}

static void *fg_key_worker(void *vp) {
  return (void *)(uintptr_t)arts_route_table_set_destroyed_object(g_fg_guid,
                                                                  vp);
}

static int run_fg(bool hand_back) {
  const char *name = hand_back ? "F" : "G";
  g_fg_guid = arts_guid_reserve(ARTS_GUID_DB, 0);
  atomic_store(&g_fg_parked, 0);
  atomic_store(&g_fg_release, 0);
  atomic_store(&g_deletes, 0);
  int *o1 = (int *)malloc(sizeof(int));
  arts_shared_ptr_t h =
      arts_route_table_install_if_absent(o1, g_fg_guid, 0, false);
  arts_shared_release(&h);
  g_fg_gen1 = arts_route_table_lookup(g_fg_guid);

  pthread_t retire_tid, key_tid;
  if (pthread_create(&retire_tid, NULL, fg_retire_worker,
                     hand_back ? (void *)1 : NULL) != 0) {
    FAIL("%s: pthread_create\n", name);
  }
  for (int i = 0; atomic_load(&g_fg_parked) == 0; i++) {
    if (i > 5000) {
      FAIL("%s: the identity retire never held the slot RETIRING\n", name);
    }
    usleep(1000);
  }
  /* By the time the identity retire parks holding RETIRING, F has already
   * installed generation 2 (inside its before-claim hook) and G has not
   * (its install runs later, from this thread): the object a handle-less
   * retire will find through the RETIRING window is generation 2 for F,
   * generation 1 for G. */
  void *fg_key_expect = hand_back ? (void *)g_fg_obj2 : (void *)o1;
  if (pthread_create(&key_tid, NULL, fg_key_worker, fg_key_expect) != 0) {
    FAIL("%s: pthread_create\n", name);
  }
  usleep(100000);
  atomic_store(&g_fg_release, 1);
  void *rv = NULL;
  pthread_join(retire_tid, &rv);
  bool retire_won = rv != NULL;
  if (!hand_back) {
    fg_install_gen2();
  }
  pthread_join(key_tid, &rv);
  bool key_won = rv != NULL;
  arts_shared_release(&g_fg_gen1);

  arts_shared_ptr_t lh = arts_route_table_lookup(g_fg_guid);
  if (hand_back) {
    if (retire_won) {
      FAIL("F: the parked retire of generation 1 took generation 2\n");
    }
    if (!key_won || lh != NULL) {
      FAIL(
          "F: the handle-less retire missed generation 2 behind a "
          "hand-back\n");
    }
  } else {
    if (!retire_won) {
      FAIL("G: the retire of generation 1 failed\n");
    }
    if (key_won || lh == NULL || arts_shared_get(lh) != g_fg_obj2) {
      FAIL("G: the handle-less retire took generation 2\n");
    }
    arts_shared_release(&lh);
    if (!arts_route_table_set_destroyed_object(g_fg_guid, g_fg_obj2)) {
      FAIL("G: retire of generation 2 failed\n");
    }
  }
  if (atomic_load(&g_deletes) != 2) {
    FAIL("%s: %d frees for two generations\n", name,
         atomic_load(&g_deletes));
  }
  return 0;
}

int main(void) {
  rt_init();
  arts_route_table_register_deleter(ARTS_GUID_DB, test_deleter);

  /* ---- Scenario A ---- */
  {
    arts_guid_t g = arts_guid_reserve(ARTS_GUID_DB, 0);
    pthread_t tids[A_THREADS];
    a_ctx_t ctx[A_THREADS];
    atomic_int gate;
    atomic_init(&gate, 0);
    atomic_store(&g_deletes, 0);
    for (int i = 0; i < A_THREADS; i++) {
      ctx[i].guid = g;
      ctx[i].gate = &gate;
      ctx[i].obj = (int *)malloc(sizeof(int));
      *ctx[i].obj = i;
      ctx[i].won = 0;
      if (pthread_create(&tids[i], NULL, a_worker, &ctx[i]) != 0) {
        FAIL("A pthread_create %d\n", i);
      }
    }
    atomic_store_explicit(&gate, 1, memory_order_release);
    int winners = 0, win_idx = -1;
    for (int i = 0; i < A_THREADS; i++) {
      pthread_join(tids[i], NULL);
      if (ctx[i].won) {
        winners++;
        win_idx = i;
      }
    }
    if (winners != 1) {
      FAIL("A: %d winners, want 1\n", winners);
    }
    /* loser objects must NOT have been freed (abandon != delete). */
    if (atomic_load(&g_deletes) != 0) {
      FAIL("A: %d deletes after CAS race — abandon ran a deleter\n",
           atomic_load(&g_deletes));
    }
    /* slot holds the winner's object. */
    arts_shared_ptr_t h = arts_route_table_lookup(g);
    if (!h || arts_shared_get(h) != ctx[win_idx].obj) {
      FAIL("A: slot does not hold the winner's object\n");
    }
    arts_shared_release(&h);
    /* losers still own their objects → free them here (the test owns them). */
    for (int i = 0; i < A_THREADS; i++) {
      if (!ctx[i].won) {
        free(ctx[i].obj);
      }
    }
    /* destroy the winner cb → deleter frees winner exactly once. */
    if (!arts_route_table_set_destroyed_object(g, ctx[win_idx].obj)) {
      FAIL("A: set_destroyed on winner returned false\n");
    }
    if (atomic_load(&g_deletes) != 1) {
      FAIL("A: winner deleter ran %d times, want 1\n", atomic_load(&g_deletes));
    }
  }

  /* ---- Scenario B ---- */
  {
    arts_guid_t g = arts_guid_reserve(ARTS_GUID_DB, 0);
    int *obj = (int *)malloc(sizeof(int));
    *obj = 0x77;
    arts_shared_ptr_t h = arts_route_table_install_if_absent(obj, g, 0, false);
    arts_shared_release(&h);
    /* Hold the slot pointer: after the destroy the key is zeroed, so a fresh
     * lookup would reserve a new slot rather than find this one.  Slots are
     * never freed, only re-keyed, so the pointer stays valid. */
    arts_route_item_t *b_item = NULL;
    arts_route_table_reserve_or_lookup(g, &b_item);
    atomic_store(&g_deletes, 0);

    pthread_t tids[B_THREADS];
    b_ctx_t ctx[B_THREADS];
    atomic_int gate;
    atomic_init(&gate, 0);
    for (int i = 0; i < B_THREADS; i++) {
      ctx[i].guid = g;
      ctx[i].obj = obj;
      ctx[i].gate = &gate;
      ctx[i].won = 0;
      if (pthread_create(&tids[i], NULL, b_worker, &ctx[i]) != 0) {
        FAIL("B pthread_create %d\n", i);
      }
    }
    atomic_store_explicit(&gate, 1, memory_order_release);
    int winners = 0;
    for (int i = 0; i < B_THREADS; i++) {
      pthread_join(tids[i], NULL);
      winners += ctx[i].won;
    }
    if (winners != 1) {
      FAIL("B: %d set_destroyed winners, want 1 (single-flight)\n", winners);
    }
    if (atomic_load(&g_deletes) != 1) {
      FAIL("B: object freed %d times, want exactly 1\n",
           atomic_load(&g_deletes));
    }
    if (__atomic_load_n(&b_item->key, __ATOMIC_ACQUIRE) != 0) {
      FAIL("B: destroy did not return the slot (key still claimed)\n");
    }
  }

  /* ---- Scenario C: create/destroy churn returns the slot every round ---- */
  {
    arts_guid_t g = arts_guid_reserve(ARTS_GUID_DB, 0);
    arts_route_item_t *c_item = NULL;
    arts_route_table_reserve_or_lookup(g, &c_item);
    atomic_store(&g_deletes, 0);
    const int ROUNDS = 2000;
    for (int r = 0; r < ROUNDS; r++) {
      int *obj = (int *)malloc(sizeof(int));
      *obj = r;
      arts_shared_ptr_t h = arts_route_table_install_if_absent(obj, g, 0, false);
      bool won = h != NULL;
      arts_shared_release(&h);
      if (!won) {
        FAIL("C round %d: install_if_absent on a known-empty slot lost\n", r);
      }
      bool destroyed = arts_route_table_set_destroyed_object(g, obj);
      if (!destroyed) {
        FAIL("C round %d: set_destroyed on a known-live slot returned false\n",
             r);
      }
      /* The slot must come back every round; churn that leaked a slot per
       * round is exactly the growth this reclamation exists to stop. */
      if (__atomic_load_n(&c_item->key, __ATOMIC_ACQUIRE) != 0) {
        FAIL("C round %d: slot still claimed after destroy\n", r);
      }
    }
    if (atomic_load(&g_deletes) != ROUNDS) {
      FAIL("C: %d frees over %d rounds (double-free or leak)\n",
           atomic_load(&g_deletes), ROUNDS);
    }
  }

  /* ---- Scenario D: a retire by identity takes only its own generation ---- */
  {
    arts_guid_t g = arts_guid_reserve(ARTS_GUID_DB, 0);
    atomic_store(&g_deletes, 0);
    int *o1 = (int *)malloc(sizeof(int));
    arts_shared_ptr_t h1 = arts_route_table_install_if_absent(o1, g, 0, false);
    if (h1 == NULL) {
      FAIL("D: first install lost\n");
    }
    arts_shared_ptr_t stranger = arts_shared_make(NULL, NULL);
    if (arts_route_table_set_destroyed_if(g, stranger)) {
      FAIL("D: a retire by a stranger's identity retired the slot\n");
    }
    arts_shared_abandon(&stranger);
    /* A handle-less retire takes generation 1; generation 2 installs. */
    if (!arts_route_table_set_destroyed_object(g, o1)) {
      FAIL("D: retire of generation 1 failed\n");
    }
    int *o2 = (int *)malloc(sizeof(int));
    arts_shared_ptr_t h2 = arts_route_table_install_if_absent(o2, g, 0, false);
    if (h2 == NULL) {
      FAIL("D: generation 2 install lost\n");
    }
    if (arts_route_table_set_destroyed_if(g, h1)) {
      FAIL("D: generation 1's identity retired generation 2\n");
    }
    arts_shared_ptr_t lh = arts_route_table_lookup(g);
    if (arts_shared_get(lh) != o2) {
      FAIL("D: generation 2 is not installed after a stale identity retire\n");
    }
    arts_shared_release(&lh);
    if (!arts_route_table_set_destroyed_if(g, h2)) {
      FAIL("D: generation 2's own identity did not retire it\n");
    }
    lh = arts_route_table_lookup(g);
    if (lh != NULL) {
      FAIL("D: an object is installed after its identity retire\n");
    }
    arts_shared_release(&h1);
    arts_shared_release(&h2);
    if (atomic_load(&g_deletes) != 2) {
      FAIL("D: %d frees for two generations\n", atomic_load(&g_deletes));
    }
  }

  /* ---- Scenario E: a pinned retire never takes a later generation ---- */
  {
    arts_guid_t g = arts_guid_reserve(ARTS_GUID_DB, 0);
    atomic_store(&g_deletes, 0);
    int *o1 = (int *)malloc(sizeof(int));
    arts_shared_ptr_t h = arts_route_table_install_if_absent(o1, g, 0, false);
    arts_shared_release(&h);
    arts_shared_ptr_t pinned = arts_route_table_lookup(g);
    if (!arts_route_table_set_destroyed_object(g, o1)) {
      FAIL("E: retire of generation 1 failed\n");
    }
    int *o2 = (int *)malloc(sizeof(int));
    h = arts_route_table_install_if_absent(o2, g, 0, false);
    if (h == NULL) {
      FAIL("E: generation 2 install lost\n");
    }
    arts_shared_release(&h);
    if (arts_route_table_set_destroyed_item(g, pinned)) {
      FAIL("E: a retire pinned on generation 1 took generation 2\n");
    }
    arts_shared_ptr_t lh = arts_route_table_lookup(g);
    if (arts_shared_get(lh) != o2) {
      FAIL("E: generation 2 left its slot\n");
    }
    arts_shared_release(&lh);
    arts_shared_release(&pinned);
    if (!arts_route_table_set_destroyed_object(g, o2)) {
      FAIL("E: retire of generation 2 failed\n");
    }
    if (atomic_load(&g_deletes) != 2) {
      FAIL("E: %d frees for two generations\n", atomic_load(&g_deletes));
    }
  }

  if (run_fg(true) != 0 || run_fg(false) != 0) {
    return 1;
  }

  printf("PASS route_table_install_if_absent: single install winner, abandon "
         "keeps loser objects, retire single-flight and bound to the object "
         "it saw\n");
  return 0;
}
