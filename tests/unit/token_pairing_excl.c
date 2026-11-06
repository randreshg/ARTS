/* SPDX-License-Identifier: Apache-2.0
 *
 * On EXCL each open request axis of a cache -- the read request and the
 * write request -- owns one count on the requester's descriptor: the CAS
 * that opens the axis takes it before the request leaves, and whichever frame
 * commits the transition that closes the axis drops it once, at its end
 * (coherence.h).  An answer acts on the descriptor its token names, and on a
 * retired one drops that count and does nothing else.  Under RETAIN the home
 * also holds one count on its own descriptor while a write request it took in
 * is unconfirmed (w != 0): every write request takes it before its CAS and
 * gives it back unless that CAS lifted w off zero, and the EXCL_CONFIRM whose
 * CAS returns w to zero drops it; a read grant the home serves holds none.
 *
 * Pairing oracle.  Each scenario builds a descriptor whose control block
 * carries the test's one reference and a counting deleter, drives the
 * transitions through the real code (the arm's acquire, the home's request
 * body, the answers' entries, the rendezvous callbacks, a create's hold, the
 * teardown sweep), then drops its reference and requires the deleter to run
 * exactly then: a count left behind keeps it from running, a count dropped
 * twice runs it before.  The wire is stubbed: a request is captured, and its
 * answer is built from it, so every answer echoes the token its request
 * carried.  A request to this rank's own home runs its body inline under a
 * count of its own, as the wire's runs under the dispatcher's pin: where the
 * last reference goes inside such a body, the deleter may run only once the
 * body has returned, and nothing may touch the descriptor afterwards.  Two
 * scenarios remove, inside such a body, every reference but the self-sends'
 * own, the walk's slot pin included, which the runtime holds across the send.
 * They check that the copies alone keep the bodies, not a schedule the runtime
 * can reach, so their poison check forbids the acquire from touching the
 * descriptor after its send, which is stricter than its pin requires.
 *
 * Scenarios, in every EXCL cell:
 *   read axis   closed by its grant, by a dismiss while the rank's write
 *               request is out, by a create's hold; an answer and a dismiss
 *               on a retired descriptor;
 *   write axis  closed by its grant (under RETAIN the migration's DELIVER,
 *               whose CONFIRM echoes the home's token), a size round leaving
 *               it open, by a create's hold;
 *   a one-sided answer's pairing callback, live and retired, and a pairing
 *   the teardown discards (the count is then the sweep's), where answers
 *   carry bytes; bytes in flight to a requester already retired: the count
 *   goes after they land, or with the discard, which owns it;
 *   the teardown sweep: one count per open axis.
 * RETAIN:
 *   a read grant the home serves, retained and never recalled, holds no home
 *   count; w 0->1 takes one and the EXCL_CONFIRM releases it, through a chain
 *   of two migrations too; a CONFIRM on a retired home takes the counting
 *   step and releases; the count is held before the CAS that lifts w, so a
 *   migration another frame starts and confirms at that point cannot free
 *   the home under the request frame (injection trees); the home's own write
 *   request granted by itself, as the resident owner, inline, and again with
 *   every reference held outside the self-sent bodies removed at the wake
 *   inside the request's body; a resident owner's own request granted by its
 *   FORWARD, the migration's landing and tokens consumed; a self-grant with no
 *   write request out is fatal; a FORWARD that finds no block: a read is
 *   dismissed with its token, a write is dropped; a held read its rank's write
 *   turn covered is dismissed by the CONFIRM that finds the home retired; the
 *   sweep's w row.
 * PURGE:
 *   the home's own write request served by its self-grant when the
 *   creator's write turn comes back, and, that turn back first, granted inside
 *   its self-sent body with every reference held outside that body removed at
 *   the wake; its own read request granted from the read drain that return
 *   opens; a read its rank's write turn covered, refused by the torn-down home,
 *   is dismissed.
 *
 * PURE test: no arts_rt, no ports, no threads.
 */
#include "coherence/coherence.c"
#include "coherence/excl/directory.c"
#include "coherence/grantreq_queue.c"
#include "coherence/rank_bitset.c"
#ifdef ARTS_RELEASE_RETAIN
#include "coherence/excl/retain.c"
#else
#include "coherence/excl/purge.c"
#endif
#include "utils/shared.c"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#endif

#if defined(ARTS_RELEASE_RETAIN) || !defined(ARTS_USE_CXL)
#define PAIRS 1 /* answers can carry one-sided bytes */
#else
#define PAIRS 0
#endif

#define HOME 0u
#define REQ 1u
#define OTHER 2u
#define READER 3u
/* A block whose GUID bounds no bytes (a request sends no landing), and one
 * whose GUID bounds no size at all (a first touch is a claimable stub). */
#define GUID_SIZED                                                             \
  ARTS_GUID_MAKE(ARTS_GUID_DB, HOME, ARTS_GUID_DB_KEY(0u, 0x61u))
#define GUID_CLAIM                                                             \
  ARTS_GUID_MAKE(ARTS_GUID_DB, HOME,                                           \
                 ARTS_GUID_DB_KEY(ARTS_GUID_DB_SZHINT_NONE, 0x62u))
/* A requester's token on another rank, opaque to the home. */
#define FAR_TOKEN ((uint64_t)0xfa400u)

unsigned int arts_global_rank_id = REQ;
unsigned int arts_global_rank_count = 4u;
uint64_t arts_guid_table_count;
struct arts_runtime_shared_s arts_node_info;
struct arts_shutdown_abandon_s arts_shutdown_abandon;
ARTS_THREAD_LOCAL_IE struct arts_runtime_private_s arts_thread_info;

#include "arts/system/shutdown.h"
#include "counter_stubs.h"

static const char *g_scenario = "setup";

static void fail(const char *what) {
  printf("FAIL token_pairing_excl: %s: %s\n", g_scenario, what);
  fflush(stdout);
  _exit(1);
}

/* Set in a child a scenario forks to see a fatal check fire. */
static bool g_expect_abort;
#define EXPECTED_ABORT 42

_Noreturn void arts_exit(int rc, enum arts_exit_origin origin) {
  (void)origin;
  if (g_expect_abort) {
    _exit(EXPECTED_ABORT);
  }
  printf("FAIL token_pairing_excl: %s: the runtime aborted\n", g_scenario);
  fflush(stdout);
  _exit(rc ? rc : 1);
}

/* Runs once at the next allocation: a step another thread takes inside a
 * window of the code under test. */
static void (*g_malloc_hook)(void);
void *arts_malloc(size_t n) {
  void (*hook)(void) = g_malloc_hook;
  g_malloc_hook = NULL;
  if (hook != NULL) {
    hook();
  }
  return malloc(n);
}
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
/* Runs at the next fuzz point where its own test says so: the steps another
 * thread takes at a decision-commit point of the code under test. */
static bool (*g_fuzz_hook)(void);
void arts_sched_fuzz_point(void) {
  bool (*hook)(void) = g_fuzz_hook;
  if (hook != NULL && hook()) {
    g_fuzz_hook = NULL;
  }
}
bool arts_net_send_held(int rank, const char *message, unsigned int length,
                        int kind, arts_guid_t guid) {
  (void)rank;
  (void)message;
  (void)length;
  (void)kind;
  (void)guid;
  return false;
}
#endif

/* Everything the scenarios never reach. */
#define UNREACHED(sig)                                                         \
  sig {                                                                        \
    printf("FAIL token_pairing_excl: %s: %s reached\n", g_scenario, __func__); \
    fflush(stdout);                                                            \
    _exit(1);                                                                  \
  }
UNREACHED(bool arts_db_create_install_local(arts_shared_ptr_t cb,
                                            struct arts_route_item_s *s,
                                            bool f))
UNREACHED(void arts_db_free(void *ptr))
UNREACHED(void arts_db_buf_ref_release_cb(void *arg))
UNREACHED(void arts_net_put_payload(int r, uint64_t a, uint64_t k, uint64_t t,
                                    const void *s, uint64_t l,
                                    void (*cb)(void *), void *arg))
UNREACHED(void arts_ooo_dispatch_or_defer_guid(arts_guid_t g, ooo_kind_t k,
                                               const void *a, uint32_t s))
UNREACHED(bool arts_ooo_retire(arts_guid_t key, arts_shared_ptr_t cb))
UNREACHED(void *arts_route_table_borrow_or_lookup(arts_shared_ptr_t h,
                                                  arts_guid_t g,
                                                  arts_shared_ptr_t *l))
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
UNREACHED(void arts_send_db_cache_destroy(unsigned int rank,
                                          arts_guid_t db_guid))
UNREACHED(void arts_db_buf_write_inplace(struct arts_db_cache_s *c,
                                         const void *d, uint64_t n))
#ifdef ARTS_USE_CXL
UNREACHED(void arts_cxl_store_flush(const void *p, size_t bytes, bool producer))
UNREACHED(void *arts_cxl_store_alloc(size_t bytes))
UNREACHED(bool arts_cxl_store_contains(const void *p))
UNREACHED(bool arts_db_buf_withdraw(struct arts_db_cache_s *c, const void *p))
#elif defined(ARTS_RELEASE_PURGE)
UNREACHED(void arts_send_db_publish_cts(unsigned int r, arts_guid_t g,
                                        const struct arts_rdzv_landing_s *l,
                                        uint64_t cv))
#endif

/* ---- the buffer plane: no block in these scenarios has bytes ---- */

arts_shared_ptr_t arts_db_buf_acquire(struct arts_db_cache_s *cache) {
  return arts_shared_slot_load(&cache->buffer);
}
void arts_db_buf_release(arts_shared_ptr_t *h) { arts_shared_release(h); }
bool arts_db_buf_ensure(struct arts_db_cache_s *cache, uint64_t db_size) {
  (void)cache;
  (void)db_size;
  return false;
}
void arts_db_buf_prepare_inplace(struct arts_db_cache_s *cache, uint64_t n) {
  (void)cache;
  (void)n;
  fail("a request placed a landing for a block with no bytes");
}
uint64_t arts_db_first_fetch_size(const struct arts_db_cache_s *cache) {
  uint64_t s = atomic_load_explicit(&cache->db_size, memory_order_relaxed);
  return s != 0u ? s : arts_db_szhint_bound(cache->db_guid);
}
bool arts_net_rdzv_local(const void *p, uint64_t len, uint64_t *a,
                         uint64_t *k) {
  (void)p;
  (void)len;
  *a = 0x1000u;
  *k = 1u;
  return true;
}

#ifdef ARTS_DEBUG_LANDING_BALANCE
int64_t arts_db_landings_outstanding(void) { return 0; }
#endif

/* An EXCL answer's landing is the requester's own stable buffer: a retired
 * one pairs any bytes in flight itself rather than handing them away. */
UNREACHED(void arts_db_rdzv_discard_landing(uint64_t txid, uint64_t len,
                                            uint64_t cookie))

/* ---- the rendezvous plane: an expectation is held for the test to fire ----
 */

static uint64_t g_txid;
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

#if PAIRS
static void pairing_fire(bool data) {
  void *arg = g_expect_arg;
  void (*f)(void *) = data ? g_expect_cb : g_expect_discard;
  g_expect_arg = NULL;
  if (arg == NULL || f == NULL) {
    fail("no expectation to fire");
  }
  f(arg);
}
#endif

/* ---- the wire: messages are captured, answers built from them ---- */

#define MAXSENT 16
static struct {
  int rank;
  unsigned int type;
  unsigned int len;
  _Alignas(8) char buf[256];
} g_sent[MAXSENT];
static int g_nsent;

void arts_transport_send_async(int rank, char *message, unsigned int length) {
  if (g_nsent == MAXSENT || length > sizeof(g_sent[0].buf)) {
    fail("too many messages");
  }
  g_sent[g_nsent].rank = rank;
  g_sent[g_nsent].type = ((struct arts_msg_header_s *)message)->message_type;
  g_sent[g_nsent].len = length;
  memcpy(g_sent[g_nsent].buf, message, length);
  g_nsent++;
}

static const void *sent(unsigned int type, int nth) {
  for (int i = 0; i < g_nsent; i++) {
    if (g_sent[i].type == type && nth-- == 0) {
      return g_sent[i].buf;
    }
  }
  fail("an expected message did not leave");
  return NULL;
}

static int count_sent(unsigned int type) {
  int n = 0;
  for (int i = 0; i < g_nsent; i++) {
    n += (g_sent[i].type == type);
  }
  return n;
}

/* Installed: the descriptor's control block names its GUID. */
arts_shared_ptr_t arts_route_table_copy_live(arts_shared_ptr_t h,
                                             arts_guid_t key) {
  return (h != NULL && arts_shared_tag(h) == (uint64_t)key)
             ? arts_shared_copy(h)
             : NULL;
}

/* The owner's lookup of a FORWARD: what the scenario installed, if any. */
static arts_shared_ptr_t g_lookup;
arts_shared_ptr_t arts_route_table_lookup_db(arts_guid_t guid) {
  if (g_lookup == NULL || arts_shared_tag(g_lookup) != (uint64_t)guid) {
    return NULL;
  }
  return arts_shared_copy(g_lookup);
}

/* A self-sent request runs its body on the held descriptor, as the engine
 * does for an installed one.  The depth lets the deleter tell whether it runs
 * inside such a body. */
static int g_dispatch_depth;

void arts_ooo_dispatch_on(arts_shared_ptr_t h, ooo_kind_t kind,
                          arts_guid_t guid, const void *args, uint32_t size) {
  char copy[256];
  if (h == NULL || size > sizeof(copy) ||
      arts_shared_tag(h) != (uint64_t)guid) {
    fail("a self-send on a descriptor that is not installed");
  }
  memcpy(copy, args, size);
  if (kind != OOO_DB_EXCL_REQUEST) {
    fail("an unexpected self-send");
  }
  g_dispatch_depth++;
  arts_handler_db_excl_request(arts_shared_get(h), copy);
  g_dispatch_depth--;
}

/* ---- the EDT side: a parked waiter's EDT and its one dependence ---- */

struct fake_edt {
  struct arts_edt_s edt;
  arts_edt_dep_t depv[1];
  struct arts_dep_acq_s acq[1];
};
static struct fake_edt *g_edts[8];
static int g_nedts;
static int g_wakes;
/* At the next wake the block is retired and the test's reference goes.  That
 * reference stands for every reference held outside the self-sent bodies: the
 * install's and the acquiring EDT's slot pin, which the runtime holds across
 * the walk's send, so no schedule the runtime can reach drops it there; the
 * wake inside a self-sent body checks that the copies alone keep the body.
 * Where g_wake_refs is set, the descriptor must carry exactly that many
 * references just before: each frame around the wake holds its own count. */
static bool g_wake_destroys;
static int64_t g_wake_refs;
static struct arts_db_s *g_db;

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
void arts_db_acquire_account(struct arts_edt_s *edt) {
  (void)edt;
  g_wakes++;
  if (g_wake_destroys) {
    g_wake_destroys = false;
#ifndef NDEBUG
    if (g_wake_refs != 0 &&
        arts_shared_debug_refs(g_db->self_cb) != g_wake_refs) {
      char m[96];
      snprintf(m, sizeof(m), "%lld references at the wake, expected %lld",
               (long long)arts_shared_debug_refs(g_db->self_cb),
               (long long)g_wake_refs);
      fail(m);
    }
#endif
    arts_shared_set_tag(g_db->self_cb, 0u);
    arts_shared_ptr_t h = g_db->self_cb;
    arts_shared_release(&h);
  }
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

static int g_deleted;
static size_t g_db_bytes;
/* The deleter fills the descriptor and keeps it (poisoned under the address
 * sanitizer) instead of freeing it, so a frame that writes it afterwards is
 * caught by the fill check, and one that reads it by the sanitizer. */
static bool g_poison;
static unsigned char *g_poisoned;

static void db_deleter(void *p) {
  if (p != g_db) {
    fail("the deleter ran on another object");
  }
  if (g_dispatch_depth != 0) {
    fail("the deleter ran inside a self-sent body");
  }
  g_deleted++;
  arts_db_cache_destructor(&g_db->cache);
  if (g_poison) {
    g_poisoned = (unsigned char *)g_db;
    memset(g_poisoned, 0xA5, g_db_bytes);
#if defined(__SANITIZE_ADDRESS__)
    ASAN_POISON_MEMORY_REGION(g_poisoned, g_db_bytes);
#endif
    return;
  }
  free(g_db);
}

/* A requester's first-touch cache (rank REQ), or the block's home descriptor
 * (rank HOME) of a block a creator on rank `creator` made.  The control
 * block holds the test's reference, which stands for the install's. */
static void begin(const char *name, arts_guid_t guid, bool home,
                  unsigned int creator) {
  g_scenario = name;
  arts_global_rank_id = home ? HOME : REQ;
  size_t sz = (sizeof(struct arts_db_s) + 63u) & ~(size_t)63u;
  g_db_bytes = sz;
  g_db = (struct arts_db_s *)aligned_alloc(64, sz);
  memset(g_db, 0, sz);
  g_db->db_type = ARTS_DB;
  arts_db_init_kind_t kind = ARTS_DB_INIT_STUB;
  if (home) {
    kind = creator == HOME ? ARTS_DB_INIT_CREATOR_HOME : ARTS_DB_INIT_HOME_RECV;
  }
  arts_db_cache_init(&g_db->cache, guid, 0u, kind, creator);
  g_db->self_cb = arts_shared_make(g_db, db_deleter);
  arts_shared_set_tag(g_db->self_cb, (uint64_t)guid);
  g_deleted = 0;
  g_nsent = 0;
  g_wakes = 0;
  g_nedts = 0;
  g_expect_arg = NULL;
  g_lookup = NULL;
  g_malloc_hook = NULL;
  g_wake_destroys = false;
  g_wake_refs = 0;
  g_poison = false;
#ifdef ARTS_DEBUG_INJECTION
  g_fuzz_hook = NULL;
#endif
}

/* The block's retire, as the route table's: its control block's tag goes. */
static void retire(void) { arts_shared_set_tag(g_db->self_cb, 0u); }

static void check_wakes(int wakes) {
  if (g_wakes != wakes) {
    char m[96];
    snprintf(m, sizeof(m), "%d waiters woke, expected %d", g_wakes, wakes);
    fail(m);
  }
}

static void finish(void) {
  for (int i = 0; i < g_nedts; i++) {
    free(g_edts[i]);
  }
  printf("ok   %s\n", g_scenario);
}

static void end(int wakes) {
  check_wakes(wakes);
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
  finish();
}

/* The test's reference goes while the descriptor is retired, and the count
 * the scenario's last step drops must be the one that frees it. */
static void drop_ref_expect_held(void) {
  arts_shared_ptr_t h = g_db->self_cb;
  arts_shared_release(&h);
  if (g_deleted != 0) {
    fail("no count held the descriptor");
  }
}

/* After a scenario whose last reference the woken writer's destroy dropped:
 * the deleter ran once the frames let go, and nothing wrote the descriptor
 * after it. */
static void end_destroyed(int wakes) {
  check_wakes(wakes);
  if (g_wake_destroys) {
    fail("no waiter woke to destroy the block");
  }
  if (g_expect_arg != NULL) {
    fail("an expectation left unfired");
  }
  if (g_deleted != 1 || g_poisoned == NULL) {
    fail("the deleter did not run once the frames let go");
  }
#if defined(__SANITIZE_ADDRESS__)
  ASAN_UNPOISON_MEMORY_REGION(g_poisoned, g_db_bytes);
#endif
  for (size_t i = 0; i < g_db_bytes; i++) {
    if (g_poisoned[i] != 0xA5u) {
      fail("the descriptor was written after its deleter ran");
    }
  }
  free(g_poisoned);
  g_poisoned = NULL;
  g_poison = false;
  finish();
}

/* An EDT acquiring the block in `mode` through the arm's acquire body. */
static void acquire(arts_db_access_mode_t mode) {
  struct fake_edt *f = (struct fake_edt *)calloc(1, sizeof(*f));
  f->edt.guid = 0xed0u + (arts_guid_t)g_nedts;
  f->edt.depc = 1u;
  f->edt.dep_acq = f->acq;
  f->depv[0].guid = g_db->cache.db_guid;
  f->depv[0].mode = mode;
  /* The slot's pin, which the firing frame stores; borrowed here, never
   * dropped. */
  f->acq[0].pin = g_db->self_cb;
  g_edts[g_nedts++] = f;
  struct arts_ooo_args_db_acquire_s a = {
      .edt = &f->edt, .db_guid = g_db->cache.db_guid, .slot = 0u};
  arts_handler_db_acquire(g_db, &a);
}

/* A create's hold of the cache. */
static uint8_t claim(void) {
  uint8_t owed = 0u;
  uint8_t census = arts_db_census_serving(false);
  if (!arts_db_create_take_hold(&g_db->cache, &owed)) {
    fail("the create's hold did not commit");
  }
  arts_db_census_served(census);
  return owed;
}

/* ---- the requests this rank sent, and their answers ---- */

static const struct arts_msg_excl_request_packet_s *request(int nth) {
  return (const struct arts_msg_excl_request_packet_s *)sent(
      MSG_DB_EXCL_REQUEST, nth);
}

static const struct arts_msg_excl_request_packet_s *
request_of(arts_db_access_mode_t mode) {
  for (int i = 0;; i++) {
    const struct arts_msg_excl_request_packet_s *r = request(i);
    if ((arts_db_access_mode_t)r->mode == mode) {
      if (r->token != arts_db_token(g_db)) {
        fail("a request did not name its descriptor");
      }
      return r;
    }
  }
}

/* The size round or the dismiss of a request. */
static void cts(arts_db_access_mode_t mode) {
  const struct arts_msg_excl_request_packet_s *r = request_of(mode);
  struct arts_msg_excl_cts_packet_s p;
  memset(&p, 0, sizeof(p));
  p.header.rank = HOME;
  p.db_guid = r->db_guid;
  p.mode = r->mode;
  p.cover = r->cover;
  p.token = r->token;
  arts_handler_db_excl_cts(&p);
}

#ifdef ARTS_RELEASE_RETAIN
#define HOME_TOKEN ((uint64_t)0x40e00u)
/* The answer, from the owner the home forwarded the request to: no payload,
 * or a one-sided one. */
static void grant(arts_db_access_mode_t mode, bool payload) {
  const struct arts_msg_excl_request_packet_s *r = request_of(mode);
  struct arts_msg_excl_deliver_packet_s p;
  memset(&p, 0, sizeof(p));
  p.header.message_type = MSG_DB_EXCL_DELIVER;
  p.header.rank = OTHER;
  p.db_guid = r->db_guid;
  p.mode = r->mode;
  p.data_size = 0u;
  p.rdzv_txid = payload ? 0x77u : 0u;
  p.token = r->token;
  p.home_token = mode == DB_MODE_RW ? HOME_TOKEN : 0u;
  arts_handler_db_excl_deliver(&p, sizeof(p));
}

/* A migration's landing and tokens are consumed by whoever grants it. */
static void check_migrate_consumed(void) {
  const struct arts_db_cache_s *c = &g_db->cache;
  if (c->migrate_rdzv.addr != 0u || c->migrate_rdzv.key != 0u ||
      c->migrate_rdzv.txid != 0u || c->migrate_rdzv.cookie != 0u ||
      c->migrate_token != 0u || c->migrate_home_token != 0u) {
    fail("the self-grant left the migration's landing or tokens behind");
  }
}

/* A migration's CONFIRM names the home by the token its DELIVER carried. */
static void check_confirm(void) {
  const struct arts_msg_excl_confirm_packet_s *c =
      (const struct arts_msg_excl_confirm_packet_s *)sent(MSG_DB_EXCL_CONFIRM,
                                                          0);
  if (c->home_token != HOME_TOKEN) {
    fail("the CONFIRM did not echo the home's token");
  }
}
#else
static void grant(arts_db_access_mode_t mode, bool payload) {
  const struct arts_msg_excl_request_packet_s *r = request_of(mode);
  struct arts_msg_excl_grant_packet_s p;
  memset(&p, 0, sizeof(p));
  p.header.message_type = MSG_DB_EXCL_GRANT;
  p.header.rank = HOME;
  p.db_guid = r->db_guid;
  p.mode = r->mode;
  p.data_size = 0u;
  p.rdzv_txid = payload ? 0x77u : 0u;
  p.token = r->token;
  arts_handler_db_excl_grant(&p, sizeof(p));
}

static void check_confirm(void) {}
#endif

/* ---- requester scenarios ---- */

static void read_granted(void) {
  begin("read axis closed by its grant", GUID_SIZED, false, HOME);
  acquire(DB_MODE_RO);
  grant(DB_MODE_RO, false);
  end(1);
}

static void write_granted(void) {
  begin("write axis closed by its grant", GUID_SIZED, false, HOME);
  acquire(DB_MODE_RW);
  grant(DB_MODE_RW, false);
  check_confirm();
  end(1);
}

static void read_dismissed(void) {
  begin("read axis closed by a dismiss, the write request out", GUID_SIZED,
        false, HOME);
  acquire(DB_MODE_RO);
  acquire(DB_MODE_RW);
  if ((request_of(DB_MODE_RW)->cover & EXCL_COVER_RO_OPEN) == 0u) {
    fail("the write request did not declare the read request");
  }
  /* The home dismisses the read request the write request declared; the
   * reader rides the write turn. */
  cts(DB_MODE_RO);
  if (count_sent(MSG_DB_EXCL_REQUEST) != 2) {
    fail("the dismiss re-sent the read request");
  }
  grant(DB_MODE_RW, false);
  end(2);
}

static void write_size_round(void) {
  begin("write size round keeps the axis open", GUID_SIZED, false, HOME);
  acquire(DB_MODE_RW);
  cts(DB_MODE_RW);
  if (count_sent(MSG_DB_EXCL_REQUEST) != 2 ||
      request(1)->token != request(0)->token) {
    fail("the size round did not re-send the same request");
  }
  grant(DB_MODE_RW, false);
  end(1);
}

static void both_claimed(void) {
  begin("read and write axes closed by a create's hold", GUID_CLAIM, false,
        HOME);
  acquire(DB_MODE_RO);
  acquire(DB_MODE_RW);
  uint8_t owed = claim();
  if ((owed & ARTS_CREATE_OWED_RO) == 0u ||
      (owed & ARTS_CREATE_OWED_RW) == 0u) {
    fail("the hold did not close both requests");
  }
  end(2);
}

static void answer_retired(bool dismiss) {
  begin(dismiss ? "dismiss on a retired descriptor"
                : "grant on a retired descriptor",
        GUID_SIZED, false, HOME);
  acquire(DB_MODE_RO);
  if (dismiss) {
    acquire(DB_MODE_RW);
  }
  retire();
  if (dismiss) {
    cts(DB_MODE_RO);
    grant(DB_MODE_RW, false);
    if (count_sent(MSG_DB_EXCL_CONFIRM) != 0) {
      fail("a retired descriptor confirmed a migration");
    }
  } else {
    grant(DB_MODE_RO, false);
  }
  end(0);
}

#if PAIRS
/* An answer whose bytes are still in flight to a retired descriptor's own
 * buffer: the count goes only once they have landed, or, when the teardown
 * finds they never did, with the discard, which owns it (no sweep reaches a
 * retired descriptor). */
static void retired_in_flight(bool landed) {
  begin(landed ? "retired requester keeps its count until the bytes land"
               : "retired requester's pairing discarded at teardown",
        GUID_SIZED, false, HOME);
  acquire(DB_MODE_RW);
  retire();
  grant(DB_MODE_RW, true);
  drop_ref_expect_held();
  pairing_fire(landed);
  if (g_deleted != 1) {
    fail("the pairing did not release the count it owns");
  }
  if (count_sent(MSG_DB_EXCL_CONFIRM) != 0) {
    fail("a retired descriptor confirmed a migration");
  }
  finish();
}
#endif

#if PAIRS
static void paired(int how) {
  static const char *names[] = {
      "pairing callback closes the write axis",
      "pairing callback on a retired descriptor",
      "pairing discarded at teardown, the count the sweep's"};
  begin(names[how], GUID_SIZED, false, HOME);
  acquire(DB_MODE_RW);
  grant(DB_MODE_RW, true);
  if (how == 1) {
    retire();
  }
  if (how == 2) {
    /* Teardown order: the sweep releases through the word, the discard ends
     * only its own context. */
    arts_db_sweep_open_items(g_db, g_db->self_cb);
    pairing_fire(false);
  } else {
    pairing_fire(true);
  }
  if (how == 0) {
    check_confirm();
  }
  end(how == 0 ? 1 : 0);
}
#endif

static void sweep_rows(void) {
  begin("teardown sweep drops one count per open axis", GUID_SIZED, false,
        HOME);
  acquire(DB_MODE_RO);
  acquire(DB_MODE_RW);
  arts_db_sweep_open_items(g_db, g_db->self_cb);
  end(0);
}

#ifdef ARTS_RELEASE_RETAIN
/* ---- home scenarios ---- */

static struct arts_ooo_args_db_excl_request_s
far_request(unsigned int rank, uint32_t mode, uint32_t cover) {
  struct arts_ooo_args_db_excl_request_s a;
  memset(&a, 0, sizeof(a));
  a.requester = rank;
  a.db_guid = g_db->cache.db_guid;
  a.mode = (arts_db_access_mode_t)mode;
  a.cover = cover;
  a.token = FAR_TOKEN + rank;
  return a;
}

static const struct arts_msg_excl_forward_packet_s *forward(int nth) {
  return (const struct arts_msg_excl_forward_packet_s *)sent(
      MSG_DB_EXCL_FORWARD, nth);
}

static void confirm_from(unsigned int rank) {
  struct arts_msg_excl_confirm_packet_s p;
  memset(&p, 0, sizeof(p));
  p.header.rank = rank;
  p.db_guid = g_db->cache.db_guid;
  p.home_token = arts_db_token(g_db);
  arts_handler_db_excl_confirm(&p);
}

static void read_retained(void) {
  begin("read grant served and retained holds no home count", GUID_SIZED, true,
        OTHER);
  struct arts_ooo_args_db_excl_request_s a =
      far_request(REQ, DB_MODE_RO, EXCL_COVER_PAR);
  arts_handler_db_excl_request(g_db, &a);
  const struct arts_msg_excl_forward_packet_s *f = forward(0);
  if (f->mode != DB_MODE_RO || f->target != REQ || f->token != a.token ||
      f->cover != EXCL_COVER_PAR || f->home_token != 0u) {
    fail("the serve FORWARD did not carry the reader's token and cover");
  }
  uint64_t s = atomic_load_explicit(&g_db->lock_state, memory_order_relaxed);
  if (LOCK_R(s) != 1u || LOCK_W(s) != 0u) {
    fail("the read grant was not counted");
  }
  end(0);
}

static void write_confirmed(bool retired) {
  begin(retired ? "CONFIRM on a retired home takes w-- and releases"
                : "w 0->1 takes the home count, its CONFIRM releases it",
        GUID_SIZED, true, OTHER);
  struct arts_ooo_args_db_excl_request_s a = far_request(REQ, DB_MODE_RW, 0u);
  arts_handler_db_excl_request(g_db, &a);
  const struct arts_msg_excl_forward_packet_s *f = forward(0);
  if (f->mode != DB_MODE_RW || f->target != REQ || f->token != a.token ||
      f->home_token != arts_db_token(g_db)) {
    fail("the migrate FORWARD did not carry the two tokens");
  }
  if (retired) {
    retire();
    drop_ref_expect_held();
    confirm_from(REQ);
    if (g_deleted != 1) {
      fail("the CONFIRM on the retired home kept its count");
    }
    finish();
    return;
  }
  confirm_from(REQ);
  end(0);
}

static void write_chain(void) {
  begin("two migrations in a row hold one home count", GUID_SIZED, true, OTHER);
  struct arts_ooo_args_db_excl_request_s a = far_request(REQ, DB_MODE_RW, 0u);
  struct arts_ooo_args_db_excl_request_s b = far_request(OTHER, DB_MODE_RW, 0u);
  arts_handler_db_excl_request(g_db, &a);
  arts_handler_db_excl_request(g_db, &b);
  confirm_from(REQ);
  /* The chain's next migration leaves for the new owner. */
  const struct arts_msg_excl_forward_packet_s *f = forward(1);
  if (f->target != OTHER || f->token != b.token ||
      f->home_token != arts_db_token(g_db)) {
    fail("the chained FORWARD did not carry the two tokens");
  }
  retire();
  drop_ref_expect_held();
  confirm_from(OTHER);
  if (g_deleted != 1) {
    fail("the chain's last CONFIRM kept the home count");
  }
  finish();
}

/* The home is the resident owner without the write permission; its own
 * write request migrates to itself inline: the write axis closes in the
 * self-grant and the home count in the CONFIRM it sends itself. */
static void self_migration(void) {
  begin("home's own write request granted by itself", GUID_SIZED, true, HOME);
  atomic_store_explicit(&g_db->cache.cache_state,
                        CACHE_MAKE_FULL(1u, CACHE_ST_IDLE, CACHE_ST_IDLE,
                                        ARTS_EXCL_NO_TARGET, 0u, 0u),
                        memory_order_relaxed);
  acquire(DB_MODE_RW);
  if (g_nsent != 0) {
    fail("the home's own request left the rank");
  }
  uint64_t s = atomic_load_explicit(&g_db->lock_state, memory_order_relaxed);
  if (LOCK_W(s) != 0u ||
      CACHE_RW_ST(atomic_load_explicit(
          &g_db->cache.cache_state, memory_order_relaxed)) != CACHE_ST_GRANT) {
    fail("the self-grant did not commit and confirm");
  }
  check_migrate_consumed();
  end(1);
}

/* The same self-migration, with every reference held outside the self-sent
 * bodies removed at the self-grant's wake: the block is retired, and the
 * test's reference goes, which stands for the install's and the walk's slot
 * pin (held across the send by the runtime, so this checks the copies, not a
 * reachable schedule).  At the wake the test's reference, the write request's
 * count, the request self-send's copy, the home's count and the FORWARD
 * self-send's copy hold the descriptor.  The CONFIRM the self-grant sends
 * itself finds the home retired, returns w to zero and drops the home's
 * count, and the self-grant drops the write request's, which leaves the
 * self-sent request's own: the deleter may run only once the request's body
 * has returned.  The poison check then forbids any touch after it, the
 * acquire's after its send included, which is stricter than the walk's pin
 * requires. */
static void self_migration_destroyed(void) {
  begin("home's own write request granted by itself, every outside "
        "reference dropped at the wake",
        GUID_SIZED, true, HOME);
  atomic_store_explicit(&g_db->cache.cache_state,
                        CACHE_MAKE_FULL(1u, CACHE_ST_IDLE, CACHE_ST_IDLE,
                                        ARTS_EXCL_NO_TARGET, 0u, 0u),
                        memory_order_relaxed);
  g_wake_destroys = true;
  g_wake_refs = 5;
  g_poison = true;
  acquire(DB_MODE_RW);
  if (g_nsent != 0) {
    fail("the home's own request left the rank");
  }
  end_destroyed(1);
}

/* A resident owner on another rank than the home, without the write
 * permission, is forwarded its own write request: the FORWARD's landing and
 * tokens are stashed, consumed by the grant it makes itself, and the home's
 * token goes back on the CONFIRM. */
static void owner_self_grant(void) {
  begin("resident owner's own write request granted by its FORWARD", GUID_SIZED,
        false, HOME);
  atomic_store_explicit(&g_db->cache.cache_state,
                        CACHE_MAKE_FULL(1u, CACHE_ST_IDLE, CACHE_ST_IDLE,
                                        ARTS_EXCL_NO_TARGET, 0u, 0u),
                        memory_order_relaxed);
  acquire(DB_MODE_RW);
  const struct arts_msg_excl_request_packet_s *r = request_of(DB_MODE_RW);
  struct arts_msg_excl_forward_packet_s f;
  memset(&f, 0, sizeof(f));
  f.header.rank = HOME;
  f.db_guid = r->db_guid;
  f.mode = DB_MODE_RW;
  f.target = REQ;
  f.rdzv.addr = 0x2000u;
  f.rdzv.key = 2u;
  f.rdzv.txid = 0x99u;
  f.token = r->token;
  f.home_token = HOME_TOKEN;
  g_lookup = g_db->self_cb;
  arts_handler_db_excl_forward(&f);
  check_confirm();
  check_migrate_consumed();
  end(1);
}

/* A FORWARD naming this resident rank as its own migration target while it
 * has no write request out is a grant for no request: fatal. */
static void self_grant_without_request(void) {
  begin("self-grant with no write request out is fatal", GUID_SIZED, false,
        HOME);
  atomic_store_explicit(&g_db->cache.cache_state,
                        CACHE_MAKE_FULL(1u, CACHE_ST_IDLE, CACHE_ST_IDLE,
                                        ARTS_EXCL_NO_TARGET, 0u, 0u),
                        memory_order_relaxed);
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    /* The fatal line the check prints is the expected outcome, not a verdict
     * of this test's run. */
    int devnull = open("/dev/null", O_WRONLY);
    (void)dup2(devnull, STDOUT_FILENO);
    (void)dup2(devnull, STDERR_FILENO);
    g_expect_abort = true;
    struct arts_msg_excl_forward_packet_s f;
    memset(&f, 0, sizeof(f));
    f.db_guid = g_db->cache.db_guid;
    f.mode = DB_MODE_RW;
    f.target = REQ;
    f.token = arts_db_token(g_db);
    f.home_token = HOME_TOKEN;
    g_lookup = g_db->self_cb;
    arts_handler_db_excl_forward(&f);
    _exit(0);
  }
  int status = 0;
  if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
      WEXITSTATUS(status) != EXPECTED_ABORT) {
    fail("the self-grant without a write request out did not abort");
  }
  end(0);
}

#ifdef ARTS_DEBUG_INJECTION
/* At the decision-commit point right after a write request's CAS lifts w off
 * zero inside a read phase, another frame's read return flips the phase and
 * starts this migration, the new owner's writers run and the block is
 * destroyed, and the new owner's CONFIRM lands on the retired home.  The
 * home count must already be held: the descriptor outlives the CONFIRM, and
 * the request frame's own count (the engine's pin here) is the last. */
static bool migration_in_take_window(void) {
  uint64_t s = atomic_load_explicit(&g_db->lock_state, memory_order_relaxed);
  if (LOCK_W(s) != 1u || LOCK_R(s) != 1u) {
    return false; /* not yet past the write request's CAS */
  }
  g_fuzz_hook = NULL;
  struct arts_msg_excl_notice_packet_s ret;
  memset(&ret, 0, sizeof(ret));
  ret.header.rank = READER;
  ret.db_guid = g_db->cache.db_guid;
  g_lookup = g_db->self_cb;
  arts_handler_db_excl_roret(&ret); /* the flip: FORWARD(migrate) to REQ */
  retire();
  arts_shared_ptr_t install = g_db->self_cb;
  arts_shared_release(&install); /* the teardown's drop of the install */
  confirm_from(REQ);
  if (g_deleted != 0) {
    fail("the home was freed under its request frame");
  }
  return true;
}

static void take_before_cas(void) {
  begin("home count taken before the CAS that lifts w", GUID_SIZED, true,
        OTHER);
  struct arts_ooo_args_db_excl_request_s rd =
      far_request(READER, DB_MODE_RO, EXCL_COVER_PAR);
  arts_handler_db_excl_request(g_db, &rd); /* the read phase, r = 1 */
  struct arts_ooo_args_db_excl_request_s a = far_request(REQ, DB_MODE_RW, 0u);
  g_fuzz_hook = migration_in_take_window;
  arts_shared_ptr_t pin = arts_shared_copy(g_db->self_cb);
  arts_handler_db_excl_request(g_db, &a);
  if (g_fuzz_hook != NULL) {
    fail("the request frame passed no fuzz point after its CAS");
  }
  if (count_sent(MSG_DB_EXCL_FORWARD) != 2) {
    fail("the flip did not start the migration");
  }
  arts_shared_release(&pin);
  if (g_deleted != 1) {
    fail("the request frame's count was not the last");
  }
  finish();
}
#endif

static void forward_missed(arts_db_access_mode_t mode) {
  begin(mode == DB_MODE_RO ? "read FORWARD that finds no block is dismissed"
                           : "write FORWARD that finds no block is dropped",
        GUID_SIZED, false, HOME);
  arts_global_rank_id = OTHER;
  struct arts_msg_excl_forward_packet_s p;
  memset(&p, 0, sizeof(p));
  p.header.rank = HOME;
  p.db_guid = GUID_SIZED;
  p.mode = mode;
  p.target = REQ;
  p.cover = EXCL_COVER_PAR;
  p.token = FAR_TOKEN;
  p.home_token = mode == DB_MODE_RW ? arts_db_token(g_db) : 0u;
  arts_handler_db_excl_forward(&p);
  if (mode == DB_MODE_RO) {
    const struct arts_msg_excl_cts_packet_s *c =
        (const struct arts_msg_excl_cts_packet_s *)sent(MSG_DB_EXCL_CTS, 0);
    if (g_sent[0].rank != (int)REQ || c->mode != DB_MODE_RO ||
        c->cover != EXCL_COVER_PAR || c->token != FAR_TOKEN) {
      fail("the dismiss did not echo the read request");
    }
  } else if (g_nsent != 0) {
    fail("a write FORWARD that missed sent something");
  }
  end(0);
}

/* A held read request its rank's write request took is still dismissed when
 * the CONFIRM ending the write phase finds the home retired; a held read that
 * was pending at the destroy is not served. */
static void retired_confirm_dismisses(void) {
  begin("CONFIRM on a retired home dismisses a covered held read", GUID_SIZED,
        true, OTHER);
  struct arts_ooo_args_db_excl_request_s w = far_request(OTHER, DB_MODE_RW, 0u);
  struct arts_ooo_args_db_excl_request_s r =
      far_request(REQ, DB_MODE_RO, EXCL_COVER_PAR);
  struct arts_ooo_args_db_excl_request_s r2 =
      far_request(READER, DB_MODE_RO, EXCL_COVER_PAR);
  struct arts_ooo_args_db_excl_request_s rw =
      far_request(REQ, DB_MODE_RW, EXCL_COVER_RO_OPEN | EXCL_COVER_RO_PAR);
  arts_handler_db_excl_request(g_db, &w);
  arts_handler_db_excl_request(g_db, &r);  /* held behind the write phase */
  arts_handler_db_excl_request(g_db, &r2); /* held, and never covered */
  arts_handler_db_excl_request(g_db, &rw); /* declares r, takes its bit */
  confirm_from(OTHER);                     /* chains the turn to REQ */
  if (count_sent(MSG_DB_EXCL_CTS) != 0) {
    fail("the held read was answered before its write turn ended");
  }
  int forwards = count_sent(MSG_DB_EXCL_FORWARD);
  retire();
  drop_ref_expect_held();
  confirm_from(REQ);
  const struct arts_msg_excl_cts_packet_s *c =
      (const struct arts_msg_excl_cts_packet_s *)sent(MSG_DB_EXCL_CTS, 0);
  if (c->mode != DB_MODE_RO || c->token != r.token ||
      c->cover != EXCL_COVER_PAR || count_sent(MSG_DB_EXCL_CTS) != 1) {
    fail("the dismiss did not echo the covered read request alone");
  }
  if (count_sent(MSG_DB_EXCL_FORWARD) != forwards) {
    fail("the retired home served a read");
  }
  if (g_deleted != 1) {
    fail("the last CONFIRM kept the home count");
  }
  finish();
}

static void sweep_home(void) {
  begin("teardown sweep drops the home count of an unconfirmed write request",
        GUID_SIZED, true, OTHER);
  struct arts_ooo_args_db_excl_request_s a = far_request(REQ, DB_MODE_RW, 0u);
  arts_handler_db_excl_request(g_db, &a);
  arts_db_sweep_open_items(g_db, g_db->self_cb);
  end(0);
}
#else
/* The creator on another rank holds the write turn; the home's own write
 * request queues behind it and is served by the home's self-grant when the
 * creator's release comes back. */
static void self_grant(void) {
  begin("home's own write request served by its self-grant", GUID_SIZED, true,
        OTHER);
  acquire(DB_MODE_RW);
  if (g_nsent != 0) {
    fail("the home's own request left the rank");
  }
  check_wakes(0);
  struct arts_ooo_args_db_excl_release_s r;
  memset(&r, 0, sizeof(r));
  r.releaser = OTHER;
  r.db_guid = g_db->cache.db_guid;
  r.mode = DB_MODE_RW;
  arts_handler_db_excl_release(g_db, &r);
  end(1);
}

/* The creator's write turn comes back with nothing queued, so the home's own
 * write request is granted inside its self-sent body.  At the grant's wake
 * every reference held outside that body is removed: the block is retired,
 * and the test's reference goes, which stands for the install's and the walk's
 * slot pin (held across the send by the runtime, so this checks the copy, not
 * a reachable schedule).  At the wake the test's reference, the request's
 * count and the self-send's copy hold the descriptor; then the grant drops the
 * request's count, which leaves the self-send's own: the deleter may run only
 * once the request's body has returned.  The poison check then forbids any
 * touch after it, the acquire's after its send included, which is stricter
 * than the walk's pin requires. */
static void self_grant_in_body(void) {
  begin("home's own write request granted inside its self-sent body",
        GUID_SIZED, true, OTHER);
  struct arts_ooo_args_db_excl_release_s r;
  memset(&r, 0, sizeof(r));
  r.releaser = OTHER;
  r.db_guid = g_db->cache.db_guid;
  r.mode = DB_MODE_RW;
  arts_handler_db_excl_release(g_db, &r);
  if (EXCL_STATE_W(atomic_load_explicit(&g_db->lock_state,
                                        memory_order_relaxed)) != 0u) {
    fail("the creator's write turn did not come back");
  }
  g_wake_destroys = true;
  g_wake_refs = 3;
  g_poison = true;
  acquire(DB_MODE_RW);
  if (g_nsent != 0) {
    fail("the home's own request left the rank");
  }
  end_destroyed(1);
}

/* Between a read request's arrival step and its count, its rank's write
 * request takes its ro_queued bit, the write turn ends and the destroy tears
 * the home down: the refused read is answered with a dismiss. */
static void cover_and_teardown(void) {
  uint64_t td = LOCK_MAKE_STATE_TD(1u, 0u, 0u, 0u);
  arts_rank_bitset_put(&g_db->ro_queued, REQ, false);
  atomic_store_explicit(&g_db->lock_state, td, memory_order_relaxed);
}

static void refused_covered_read(void) {
  begin("covered read refused by a torn-down home is dismissed", GUID_SIZED,
        true, OTHER);
  struct arts_ooo_args_db_excl_request_s a;
  memset(&a, 0, sizeof(a));
  a.requester = REQ;
  a.db_guid = g_db->cache.db_guid;
  a.mode = DB_MODE_RO;
  a.cover = EXCL_COVER_PAR;
  a.token = FAR_TOKEN;
  g_malloc_hook = cover_and_teardown; /* the read's queue node */
  arts_handler_db_excl_request(g_db, &a);
  const struct arts_msg_excl_cts_packet_s *c =
      (const struct arts_msg_excl_cts_packet_s *)sent(MSG_DB_EXCL_CTS, 0);
  if (g_sent[0].rank != (int)REQ || c->mode != DB_MODE_RO ||
      c->token != FAR_TOKEN || c->cover != EXCL_COVER_PAR) {
    fail("the dismiss did not echo the refused read request");
  }
  end(0);
}

/* The home's own read request waits behind the creator's write turn and is
 * granted from the read drain the turn's return opens, its node carrying the
 * home's token. */
static void self_read_grant(void) {
  begin("home's own read request granted from the read drain", GUID_SIZED, true,
        OTHER);
  acquire(DB_MODE_RO);
  check_wakes(0);
  struct arts_ooo_args_db_excl_release_s r;
  memset(&r, 0, sizeof(r));
  r.releaser = OTHER;
  r.db_guid = g_db->cache.db_guid;
  r.mode = DB_MODE_RW;
  arts_handler_db_excl_release(g_db, &r);
  end(1);
}
#endif

int main(void) {
  read_granted();
  write_granted();
  read_dismissed();
  write_size_round();
  both_claimed();
  answer_retired(false);
  answer_retired(true);
#if PAIRS
  paired(0);
  paired(1);
  paired(2);
  retired_in_flight(true);
  retired_in_flight(false);
#endif
  sweep_rows();
#ifdef ARTS_RELEASE_RETAIN
  read_retained();
  write_confirmed(false);
  write_confirmed(true);
  write_chain();
  self_migration();
  self_migration_destroyed();
  owner_self_grant();
  self_grant_without_request();
#ifdef ARTS_DEBUG_INJECTION
  take_before_cas();
#endif
  forward_missed(DB_MODE_RO);
  forward_missed(DB_MODE_RW);
  retired_confirm_dismisses();
  sweep_home();
#else
  self_grant();
  self_grant_in_body();
  self_read_grant();
  refused_covered_read();
#endif
  printf("PASS token_pairing_excl\n");
  return 0;
}
