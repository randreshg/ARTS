/* SPDX-License-Identifier: Apache-2.0
 *
 * route_table_growth_residency — one growth of a segment chain builds one
 * segment, and a segment's memory is resident only where slots are touched.
 *
 * T threads, released together, each claim a key whose probe window in the
 * root segment is full, so every one of them reaches the end of the chain at
 * once.  Both growth sites are driven: search_for_empty on a standalone table
 * (phase A) and reserve_or_lookup through the key's table (phase B).  For
 * each, the test asserts:
 *   - the next segment is built once: one call of the table's constructor;
 *   - every key ends in exactly one slot, and the chain grew by one segment;
 *   - the grown segment's resident pages (mincore) are only pages holding a
 *     probe window some key touched, not the segment.
 * Phase A also bounds the root segment's resident pages the same way and the
 * peak resident set (VmHWM) while the chain grows to well under one segment,
 * and its delete drains the one slot holding a payload and writes no other
 * slot.  While the builder sits in the constructor, phase B runs every chain
 * walker against the table (key search, iteration, parked count, the
 * retire-time pin), each of which must take the unfinished link for the end
 * of the chain.  Phase C returns a slot of a full window while the next
 * segment is being built: a reserver that claims that slot ends its settle
 * walk at the unfinished link and publishes there, and a reserver of the same
 * key that claims in the new segment after publication must resolve to that
 * one slot.
 *
 * The constructor is the table's own newFunc, wrapped to count calls and to
 * hold the builder until the test releases it, after the other threads have
 * had time to reach the chain's end.  No runtime: route_table.c + guid.c +
 * shared.c #include'd.
 */

#include "arts/utils/stdatomic_compat.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
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

struct arts_route_item_s;
void arts_ooo_drain(struct arts_route_item_s *s) { (void)s; }
void arts_ooo_redrive_all(struct arts_route_item_s *s) { (void)s; }

#include "../../libs/src/core/gas/guid.c"
#include "../../libs/src/core/gas/route_table.c"
#include "../../libs/src/core/utils/shared.c"

struct arts_runtime_shared_s arts_node_info;
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;

/* As the engine's: freeing a slot's parked payloads detaches its list, a
 * write to the slot. */
static atomic_uint g_frees;
void arts_ooo_free_all(struct arts_route_item_s *s) {
  atomic_fetch_add(&g_frees, 1);
  (void)arts_lf_stack_drain(&s->ooo_list);
}

static int g_failed;
#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      (void)fprintf(stderr,                                                    \
                    "FAIL route_table_growth_residency: " __VA_ARGS__);        \
      g_failed = 1;                                                            \
    }                                                                          \
  } while (0)

#define THREADS 8
/* A root of 2^16 windows is 32 MiB, so the grown segment is 64 MiB. */
#define ROOT_SHIFT 16u
#define ROOT_SIZE (1u << ROOT_SHIFT)
#define BUILD_HOLD_MS 300
/* COLLISION_RESOLVES keys fill the root window; THREADS more overflow it; the
 * last one is never claimed and serves the walkers' missed searches. */
#define NKEYS (COLLISION_RESOLVES + THREADS + 1)

static size_t seg_bytes(unsigned int size) {
  return (size_t)size * COLLISION_RESOLVES * sizeof(arts_route_item_t);
}

/* ── the table's constructor, counted and held ─────────────────────────── */

static atomic_uint g_builds;
static atomic_int g_building;
static atomic_int g_release;

static arts_route_table_t *held_new_route_table(unsigned int size,
                                                unsigned int shift) {
  atomic_fetch_add(&g_builds, 1);
  atomic_store(&g_building, 1);
  struct timespec ts = {0, BUILD_HOLD_MS * 1000000L};
  nanosleep(&ts, NULL);
  while (!atomic_load(&g_release)) {
    sched_yield();
  }
  return arts_new_route_table(size, shift);
}

/* ── keys ──────────────────────────────────────────────────────────────── */

/* NKEYS GUIDs of `kind` (keys below `limit`) sharing the root's window 0. */
static int collect_keys(arts_guid_kind_t kind, uint64_t limit,
                        arts_guid_t *out) {
  int found = 0;
  for (uint64_t k = 1; k < limit && found < NKEYS; k++) {
    arts_guid_t g = ARTS_GUID_MAKE(kind, 0, k);
    if (get_route_table_key((uint64_t)g, ROOT_SHIFT) == 0) {
      out[found++] = g;
    }
  }
  return found;
}

/* ── residency ─────────────────────────────────────────────────────────── */

static size_t g_page;

/* Transparent huge pages of every size are off for this process, so a fault
 * makes one base page resident whatever the host enables. */
static int residency_init(void) {
  g_page = (size_t)sysconf(_SC_PAGESIZE);
  return prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);
}

/* Resident pages of the pages spanning [base, base+len). */
static size_t resident_pages(const void *base, size_t len) {
  uintptr_t lo = (uintptr_t)base & ~(uintptr_t)(g_page - 1);
  uintptr_t hi =
      ((uintptr_t)base + len + g_page - 1) & ~(uintptr_t)(g_page - 1);
  size_t pages = (size_t)(hi - lo) / g_page;
  unsigned char *vec = (unsigned char *)malloc(pages);
  if (vec == NULL || mincore((void *)lo, (size_t)(hi - lo), vec) != 0) {
    free(vec);
    return (size_t)-1;
  }
  size_t n = 0;
  for (size_t i = 0; i < pages; i++) {
    n += vec[i] & 1u;
  }
  free(vec);
  return n;
}

/* Pages the probe windows of keys[0..n) can make resident in segment t. */
static size_t window_page_bound(const arts_route_table_t *t,
                                const arts_guid_t *keys, int n) {
  size_t distinct = 0;
  size_t seen[NKEYS];
  for (int i = 0; i < n; i++) {
    uint64_t pos = get_route_table_key((uint64_t)keys[i], t->shift);
    size_t lo = (size_t)pos * sizeof(arts_route_item_t) / g_page;
    size_t hi =
        ((size_t)(pos + COLLISION_RESOLVES) * sizeof(arts_route_item_t) - 1) /
        g_page;
    for (size_t gr = lo; gr <= hi; gr++) {
      int dup = 0;
      for (size_t j = 0; j < distinct; j++) {
        dup |= seen[j] == gr;
      }
      if (!dup && distinct < NKEYS) {
        seen[distinct++] = gr;
      }
    }
  }
  return distinct;
}

static long status_kb(const char *field) {
  FILE *f = fopen("/proc/self/status", "r");
  if (f == NULL) {
    return -1;
  }
  char line[256];
  long kb = -1;
  size_t flen = strlen(field);
  while (fgets(line, sizeof(line), f) != NULL) {
    if (strncmp(line, field, flen) == 0) {
      kb = strtol(line + flen, NULL, 10);
      break;
    }
  }
  fclose(f);
  return kb;
}

static int reset_peak_rss(void) {
  int fd = open("/proc/self/clear_refs", O_WRONLY);
  if (fd < 0) {
    return -1;
  }
  int ok = write(fd, "5", 1) == 1;
  close(fd);
  return ok ? 0 : -1;
}

/* ── racing claimers ───────────────────────────────────────────────────── */

typedef struct {
  pthread_barrier_t *start;
  arts_route_table_t *table; /* phase A: the standalone table */
  arts_guid_t key;
  arts_route_item_t *slot;
} claimer_t;

static void *claim_by_search_for_empty(void *vp) {
  claimer_t *c = (claimer_t *)vp;
  pthread_barrier_wait(c->start);
  c->slot = arts_route_table_search_for_empty(c->table, c->key);
  return NULL;
}

static void *claim_by_reserve(void *vp) {
  claimer_t *c = (claimer_t *)vp;
  pthread_barrier_wait(c->start);
  arts_route_table_reserve_or_lookup(c->key, &c->slot);
  return NULL;
}

/* Common checks once the claimers are joined. */
static void check_growth(const char *phase, arts_route_table_t *root,
                         const arts_guid_t *keys, claimer_t *c) {
  arts_route_table_t *seg =
      atomic_load_explicit(&root->next, memory_order_acquire);
  unsigned builds = atomic_load(&g_builds);
  (void)fprintf(stderr, "%s: constructor calls %u\n", phase, builds);
  CHECK(builds == 1, "%s: the segment was built %u times\n", phase, builds);
  if (seg == NULL) {
    CHECK(0, "%s: the chain did not grow\n", phase);
    return;
  }
  CHECK(atomic_load_explicit(&seg->next, memory_order_acquire) == NULL,
        "%s: the chain grew past one segment\n", phase);
  CHECK(seg->size == 2 * root->size && seg->shift == root->shift + 1,
        "%s: grown segment is size %u shift %u\n", phase, seg->size,
        seg->shift);
  for (int t = 0; t < THREADS; t++) {
    arts_route_item_t *s = c[t].slot;
    CHECK(s != NULL && s >= seg->data &&
              s < seg->data + (size_t)seg->size * COLLISION_RESOLVES,
          "%s: thread %d's slot is not in the grown segment\n", phase, t);
    CHECK(s != NULL &&
              atomic_load_explicit(&s->key, memory_order_acquire) == c[t].key,
          "%s: thread %d's slot does not hold its key\n", phase, t);
    CHECK(arts_route_table_search_for_key(root, c[t].key) == s,
          "%s: thread %d's key resolves elsewhere\n", phase, t);
    for (int u = 0; u < t; u++) {
      CHECK(c[u].slot != s, "%s: threads %d and %d share a slot\n", phase, u,
            t);
    }
  }
  size_t res = resident_pages(seg->data, seg_bytes(seg->size));
  size_t bound = window_page_bound(seg, keys, NKEYS);
  size_t total = seg_bytes(seg->size) / g_page;
  (void)fprintf(stderr,
                "%s: grown segment resident %zu of %zu pages (bound %zu)\n",
                phase, res, total, bound);
  CHECK(res <= bound, "%s: grown segment has %zu resident pages, bound %zu\n",
        phase, res, bound);
}

static void reset_counts(int released) {
  atomic_store(&g_builds, 0);
  atomic_store(&g_building, 0);
  atomic_store(&g_release, released);
}

static int link_is_growing(arts_route_table_t *t) {
  return atomic_load_explicit(&t->next, memory_order_acquire) ==
         ARTS_ROUTE_TABLE_GROWING;
}

/* ── phase A: search_for_empty on a standalone table ───────────────────── */

static void phase_a(void) {
  arts_guid_t keys[NKEYS];
  if (collect_keys(ARTS_GUID_DB, 1ull << 30, keys) < NKEYS) {
    CHECK(0, "A: not enough colliding keys\n");
    return;
  }
  arts_route_table_t *root = arts_new_route_table(ROOT_SIZE, ROOT_SHIFT);
  root->newFunc = held_new_route_table;
  for (int i = 0; i < COLLISION_RESOLVES; i++) {
    arts_route_table_search_for_empty(root, keys[i]);
  }

  reset_counts(1);
  pthread_barrier_t start;
  pthread_barrier_init(&start, NULL, THREADS + 1);
  pthread_t tid[THREADS];
  claimer_t c[THREADS];
  for (int t = 0; t < THREADS; t++) {
    c[t] = (claimer_t){&start, root, keys[COLLISION_RESOLVES + t], NULL};
    pthread_create(&tid[t], NULL, claim_by_search_for_empty, &c[t]);
  }
  int peak_ok = reset_peak_rss() == 0;
  long rss0 = status_kb("VmRSS:");
  pthread_barrier_wait(&start);
  for (int t = 0; t < THREADS; t++) {
    pthread_join(tid[t], NULL);
  }
  long hwm = status_kb("VmHWM:");
  pthread_barrier_destroy(&start);

  check_growth("A", root, keys, c);
  size_t res = resident_pages(root->data, seg_bytes(root->size));
  size_t bound = window_page_bound(root, keys, NKEYS);
  (void)fprintf(stderr,
                "A: root segment resident %zu of %zu pages (bound %zu)\n", res,
                seg_bytes(root->size) / g_page, bound);
  CHECK(res <= bound, "A: root segment has %zu resident pages, bound %zu\n",
        res, bound);
  if (peak_ok && rss0 > 0 && hwm > 0) {
    long grew_kb = hwm - rss0;
    (void)fprintf(stderr, "A: peak resident set grew %ld KiB during growth\n",
                  grew_kb);
    CHECK((size_t)(grew_kb > 0 ? grew_kb : 0) * 1024 <
              seg_bytes(2 * ROOT_SIZE) / 4,
          "A: peak resident set grew %ld KiB, a quarter segment is %zu KiB\n",
          grew_kb, seg_bytes(2 * ROOT_SIZE) / 4 / 1024);
  } else {
    (void)fprintf(stderr, "A: peak resident set not measurable, skipped\n");
  }

  /* Deleting the chain frees the payloads parked on it and writes no slot
   * that holds none, so it populates no page the run never wrote. */
  static arts_lf_link_t parked;
  arts_lf_stack_push(&root->data[0].ooo_list, &parked);
  atomic_store(&g_frees, 0);
  arts_delete_route_table(root);
  unsigned frees = atomic_load(&g_frees);
  (void)fprintf(stderr, "A: delete drained %u slot(s)\n", frees);
  CHECK(frees == 1,
        "A: delete drained %u slots; one slot holds a parked payload\n", frees);
}

/* ── phase B: reserve_or_lookup through the key's table ────────────────── */

static void rt_init(arts_route_table_t *root) {
  arts_thread_info.thread_id = 0;
  arts_node_info.total_thread_count = 1;
  arts_node_info.gpu = 0;
  num_tables = 1;
  min_global_guid_thread = 0;
  max_global_guid_thread = 1;
  keys_per_thread = 1ull << 40;
  global_guid_on = 0;
  arts_node_info.route_table =
      (arts_route_table_t **)calloc(1, sizeof(arts_route_table_t *));
  arts_node_info.route_table[0] = root;
  for (int s = 0; s < ARTS_REMOTE_ROUTE_SHARDS; s++) {
    arts_node_info.remote_route_table[s] = arts_new_route_table(16, 4);
  }
}

static void rt_fini(void) {
  arts_delete_route_table(arts_node_info.route_table[0]);
  for (int s = 0; s < ARTS_REMOTE_ROUTE_SHARDS; s++) {
    arts_delete_route_table(arts_node_info.remote_route_table[s]);
  }
  free(arts_node_info.route_table);
}

/* Every walker, run while the builder holds the chain's end. */
static void walk_while_building(const arts_guid_t *keys) {
  arts_route_table_t *root = arts_node_info.route_table[0];
  arts_guid_t missing = keys[NKEYS - 1];
  CHECK(arts_route_table_search_for_key(root, missing) == NULL,
        "B: a missed search found a slot mid-growth\n");
  CHECK(arts_route_table_lookup(missing) == NULL,
        "B: a lookup of an unclaimed key found an object mid-growth\n");
  CHECK(route_table_pin_published(missing) == NULL,
        "B: the retire-time pin found an object mid-growth\n");
  CHECK(!arts_route_table_set_destroyed_object(missing, &missing),
        "B: an object retire of an unclaimed key succeeded mid-growth\n");
  unsigned keyed = 0;
  arts_route_table_iterator_t iter;
  arts_reset_route_table_iterator(&iter, root);
  for (arts_route_item_t *it = arts_route_table_iterate(&iter); it != NULL;
       it = arts_route_table_iterate(&iter)) {
    keyed++;
  }
  CHECK(keyed == COLLISION_RESOLVES,
        "B: iteration mid-growth saw %u keyed slots, expected %d\n", keyed,
        COLLISION_RESOLVES);
  CHECK(arts_route_table_parked_count(root) == 0,
        "B: parked count mid-growth is nonzero\n");
}

static void phase_b(void) {
  arts_guid_t keys[NKEYS];
  if (collect_keys(ARTS_GUID_EVENT, 1ull << 30, keys) < NKEYS) {
    CHECK(0, "B: not enough colliding keys\n");
    return;
  }
  arts_route_table_t *root = arts_new_route_table(ROOT_SIZE, ROOT_SHIFT);
  rt_init(root);
  root->newFunc = held_new_route_table;
  for (int i = 0; i < COLLISION_RESOLVES; i++) {
    arts_route_item_t *s = NULL;
    arts_route_table_reserve_or_lookup(keys[i], &s);
  }

  reset_counts(0);
  pthread_barrier_t start;
  pthread_barrier_init(&start, NULL, THREADS + 1);
  pthread_t tid[THREADS];
  claimer_t c[THREADS];
  for (int t = 0; t < THREADS; t++) {
    c[t] = (claimer_t){&start, NULL, keys[COLLISION_RESOLVES + t], NULL};
    pthread_create(&tid[t], NULL, claim_by_reserve, &c[t]);
  }
  pthread_barrier_wait(&start);
  while (atomic_load(&g_building) == 0) {
    sched_yield();
  }
  CHECK(link_is_growing(root), "B: the link is not GROWING while building\n");
  walk_while_building(keys);
  CHECK(link_is_growing(root), "B: the link left GROWING during the walk\n");
  atomic_store(&g_release, 1);
  for (int t = 0; t < THREADS; t++) {
    pthread_join(tid[t], NULL);
  }
  pthread_barrier_destroy(&start);
  check_growth("B", root, keys, c);
  rt_fini();
}

/* ── phase C: a settle walk that ends at the unfinished link ───────────── */

#define C_SHIFT 4u
#define C_SIZE (1u << C_SHIFT)

typedef struct {
  arts_guid_t key;
  arts_route_item_t *slot;
} reserver_t;

static void *reserve_one(void *vp) {
  reserver_t *r = (reserver_t *)vp;
  arts_route_table_reserve_or_lookup(r->key, &r->slot);
  return NULL;
}

static void phase_c(void) {
  arts_guid_t fill[COLLISION_RESOLVES];
  arts_guid_t key = 0;
  int found = 0;
  for (uint64_t k = 1; found <= COLLISION_RESOLVES && k < (1ull << 30); k++) {
    arts_guid_t g = ARTS_GUID_MAKE(ARTS_GUID_EVENT, 0, k);
    if (get_route_table_key((uint64_t)g, C_SHIFT) == 0) {
      if (found < COLLISION_RESOLVES) {
        fill[found] = g;
      } else {
        key = g;
      }
      found++;
    }
  }
  if (found <= COLLISION_RESOLVES) {
    CHECK(0, "C: not enough colliding keys\n");
    return;
  }
  arts_route_table_t *root = arts_new_route_table(C_SIZE, C_SHIFT);
  rt_init(root);
  for (int i = 0; i < COLLISION_RESOLVES; i++) {
    arts_route_item_t *s = NULL;
    arts_route_table_reserve_or_lookup(fill[i], &s);
  }
  root->newFunc = held_new_route_table;
  reset_counts(0);

  /* A finds the window full and builds the next segment, held. */
  reserver_t a = {key, NULL};
  pthread_t ta;
  pthread_create(&ta, NULL, reserve_one, &a);
  while (atomic_load(&g_building) == 0) {
    sched_yield();
  }
  /* A retire returns one slot of the window while A builds. */
  arts_route_item_t *returned = arts_route_table_search_for_key(root, fill[3]);
  atomic_store_explicit(&returned->key, (arts_guid_t)0, memory_order_release);
  CHECK(link_is_growing(root), "C: the link is not GROWING before B\n");
  /* B claims the returned slot; its settle walk ends at the unfinished link. */
  reserver_t b = {key, NULL};
  pthread_t tb;
  pthread_create(&tb, NULL, reserve_one, &b);
  pthread_join(tb, NULL);
  CHECK(link_is_growing(root), "C: the link left GROWING before B finished\n");
  atomic_store(&g_release, 1);
  pthread_join(ta, NULL);

  CHECK(b.slot == returned, "C: B did not publish in the returned slot\n");
  CHECK(a.slot == b.slot, "C: A and B resolved the key to different slots\n");
  unsigned keyed = 0, marked = 0;
  for (arts_route_table_t *t = root; t != NULL;
       t = atomic_load_explicit(&t->next, memory_order_acquire)) {
    for (uint64_t i = 0; i < (uint64_t)t->size * COLLISION_RESOLVES; i++) {
      arts_guid_t k =
          atomic_load_explicit(&t->data[i].key, memory_order_acquire);
      keyed += k == key;
      marked += k == route_key_claiming(key);
    }
  }
  (void)fprintf(stderr, "C: slots publishing the key %u, unsettled claims %u\n",
                keyed, marked);
  CHECK(keyed == 1 && marked == 0,
        "C: %u slots publish the key and %u claims are unsettled\n", keyed,
        marked);
  rt_fini();
}

int main(void) {
  CHECK(residency_init() == 0,
        "transparent huge pages could not be turned off\n");
  phase_a();
  phase_b();
  phase_c();
  if (g_failed) {
    return 1;
  }
  (void)printf("PASS route_table_growth_residency\n");
  return 0;
}
