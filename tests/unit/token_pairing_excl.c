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
 * body and teardown, the answers' entries, the rendezvous callbacks, the
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
 *   read axis   closed by its grant;
 *   write axis  closed by its grant (under RETAIN the migration's DELIVER,
 *               whose CONFIRM echoes the home's token);
 *   both axes open on a retired descriptor: each answer that moves no bytes
 *   drops its own count alone, waking and confirming nothing;
 *   a one-sided answer's pairing callback, live and retired, and a pairing
 *   the teardown discards (the count is then the sweep's), where answers
 *   carry bytes; bytes in flight to a requester already retired: the count
 *   goes after they land, or with the discard, which owns it;
 *   the teardown sweep: one count per open axis;
 *   where grants carry a payload, the home's refusal of a request from
 *   another rank that advertises no landing for a block with bytes, on either
 *   axis, ending the run with the plane's line, and its own write request
 *   taken in without one; where they carry none (the CXL store), such
 *   requests granted.
 * RETAIN:
 *   the two requests of a rank are independent: a migration's DELIVER wakes
 *   the writers alone and sets the readers parked on the read request aside,
 *   a reader that arrives at the owner is a local hit, and the home forwards
 *   the read request to this rank, the owner, which answers it in place:
 *   exactly the readers set aside resume and the RO_RETURN goes back, a
 *   RECALL before it marking nothing and one after it finding the read axis
 *   idle; the same on the home rank, the CONFIRM, the forward to itself and
 *   the answer inline in the DELIVER's frame, so the readers must be set
 *   aside before the CONFIRM; the readers set aside before the migration's
 *   writer wakes, so a release, a migration away and a read DELIVER inside
 *   that wake find them; readers set aside leave with the bytes and the
 *   next owner's read DELIVER returns them with the ones that parked since;
 *   the serve judges its gate again after its drain and pushes the batch
 *   back when a writer went live while it stood between its gate and its
 *   drain; a read DELIVER at an owner and a forward of a rank's own read
 *   request to a rank that is not its owner are fatal.
 *   A read grant the home serves, retained and never recalled, holds no home
 *   count; w 0->1 takes one and the EXCL_CONFIRM releases it, through a chain
 *   of two migrations too; a CONFIRM on a retired home takes the counting
 *   step and releases, and serves none of the read requests held there; the
 *   count is held before the CAS that lifts w, so a migration another frame
 *   starts and confirms at that point cannot free the home under the request
 *   frame (injection trees); the home's own write request granted by itself,
 *   as the resident owner, inline, and again with every reference held
 *   outside the self-sent bodies removed at the wake inside the request's
 *   body; a resident owner's own request granted by its FORWARD, the
 *   migration's landing and tokens consumed; a self-grant with no write
 *   request out is fatal; a FORWARD that finds no block is dropped; the
 *   sweep's w row.
 *   The home's teardown: a destroy retires the block and only then tells
 *   each rank of its roster, once; a second destroy of the same home tells no
 *   rank again.
 * PURGE:
 *   the two requests of a rank are independent: a write grant wakes the
 *   writers alone and sets the readers parked on the read request aside,
 *   the turn ends without them, a second turn adds to them, and the read
 *   grant returns them all; the readers are set aside before a writer wakes,
 *   so a turn that ends inside that wake is followed by a read grant that
 *   finds them; a holder of the write turn that becomes a reader
 *   while the read request is out; a read grant under the rank's write grant
 *   and a second read grant are fatal.
 *   The home: its own write request served by its self-grant when the
 *   creator's write turn comes back, and, that turn back first, granted inside
 *   its self-sent body with every reference held outside that body removed at
 *   the wake; its own read request granted from the read drain that return
 *   opens; a read request granted into an open read phase although its
 *   rank's write request waits; a read request held through its rank's write
 *   turn and granted when the turn comes back, with no other answer; a read
 *   request refused by the torn-down home is answered nothing.
 *   The home's teardown, at the edge where its word is marked with nothing
 *   counted: a destroy that finds a release still counted only marks the
 *   word, and that release retires the block and only then tells each rank of
 *   the roster, once; a destroy that finds nothing counted does both at once;
 *   a second destroy claims no teardown; a request on either axis that
 *   reaches the torn-down word is neither counted nor answered, a refused
 *   write request's node staying queued until the descriptor goes.
 *
 * Of the transitions a correct run reaches, one is driven here alone: the
 * serve's second judgement of its gate, which needs a writer to go live while
 * the serve stands between its gate and its drain, a stall only this
 * harness's ensure hook makes.  A write turn's setting aside of the parked
 * readers and an owner's answer to its own read request held through the
 * write phase also run on a real run, in db_read_parked_under_write_turn.
 *
 * PURE test: no arts_rt, no ports, no threads.
 */
#include "coherence/coherence.c"
#include "coherence/excl/directory.c"
#include "coherence/grantreq_queue.c"
#include "coherence/rank_bitset.c"
#ifdef ARTS_RETENTION_RETAIN
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

#if defined(ARTS_RETENTION_RETAIN) || !defined(ARTS_USE_CXL)
#define PAIRS 1 /* answers can carry one-sided bytes */
#else
#define PAIRS 0
#endif

#define HOME 0u
#define REQ 1u
#define OTHER 2u
#define READER 3u
/* A block whose GUID bounds no bytes: a request sends no landing. */
#define GUID_SIZED                                                             \
  ARTS_GUID_MAKE(ARTS_GUID_DB, HOME, ARTS_GUID_DB_KEY(0u, 0x61u))
/* A block of BYTES bytes, whose GUID bounds that size. */
#define BYTES 64u
#define GUID_BYTES                                                             \
  ARTS_GUID_MAKE(ARTS_GUID_DB, HOME, ARTS_GUID_DB_KEY(1u, 0x63u))
/* A requester's token on another rank, opaque to the home. */
#define FAR_TOKEN ((uint64_t)0xfa400u)

unsigned int arts_global_rank_id = REQ;
unsigned int arts_global_rank_count = 4u;
uint64_t arts_guid_table_count;
struct arts_runtime_shared_s arts_node_info;
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
    _exit(rc == 1 ? EXPECTED_ABORT : 1);
  }
  printf("FAIL token_pairing_excl: %s: the runtime aborted\n", g_scenario);
  fflush(stdout);
  _exit(rc ? rc : 1);
}

/* Runs once at the next allocation: a step another thread takes inside a
 * window of the code under test. */
static void (*g_malloc_hook)(void);
/* The runtime's allocations not yet freed. */
static long g_live_allocs;
void *arts_malloc(size_t n) {
  void (*hook)(void) = g_malloc_hook;
  g_malloc_hook = NULL;
  if (hook != NULL) {
    hook();
  }
  g_live_allocs++;
  return malloc(n);
}
void *arts_calloc(size_t n, size_t s) {
  g_live_allocs++;
  return calloc(n, s);
}
void *arts_malloc_aligned(size_t n, size_t a) {
  g_live_allocs++;
  return aligned_alloc(a, (n + a - 1u) / a * a);
}
void arts_free(void *p) {
  g_live_allocs -= (p != NULL);
  free(p);
}

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
bool arts_net_send_held_read(int rank, const char *message,
                             unsigned int length) {
  (void)rank;
  (void)message;
  (void)length;
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
                                            struct arts_route_item_s *s))
UNREACHED(void arts_db_free(void *ptr))
UNREACHED(void arts_db_buf_ref_release_cb(void *arg))
UNREACHED(void arts_net_put_payload(int r, uint64_t a, uint64_t k, uint64_t t,
                                    const void *s, uint64_t l,
                                    void (*cb)(void *), void *arg))
UNREACHED(void arts_ooo_dispatch_or_defer_guid(arts_guid_t g, ooo_kind_t k,
                                               const void *a, uint32_t s))
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
UNREACHED(void arts_db_buf_write_inplace(struct arts_db_cache_s *c,
                                         const void *d, uint64_t n))
#ifdef ARTS_USE_CXL
UNREACHED(void arts_cxl_store_flush(const void *p, size_t bytes, bool producer))
UNREACHED(void *arts_cxl_store_alloc(size_t bytes))
UNREACHED(bool arts_cxl_store_contains(const void *p))
#endif

/* ---- the buffer plane: no block in these scenarios has bytes, but for the
 * home's own request of one that has, whose forward places the home's
 * landing (g_place_landing) ---- */

arts_shared_ptr_t arts_db_buf_acquire(struct arts_db_cache_s *cache) {
  return arts_shared_slot_load(&cache->buffer);
}
void arts_db_buf_release(arts_shared_ptr_t *h) { arts_shared_release(h); }
/* Runs once at the next ensure: a step another thread takes while a server
 * stands between its gate and its drain. */
static void (*g_ensure_hook)(void);
bool arts_db_buf_ensure(struct arts_db_cache_s *cache, uint64_t db_size) {
  (void)cache;
  (void)db_size;
  void (*hook)(void) = g_ensure_hook;
  g_ensure_hook = NULL;
  if (hook != NULL) {
    hook();
  }
  return false;
}
static bool g_place_landing;
static void landing_free(void *p) { free(p); }
void arts_db_buf_prepare_inplace(struct arts_db_cache_s *cache, uint64_t n) {
  if (!g_place_landing) {
    fail("a request placed a landing for a block with no bytes");
  }
  struct arts_db_buffer_s *b =
      (struct arts_db_buffer_s *)calloc(1, sizeof(*b) + n);
  b->cb = arts_shared_make(b, landing_free);
  arts_shared_slot_store(&cache->buffer, b->cb);
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

/* A live handle is borrowed; otherwise the lookup's copy, which the caller
 * releases. */
void *arts_route_table_borrow_or_lookup(arts_shared_ptr_t h, arts_guid_t guid,
                                        arts_shared_ptr_t *looked) {
  *looked = NULL;
  if (h != NULL && arts_shared_tag(h) == (uint64_t)guid) {
    return arts_shared_get(h);
  }
  *looked = arts_route_table_lookup_db(guid);
  return arts_shared_get(*looked);
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
/* Runs once inside the next wake: what the woken EDT and the ranks it
 * messages do before the waking frame goes on. */
static void (*g_wake_hook)(void);

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
  void (*hook)(void) = g_wake_hook;
  g_wake_hook = NULL;
  if (hook != NULL) {
    hook();
  }
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

/* ---- the home's teardown: the retire and the roster's notices ---- */

/* Set by a scenario that tears its home down: no other reaches the retire
 * or sends a notice. */
static bool g_teardown;
/* A retire through the engine detached the scenario's block. */
static bool g_retired;
/* Retires that found the block retired already. */
static int g_retire_misses;
/* The runtime's live allocations when the scenario began. */
static long g_allocs_at_begin;

/* The home's teardown retires the descriptor as the route table's single
 * flight does: the first retire clears the tag and drops the install's
 * reference, which the test's stands for; a retire that finds the object's
 * tag cleared detaches nothing, releases nothing and answers false.  Those
 * answers are counted (g_retire_misses). */
bool arts_ooo_retire(arts_guid_t key, arts_shared_ptr_t cb) {
  if (!g_teardown) {
    fail("a retire in a scenario that tears no home down");
  }
  if (cb != g_db->self_cb) {
    fail("a retire of another object");
  }
  if (arts_shared_tag(cb) != (uint64_t)key) {
    g_retire_misses++;
    return false;
  }
  arts_shared_set_tag(cb, 0u);
  g_retired = true;
  arts_shared_ptr_t h = cb;
  arts_shared_release(&h);
  return true;
}

/* A roster rank is told of the destroy only by the frame whose retire
 * detached the block, so no notice precedes that retire.  The notice is
 * recorded with the messages the wire captured. */
void arts_send_db_cache_destroy(unsigned int rank, arts_guid_t db_guid) {
  (void)db_guid;
  if (!g_teardown) {
    fail("a destroy notice in a scenario that tears no home down");
  }
  if (!g_retired) {
    fail("a destroy notice left before the block's retire");
  }
  if (g_nsent == MAXSENT) {
    fail("too many messages");
  }
  g_sent[g_nsent].rank = (int)rank;
  g_sent[g_nsent].type = MSG_DB_CACHE_DESTROY;
  g_sent[g_nsent].len = 0u;
  g_nsent++;
}

/* A requester's first-touch cache (rank REQ), or the block's home descriptor
 * (rank HOME) of a block a creator on rank `creator` made.  The control
 * block holds the test's reference, which stands for the install's. */
static void begin(const char *name, arts_guid_t guid, bool home,
                  unsigned int creator) {
  g_scenario = name;
  g_teardown = false;
  g_retired = false;
  g_retire_misses = 0;
  g_allocs_at_begin = g_live_allocs;
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
  g_wake_hook = NULL;
  g_ensure_hook = NULL;
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
  if (g_retire_misses != 0) {
    fail("a retire found the block retired already");
  }
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

#ifdef ARTS_RETENTION_RETAIN
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
      c->migrate_rdzv.txid != 0u || c->migrate_token != 0u ||
      c->migrate_home_token != 0u) {
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

/* With both axes open, the block is retired and the install's reference
 * goes: each answer, moving no bytes, finds the descriptor retired and drops
 * its own request's count and nothing else, so the descriptor ends at the
 * second, and nothing wakes or confirms. */
static void answer_retired(void) {
  begin("answers on a retired descriptor, both axes open", GUID_SIZED, false,
        HOME);
  acquire(DB_MODE_RO);
  acquire(DB_MODE_RW);
  retire();
  drop_ref_expect_held();
  g_poison = true;
  grant(DB_MODE_RO, false);
  if (g_deleted != 0) {
    fail("the read answer dropped more than its own request's count");
  }
  grant(DB_MODE_RW, false);
  if (count_sent(MSG_DB_EXCL_CONFIRM) != 0) {
    fail("a retired descriptor confirmed a migration");
  }
  if (g_deleted != 1) {
    fail("the write answer did not drop its own request's count");
  }
  end_destroyed(0);
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

/* Runs `body` in a child and requires the runtime's fatal exit from it. */
static void expect_abort(void (*body)(void), const char *what) {
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    /* The fatal line the check prints is the expected outcome, not a verdict
     * of this test's run. */
    int devnull = open("/dev/null", O_WRONLY);
    (void)dup2(devnull, STDOUT_FILENO);
    (void)dup2(devnull, STDERR_FILENO);
    g_expect_abort = true;
    body();
    _exit(0);
  }
  int status = 0;
  if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
      WEXITSTATUS(status) != EXPECTED_ABORT) {
    fail(what);
  }
}

/* The home descriptor of a block of BYTES bytes a creator on OTHER made. */
static void begin_bytes(const char *name) {
  begin(name, GUID_BYTES, true, OTHER);
  atomic_store_explicit(&g_db->cache.db_size, BYTES, memory_order_relaxed);
}

/* A request from another rank that advertises no landing, as the engine
 * hands it to the home: under the dispatcher's pin. */
static void bare_request(unsigned int rank, arts_db_access_mode_t mode) {
  struct arts_ooo_args_db_excl_request_s a;
  memset(&a, 0, sizeof(a));
  a.requester = rank;
  a.db_guid = g_db->cache.db_guid;
  a.mode = (uint32_t)mode;
  a.token = FAR_TOKEN + rank;
  arts_shared_ptr_t pin = arts_shared_copy(g_db->self_cb);
  arts_handler_db_excl_request(g_db, &a);
  arts_shared_release(&pin);
}

#ifndef ARTS_USE_CXL
static void bare_write_far(void) { bare_request(REQ, DB_MODE_RW); }
static void bare_read_far(void) { bare_request(REQ, DB_MODE_RO); }
/* An acquire on the home rank: its request runs inline and advertises no
 * landing (where another rank owns the block the home draws one as it
 * forwards). */
static void own_write_acquire(void) {
  g_place_landing = true;
  acquire(DB_MODE_RW);
}

/* Runs `body` in a child and requires the home's refusal of a request that
 * advertised no landing for a block of BYTES bytes, from REQ. */
static void expect_refused(void (*body)(void), const char *what) {
  int fds[2];
  if (pipe(fds) != 0) {
    fail("no pipe for the child's report");
  }
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    int devnull = open("/dev/null", O_WRONLY);
    (void)dup2(devnull, STDOUT_FILENO);
    (void)dup2(fds[1], STDERR_FILENO);
    (void)close(fds[0]);
    g_expect_abort = true;
    body();
    _exit(0);
  }
  (void)close(fds[1]);
  char out[1024];
  size_t n = 0;
  ssize_t r;
  while (n < sizeof(out) - 1u &&
         (r = read(fds[0], out + n, sizeof(out) - 1u - n)) > 0) {
    n += (size_t)r;
  }
  out[n] = '\0';
  (void)close(fds[0]);
  char line[128];
  (void)snprintf(line, sizeof(line),
                 "excl: guid %lu: a request from rank %u advertised no "
                 "landing for a block of %u bytes",
                 (unsigned long)g_db->cache.db_guid, REQ, BYTES);
  int status = 0;
  if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
      WEXITSTATUS(status) != EXPECTED_ABORT || strstr(out, line) == NULL) {
    fail(what);
  }
}

/* Runs `body` in a child and requires it to return, the run going on, and
 * to leave what `served` requires of it. */
static void expect_served(void (*body)(void), bool (*served)(void),
                          const char *what) {
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    int devnull = open("/dev/null", O_WRONLY);
    (void)dup2(devnull, STDOUT_FILENO);
    (void)dup2(devnull, STDERR_FILENO);
    g_expect_abort = true;
    body();
    _exit(served() ? 0 : 1);
  }
  int status = 0;
  if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
      WEXITSTATUS(status) != 0) {
    fail(what);
  }
}

/* Every requester on another rank sizes its landing from the block's name,
 * so one that advertises none for a block with bytes is refused where the
 * home takes it in, on either axis, ending the run with the plane's line. */
static void landing_less_refused(int axis) {
  begin_bytes(axis == 0
                  ? "landing-less write request from another rank refused"
                  : "landing-less read request from another rank refused");
  expect_refused(axis == 0 ? bare_write_far : bare_read_far,
                 "the home took in a landing-less request");
  end(0);
}

/* The home took its own write request in.  The creator, OTHER, holds the
 * block: under RETAIN, as its owner, the home's FORWARD asks it to migrate
 * the block to the home, into the landing the home drew as it forwarded;
 * under PURGE the request waits behind the creator's write turn, counted and
 * at the front of the queue, and nothing leaves or wakes. */
static bool own_request_taken(void) {
#ifdef ARTS_RETENTION_RETAIN
  if (g_nsent != 1 || g_sent[0].type != MSG_DB_EXCL_FORWARD ||
      g_sent[0].rank != (int)OTHER) {
    return false;
  }
  const struct arts_msg_excl_forward_packet_s *f =
      (const struct arts_msg_excl_forward_packet_s *)g_sent[0].buf;
  return f->mode == DB_MODE_RW && f->target == HOME &&
         f->token == arts_db_token(g_db) &&
         f->home_token == arts_db_token(g_db) && f->rdzv.txid != 0u;
#else
  unsigned int rank = ~0u;
  uint64_t token = 0u;
  return g_nsent == 0 && g_wakes == 0 &&
         EXCL_STATE_W(atomic_load_explicit(&g_db->lock_state,
                                           memory_order_relaxed)) == 2u &&
         arts_home_grantreq_queue_peek_linked(&g_db->rw_waiters, &rank, NULL,
                                              NULL, &token) &&
         rank == HOME && token == arts_db_token(g_db);
#endif
}

/* The home's own request advertises no landing for a block with bytes, and
 * the home takes it in. */
static void own_request_taken_in(void) {
  begin_bytes("home's own landing-less write request taken in");
  expect_served(own_write_acquire, own_request_taken,
                "the home refused its own request, or did not take it in");
  end(0);
}
#endif

static void set_one_reader_aside(void) {
  arts_excl_set_readers_aside(&g_db->cache, 1u);
}

/* The readers a serving transition sets aside go back into the cache word's
 * read field with the answer, so the add takes a count that fills the field
 * and refuses one past it. */
static void readers_aside_field_limit(void) {
  begin("readers set aside past the read field are fatal", GUID_SIZED, false,
        HOME);
#ifdef ARTS_RETENTION_RETAIN
  uint64_t limit = CACHE_OWNER_RC_MASK;
  uint64_t serving = CACHE_MAKE_FULL(1u, CACHE_ST_GRANT, CACHE_ST_REQ,
                                     ARTS_EXCL_NO_TARGET, 1u, 0u);
#else
  uint64_t limit = CACHE_CNT_MASK;
  uint64_t serving = CACHE_MAKE(CACHE_ST_GRANT, CACHE_ST_REQ, 1u, 0u);
#endif
  uint64_t seed =
      atomic_load_explicit(&g_db->cache.cache_state, memory_order_relaxed);
  atomic_store_explicit(&g_db->cache.cache_state, serving,
                        memory_order_relaxed);
  atomic_store_explicit(&g_db->cache.ro_aside, (uint32_t)(limit - 1u),
                        memory_order_relaxed);
  set_one_reader_aside();
  if (atomic_load_explicit(&g_db->cache.ro_aside, memory_order_relaxed) !=
      (uint32_t)limit) {
    fail("the add did not fill the read field");
  }
  expect_abort(set_one_reader_aside,
               "a count set aside past the read field did not abort");
  atomic_store_explicit(&g_db->cache.ro_aside, 0u, memory_order_relaxed);
  atomic_store_explicit(&g_db->cache.cache_state, seed, memory_order_relaxed);
  end(0);
}

/* ---- the home's teardown ---- */

/* A destroy at the home, as the engine runs it: under the dispatcher's
 * pin. */
static void destroy_arrives(void) {
  arts_shared_ptr_t pin = arts_shared_copy(g_db->self_cb);
  arts_db_destroy_arrived(g_db);
  arts_shared_release(&pin);
}

/* The home descriptor of a block a creator on OTHER made and REQ cached: its
 * roster names both. */
static void begin_teardown(const char *name) {
  begin(name, GUID_SIZED, true, OTHER);
  g_teardown = true;
  arts_rank_bitset_note(&g_db->cached_ranks, REQ);
  g_poison = true;
}

/* The home retired its block and sent, in all, one notice to OTHER and one to
 * REQ. */
static bool told_roster_once(void) {
  int to_req = 0, to_other = 0;
  for (int i = 0; i < g_nsent; i++) {
    if (g_sent[i].type != MSG_DB_CACHE_DESTROY) {
      return false;
    }
    to_req += g_sent[i].rank == (int)REQ;
    to_other += g_sent[i].rank == (int)OTHER;
  }
  return g_retired && g_nsent == 2 && to_req == 1 && to_other == 1;
}

/* The frames that still held the retired home let go: the deleter runs then,
 * once, and frees everything the home allocated. */
static void end_torn_down(arts_shared_ptr_t *held) {
  if (g_deleted != 0) {
    fail("the descriptor went while a frame still held it");
  }
  arts_shared_release(held);
  if (g_live_allocs != g_allocs_at_begin) {
    fail("the retired home's deleter left an allocation behind");
  }
  end_destroyed(0);
}

#ifdef ARTS_RETENTION_RETAIN
/* ---- home scenarios ---- */

/* Two destroys reach the home while it is still held (each on its own
 * dispatch pin), the second a program error.  The first retires the block
 * and only then tells each roster rank, once (a notice that precedes the
 * retire is refused where it is recorded); the second's retire detaches
 * nothing, so it tells no rank again. */
static void home_teardown_once(void) {
  begin_teardown("the home's teardown tells each roster rank once");
  arts_shared_ptr_t held = arts_shared_copy(g_db->self_cb);
  destroy_arrives();
  if (!told_roster_once()) {
    fail("the destroy did not retire the block and tell each roster rank "
         "exactly once");
  }
  destroy_arrives();
  printf("     notices 2 then %d, retires that found the block retired %d\n",
         g_nsent - 2, g_retire_misses);
  if (g_nsent != 2) {
    fail("a second destroy told the roster again");
  }
  g_retire_misses = 0;
  end_torn_down(&held);
}

static struct arts_ooo_args_db_excl_request_s far_request(unsigned int rank,
                                                          uint32_t mode) {
  struct arts_ooo_args_db_excl_request_s a;
  memset(&a, 0, sizeof(a));
  a.requester = rank;
  a.db_guid = g_db->cache.db_guid;
  a.mode = (arts_db_access_mode_t)mode;
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
  struct arts_ooo_args_db_excl_request_s a = far_request(REQ, DB_MODE_RO);
  arts_handler_db_excl_request(g_db, &a);
  const struct arts_msg_excl_forward_packet_s *f = forward(0);
  if (f->mode != DB_MODE_RO || f->target != REQ || f->token != a.token ||
      f->home_token != 0u) {
    fail("the serve FORWARD did not carry the reader's token");
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
  struct arts_ooo_args_db_excl_request_s a = far_request(REQ, DB_MODE_RW);
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
  struct arts_ooo_args_db_excl_request_s a = far_request(REQ, DB_MODE_RW);
  struct arts_ooo_args_db_excl_request_s b = far_request(OTHER, DB_MODE_RW);
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
      far_request(READER, DB_MODE_RO);
  arts_handler_db_excl_request(g_db, &rd); /* the read phase, r = 1 */
  struct arts_ooo_args_db_excl_request_s a = far_request(REQ, DB_MODE_RW);
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
  begin(mode == DB_MODE_RO ? "read FORWARD that finds no block is dropped"
                           : "write FORWARD that finds no block is dropped",
        GUID_SIZED, false, HOME);
  arts_global_rank_id = OTHER;
  struct arts_msg_excl_forward_packet_s p;
  memset(&p, 0, sizeof(p));
  p.header.rank = HOME;
  p.db_guid = GUID_SIZED;
  p.mode = mode;
  p.target = REQ;
  p.token = FAR_TOKEN;
  p.home_token = mode == DB_MODE_RW ? arts_db_token(g_db) : 0u;
  arts_handler_db_excl_forward(&p);
  if (g_nsent != 0) {
    fail("a FORWARD that missed sent something");
  }
  end(0);
}

/* A CONFIRM that finds the home retired takes its counting step and serves
 * none of the read requests held behind the write phase it ends: each was
 * pending at the destroy, and the home's teardown frees its node. */
static void retired_confirm_serves_nothing(void) {
  begin("CONFIRM on a retired home serves no held read", GUID_SIZED, true,
        OTHER);
  struct arts_ooo_args_db_excl_request_s w = far_request(OTHER, DB_MODE_RW);
  struct arts_ooo_args_db_excl_request_s r = far_request(REQ, DB_MODE_RO);
  struct arts_ooo_args_db_excl_request_s r2 =
      far_request(READER, DB_MODE_RO);
  struct arts_ooo_args_db_excl_request_s rw = far_request(REQ, DB_MODE_RW);
  arts_handler_db_excl_request(g_db, &w);
  arts_handler_db_excl_request(g_db, &r);  /* held behind the write phase */
  arts_handler_db_excl_request(g_db, &r2); /* held too */
  arts_handler_db_excl_request(g_db, &rw); /* queued behind OTHER's turn */
  confirm_from(OTHER);                     /* chains the turn to REQ */
  int forwards = count_sent(MSG_DB_EXCL_FORWARD);
  retire();
  drop_ref_expect_held();
  confirm_from(REQ);
  if (count_sent(MSG_DB_EXCL_FORWARD) != forwards) {
    fail("the retired home answered a held read");
  }
  if (g_deleted != 1) {
    fail("the last CONFIRM kept the home count");
  }
  finish();
}

/* ---- the requester's two axes ---- */

#define ST_I CACHE_ST_IDLE
#define ST_Q CACHE_ST_REQ
#define ST_G CACHE_ST_GRANT
#define ST_GP CACHE_ST_GRANT_PURGE
#define OWNED(own, rws, ros, wc, rc)                                           \
  CACHE_MAKE_FULL(own, rws, ros, ARTS_EXCL_NO_TARGET, wc, rc)

static void check_cache(uint64_t want, uint32_t want_aside, const char *what) {
  uint64_t w =
      atomic_load_explicit(&g_db->cache.cache_state, memory_order_relaxed);
  uint32_t a =
      atomic_load_explicit(&g_db->cache.ro_aside, memory_order_relaxed);
  if (w != want || a != want_aside) {
    char m[200];
    snprintf(m, sizeof(m), "%s (word=%llx aside=%u, expected %llx and %u)",
             what, (unsigned long long)w, a, (unsigned long long)want,
             want_aside);
    fail(m);
  }
}

/* The home forwards this rank its own read request: the token its request
 * carried, a landing the owner moves no bytes into. */
static void forward_own_read(void) {
  const struct arts_msg_excl_request_packet_s *r = request_of(DB_MODE_RO);
  struct arts_msg_excl_forward_packet_s f;
  memset(&f, 0, sizeof(f));
  f.header.rank = HOME;
  f.db_guid = r->db_guid;
  f.mode = DB_MODE_RO;
  f.target = arts_global_rank_id;
  f.rdzv = r->rdzv;
  f.token = r->token;
  g_lookup = g_db->self_cb;
  arts_handler_db_excl_forward(&f);
}

static void recall(void) {
  struct arts_msg_excl_notice_packet_s p;
  memset(&p, 0, sizeof(p));
  p.header.rank = HOME;
  p.db_guid = g_db->cache.db_guid;
  g_lookup = g_db->self_cb;
  arts_handler_db_excl_recall(&p);
}

/* Two readers park on the read request, the write request leaves beside it,
 * and a RECALL overtakes the read request's answer.  The migration's DELIVER
 * wakes the writer alone and sets both readers aside; a reader that arrives
 * at the owner is a local hit; the writer's release keeps the sticky grant;
 * the home forwards the read request to this rank, the owner: exactly the
 * two resume, the RO_RETURN goes back and the early token is cleared; a
 * RECALL after it finds the read axis idle. */
static void own_read_after_migration(void) {
  begin("the owner answers its own read request held through its migration",
        GUID_SIZED, false, HOME);
  acquire(DB_MODE_RO);
  acquire(DB_MODE_RW);
  acquire(DB_MODE_RO);
  if (count_sent(MSG_DB_EXCL_REQUEST) != 2 ||
      request_of(DB_MODE_RO)->pad != 0u ||
      request_of(DB_MODE_RW)->pad != 0u) {
    fail("each axis did not send its own request, its pad written 0");
  }
  recall();
  check_cache(OWNED(0, ST_Q, ST_Q, 1u, 2u) | CACHE_OWNER_LEASED_MASK, 0u,
              "the recall did not leave its token on the read request");
  grant(DB_MODE_RW, false);
  check_wakes(1);
  check_confirm();
  check_cache(OWNED(1, ST_G, ST_Q, 1u, 0u) | CACHE_OWNER_LEASED_MASK, 2u,
              "the migration did not set the parked readers aside");
  acquire(DB_MODE_RO); /* a local hit at the owner */
  check_wakes(2);
  arts_db_release_rw(&g_db->cache, NULL);
  check_cache(OWNED(1, ST_G, ST_Q, 0u, 1u) | CACHE_OWNER_LEASED_MASK, 2u,
              "the writer's release moved the read axis");
  forward_own_read();
  check_wakes(4);
  if (count_sent(MSG_DB_EXCL_RORET) != 1 ||
      count_sent(MSG_DB_EXCL_DELIVER) != 0) {
    fail("the answer did not return the home's count alone");
  }
  check_cache(OWNED(1, ST_G, ST_I, 0u, 3u), 0u,
              "the answer did not return the readers set aside");
  recall();
  if (count_sent(MSG_DB_EXCL_RORET) != 1) {
    fail("a recall after the answer returned the count again");
  }
  arts_db_release_ro(&g_db->cache);
  arts_db_release_ro(&g_db->cache);
  arts_db_release_ro(&g_db->cache);
  check_cache(OWNED(1, ST_G, ST_I, 0u, 0u), 0u,
              "the readers' releases left the word");
  end(4);
}

/* The owner's answer to its own read request is an answer like any other:
 * it acts on the descriptor the request's token names, whose block's slot a
 * retire has emptied, and there it commits nothing, resumes and sends
 * nothing, and drops the request's count. */
static void own_read_on_retired(void) {
  begin("the owner's answer to its own read request on a retired descriptor "
        "drops the count alone",
        GUID_SIZED, false, HOME);
  acquire(DB_MODE_RO);
  acquire(DB_MODE_RW);
  grant(DB_MODE_RW, false);
  arts_db_release_rw(&g_db->cache, NULL);
  check_cache(OWNED(1, ST_G, ST_Q, 0u, 0u), 1u,
              "the migration did not set the parked reader aside");
  int sent = g_nsent;
  retire();
  drop_ref_expect_held();
  forward_own_read();
  if (g_nsent != sent) {
    fail("the answer on a retired descriptor sent something");
  }
  if (g_deleted != 1) {
    fail("the answer on a retired descriptor kept the request's count");
  }
  check_wakes(1);
  finish();
}

/* The home rank's own requests run inline: the write request migrates the
 * block here from OTHER, and the read request waits behind that write phase.
 * The DELIVER's frame sends the CONFIRM inline, the CONFIRM's flip forwards
 * the held read request to this rank, the owner, and the answer and its
 * RO_RETURN run inline too: the readers must already be set aside. */
static void own_read_inline_at_home(void) {
  begin("the home answers its own read request inside its DELIVER's frame",
        GUID_SIZED, true, OTHER);
  acquire(DB_MODE_RW); /* a migration to this rank, FORWARD to OTHER */
  acquire(DB_MODE_RO); /* held behind the write phase */
  if (count_sent(MSG_DB_EXCL_FORWARD) != 1 || g_nsent != 1) {
    fail("the home's own requests left the rank");
  }
  struct arts_msg_excl_deliver_packet_s d;
  memset(&d, 0, sizeof(d));
  d.header.rank = OTHER;
  d.db_guid = g_db->cache.db_guid;
  d.mode = DB_MODE_RW;
  d.token = forward(0)->token;
  d.home_token = forward(0)->home_token;
  g_lookup = g_db->self_cb;
  arts_handler_db_excl_deliver(&d, sizeof(d));
  check_wakes(2);
  check_cache(OWNED(1, ST_G, ST_I, 1u, 1u), 0u,
              "the inline answer did not return the reader set aside");
  uint64_t ls = atomic_load_explicit(&g_db->lock_state, memory_order_relaxed);
  if (LOCK_W(ls) != 0u || LOCK_R(ls) != 0u ||
      LOCK_PHASE(ls) != EXCL_PHASE_IDLE || LOCK_OWNER(ls) != HOME) {
    fail("the home did not take the CONFIRM and the RO_RETURN");
  }
  arts_db_release_ro(&g_db->cache);
  arts_db_release_rw(&g_db->cache, NULL);
  end(2);
}

/* The bytes leave this rank while its readers are set aside (their request
 * has not been counted at the home yet); a reader that arrives after parks
 * on the request, and the next owner's read DELIVER returns all of them. */
static void readers_aside_leave_with_the_bytes(void) {
  begin("readers set aside leave with the bytes and come back with the read "
        "grant",
        GUID_SIZED, false, HOME);
  acquire(DB_MODE_RO);
  acquire(DB_MODE_RW);
  grant(DB_MODE_RW, false);
  check_wakes(1);
  arts_db_release_rw(&g_db->cache, NULL);
  struct arts_msg_excl_forward_packet_s f;
  memset(&f, 0, sizeof(f));
  f.header.rank = HOME;
  f.db_guid = g_db->cache.db_guid;
  f.mode = DB_MODE_RW;
  f.target = OTHER;
  f.rdzv.txid = 0u;
  f.token = FAR_TOKEN;
  f.home_token = HOME_TOKEN;
  g_lookup = g_db->self_cb;
  arts_handler_db_excl_forward(&f);
  if (count_sent(MSG_DB_EXCL_DELIVER) != 1) {
    fail("the sticky owner did not ship to the next writer");
  }
  check_cache(OWNED(0, ST_I, ST_Q, 0u, 0u), 1u,
              "the ship took the readers set aside");
  acquire(DB_MODE_RO); /* parks on the read request */
  check_wakes(1);
  grant(DB_MODE_RO, false);
  check_wakes(3);
  check_cache(OWNED(0, ST_I, ST_G, 0u, 2u) | CACHE_OWNER_LEASED_MASK, 0u,
              "the read grant did not return the readers set aside");
  arts_db_release_ro(&g_db->cache);
  arts_db_release_ro(&g_db->cache);
  end(3);
}

/* The writer a migration's DELIVER wakes runs and releases at once, the
 * home forwards the next migration and the bytes leave for OTHER, and
 * OTHER's read DELIVER of this rank's read request comes back, all before
 * the DELIVER's frame goes on: the parked reader was set aside before the
 * wake, so the read grant finds it. */
static void migration_ends_in_wake(void) {
  arts_db_release_rw(&g_db->cache, NULL);
  struct arts_msg_excl_forward_packet_s f;
  memset(&f, 0, sizeof(f));
  f.header.rank = HOME;
  f.db_guid = g_db->cache.db_guid;
  f.mode = DB_MODE_RW;
  f.target = OTHER;
  f.token = FAR_TOKEN;
  f.home_token = HOME_TOKEN;
  g_lookup = g_db->self_cb;
  arts_handler_db_excl_forward(&f);
  check_cache(OWNED(0, ST_I, ST_Q, 0u, 0u), 1u,
              "the reader was not set aside before the writer woke");
  grant(DB_MODE_RO, false);
}

static void readers_aside_before_the_wake(void) {
  begin("readers are set aside before the migration wakes a writer", GUID_SIZED,
        false, HOME);
  acquire(DB_MODE_RO);
  acquire(DB_MODE_RW);
  g_wake_hook = migration_ends_in_wake;
  grant(DB_MODE_RW, false);
  check_wakes(2);
  check_cache(OWNED(0, ST_I, ST_G, 0u, 1u) | CACHE_OWNER_LEASED_MASK, 0u,
              "the read grant did not return the reader set aside");
  arts_db_release_ro(&g_db->cache);
  end(2);
}

/* The serve's gate holds for the batch it drains.  The owner, its write
 * grant sticky, is forwarded C's read: thread A passes the gate and demotes,
 * and stalls before its drain.  Meanwhile thread B serves C, a local writer
 * W asks for the write turn and the home grants it to this resident rank, W
 * runs, and D's read is forwarded and parked behind W.  A then drains D's
 * node: it must judge the gate again on a later word and push D back, and
 * W's release serves D. */
static void serve_stall(void) {
  /* B */
  owner_serve_readers(&g_db->cache);
  if (count_sent(MSG_DB_EXCL_DELIVER) != 1) {
    fail("the second server did not serve C");
  }
  /* W */
  acquire(DB_MODE_RW);
  const struct arts_msg_excl_request_packet_s *r = request_of(DB_MODE_RW);
  struct arts_msg_excl_forward_packet_s f;
  memset(&f, 0, sizeof(f));
  f.header.rank = HOME;
  f.db_guid = r->db_guid;
  f.mode = DB_MODE_RW;
  f.target = REQ;
  f.token = r->token;
  f.home_token = HOME_TOKEN;
  arts_handler_db_excl_forward(&f);
  check_confirm();
  check_wakes(1);
  /* D */
  memset(&f, 0, sizeof(f));
  f.header.rank = HOME;
  f.db_guid = r->db_guid;
  f.mode = DB_MODE_RO;
  f.target = READER;
  f.token = FAR_TOKEN + READER;
  f.tag = 1u;
  arts_handler_db_excl_forward(&f);
}

static int delivers_to(unsigned int rank) {
  int n = 0;
  for (int i = 0; i < g_nsent; i++) {
    n += g_sent[i].type == MSG_DB_EXCL_DELIVER && g_sent[i].rank == (int)rank;
  }
  return n;
}

static void serve_rejudges_gate(void) {
  begin("the serve judges its gate again after its drain", GUID_SIZED, false,
        HOME);
  atomic_store_explicit(&g_db->cache.cache_state, OWNED(1, ST_G, ST_I, 0u, 0u),
                        memory_order_relaxed);
  g_lookup = g_db->self_cb;
  g_ensure_hook = serve_stall;
  struct arts_msg_excl_forward_packet_s f;
  memset(&f, 0, sizeof(f));
  f.header.rank = HOME;
  f.db_guid = g_db->cache.db_guid;
  f.mode = DB_MODE_RO;
  f.target = OTHER;
  f.token = FAR_TOKEN + OTHER;
  arts_handler_db_excl_forward(&f); /* A */
  if (g_ensure_hook != NULL) {
    fail("the serve never reached its drain");
  }
  if (delivers_to(OTHER) != 1 || delivers_to(READER) != 0) {
    fail("a read copy left while a writer of this rank was live");
  }
  check_cache(OWNED(1, ST_G, ST_I, 1u, 0u), 0u,
              "the writer does not hold the write grant");
  arts_db_release_rw(&g_db->cache, NULL);
  if (delivers_to(READER) != 1) {
    fail("the writer's release did not serve the read pushed back");
  }
  check_cache(OWNED(1, ST_I, ST_I, 0u, 0u), 0u,
              "the serve did not relinquish the write grant");
  end(1);
}

static void read_grant_at_owner(void) { grant(DB_MODE_RO, false); }

static void own_read_not_owner(void) { forward_own_read(); }

/* A read DELIVER never meets this rank resident, and the home forwards a
 * rank its own read request only when it names that rank the owner. */
static void stray_read_answers(void) {
  begin("a read grant at an owner, and its own read forwarded to a rank that "
        "is not the owner, are fatal",
        GUID_SIZED, false, HOME);
  acquire(DB_MODE_RO);
  expect_abort(own_read_not_owner,
               "the owner's answer at a rank that is not the owner did not "
               "abort");
  acquire(DB_MODE_RW);
  grant(DB_MODE_RW, false);
  expect_abort(read_grant_at_owner, "a read grant at an owner did not abort");
  forward_own_read();
  arts_db_release_rw(&g_db->cache, NULL);
  arts_db_release_ro(&g_db->cache);
  end(2);
}

static void sweep_home(void) {
  begin("teardown sweep drops the home count of an unconfirmed write request",
        GUID_SIZED, true, OTHER);
  struct arts_ooo_args_db_excl_request_s a = far_request(REQ, DB_MODE_RW);
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

/* ---- the home's side of requests from other ranks ---- */

static void far_request(unsigned int rank, arts_db_access_mode_t mode) {
  struct arts_ooo_args_db_excl_request_s a;
  memset(&a, 0, sizeof(a));
  a.requester = rank;
  a.db_guid = g_db->cache.db_guid;
  a.mode = (uint32_t)mode;
  a.token = FAR_TOKEN + rank;
  arts_handler_db_excl_request(g_db, &a);
}

static void far_release(unsigned int rank, arts_db_access_mode_t mode) {
  struct arts_ooo_args_db_excl_release_s r;
  memset(&r, 0, sizeof(r));
  r.releaser = rank;
  r.db_guid = g_db->cache.db_guid;
  r.mode = (uint32_t)mode;
  arts_handler_db_excl_release(g_db, &r);
}

/* The nth grant the home sent: to `rank`, in `mode`, echoing that rank's
 * token. */
static void check_far_grant(int nth, unsigned int rank,
                            arts_db_access_mode_t mode) {
  const struct arts_msg_excl_grant_packet_s *g =
      (const struct arts_msg_excl_grant_packet_s *)sent(MSG_DB_EXCL_GRANT, nth);
  int at = 0;
  for (int i = 0, k = nth; i < g_nsent; i++) {
    if (g_sent[i].type == MSG_DB_EXCL_GRANT && k-- == 0) {
      at = i;
    }
  }
  if (g_sent[at].rank != (int)rank || g->mode != (uint32_t)mode ||
      g->token != FAR_TOKEN + rank) {
    fail("a grant did not answer the request it was owed to");
  }
}

static void check_lock(uint32_t w, uint32_t r, const char *what) {
  uint64_t s = atomic_load_explicit(&g_db->lock_state, memory_order_relaxed);
  if (EXCL_STATE_TEARDOWN(s) != 0u || EXCL_STATE_W(s) != w ||
      EXCL_STATE_R(s) != r) {
    fail(what);
  }
}

/* A read phase is open and another rank's write request waits behind it.
 * That rank's read request is granted into the phase, and the write turn
 * follows both readers' releases. */
static void read_joins_open_phase(void) {
  begin("read request granted into an open read phase, its rank's write "
        "request waiting",
        GUID_SIZED, true, OTHER);
  far_release(OTHER, DB_MODE_RW); /* the creator's hold */
  far_request(READER, DB_MODE_RO);
  check_far_grant(0, READER, DB_MODE_RO);
  far_request(REQ, DB_MODE_RW);
  if (count_sent(MSG_DB_EXCL_GRANT) != 1) {
    fail("a write request was granted into a read phase");
  }
  far_request(REQ, DB_MODE_RO);
  check_far_grant(1, REQ, DB_MODE_RO);
  check_lock(1u, 2u,
             "the read request was not counted beside the "
             "waiting writer");
  far_release(READER, DB_MODE_RO);
  if (count_sent(MSG_DB_EXCL_GRANT) != 2) {
    fail("the write turn did not wait for the second reader");
  }
  far_release(REQ, DB_MODE_RO);
  check_far_grant(2, REQ, DB_MODE_RW);
  far_release(REQ, DB_MODE_RW);
  check_lock(0u, 0u, "the turns did not all come back");
  if (g_nsent != 3) {
    fail("the home sent something other than the three grants");
  }
  end(0);
}

/* A rank's read request arrives while that rank holds the write turn: it is
 * counted, held, and granted as a read once the turn has come back.  The
 * home answers it with nothing else. */
static void read_held_through_own_write_turn(void) {
  begin("read request held through its rank's write turn", GUID_SIZED, true,
        OTHER);
  far_release(OTHER, DB_MODE_RW);
  far_request(REQ, DB_MODE_RW);
  check_far_grant(0, REQ, DB_MODE_RW);
  far_request(REQ, DB_MODE_RO);
  check_lock(1u, 1u, "the held read request was not counted");
  if (g_nsent != 1) {
    fail("the home answered a read request under its rank's write turn");
  }
  far_release(REQ, DB_MODE_RW);
  check_far_grant(1, REQ, DB_MODE_RO);
  far_release(REQ, DB_MODE_RO);
  check_lock(0u, 0u, "the turns did not all come back");
  if (g_nsent != 2) {
    fail("the home sent something other than the two grants");
  }
  end(0);
}

/* A read request that reaches a torn-down home was pending at the destroy:
 * it is neither counted nor answered. */
static void tear_down(void) {
  atomic_store_explicit(&g_db->lock_state, LOCK_MAKE_STATE_TD(1u, 0u, 0u, 0u),
                        memory_order_relaxed);
}

static void refused_read(void) {
  begin("read request refused by a torn-down home is answered nothing",
        GUID_SIZED, true, OTHER);
  g_malloc_hook = tear_down; /* the read's queue node */
  far_request(REQ, DB_MODE_RO);
  if (g_nsent != 0) {
    fail("the torn-down home answered a read request");
  }
  if (atomic_load_explicit(&g_db->lock_state, memory_order_relaxed) !=
      LOCK_MAKE_STATE_TD(1u, 0u, 0u, 0u)) {
    fail("the torn-down home counted a read request");
  }
  end(0);
}

/* The home's teardown runs where its word reaches "marked, nothing counted".
 * A destroy that finds a write turn still out (the creator's, whose release
 * is on its way) only marks the word: it retires nothing and tells no rank;
 * the release that brings the count to zero retires the block and only then
 * tells each roster rank, once.  A destroy that finds nothing counted does
 * both at once.  A second destroy, a program error, claims no teardown.  A
 * request whose frame took the descriptor before the retire and reaches the
 * torn-down word was pending at the destroy: it is neither counted nor
 * answered on either axis, the read request's node freed by its handler, the
 * write request's left on the queue, which goes with the descriptor. */
static void home_teardown(bool release_out) {
  begin_teardown(release_out
                     ? "a destroy with a release out marks the home; the "
                       "release tears it down, telling each roster rank once"
                     : "a destroy with nothing out tears the home down at "
                       "once, telling each roster rank once");
  /* The pins of the frames that run on the home after its retire. */
  arts_shared_ptr_t held = arts_shared_copy(g_db->self_cb);
  if (!release_out) {
    far_release(OTHER, DB_MODE_RW); /* the creator's hold */
  }
  destroy_arrives();
  if (release_out) {
    if (g_retired || g_nsent != 0) {
      fail("a destroy tore the home down while a release was still counted");
    }
    if (atomic_load_explicit(&g_db->lock_state, memory_order_relaxed) !=
        LOCK_MAKE_STATE_TD(1u, 0u, 1u, 0u)) {
      fail("the destroy did not mark the home's word and leave its count");
    }
    far_release(OTHER, DB_MODE_RW);
  }
  if (!told_roster_once()) {
    fail("the teardown did not retire the block and tell each roster rank "
         "exactly once");
  }
  const uint64_t torn = LOCK_MAKE_STATE_TD(1u, 0u, 0u, 0u);
  destroy_arrives();
  if (g_nsent != 2 || g_retire_misses != 0 ||
      atomic_load_explicit(&g_db->lock_state, memory_order_relaxed) != torn) {
    fail("a second destroy claimed a teardown");
  }
  long allocs = g_live_allocs;
  far_request(REQ, DB_MODE_RO);
  if (g_nsent != 2 ||
      atomic_load_explicit(&g_db->lock_state, memory_order_relaxed) != torn) {
    fail("the torn-down home counted or answered a read request");
  }
  if (g_live_allocs != allocs) {
    fail("the refused read request's node was not freed");
  }
  far_request(REQ, DB_MODE_RW);
  if (g_nsent != 2 ||
      atomic_load_explicit(&g_db->lock_state, memory_order_relaxed) != torn) {
    fail("the torn-down home counted or answered a write request");
  }
  unsigned int rank = ~0u;
  uint64_t token = 0u;
  if (g_live_allocs != allocs + 1 ||
      !arts_home_grantreq_queue_peek_linked(&g_db->rw_waiters, &rank, NULL,
                                            NULL, &token) ||
      rank != REQ || token != FAR_TOKEN + REQ) {
    fail("the refused write request's node is not the one left on the queue");
  }
  end_torn_down(&held);
}

/* ---- the requester's two axes ---- */

static void check_cache(uint64_t want, uint32_t want_aside, const char *what) {
  uint64_t w =
      atomic_load_explicit(&g_db->cache.cache_state, memory_order_relaxed);
  uint32_t a =
      atomic_load_explicit(&g_db->cache.ro_aside, memory_order_relaxed);
  if (w != want || a != want_aside) {
    char m[160];
    snprintf(m, sizeof(m), "%s (word=%llx aside=%u, expected %llx and %u)",
             what, (unsigned long long)w, a, (unsigned long long)want,
             want_aside);
    fail(m);
  }
}

static void check_released(int n, arts_db_access_mode_t last) {
  if (count_sent(MSG_DB_EXCL_RELEASE) != n) {
    fail("a turn's end sent no release, or one was sent early");
  }
  if (n != 0 && ((const struct arts_msg_excl_release_packet_s *)sent(
                     MSG_DB_EXCL_RELEASE, n - 1))
                        ->mode != (uint32_t)last) {
    fail("a turn's end returned the other axis's grant");
  }
}

#define ST_I CACHE_ST_IDLE
#define ST_Q CACHE_ST_REQ
#define ST_G CACHE_ST_GRANT

/* The write grant arrives while the read request is out: it wakes the writer
 * alone, a reader that arrives under it is a local hit, and the turn ends
 * with the first readers still parked.  A second write turn finds one more
 * parked.  The read grant wakes all three. */
static void write_turns_set_readers_aside(void) {
  begin("write grants wake writers alone; the read grant returns the readers "
        "set aside",
        GUID_SIZED, false, HOME);
  acquire(DB_MODE_RO);
  acquire(DB_MODE_RO);
  acquire(DB_MODE_RW);
  if (count_sent(MSG_DB_EXCL_REQUEST) != 2 ||
      request_of(DB_MODE_RO)->pad != 0u ||
      request_of(DB_MODE_RW)->pad != 0u) {
    fail("each axis did not send its own request, its pad written 0");
  }
  grant(DB_MODE_RW, false);
  check_wakes(1);
  check_cache(CACHE_MAKE(ST_G, ST_Q, 1u, 0u), 2u,
              "the write grant did not set the parked readers aside");
  acquire(DB_MODE_RO); /* a local hit under the write grant */
  check_wakes(2);
  arts_db_release_ro(&g_db->cache);
  check_released(0, DB_MODE_RW);
  arts_db_release_rw(&g_db->cache, NULL);
  check_released(1, DB_MODE_RW);
  check_cache(CACHE_MAKE(ST_I, ST_Q, 0u, 0u), 2u,
              "the write turn did not end with the readers parked");
  acquire(DB_MODE_RO); /* joins the read request that is out */
  acquire(DB_MODE_RW);
  if (count_sent(MSG_DB_EXCL_REQUEST) != 3) {
    fail("the second write turn's request did not leave alone");
  }
  grant(DB_MODE_RW, false);
  check_wakes(3);
  check_cache(CACHE_MAKE(ST_G, ST_Q, 1u, 0u), 3u,
              "the second write grant did not add the reader parked since");
  arts_db_release_rw(&g_db->cache, NULL);
  check_released(2, DB_MODE_RW);
  grant(DB_MODE_RO, false);
  check_wakes(6);
  check_cache(CACHE_MAKE(ST_I, ST_G, 0u, 3u), 0u,
              "the read grant did not return the readers set aside");
  arts_db_release_ro(&g_db->cache);
  arts_db_release_ro(&g_db->cache);
  check_released(2, DB_MODE_RW);
  arts_db_release_ro(&g_db->cache);
  check_released(3, DB_MODE_RO);
  end(6);
}

/* The writer a write grant wakes runs and releases at once, the turn's end
 * reaches the home and the read grant comes back, all before the grant's
 * frame goes on: the parked reader was set aside before the wake, so the
 * read grant finds it. */
static void turn_ends_in_wake(void) {
  arts_db_release_rw(&g_db->cache, NULL);
  check_released(1, DB_MODE_RW);
  check_cache(CACHE_MAKE(ST_I, ST_Q, 0u, 0u), 1u,
              "the reader was not set aside before the writer woke");
  grant(DB_MODE_RO, false);
}

static void write_turn_ends_inside_wake(void) {
  begin("readers are set aside before the write grant wakes a writer",
        GUID_SIZED, false, HOME);
  acquire(DB_MODE_RO);
  acquire(DB_MODE_RW);
  g_wake_hook = turn_ends_in_wake;
  grant(DB_MODE_RW, false);
  check_wakes(2);
  check_cache(CACHE_MAKE(ST_I, ST_G, 0u, 1u), 0u,
              "the read grant did not return the reader set aside");
  arts_db_release_ro(&g_db->cache);
  check_released(2, DB_MODE_RO);
  end(2);
}

/* A holder of the write turn becomes a reader while its rank's read request
 * is out: it holds on, the turn ends at its read release, and the parked
 * reader still waits for the read grant. */
static void downgrade_with_read_request_out(void) {
  begin("downgrade while the read request is out", GUID_SIZED, false, HOME);
  acquire(DB_MODE_RO);
  acquire(DB_MODE_RW);
  grant(DB_MODE_RW, false);
  check_wakes(1);
  arts_db_join_read(&g_db->cache);
  arts_db_release_rw(&g_db->cache, NULL);
  check_released(0, DB_MODE_RW);
  check_cache(CACHE_MAKE(ST_G, ST_Q, 0u, 1u), 1u,
              "the joined reader does not hold the write turn");
  arts_db_release_ro(&g_db->cache);
  check_released(1, DB_MODE_RW);
  check_cache(CACHE_MAKE(ST_I, ST_Q, 0u, 0u), 1u,
              "the turn did not end at the joined reader's release");
  check_wakes(1);
  grant(DB_MODE_RO, false);
  check_wakes(2);
  arts_db_release_ro(&g_db->cache);
  check_released(2, DB_MODE_RO);
  end(2);
}

static void read_grant_now(void) { grant(DB_MODE_RO, false); }

/* The home grants a read request only once no write turn is out, so a read
 * grant that meets the rank's write grant, the readers set aside, answers
 * nothing; so does a second grant of one request. */
static void stray_read_grants(void) {
  begin("read grant under the rank's write grant is fatal", GUID_SIZED, false,
        HOME);
  acquire(DB_MODE_RO);
  acquire(DB_MODE_RW);
  grant(DB_MODE_RW, false);
  expect_abort(read_grant_now,
               "a read grant under the write grant did not abort");
  arts_db_release_rw(&g_db->cache, NULL);
  grant(DB_MODE_RO, false);
  expect_abort(read_grant_now, "a second read grant did not abort");
  arts_db_release_ro(&g_db->cache);
  end(2);
}

#ifdef ARTS_USE_CXL
/* Where grants carry no payload a request advertises no landing whatever the
 * block's size, and the home grants it, on either axis. */
static void landing_less_granted(void) {
  begin_bytes("landing-less requests granted where grants carry no payload");
  far_release(OTHER, DB_MODE_RW); /* the creator's hold */
  bare_request(REQ, DB_MODE_RW);
  check_far_grant(0, REQ, DB_MODE_RW);
  far_release(REQ, DB_MODE_RW);
  bare_request(REQ, DB_MODE_RO);
  check_far_grant(1, REQ, DB_MODE_RO);
  far_release(REQ, DB_MODE_RO);
  check_lock(0u, 0u, "the turns did not all come back");
  if (g_nsent != 2) {
    fail("the home sent something other than the two grants");
  }
  end(0);
}
#endif

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
  answer_retired();
#ifndef ARTS_USE_CXL
  landing_less_refused(0);
  landing_less_refused(1);
  own_request_taken_in();
#endif
#if PAIRS
  paired(0);
  paired(1);
  paired(2);
  retired_in_flight(true);
  retired_in_flight(false);
#endif
  sweep_rows();
  readers_aside_field_limit();
#ifdef ARTS_RETENTION_RETAIN
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
  retired_confirm_serves_nothing();
  sweep_home();
  home_teardown_once();
  own_read_after_migration();
  own_read_on_retired();
  own_read_inline_at_home();
  readers_aside_leave_with_the_bytes();
  readers_aside_before_the_wake();
  serve_rejudges_gate();
  stray_read_answers();
#else
  write_turns_set_readers_aside();
  write_turn_ends_inside_wake();
  downgrade_with_read_request_out();
  stray_read_grants();
#ifdef ARTS_USE_CXL
  landing_less_granted();
#endif
  self_grant();
  self_grant_in_body();
  self_read_grant();
  read_joins_open_phase();
  read_held_through_own_write_turn();
  refused_read();
  home_teardown(true);
  home_teardown(false);
#endif
  printf("PASS token_pairing_excl\n");
  return 0;
}
