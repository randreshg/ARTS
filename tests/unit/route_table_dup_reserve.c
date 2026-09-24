/* SPDX-License-Identifier: Apache-2.0
 *
 * route_table_dup_reserve — one key, one published slot, across slot return.
 *
 * The shape: G's first probe slot E is held by a dying key K.  Reserver B
 * searches for G and misses.  Before B claims, reserver A searches, passes E
 * (still K's), claims the next probe slot L, and installs G's object there.
 * K dies and returns E.  B now claims E, the first free slot in probe order.
 *
 * The reservation invariant is that at most one slot publishes a key and an
 * installed object is never moved.  B's claim is made under a marker no search
 * matches, and B's settling scan finds L publishing G, so B gives E up and
 * resolves to L.  Asserted:
 *   1. one slot is keyed G, it is L, E is free, B resolved to L, and a lookup
 *      of G finds A's object;
 *   2. a create of G's next generation issued while B's claim is unsettled
 *      (the queued early create) finds the object and loses — it does not
 *      install into E — and installs once A's object is retired;
 *   3. a reserver whose scan meets a slot held RETIRING waits for the retire
 *      to settle: a hand-back (the key restored) makes it resolve to that
 *      slot, a return (the key zeroed) lets it publish its own claim.
 *
 * Deterministic: route_table.c's after-miss and after-claim injection points
 * run the other parties inside B's windows.  No runtime: route_table.c +
 * guid.c + shared.c #include'd; the OoO engine is stubbed.
 */

#include <pthread.h>
#include <time.h>
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
static void test_after_miss(arts_guid_t key);
static void test_after_claim(arts_guid_t key);
#define ROUTE_TABLE_AFTER_MISS(key) test_after_miss(key)
#define ROUTE_TABLE_AFTER_CLAIM(key) test_after_claim(key)
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
    (void)fprintf(stderr, "FAIL route_table_dup_reserve: " __VA_ARGS__); \
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


static arts_guid_t g_key;
static int g_miss_armed;
static int g_claim_armed; /* 1: early create; 2: hand-back; 3: return */
static int g_claim_ran;
static int g_claim_after_miss; /* armed for B once A's reservation is done */
static int g_obj = 0x5eed;
static int g_next_obj = 0x5eee;
static arts_route_item_t *g_first_probe;
static arts_route_item_t *g_retiring_slot;
static arts_shared_ptr_t g_cb;       /* A's handle on its installed object */
static arts_shared_ptr_t g_early_cb; /* the early create's result */
static pthread_t g_settler;

static void test_after_miss(arts_guid_t key) {
  if (!g_miss_armed || key != g_key) {
    return;
  }
  g_miss_armed = 0;
  /* A: passes E (K's), claims L, installs the object there. */
  g_cb = arts_route_table_install_if_absent(&g_obj, key, 0, false);
  /* K dies: E is returned. */
  __atomic_store_n(&g_first_probe->key, (arts_guid_t)0, __ATOMIC_RELEASE);
  g_claim_armed = g_claim_after_miss;
  g_claim_after_miss = 0;
}

static void *settle_retire(void *arg) {
  arts_guid_t to = (arts_guid_t)(uintptr_t)arg;
  struct timespec pause = {0, 100000000};
  nanosleep(&pause, NULL);
  __atomic_store_n(&g_retiring_slot->key, to, __ATOMIC_RELEASE);
  return NULL;
}

static void test_after_claim(arts_guid_t key) {
  if (!g_claim_armed || key != g_key) {
    return;
  }
  int what = g_claim_armed;
  g_claim_armed = 0;
  g_claim_ran = what;
  if (what == 1) {
    /* The next generation's create, issued once A's object is installed. */
    g_early_cb = arts_route_table_install_if_absent(&g_next_obj, key, 0, false);
    return;
  }
  /* A retire holds the slot; it settles 100 ms into the reserver's scan. */
  arts_guid_t to = what == 2 ? key : (arts_guid_t)0;
  if (pthread_create(&g_settler, NULL, settle_retire, (void *)(uintptr_t)to)) {
    (void)fprintf(stderr, "FAIL route_table_dup_reserve: pthread_create\n");
    exit(1);
  }
}

/* A fresh key whose first probe slot is held by a live stranger K. */
static arts_route_table_t *fresh_key(void) {
  g_key = arts_guid_reserve(ARTS_GUID_DB, 0);
  arts_route_table_t *rt = arts_get_route_table(g_key);
  uint64_t p0 = get_route_table_key((uint64_t)g_key, rt->shift);
  g_first_probe = &rt->data[p0];
  __atomic_store_n(&g_first_probe->key, g_key ^ (arts_guid_t)0x40,
                   __ATOMIC_RELEASE);
  return rt;
}

static unsigned int slots_keyed(arts_route_table_t *rt) {
  unsigned int keyed = 0;
  for (arts_route_table_t *t = rt; t != NULL; t = t->next) {
    uint64_t pos = get_route_table_key((uint64_t)g_key, t->shift);
    for (int i = 0; i < COLLISION_RESOLVES; i++) {
      if (__atomic_load_n(&t->data[pos + i].key, __ATOMIC_ACQUIRE) == g_key) {
        keyed++;
      }
    }
  }
  return keyed;
}

static int lookup_is(void *obj) {
  arts_shared_ptr_t h = arts_route_table_lookup(g_key);
  int ok = arts_shared_get(h) == obj;
  arts_shared_release(&h);
  return ok;
}

int main(void) {
  rt_init();

  /* ---- 1. Two reservers: the later claim gives up, nothing moves ---- */
  arts_route_table_t *rt = fresh_key();
  arts_route_item_t *second = g_first_probe + 1;
  g_miss_armed = 1;
  arts_route_item_t *b = NULL;
  arts_route_table_reserve_or_lookup(g_key, &b);
  if (g_miss_armed) {
    FAIL("1: the injected reserver never ran\n");
  }
  if (slots_keyed(rt) != 1u) {
    FAIL("1: %u slots answer for one key\n", slots_keyed(rt));
  }
  if (b != second) {
    FAIL("1: the reservation did not resolve to the slot holding the object\n");
  }
  if (__atomic_load_n(&g_first_probe->key, __ATOMIC_ACQUIRE) != 0) {
    FAIL("1: the given-up claim was not returned\n");
  }
  if (!lookup_is(&g_obj) || !arts_atomic_shared_holds(&second->value, g_cb)) {
    FAIL("1: the object is not where it was installed\n");
  }
  if (!arts_route_table_set_destroyed_item(g_key, g_cb)) {
    FAIL("1: the object's destroy did not land\n");
  }
  arts_shared_release(&g_cb);
  if (slots_keyed(rt) != 0u) {
    FAIL("1: a slot still answers for the destroyed key\n");
  }

  /* ---- 2. The queued next create during an unsettled claim ---- */
  rt = fresh_key();
  second = g_first_probe + 1;
  g_miss_armed = 1;
  g_claim_after_miss = 1;
  g_claim_ran = 0;
  arts_route_table_reserve_or_lookup(g_key, &b);
  if (g_claim_ran != 1) {
    FAIL("2: the early create never ran inside the claim\n");
  }
  if (g_early_cb != NULL) {
    FAIL("2: the next generation installed beside a live one\n");
  }
  if (slots_keyed(rt) != 1u || b != second) {
    FAIL("2: %u slots answer for one key after the claim\n", slots_keyed(rt));
  }
  if (!lookup_is(&g_obj)) {
    FAIL("2: the live generation was lost\n");
  }
  if (!arts_route_table_set_destroyed_item(g_key, g_cb)) {
    FAIL("2: the live generation's destroy did not land\n");
  }
  arts_shared_release(&g_cb);
  g_early_cb = arts_route_table_install_if_absent(&g_next_obj, g_key, 0, false);
  if (g_early_cb == NULL || !lookup_is(&g_next_obj)) {
    FAIL("2: the next generation did not install after the destroy\n");
  }
  if (!arts_route_table_set_destroyed_item(g_key, g_early_cb)) {
    FAIL("2: the next generation's destroy did not land\n");
  }
  arts_shared_release(&g_early_cb);

  /* ---- 3. A scan that meets a retiring slot ---- */
  for (int what = 2; what <= 3; what++) {
    rt = fresh_key();
    /* G is published on L with an object; a retire holds L. */
    g_cb = arts_route_table_install_if_absent(&g_obj, g_key, 0, false);
    g_retiring_slot = g_first_probe + 1;
    if (g_cb == NULL || slots_keyed(rt) != 1u ||
        __atomic_load_n(&g_retiring_slot->key, __ATOMIC_ACQUIRE) != g_key) {
      FAIL("3: setup did not publish the key on the second probe slot\n");
    }
    __atomic_store_n(&g_retiring_slot->key, ARTS_ROUTE_KEY_RETIRING,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_first_probe->key, (arts_guid_t)0, __ATOMIC_RELEASE);
    g_claim_armed = what;
    arts_route_table_reserve_or_lookup(g_key, &b);
    pthread_join(g_settler, NULL);
    if (g_claim_ran != what) {
      FAIL("3: the retire never settled inside the claim\n");
    }
    if (slots_keyed(rt) != 1u) {
      FAIL("3.%d: %u slots answer for one key\n", what, slots_keyed(rt));
    }
    if (what == 2) {
      if (b != g_retiring_slot || !lookup_is(&g_obj)) {
        FAIL("3: after a hand-back the reservation missed the object\n");
      }
      if (!arts_route_table_set_destroyed_item(g_key, g_cb)) {
        FAIL("3: the handed-back object's destroy did not land\n");
      }
    } else {
      if (b != g_first_probe) {
        FAIL("3: after a return the claim was not published\n");
      }
      /* The retire that returned L took the object in this simulation. */
      arts_shared_ptr_t out = arts_atomic_shared_exchange(
          &g_retiring_slot->value, NULL);
      arts_shared_set_tag(out, 0);
      arts_shared_release(&out);
      arts_route_item_t *again = NULL;
      arts_route_table_reserve_or_lookup(g_key, &again);
      if (again != b) {
        FAIL("3: the published reservation does not answer for the key\n");
      }
    }
    arts_shared_release(&g_cb);
  }

  printf("PASS route_table_dup_reserve: the later claim gave up and nothing "
         "moved; the queued next create lost to the live generation and "
         "installed after its destroy; a retiring slot was waited out\n");
  return 0;
}
