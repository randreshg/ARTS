/* SPDX-License-Identifier: Apache-2.0
 *
 * The home's publication waiters, under the cells whose home holds the
 * canonical bytes (VAL and INV with the HOME read source).  A block a remote
 * creator made has its home descriptor installed unpublished: the home's
 * buffer exists at version 0.  A read acquire by an EDT on the home's own rank
 * finds nothing to serve and parks on the descriptor's publication waiters,
 * with no request of its own; the creator's first publication, arriving as
 * the commit leg of a publish, drains them.
 *
 *   - the park leaves nothing on the wire, takes no hold on a buffer and
 *     resolves nothing;
 *   - the commit leg's payload pairing stamps the version, wakes the waiter
 *     exactly once, and the slot it is woken with holds the published bytes;
 *   - the wake is counted by the census the way a serve of that rank's
 *     acquire is: remote where the copy comes from the publisher's rank, local
 *     where the home holds the write right (PURGE);
 *   - the publisher's release is acknowledged once, to the rank that
 *     published;
 *   - an acquire after the publication is a local hit and parks nothing.
 *
 * PURE test: no arts_rt, no ports, no threads.
 */
#include "coherence/coherence.c"
#include "coherence/grant.c"
#include "coherence/grant_queue.c"
#include "coherence/grantreq_queue.c"
#include "coherence/handlers.c"
#include "coherence/rank_bitset.c"
#include "coherence/rank_u64_map.c"
#include "coherence/reader_pool.c"
#include "coherence/senders.c"
#ifdef ARTS_READ_SOURCE_OWNER
#include "coherence/grant_owner.c"
#else
#include "coherence/grant_home.c"
#endif
#ifdef ARTS_RETENTION_PURGE
#include "coherence/grant_purge.c"
#else
#include "coherence/grant_retain.c"
#endif
#ifdef ARTS_VALIDITY_INV
#include "coherence/inv/directory.c"
#include "coherence/inv/engine.c"
#ifdef ARTS_READ_SOURCE_OWNER
#include "coherence/inv/owner.c"
#else
#include "coherence/inv/home.c"
#endif
#else
#ifdef ARTS_READ_SOURCE_OWNER
#include "coherence/val/owner.c"
#else
#include "coherence/val/home.c"
#endif
#endif
#include "utils/shared.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HOME 0u
#define REQ 1u
#define SIZE 64u
#define GUID_SIZED                                                             \
  ARTS_GUID_MAKE(ARTS_GUID_DB, HOME, ARTS_GUID_DB_KEY(1u, 0x61u))
#define PUBLISH_TXID 0x77u
#define PATTERN 0x5au

unsigned int arts_global_rank_id = HOME;
unsigned int arts_global_rank_count = 2u;
uint64_t arts_guid_table_count;
struct arts_runtime_shared_s arts_node_info;
ARTS_THREAD_LOCAL_IE struct arts_runtime_private_s arts_thread_info;

#include "arts/counter/counter.h"
#include "arts/system/shutdown.h"

/* The census counters the wakes and hits move, seen through the counter
 * entry the runtime's increments resolve to. */
ARTS_THREAD_LOCAL_IE arts_counter_t
    arts_thread_local_counters[NUM_COUNTER_TYPES];
static uint64_t g_remote;
static uint64_t g_local_hit;
void arts_counter_increment_by(arts_counter_t *counter, uint64_t num) {
  if (counter == &arts_thread_local_counters[NUM_DB_ACQUIRE_REMOTE]) {
    g_remote += num;
  } else if (counter == &arts_thread_local_counters[NUM_DB_ACQUIRE_LOCAL_HIT]) {
    g_local_hit += num;
  }
}

static const char *g_scenario = "setup";

static void fail(const char *what) {
  printf("FAIL db_publication_waiter_census: %s: %s\n", g_scenario, what);
  fflush(stdout);
  _exit(1);
}

_Noreturn void arts_exit(int rc, enum arts_exit_origin origin) {
  (void)origin;
  printf("FAIL db_publication_waiter_census: %s: the runtime aborted\n",
         g_scenario);
  fflush(stdout);
  _exit(rc ? rc : 1);
}
void *arts_malloc(size_t n) { return malloc(n); }
void *arts_calloc(size_t n, size_t s) { return calloc(n, s); }
void *arts_malloc_aligned(size_t n, size_t a) {
  return aligned_alloc(a, (n + a - 1u) / a * a);
}
void arts_free(void *p) { free(p); }

unsigned int arts_guid_get_rank(arts_guid_t g) {
  (void)g;
  return HOME;
}
#ifdef ARTS_DEBUG_INJECTION
void arts_sched_fuzz_point(void) {}
bool arts_net_send_held(int rank, const char *message, unsigned int length,
                        int kind) {
  (void)rank;
  (void)message;
  (void)length;
  (void)kind;
  return false;
}
#endif

/* Everything the scenarios never reach. */
#define UNREACHED(sig)                                                         \
  sig {                                                                        \
    printf("FAIL db_publication_waiter_census: %s: %s reached\n", g_scenario,  \
           __func__);                                                          \
    fflush(stdout);                                                            \
    _exit(1);                                                                  \
  }
UNREACHED(bool arts_db_create_install_local(arts_shared_ptr_t cb,
                                            struct arts_route_item_s *s))
UNREACHED(void arts_db_free(void *ptr))
UNREACHED(void arts_db_buf_ref_release_cb(void *arg))
UNREACHED(void arts_net_put_payload(int r, uint64_t a, uint64_t k, uint64_t t,
                                    const void *s, uint64_t l,
                                    void (*cb)(void *), void *arg))
UNREACHED(void arts_transport_send_payload_async_free(
    int r, char *m, unsigned int n, char *p, unsigned int o, uint64_t s,
    void (*f)(void *)))
UNREACHED(void arts_ooo_dispatch_or_defer_guid(arts_guid_t g, ooo_kind_t k,
                                               const void *a, uint32_t s))
UNREACHED(arts_shared_ptr_t arts_route_table_lookup(arts_guid_t g))
UNREACHED(bool arts_route_table_install_handle_into(arts_route_item_t *s,
                                                    arts_shared_ptr_t cb,
                                                    arts_guid_t k))
UNREACHED(arts_shared_ptr_t arts_route_table_install_or_get(
    arts_route_item_t *s, arts_shared_ptr_t cb, arts_guid_t k))
UNREACHED(arts_shared_ptr_t arts_route_table_make_handle(void *o,
                                                         arts_guid_t k))
UNREACHED(bool arts_route_table_live(arts_shared_ptr_t h, arts_guid_t k))
UNREACHED(void arts_route_table_iterator_reset(arts_route_table_iterator_t *i,
                                               arts_route_table_t *t))
UNREACHED(
    arts_route_item_t *arts_route_table_iterate(arts_route_table_iterator_t *i))
UNREACHED(unsigned int arts_route_table_parked_count(arts_route_table_t *t))
UNREACHED(bool arts_edt_admitted(arts_shared_ptr_t h))
UNREACHED(bool arts_ooo_retire(arts_guid_t key, arts_shared_ptr_t cb))
UNREACHED(struct arts_db_buffer_s *arts_db_buf_install(
    struct arts_db_cache_s *c, uint64_t v, const void *d, uint64_t s))
/* The publish credit an acknowledgement teaches the releaser. */
bool arts_net_rdzv_local(const void *p, uint64_t len, uint64_t *a,
                         uint64_t *k) {
  (void)p;
  (void)len;
  *a = 0x1000u;
  *k = 1u;
  return true;
}

/* ---- the buffer plane: the slot is real, a landing is a heap stand-in ---- */

static int64_t g_landings; /* drawn and not yet ended */
static uint64_t g_txid;

arts_shared_ptr_t arts_db_buf_acquire(struct arts_db_cache_s *cache) {
  return arts_shared_slot_load(&cache->buffer);
}
void arts_db_buf_release(arts_shared_ptr_t *h) { arts_shared_release(h); }
bool arts_db_buf_ensure(struct arts_db_cache_s *cache, uint64_t db_size) {
  (void)cache;
  (void)db_size;
  return false;
}
uint64_t arts_db_first_fetch_size(const struct arts_db_cache_s *cache) {
  uint64_t s = atomic_load_explicit(&cache->db_size, memory_order_relaxed);
  return s != 0u ? s : arts_db_szhint_bound(cache->db_guid);
}
struct arts_db_buffer_s *
arts_db_buf_landing_alloc(uint64_t db_size, struct arts_rdzv_landing_s *out) {
  struct arts_db_buffer_s *b =
      (struct arts_db_buffer_s *)calloc(1, sizeof(*b) + db_size);
  g_landings++;
  out->addr = 0x1000u;
  out->key = 1u;
  out->txid = ++g_txid;
  out->cookie = (uint64_t)(uintptr_t)b;
  return b;
}
static void landing_end(struct arts_db_buffer_s *b) {
  if (b == NULL) {
    fail("a landing ended that was never drawn");
  }
  free(b);
  g_landings--;
}
void arts_db_buf_landing_return(struct arts_db_buffer_s *b) { landing_end(b); }
bool arts_db_buf_adopt_landing(struct arts_db_cache_s *c, uint64_t v,
                               struct arts_db_buffer_s *l, uint64_t s) {
  (void)c;
  (void)v;
  (void)s;
  landing_end(l);
  return true;
}
struct arts_db_buffer_s *arts_db_buf_install_landed(struct arts_db_cache_s *c,
                                                    uint64_t v,
                                                    struct arts_db_buffer_s *l,
                                                    uint64_t s) {
  (void)c;
  (void)s;
  (void)v;
  landing_end(l);
  return NULL;
}
#ifdef ARTS_DEBUG_LANDING_BALANCE
int64_t arts_db_landings_outstanding(void) { return g_landings; }
#endif

/* ---- the rendezvous plane: an expectation held for the test to fire ---- */

static void (*g_expect_cb)(void *);
static void (*g_expect_discard)(void *);
static void *g_expect_arg;

void arts_net_rdzv_expect(uint64_t txid, uint64_t len, void (*on_data)(void *),
                          void (*on_discard)(void *), void *arg) {
  (void)txid;
  (void)len;
  if (g_expect_arg != NULL) {
    fail("a second expectation while one is held");
  }
  g_expect_cb = on_data;
  g_expect_discard = on_discard;
  g_expect_arg = arg;
}
uint64_t arts_net_rdzv_txid_next(void) { return ++g_txid; }

static void pairing_fire(bool data) {
  void *arg = g_expect_arg;
  void (*f)(void *) = data ? g_expect_cb : g_expect_discard;
  g_expect_arg = NULL;
  if (arg == NULL || f == NULL) {
    fail("no expectation to fire");
  }
  f(arg);
}

/* The stamp a landed publish makes in place on the home's stable buffer. */
void arts_db_buf_bump_inplace(struct arts_db_cache_s *c, uint64_t v) {
  arts_shared_ptr_t h = arts_db_buf_acquire(c);
  struct arts_db_buffer_s *b = (struct arts_db_buffer_s *)arts_shared_get(h);
  if (b == NULL) {
    fail("a version stamp with no buffer installed");
  }
  if (atomic_load_explicit(&b->version, memory_order_relaxed) >= v) {
    fail("a version stamp that does not move the version up");
  }
  atomic_store_explicit(&b->version, v, memory_order_release);
  arts_db_buf_release(&h);
}

/* ---- the wire: messages are captured ---- */

#define MAXSENT 16
static struct {
  int rank;
  unsigned int type;
  _Alignas(8) char buf[256];
} g_sent[MAXSENT];
static int g_nsent;

void arts_transport_send_async(int rank, char *message, unsigned int length) {
  if (g_nsent == MAXSENT || length > sizeof(g_sent[0].buf)) {
    fail("too many messages");
  }
  g_sent[g_nsent].rank = rank;
  g_sent[g_nsent].type = ((struct arts_msg_header_s *)message)->message_type;
  memcpy(g_sent[g_nsent].buf, message, length);
  g_nsent++;
}

static int count_sent(unsigned int type) {
  int n = 0;
  for (int i = 0; i < g_nsent; i++) {
    n += (g_sent[i].type == type);
  }
  return n;
}

/* The scenarios' descriptor is always installed: a self-send borrows or
 * copies it, as the route table does for a live handle. */
void *arts_route_table_borrow_or_lookup(arts_shared_ptr_t h, arts_guid_t g,
                                        arts_shared_ptr_t *l) {
  *l = NULL;
  if (h == NULL || arts_shared_tag(h) != (uint64_t)g) {
    fail("a borrow of a descriptor that is not installed");
  }
  return arts_shared_get(h);
}
arts_shared_ptr_t arts_route_table_copy_live(arts_shared_ptr_t h,
                                             arts_guid_t g) {
  if (h == NULL || arts_shared_tag(h) != (uint64_t)g) {
    fail("a copy of a descriptor that is not installed");
  }
  return arts_shared_copy(h);
}

void arts_ooo_dispatch_on(arts_shared_ptr_t h, ooo_kind_t kind,
                          arts_guid_t guid, const void *args, uint32_t size) {
  (void)h;
  (void)kind;
  (void)guid;
  (void)args;
  (void)size;
  fail("a self-send");
}

/* ---- the EDT side: a parked waiter's EDT and its one dependence ---- */

struct fake_edt {
  struct arts_edt_s edt;
  arts_edt_dep_t depv[1];
  struct arts_dep_acq_s acq[1];
};
static struct fake_edt *g_edts[4];
static int g_nedts;
static int g_wakes;

void *arts_get_depv(void *edt_ptr) {
  return ((struct fake_edt *)edt_ptr)->depv;
}
bool arts_dep_is_serialized(const arts_edt_dep_t *depv, uint32_t i) {
  (void)depv;
  (void)i;
  return false;
}
void arts_db_fill_aliases(struct arts_edt_s *edt, unsigned int slot) {
  (void)edt;
  (void)slot;
}
bool arts_db_rw_secure(struct arts_edt_s *edt, unsigned int slot) {
  (void)edt;
  (void)slot;
  return false;
}
void arts_db_drain_resume_list(void) {}
/* The slot's resolution, whichever frame accounts it: the wake of a parked
 * waiter, or an acquire that hit. */
void arts_db_acquire_account(struct arts_edt_s *edt) {
  (void)edt;
  g_wakes++;
}
void arts_db_acquire_resolved(struct arts_edt_s *edt, unsigned int slot) {
  (void)edt;
  (void)slot;
  g_wakes++;
}
#ifndef NDEBUG
void arts_dep_acq_resolve_once(struct arts_edt_s *edt, uint32_t slot) {
  (void)edt;
  (void)slot;
}
#endif
#ifdef ARTS_DEBUG_WAKE_GUID
void arts_dep_waiter_check(const struct arts_edt_s *edt, arts_guid_t edt_guid,
                           unsigned int slot) {
  (void)slot;
  if (edt->guid != edt_guid) {
    fail("a waiter woke another EDT");
  }
}
#endif

/* ---- the descriptor and the oracle ---- */

static struct arts_db_s *g_db;
static int g_deleted;

static void db_deleter(void *p) {
  if (p != g_db) {
    fail("the deleter ran on another object");
  }
  g_deleted++;
  arts_db_cache_destructor(&g_db->cache);
  free(g_db);
}

static void buf_free(void *p) { free(p); }

/* The block's home descriptor, as the create handler builds one for a creator
 * on another rank: its buffer installed at version 0 (unpublished).  The
 * control block holds the test's reference, which stands for the install's. */
static void begin(const char *name) {
  g_scenario = name;
  arts_global_rank_id = HOME;
  size_t sz = (sizeof(struct arts_db_s) + 63u) & ~(size_t)63u;
  g_db = (struct arts_db_s *)aligned_alloc(64, sz);
  memset(g_db, 0, sz);
  g_db->db_type = ARTS_DB;
  arts_db_cache_init(&g_db->cache, GUID_SIZED, SIZE, ARTS_DB_INIT_HOME_RECV,
                     REQ);
  g_db->self_cb = arts_shared_make(g_db, db_deleter);
  arts_shared_set_tag(g_db->self_cb, (uint64_t)GUID_SIZED);
  struct arts_db_buffer_s *b =
      (struct arts_db_buffer_s *)calloc(1, sizeof(*b) + SIZE);
  b->cb = arts_shared_make(b, buf_free);
  arts_shared_slot_store(&g_db->cache.buffer, b->cb);
  g_deleted = 0;
  g_nsent = 0;
  g_wakes = 0;
  g_nedts = 0;
  g_expect_arg = NULL;
  g_remote = 0;
  g_local_hit = 0;
}

static struct arts_db_buffer_s *home_buffer(void) {
  arts_shared_ptr_t h = arts_db_buf_acquire(&g_db->cache);
  struct arts_db_buffer_s *b = (struct arts_db_buffer_s *)arts_shared_get(h);
  arts_db_buf_release(&h);
  return b;
}

/* The references the home's buffer carries beyond the slot's own view of it:
 * a park takes none, and a woken EDT's hold is returned by its release. */
static int64_t buf_refs(void) {
#ifndef NDEBUG
  return arts_shared_debug_refs(home_buffer()->cb);
#else
  return 0;
#endif
}

static void buf_refs_are(int64_t want, const char *what) {
  if (buf_refs() != want) {
    fail(what);
  }
}

/* The buffer hold a woken EDT's slot took, which the EDT's release drops. */
static void release_hold(struct fake_edt *f) {
  struct arts_db_buffer_s *b = arts_db_buf_from_data(f->acq[0].ptr);
  f->acq[0].ptr = NULL;
  arts_shared_ptr_t h = b->cb;
  arts_db_buf_release(&h);
}

static void end(void) {
  if (g_expect_arg != NULL) {
    fail("an expectation left unfired");
  }
  if (g_deleted != 0) {
    fail("a count was dropped that was never taken");
  }
  arts_shared_ptr_t h = g_db->self_cb;
  arts_shared_release(&h);
  if (g_deleted != 1) {
    fail("a count was left behind");
  }
  if (g_landings != 0) {
    fail("a landing was left out");
  }
  for (int i = 0; i < g_nedts; i++) {
    if (g_edts[i]->acq[0].ptr != NULL) {
      fail("an EDT's buffer hold was never released");
    }
    free(g_edts[i]);
  }
  printf("ok   %s\n", g_scenario);
}

/* A read acquire by an EDT of this rank, through the arm's acquire body. */
static struct fake_edt *acquire_ro(void) {
  struct fake_edt *f = (struct fake_edt *)calloc(1, sizeof(*f));
  f->edt.guid = 0xed0u + (arts_guid_t)g_nedts;
  f->edt.depc = 1u;
  f->edt.dep_acq = f->acq;
  f->depv[0].guid = g_db->cache.db_guid;
  f->depv[0].mode = DB_MODE_RO;
  f->acq[0].pin = g_db->self_cb; /* borrowed, as the firing frame's */
  g_edts[g_nedts++] = f;
  struct arts_ooo_args_db_acquire_s a = {
      .edt = &f->edt, .db_guid = g_db->cache.db_guid, .slot = 0u};
  arts_handler_db_acquire(g_db, &a);
  return f;
}

static bool parked(void) {
  return atomic_load_explicit(&g_db->cache.publication_waiters.head,
                              memory_order_relaxed) != NULL;
}

static void check_bytes(const struct fake_edt *f) {
  const char *p = (const char *)f->acq[0].ptr;
  if (p == NULL || p != home_buffer()->data) {
    fail("the slot does not hold the home's published buffer");
  }
  for (unsigned int i = 0; i < SIZE; i++) {
    if ((unsigned char)p[i] != PATTERN) {
      fail("the slot does not hold the published bytes");
    }
  }
}

/* The creator's first publication, as the home's publish handler takes its
 * commit leg: the bytes landed in place, the pairing fires. */
static void first_publication(void) {
  struct arts_ooo_args_db_publish_s a = {
      .releaser = REQ,
      .db_guid = GUID_SIZED,
      .version = 1u,
      .cv = 0u,
      .data_size = SIZE,
      .rdzv_txid = PUBLISH_TXID,
      .rdzv_cookie = 0u,
      .returns_grant = 0u,
      .token = 0x7000u,
  };
  memset(home_buffer()->data, PATTERN, SIZE);
  arts_handler_db_publish(g_db, &a);
  if (g_wakes != 0) {
    fail("a waiter woke before the payload's pairing");
  }
  void *arg = g_expect_arg;
  g_expect_arg = NULL;
  if (arg == NULL) {
    fail("the commit leg held no expectation");
  }
  g_expect_cb(arg);
}

static void census_is(uint64_t remote, uint64_t local_hit) {
#if ARTS_DB_CENSUS
  if (g_remote != remote || g_local_hit != local_hit) {
    char m[128];
    snprintf(m, sizeof(m),
             "census remote %llu local hit %llu, expected %llu "
             "and %llu",
             (unsigned long long)g_remote, (unsigned long long)g_local_hit,
             (unsigned long long)remote, (unsigned long long)local_hit);
    fail(m);
  }
#else
  (void)remote;
  (void)local_hit;
#endif
}

/* The first publication wakes the home-rank reader that parked before it,
 * once, with the published bytes. */
static void reader_before_publication(void) {
  begin("a home-rank read parks until the first publication");
  int64_t refs0 = buf_refs();
  struct fake_edt *f = acquire_ro();
  if (g_wakes != 0 || !parked() || f->acq[0].ptr != NULL || g_nsent != 0) {
    fail("the read did not park on the publication waiters, silent");
  }
  buf_refs_are(refs0, "the park took a hold on the buffer");
  census_is(0u, 0u);
  first_publication();
  if (g_wakes != 1 || parked()) {
    fail("the first publication did not wake the waiter exactly once");
  }
  check_bytes(f);
  if (atomic_load_explicit(&home_buffer()->version, memory_order_acquire) !=
      1u) {
    fail("the first publication stamped another version");
  }
  /* Counted from the publisher's rank: remote, but where the home holds the
   * write right the home's own rank counts it local. */
#ifdef ARTS_RETENTION_PURGE
  census_is(0u, 1u);
#else
  census_is(1u, 0u);
#endif
  if (count_sent(MSG_DB_PUBLISH_ACK) != 1) {
    fail("the publisher's release was not acknowledged exactly once");
  }
  for (int i = 0; i < g_nsent; i++) {
    if (g_sent[i].type == MSG_DB_PUBLISH_ACK && g_sent[i].rank != (int)REQ) {
      fail("the acknowledgement left for another rank than the publisher");
    }
  }
  buf_refs_are(refs0 + 1, "the wake took no hold on the buffer");
  release_hold(f);
  buf_refs_are(refs0, "the release did not return the wake's hold");

  /* A read after the publication hits at once and parks nothing. */
  struct fake_edt *g = acquire_ro();
  if (g_wakes != 2 || parked()) {
    fail("a read after the publication did not resolve at once");
  }
  check_bytes(g);
#ifdef ARTS_RETENTION_PURGE
  census_is(0u, 2u);
#else
  census_is(1u, 1u);
#endif
  release_hold(g);
  buf_refs_are(refs0, "the second release did not return its hold");
  end();
}

int main(void) {
  reader_before_publication();
  printf("PASS db_publication_waiter_census\n");
  return 0;
}
