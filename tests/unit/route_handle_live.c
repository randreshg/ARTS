/* SPDX-License-Identifier: Apache-2.0
 *
 * route_handle_live — a control block records the route slot it was installed
 * into, and the route table's primitives built on that record.
 *
 *   A. Liveness.  A handle made by arts_route_table_make_handle is not live
 *      and has no home; installed by arts_route_table_install_handle_into it
 *      is live for its key (not for another) and its home is the slot; once
 *      retired (the dispatched-item form) it is not live, its home is kept,
 *      and its object is freed only at the last reference.
 *   B. install_handle_into is one-shot: into an occupied slot, or a slot that
 *      no longer names the key (another key's, or returned), it returns false
 *      and the control block stays unpublished with no home: abandoned, it
 *      never runs the deleter on the object the test still holds.
 *   C. install_or_get pins the occupant (a handle other than the caller's)
 *      and leaves the caller's control block unpublished; into an empty slot
 *      it installs and returns the caller's own control block, counted and
 *      live; handed a slot that changed hands, it resolves the key again.
 *   D. borrow_or_lookup borrows a live handle and takes nothing; for a
 *      retired handle, an unpublished one or none it looks the key up and
 *      hands the caller the counted result; a miss returns NULL.  The handle
 *      passed in is never released.
 *   E. Every install of the global table records the home:
 *      install_handle_into, install_handle_if_absent and install_handle_fresh;
 *      a lost install records none, and neither does a mirror slot's
 *      install_data.
 *   F. An identity retire of a handle with a home reaches its slot with no
 *      search, a stale handle (retired, home kept) takes nothing from the
 *      key's next generation, and a handle with no home falls back to the
 *      search.
 *   G. Only a won install CAS writes the home, after it: the install-window
 *      hook (between the key check and the CAS) finds no home yet; a won
 *      install_handle_into then carries its slot; with the hook also moving
 *      the empty slot's value word, the CAS loses, install_handle_into returns
 *      false and no home is written, and install_or_get retries and installs.
 *   H. A retire inside the window between a won install CAS and its home
 *      store (the scheduling hook there): the pre-install holder's copy is not
 *      live yet, the retire takes the cb through the key search, and once the
 *      install returns the cb has its home, is not live, and a second retire
 *      takes nothing.
 *   I. Debug only: an install whose caller holds no reference of its own ends
 *      the process naming the GUID (in a forked child, before any other
 *      scenario runs).
 *   J. The teardown sweep (arts_clean_up_route_table) clears the tag of each
 *      cb it detaches: a holder's copy of an installed cb is not live after
 *      it, and the object is freed at the holder's release.
 * The drain stub checks that every install's drain finds the installed cb
 * with its home already stored, and counts those drains.
 * Where the tree captures NUM_ROUTE_PROBE and NUM_ROUTE_PIN, the probes and
 * pins each step spends are checked exactly; elsewhere only behaviour is.
 *
 * No runtime: route_table.c + guid.c + shared.c #include'd; the OoO engine is
 * stubbed, and the counter increments are recorded here.
 */

#include "arts/utils/stdatomic_compat.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
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
void arts_ooo_redrive_all(struct arts_route_item_s *s) { (void)s; }
void arts_ooo_free_all(struct arts_route_item_s *s) { (void)s; }

/* Install-window hooks: a thread with one armed runs it once, in its next
 * install, between the key check and the CAS (before) or between a won CAS
 * and the home store (after). */
static __thread void (*t_before_install_cas)(void);
static __thread void (*t_after_install_cas)(void);
static void run_armed(void (**armed)(void)) {
  void (*f)(void) = *armed;
  *armed = NULL;
  if (f != NULL) {
    f();
  }
}
#define ROUTE_TABLE_BEFORE_INSTALL_CAS(key)                                    \
  ((void)(key), run_armed(&t_before_install_cas))
#define ROUTE_TABLE_AFTER_INSTALL_CAS(key)                                     \
  ((void)(key), run_armed(&t_after_install_cas))

#include "../../libs/src/core/gas/guid.c"
#include "../../libs/src/core/gas/route_table.c"
#include "../../libs/src/core/utils/shared.c"

struct arts_runtime_shared_s arts_node_info;
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;

ARTS_THREAD_LOCAL arts_counter_t arts_thread_local_counters[NUM_COUNTER_TYPES];
void arts_counter_increment_by(arts_counter_t *counter, uint64_t num) {
  atomic_fetch_add_explicit(&counter->count, num, memory_order_relaxed);
}

/* Every install drains its slot on the installed cb after storing the home:
 * a drain whose cb's home is not this slot ends the test.  A drain whose cb
 * the slot no longer holds (its object retired inside the install) counts
 * nothing. */
static int g_drains;
void arts_ooo_drain_on(struct arts_route_item_s *s,
                       arts_shared_ptr_t installed) {
  if (arts_shared_home(installed) != (void *)s) {
    (void)fprintf(stderr, "FAIL route_handle_live: a drain ran before its "
                          "install stored the home\n");
    exit(1);
  }
  if (arts_shared_slot_holds(&s->value, installed)) {
    g_drains++;
  }
}

/* Installs `obj` under `key` as the runtime's creates do: wrapped in its
 * control block, with a reference of the caller's own held across the
 * install.  A win returns that reference; a loss abandons the control block
 * (the object stays the caller's) and returns NULL. */
static arts_shared_ptr_t install_obj(void *obj, arts_guid_t key) {
  arts_shared_ptr_t cb = arts_route_table_make_handle(obj, key);
  arts_shared_ptr_t mine = arts_shared_copy(cb);
  if (arts_route_table_install_handle_if_absent(cb, key)) {
    return mine;
  }
  arts_shared_release(&mine);
  arts_shared_abandon(&cb);
  return NULL;
}

static _Atomic(int) g_deletes;
static void test_deleter(void *obj) {
  atomic_fetch_add_explicit(&g_deletes, 1, memory_order_relaxed);
  free(obj);
}

static int deletes(void) {
  return atomic_load_explicit(&g_deletes, memory_order_relaxed);
}

static int *new_obj(int v) {
  int *o = (int *)malloc(sizeof(int));
  *o = v;
  return o;
}

#define FAIL(...)                                                              \
  do {                                                                         \
    (void)fprintf(stderr, "FAIL route_handle_live: " __VA_ARGS__);             \
    return 1;                                                                  \
  } while (0)

/* The probes and pins this thread has spent; the scenarios run on one
 * thread. */
typedef struct {
  uint64_t probes;
  uint64_t pins;
} spent_t;

static spent_t spent_now(void) {
  spent_t s = {
      atomic_load_explicit(&arts_thread_local_counters[NUM_ROUTE_PROBE].count,
                           memory_order_relaxed),
      atomic_load_explicit(&arts_thread_local_counters[NUM_ROUTE_PIN].count,
                           memory_order_relaxed)};
  return s;
}

static int spent_check(const char *what, spent_t from, uint64_t probes,
                       uint64_t pins) {
  spent_t now = spent_now();
  (void)what;
  (void)now;
  (void)from;
  (void)probes;
  (void)pins;
#if ENABLE_NUM_ROUTE_PROBE
  if (now.probes - from.probes != probes) {
    FAIL("%s spent %" PRIu64 " probes, want %" PRIu64 "\n", what,
         now.probes - from.probes, probes);
  }
#endif
#if ENABLE_NUM_ROUTE_PIN
  if (now.pins - from.pins != pins) {
    FAIL("%s spent %" PRIu64 " pins, want %" PRIu64 "\n", what,
         now.pins - from.pins, pins);
  }
#endif
  return 0;
}

#define SPENT(what, from, probes, pins)                                        \
  do {                                                                         \
    if (spent_check((what), (from), (probes), (pins)) != 0) {                  \
      return 1;                                                                \
    }                                                                          \
  } while (0)

/* Node init of a one-thread rank as the runtime runs it (the geometry, then
 * thread 0's key counters), and its route tables. */
static void rt_init(void) {
  arts_thread_info.thread_id = 0;
  arts_node_info.total_thread_count = 1;
  arts_node_info.keys = (uint64_t **)calloc(1, sizeof(uint64_t *));
  arts_guid_node_geometry_init();
  arts_guid_key_generator_init();
  arts_node_info.route_table =
      (arts_route_table_t **)calloc(1, sizeof(arts_route_table_t *));
  arts_node_info.route_table[0] = arts_new_route_table(1024, 10);
  for (int s = 0; s < ARTS_REMOTE_ROUTE_SHARDS; s++) {
    arts_node_info.remote_route_table[s] = arts_new_route_table(64, 6);
  }
}

static arts_route_item_t *slot_of(arts_guid_t g) {
  arts_route_item_t *slot = NULL;
  arts_route_table_reserve_or_lookup(g, &slot);
  return slot;
}

/* ── A: liveness across install and retire ─────────────────────────────── */
static int run_a(void) {
  arts_guid_t g = arts_guid_reserve(ARTS_GUID_DB, 0);
  arts_guid_t other = arts_guid_reserve(ARTS_GUID_DB, 0);
  atomic_store_explicit(&g_deletes, 0, memory_order_relaxed);
  g_drains = 0;
  int *obj = new_obj(1);
  arts_shared_ptr_t cb = arts_route_table_make_handle(obj, g);
  if (arts_route_table_live(NULL, g)) {
    FAIL("A: a NULL handle is live\n");
  }
  if (arts_shared_home(cb) != NULL || arts_route_table_live(cb, g)) {
    FAIL("A: a made handle is live before its install\n");
  }
  arts_route_item_t *slot = slot_of(g);
  arts_shared_ptr_t pin = arts_shared_copy(cb);
  spent_t s = spent_now();
  if (!arts_route_table_install_handle_into(slot, cb, g)) {
    FAIL("A: install_handle_into lost on the key's empty slot\n");
  }
  if (arts_shared_home(pin) != (void *)slot) {
    FAIL("A: the install did not record its slot as the home\n");
  }
  if (!arts_route_table_live(pin, g)) {
    FAIL("A: an installed handle is not live\n");
  }
  if (arts_route_table_live(pin, other)) {
    FAIL("A: an installed handle is live for another key\n");
  }
  SPENT("A: install_handle_into and live", s, 0, 0);
  arts_shared_ptr_t lh = arts_route_table_lookup(g);
  if (lh != pin) {
    FAIL("A: the key does not publish the installed handle\n");
  }
  arts_shared_release(&lh);
  s = spent_now();
  if (!arts_route_table_set_destroyed_item(g, pin)) {
    FAIL("A: the retire of a live handle failed\n");
  }
  SPENT("A: the retire of a live handle", s, 0, 0);
  if (arts_route_table_live(pin, g)) {
    FAIL("A: a retired handle is live\n");
  }
  if (arts_shared_home(pin) != (void *)slot) {
    FAIL("A: the retire rewrote the home\n");
  }
  if (atomic_load_explicit(&slot->key, memory_order_acquire) != 0) {
    FAIL("A: the retire did not return the slot\n");
  }
  if (deletes() != 0) {
    FAIL("A: the object was freed under a reference\n");
  }
  arts_shared_release(&pin);
  if (deletes() != 1) {
    FAIL("A: %d frees at the last reference, want 1\n", deletes());
  }
  if (g_drains != 1) {
    FAIL("A: %d drains saw their install, want 1\n", g_drains);
  }
  return 0;
}

/* ── B: install_handle_into is one-shot ────────────────────────────────── */
static int run_b(void) {
  arts_guid_t g = arts_guid_reserve(ARTS_GUID_DB, 0);
  arts_guid_t g3 = arts_guid_reserve(ARTS_GUID_DB, 0);
  atomic_store_explicit(&g_deletes, 0, memory_order_relaxed);
  int *a = new_obj(2);
  arts_shared_ptr_t ha = install_obj(a, g);
  if (ha == NULL) {
    FAIL("B: the occupant's install lost\n");
  }
  arts_route_item_t *slot = slot_of(g);
  int *b = new_obj(3);
  arts_shared_ptr_t cb = arts_route_table_make_handle(b, g);
  spent_t s = spent_now();
  if (arts_route_table_install_handle_into(slot, cb, g)) {
    FAIL("B: installed over an occupant\n");
  }
  int *b3 = new_obj(30);
  arts_shared_ptr_t cb3 = arts_route_table_make_handle(b3, g3);
  if (arts_route_table_install_handle_into(slot, cb3, g3)) {
    FAIL("B: installed into a slot that names another key\n");
  }
  SPENT("B: lost install_handle_into", s, 0, 0);
  if (arts_shared_home(cb3) != NULL) {
    FAIL("B: a lost install recorded a home\n");
  }
  arts_shared_abandon(&cb3);
  free(b3);
  if (arts_shared_home(cb) != NULL || arts_route_table_live(cb, g)) {
    FAIL("B: a lost install published the handle\n");
  }
  arts_shared_ptr_t lh = arts_route_table_lookup(g);
  if (arts_shared_get(lh) != a) {
    FAIL("B: a lost install replaced the occupant\n");
  }
  arts_shared_release(&lh);
  if (!arts_route_table_set_destroyed_if(g, ha)) {
    FAIL("B: the occupant's retire failed\n");
  }
  /* Empty, but returned: it names the key no longer. */
  if (arts_route_table_install_handle_into(slot, cb, g)) {
    FAIL("B: installed into a returned slot\n");
  }
  if (arts_shared_home(cb) != NULL) {
    FAIL("B: a lost install recorded a home\n");
  }
  arts_shared_abandon(&cb);
  if (deletes() != 0 || *b != 3) {
    FAIL("B: abandoning a lost install's handle ran its deleter\n");
  }
  free(b);
  arts_shared_release(&ha);
  if (deletes() != 1) {
    FAIL("B: %d frees, want the occupant's 1\n", deletes());
  }
  return 0;
}

/* ── C: install_or_get ─────────────────────────────────────────────────── */
static int run_c(void) {
  atomic_store_explicit(&g_deletes, 0, memory_order_relaxed);
  g_drains = 0;

  /* The occupant is pinned; the caller's control block is untouched. */
  arts_guid_t g = arts_guid_reserve(ARTS_GUID_DB, 0);
  int *a = new_obj(4);
  arts_shared_ptr_t ha = install_obj(a, g);
  if (ha == NULL) {
    FAIL("C: the occupant's install lost\n");
  }
  arts_route_item_t *slot = slot_of(g);
  int *b = new_obj(5);
  arts_shared_ptr_t cb = arts_route_table_make_handle(b, g);
  spent_t s = spent_now();
  arts_shared_ptr_t ret = arts_route_table_install_or_get(slot, cb, g);
  SPENT("C: install_or_get on an occupant", s, 0, 1);
  if (ret == cb || ret != ha) {
    FAIL("C: install_or_get on an occupant did not return the occupant\n");
  }
  if (arts_shared_home(cb) != NULL || arts_route_table_live(cb, g)) {
    FAIL("C: install_or_get on an occupant published the caller's handle\n");
  }
  arts_shared_abandon(&cb);
  if (deletes() != 0 || *b != 5) {
    FAIL("C: abandoning the caller's handle ran its deleter\n");
  }
  free(b);
  arts_shared_release(&ret);
  if (!arts_route_table_set_destroyed_if(g, ha)) {
    FAIL("C: the occupant's retire failed\n");
  }
  arts_shared_release(&ha);

  /* An empty slot takes the caller's control block. */
  arts_guid_t g2 = arts_guid_reserve(ARTS_GUID_DB, 0);
  arts_route_item_t *s2 = slot_of(g2);
  int *c = new_obj(6);
  arts_shared_ptr_t cc = arts_route_table_make_handle(c, g2);
  s = spent_now();
  arts_shared_ptr_t r2 = arts_route_table_install_or_get(s2, cc, g2);
  SPENT("C: install_or_get into an empty slot", s, 0, 0);
  if (r2 != cc) {
    FAIL("C: install_or_get into an empty slot returned another handle\n");
  }
  if (!arts_route_table_live(r2, g2) || arts_shared_home(r2) != (void *)s2) {
    FAIL("C: install_or_get's install is not live in its slot\n");
  }
  arts_shared_ptr_t lh = arts_route_table_lookup(g2);
  if (lh != cc) {
    FAIL("C: the key does not publish install_or_get's install\n");
  }
  arts_shared_release(&lh);

  /* A slot that changed hands after the caller resolved it: returned by a
   * retire, so it names the key no longer. */
  arts_guid_t g3 = arts_guid_reserve(ARTS_GUID_DB, 0);
  arts_route_item_t *s3 = slot_of(g3);
  arts_shared_ptr_t h0 = install_obj(new_obj(7), g3);
  if (h0 == NULL || !arts_route_table_set_destroyed_if(g3, h0)) {
    FAIL("C: the earlier generation's install or retire failed\n");
  }
  arts_shared_release(&h0);
  int *d = new_obj(8);
  arts_shared_ptr_t cd = arts_route_table_make_handle(d, g3);
  s = spent_now();
  arts_shared_ptr_t r3 = arts_route_table_install_or_get(s3, cd, g3);
  SPENT("C: install_or_get on a slot that changed hands", s, 1, 0);
  if (r3 != cd || !arts_route_table_live(r3, g3)) {
    FAIL("C: install_or_get on a returned slot did not install\n");
  }
  lh = arts_route_table_lookup(g3);
  if (lh != cd || arts_shared_home(cd) != (void *)slot_of(g3)) {
    FAIL("C: install_or_get's install is not the key's slot's\n");
  }
  arts_shared_release(&lh);

  if (!arts_route_table_set_destroyed_if(g2, r2) ||
      !arts_route_table_set_destroyed_if(g3, r3)) {
    FAIL("C: the retires of install_or_get's installs failed\n");
  }
  arts_shared_release(&r2);
  arts_shared_release(&r3);
  if (deletes() != 4) {
    FAIL("C: %d frees for four installed objects\n", deletes());
  }
  if (g_drains != 4) {
    FAIL("C: %d drains saw their install, want 4\n", g_drains);
  }
  return 0;
}

/* ── D: borrow_or_lookup ───────────────────────────────────────────────── */
static int run_d(void) {
  atomic_store_explicit(&g_deletes, 0, memory_order_relaxed);
  arts_guid_t g = arts_guid_reserve(ARTS_GUID_DB, 0);
  int *a = new_obj(9);
  arts_shared_ptr_t ha = install_obj(a, g);
  if (ha == NULL) {
    FAIL("D: the install lost\n");
  }
  arts_shared_ptr_t looked = ha;
  spent_t s = spent_now();
  void *o = arts_route_table_borrow_or_lookup(ha, g, &looked);
  SPENT("D: the borrow of a live handle", s, 0, 0);
  if (o != a || looked != NULL) {
    FAIL("D: a live handle was not borrowed\n");
  }

  /* Retired, with the key's next generation installed. */
  if (!arts_route_table_set_destroyed_if(g, ha)) {
    FAIL("D: the retire failed\n");
  }
  int *a2 = new_obj(10);
  arts_shared_ptr_t ha2 = install_obj(a2, g);
  if (ha2 == NULL) {
    FAIL("D: the next generation's install lost\n");
  }
  s = spent_now();
  o = arts_route_table_borrow_or_lookup(ha, g, &looked);
  SPENT("D: a retired handle's lookup", s, 1, 1);
  if (o != a2 || looked != ha2) {
    FAIL("D: a retired handle did not resolve to the installed object\n");
  }
  arts_shared_release(&looked);

  o = arts_route_table_borrow_or_lookup(NULL, g, &looked);
  if (o != a2 || looked != ha2) {
    FAIL("D: no handle did not resolve to the installed object\n");
  }
  arts_shared_release(&looked);

  int *u = new_obj(11);
  arts_shared_ptr_t cu = arts_route_table_make_handle(u, g);
  o = arts_route_table_borrow_or_lookup(cu, g, &looked);
  if (o != a2 || looked != ha2) {
    FAIL("D: an unpublished handle did not resolve to the installed "
         "object\n");
  }
  arts_shared_release(&looked);
  arts_shared_abandon(&cu);
  free(u);

  arts_guid_t gm = arts_guid_reserve(ARTS_GUID_DB, 0);
  s = spent_now();
  o = arts_route_table_borrow_or_lookup(NULL, gm, &looked);
  SPENT("D: a miss", s, 1, 0);
  if (o != NULL || looked != NULL) {
    FAIL("D: a miss returned an object\n");
  }

  /* The retired handle still holds its object's last reference. */
  if (deletes() != 0) {
    FAIL("D: borrow_or_lookup released the handle passed in\n");
  }
  arts_shared_release(&ha);
  if (!arts_route_table_set_destroyed_if(g, ha2)) {
    FAIL("D: the next generation's retire failed\n");
  }
  arts_shared_release(&ha2);
  if (deletes() != 2) {
    FAIL("D: %d frees for two generations\n", deletes());
  }
  return 0;
}

/* ── E: every install records the home ─────────────────────────────────── */
static int sentinel_mirror;

static int run_e(void) {
  atomic_store_explicit(&g_deletes, 0, memory_order_relaxed);
  g_drains = 0;

  arts_guid_t g1 = arts_guid_reserve(ARTS_GUID_DB, 0);
  arts_shared_ptr_t cb1 = arts_route_table_make_handle(new_obj(12), g1);
  arts_shared_ptr_t h1 = arts_shared_copy(cb1);
  if (!arts_route_table_install_handle_into(slot_of(g1), cb1, g1) ||
      arts_shared_home(h1) != (void *)slot_of(g1) ||
      !arts_route_table_live(h1, g1)) {
    FAIL("E: install_handle_into did not record its slot\n");
  }

  arts_guid_t g2 = arts_guid_reserve(ARTS_GUID_DB, 0);
  arts_shared_ptr_t cb2 = arts_route_table_make_handle(new_obj(13), g2);
  arts_shared_ptr_t h2 = arts_shared_copy(cb2);
  if (!arts_route_table_install_handle_if_absent(cb2, g2) ||
      arts_shared_home(h2) != (void *)slot_of(g2) ||
      !arts_route_table_live(h2, g2)) {
    FAIL("E: install_handle_if_absent did not record its slot\n");
  }

  int *lost = new_obj(14);
  arts_shared_ptr_t cl = arts_route_table_make_handle(lost, g2);
  if (arts_route_table_install_handle_if_absent(cl, g2) ||
      arts_shared_home(cl) != NULL) {
    FAIL("E: a lost install_handle_if_absent recorded a home\n");
  }
  arts_shared_abandon(&cl);
  free(lost);

  arts_guid_t g3 = arts_guid_reserve(ARTS_GUID_DB, 0);
  arts_shared_ptr_t cb3 = arts_route_table_make_handle(new_obj(15), g3);
  arts_shared_ptr_t h3 = arts_shared_copy(cb3);
  if (!arts_route_table_install_handle_fresh(cb3, g3) ||
      arts_shared_home(h3) != (void *)slot_of(g3) ||
      !arts_route_table_live(h3, g3)) {
    FAIL("E: install_handle_fresh did not record its slot\n");
  }

  arts_route_table_t *mirror = arts_new_route_table(64, 6);
  arts_guid_t gm = arts_guid_reserve(ARTS_GUID_DB, 0);
  arts_route_item_t *item = arts_route_table_search_for_empty(mirror, gm);
  if (!arts_route_item_install_data(item, &sentinel_mirror, NULL)) {
    FAIL("E: install_data lost on an empty mirror slot\n");
  }
  arts_shared_ptr_t hm = arts_route_item_acquire(item);
  if (arts_shared_get(hm) != &sentinel_mirror || arts_shared_home(hm) != NULL) {
    FAIL("E: a mirror slot's install_data recorded a home\n");
  }
  arts_shared_release(&hm);
  (void)arts_clean_up_route_table(mirror, NULL);
  arts_delete_route_table(mirror);

  if (!arts_route_table_set_destroyed_if(g1, h1) ||
      !arts_route_table_set_destroyed_if(g2, h2) ||
      !arts_route_table_set_destroyed_if(g3, h3)) {
    FAIL("E: a retire through the recorded home failed\n");
  }
  arts_shared_release(&h1);
  arts_shared_release(&h2);
  arts_shared_release(&h3);
  if (deletes() != 3) {
    FAIL("E: %d frees for three installed objects\n", deletes());
  }
  if (g_drains != 3) {
    FAIL("E: %d drains saw their install, want 3\n", g_drains);
  }
  return 0;
}

/* ── F: the identity retire through the home ───────────────────────────── */
static int run_f(void) {
  atomic_store_explicit(&g_deletes, 0, memory_order_relaxed);
  arts_guid_t g = arts_guid_reserve(ARTS_GUID_DB, 0);
  arts_shared_ptr_t h1 = install_obj(new_obj(16), g);
  if (h1 == NULL) {
    FAIL("F: the install lost\n");
  }

  spent_t s = spent_now();
  if (arts_route_table_set_destroyed_if(g, NULL)) {
    FAIL("F: a retire of no handle retired something\n");
  }
  SPENT("F: a retire of no handle", s, 0, 0);

  arts_shared_ptr_t stranger = arts_shared_make(NULL, NULL);
  s = spent_now();
  if (arts_route_table_set_destroyed_if(g, stranger)) {
    FAIL("F: a retire by a stranger's identity retired the slot\n");
  }
  SPENT("F: a retire through a handle with no home", s, 1, 0);
  arts_shared_abandon(&stranger);

  s = spent_now();
  if (!arts_route_table_set_destroyed_if(g, h1)) {
    FAIL("F: the retire of a live handle failed\n");
  }
  SPENT("F: the retire of a live handle", s, 0, 0);

  arts_shared_ptr_t h2 = install_obj(new_obj(17), g);
  if (h2 == NULL) {
    FAIL("F: the next generation's install lost\n");
  }
  s = spent_now();
  if (arts_route_table_set_destroyed_if(g, h1) ||
      arts_route_table_set_destroyed_item(g, h1)) {
    FAIL("F: a stale handle retired the next generation\n");
  }
  SPENT("F: the retires of a stale handle", s, 0, 0);
  arts_shared_ptr_t lh = arts_route_table_lookup(g);
  if (lh != h2) {
    FAIL("F: the next generation left its slot\n");
  }
  arts_shared_release(&lh);

  if (!arts_route_table_set_destroyed_item(g, h2)) {
    FAIL("F: the next generation's retire failed\n");
  }
  arts_shared_release(&h1);
  arts_shared_release(&h2);
  if (deletes() != 2) {
    FAIL("F: %d frees for two generations\n", deletes());
  }
  return 0;
}

/* ── G: the home is written only by a won CAS, after it ────────────────── */
static arts_route_item_t *g_move_slot;
static arts_shared_ptr_t g_move_cb;
static int g_window_unrun;
static void *g_home_in_window;

/* Inside an install's window, before its CAS: records the cb's home. */
static void record_home(void) {
  g_home_in_window = arts_shared_home(g_move_cb);
}

/* As record_home, then moves the slot's value word, leaving it empty, so the
 * CAS loses. */
static void record_home_and_move(void) {
  record_home();
  arts_shared_slot_store(&g_move_slot->value, NULL);
}

static int run_g(void) {
  atomic_store_explicit(&g_deletes, 0, memory_order_relaxed);
  g_drains = 0;

  /* A won install: no home before its CAS, the slot after it. */
  arts_guid_t g1 = arts_guid_reserve(ARTS_GUID_DB, 0);
  arts_route_item_t *s1 = slot_of(g1);
  arts_shared_ptr_t c1 = arts_route_table_make_handle(new_obj(19), g1);
  arts_shared_ptr_t h1 = arts_shared_copy(c1);
  g_move_slot = s1;
  g_move_cb = c1;
  g_home_in_window = &g_window_unrun;
  t_before_install_cas = record_home;
  if (!arts_route_table_install_handle_into(s1, c1, g1)) {
    FAIL("G: install_handle_into lost on the key's empty slot\n");
  }
  if (g_home_in_window != NULL) {
    FAIL("G: the home was written before the install's CAS\n");
  }
  if (arts_shared_home(h1) != (void *)s1 || !arts_route_table_live(h1, g1)) {
    FAIL("G: a won install CAS did not write its home\n");
  }

  /* A lost install writes no home. */
  arts_guid_t g = arts_guid_reserve(ARTS_GUID_DB, 0);
  arts_route_item_t *slot = slot_of(g);
  arts_shared_ptr_t cb = arts_route_table_make_handle(new_obj(18), g);
  g_move_slot = slot;
  g_move_cb = cb;
  g_home_in_window = &g_window_unrun;
  t_before_install_cas = record_home_and_move;
  arts_shared_ptr_t pin = arts_shared_copy(cb);
  bool won = arts_route_table_install_handle_into(slot, cb, g);
  arts_shared_release(&pin);
  if (won) {
    FAIL("G: install_handle_into won although its CAS lost\n");
  }
  if (g_home_in_window != NULL) {
    FAIL("G: the home was written before the lost CAS\n");
  }
  if (arts_shared_home(cb) != NULL || arts_route_table_live(cb, g)) {
    FAIL("G: a lost install CAS wrote a home\n");
  }
  if (!arts_shared_slot_empty(&slot->value)) {
    FAIL("G: a lost install CAS published the handle\n");
  }

  /* install_or_get loses its first CAS, retries and installs. */
  g_home_in_window = &g_window_unrun;
  t_before_install_cas = record_home_and_move;
  spent_t s = spent_now();
  arts_shared_ptr_t ret = arts_route_table_install_or_get(slot, cb, g);
  SPENT("G: install_or_get past a lost CAS", s, 0, 0);
  if (g_home_in_window != NULL) {
    FAIL("G: install_or_get's first install did not reach its CAS with no "
         "home\n");
  }
  if (ret != cb || arts_shared_home(ret) != (void *)slot ||
      !arts_route_table_live(ret, g)) {
    FAIL("G: install_or_get did not install after a lost CAS\n");
  }

  if (!arts_route_table_set_destroyed_if(g, ret) ||
      !arts_route_table_set_destroyed_if(g1, h1)) {
    FAIL("G: the retires failed\n");
  }
  arts_shared_release(&ret);
  arts_shared_release(&h1);
  if (deletes() != 2) {
    FAIL("G: %d frees for two installed objects\n", deletes());
  }
  if (g_drains != 2) {
    FAIL("G: %d drains saw their install, want 2\n", g_drains);
  }
  return 0;
}

/* ── H: a retire inside the window between a won CAS and the home store ── */
static arts_guid_t h_key;
static arts_shared_ptr_t h_holder;
static bool h_live_in_window;
static bool h_retired_in_window;
static spent_t h_spent_in_window;

/* Inside the window: the pre-install holder's copy, then a retire of it. */
static void retire_in_window(void) {
  h_live_in_window = arts_route_table_live(h_holder, h_key);
  spent_t s = spent_now();
  h_retired_in_window = arts_route_table_set_destroyed_if(h_key, h_holder);
  spent_t e = spent_now();
  h_spent_in_window.probes = e.probes - s.probes;
  h_spent_in_window.pins = e.pins - s.pins;
}

static int run_h(void) {
  atomic_store_explicit(&g_deletes, 0, memory_order_relaxed);
  h_key = arts_guid_reserve(ARTS_GUID_DB, 0);
  arts_route_item_t *slot = slot_of(h_key);
  arts_shared_ptr_t cb = arts_route_table_make_handle(new_obj(20), h_key);
  h_holder = arts_shared_copy(cb);
  h_live_in_window = true;
  h_retired_in_window = false;
  t_after_install_cas = retire_in_window;
  if (!arts_route_table_install_handle_into(slot, cb, h_key)) {
    FAIL("H: install_handle_into lost on the key's empty slot\n");
  }
  if (t_after_install_cas != NULL) {
    FAIL("H: the window between the install CAS and its home store never "
         "ran\n");
  }
  if (h_live_in_window) {
    FAIL("H: the pre-install holder's copy was live before the home store\n");
  }
  if (!h_retired_in_window) {
    FAIL("H: the retire inside the window did not take the cb\n");
  }
#if ENABLE_NUM_ROUTE_PROBE
  if (h_spent_in_window.probes != 1) {
    FAIL("H: the retire inside the window spent %" PRIu64
         " probes, want the search's 1\n",
         h_spent_in_window.probes);
  }
#endif
#if ENABLE_NUM_ROUTE_PIN
  if (h_spent_in_window.pins != 0) {
    FAIL("H: the retire inside the window spent %" PRIu64 " pins\n",
         h_spent_in_window.pins);
  }
#endif
  if (arts_shared_home(h_holder) != (void *)slot) {
    FAIL("H: the install did not store its home after the retire\n");
  }
  if (arts_route_table_live(h_holder, h_key)) {
    FAIL("H: a cb retired inside its install is live\n");
  }
  spent_t s = spent_now();
  if (arts_route_table_set_destroyed_if(h_key, h_holder)) {
    FAIL("H: a second retire took the cb\n");
  }
  SPENT("H: the second retire", s, 0, 0);
  if (atomic_load_explicit(&slot->key, memory_order_acquire) != 0) {
    FAIL("H: the retire did not return the slot\n");
  }
  if (deletes() != 0) {
    FAIL("H: the object was freed under the holder's reference\n");
  }
  arts_shared_release(&h_holder);
  if (deletes() != 1) {
    FAIL("H: %d frees at the last reference, want 1\n", deletes());
  }
  return 0;
}

/* ── J: the teardown sweep leaves no cb that reads as live ─────────────── */
static int run_j(void) {
  atomic_store_explicit(&g_deletes, 0, memory_order_relaxed);
  arts_route_table_t *table = arts_new_route_table(64, 6);
  arts_guid_t g = arts_guid_reserve(ARTS_GUID_DB, 0);
  arts_route_item_t *item = arts_route_table_search_for_empty(table, g);
  arts_shared_ptr_t cb = arts_route_table_make_handle(new_obj(22), g);
  arts_shared_ptr_t holder = arts_shared_copy(cb);
  if (!arts_route_table_install_handle_into(item, cb, g) ||
      !arts_route_table_live(holder, g)) {
    FAIL("J: the install into the swept table's slot failed\n");
  }
  if (arts_clean_up_route_table(table, NULL) != 0) {
    FAIL("J: the sweep kept an object\n");
  }
  if (arts_route_table_live(holder, g)) {
    FAIL("J: a cb the sweep detached is live\n");
  }
  if (deletes() != 0) {
    FAIL("J: the sweep freed the object under the holder's reference\n");
  }
  arts_shared_release(&holder);
  if (deletes() != 1) {
    FAIL("J: %d frees at the holder's release, want 1\n", deletes());
  }
  arts_delete_route_table(table);
  return 0;
}

#ifndef NDEBUG
/* ── I: an install with no reference of the caller's own (child) ───────── */
static void child_unheld_install(void) {
  arts_guid_t g = arts_guid_reserve(ARTS_GUID_DB, 0);
  arts_shared_ptr_t cb = arts_route_table_make_handle(new_obj(21), g);
  (void)arts_route_table_install_handle_into(slot_of(g), cb, g);
  (void)fprintf(stderr, "child: the unheld install returned\n");
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

static int run_i(void) {
  static char out[8192];
  const char *why =
      run_child(child_unheld_install, "without a reference of the caller's own",
                out, sizeof(out));
  if (why != NULL) {
    (void)fputs(out, stderr);
    FAIL("I: %s\n", why);
  }
  return 0;
}
#endif

int main(void) {
  rt_init();
  arts_route_table_register_deleter(ARTS_GUID_DB, test_deleter);

#ifndef NDEBUG
  if (run_i() != 0) {
    return 1;
  }
#endif
  if (run_a() != 0 || run_b() != 0 || run_c() != 0 || run_d() != 0 ||
      run_e() != 0 || run_f() != 0 || run_g() != 0 || run_h() != 0 ||
      run_j() != 0) {
    return 1;
  }

  free(arts_node_info.keys[0]);
  free(arts_node_info.keys);
  arts_node_info.keys = NULL;
  arts_guid_generator_cleanup();

  printf(
      "PASS route_handle_live: a control block knows its slot, liveness "
      "follows install and retire, install_handle_into is one-shot, "
      "install_or_get installs or pins the occupant, borrow_or_lookup "
      "borrows a live handle, the identity retire goes through the home, "
      "only a won install CAS writes the home, a retire inside the install's "
      "window goes through the search, an unheld install is caught in "
      "Debug\n");
  return 0;
}
