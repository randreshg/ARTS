/* SPDX-License-Identifier: Apache-2.0
 *
 * On VAL and INV each open request axis of a cache -- the read request, the
 * write request and the publish flight -- owns one count on the requester's
 * descriptor: the CAS that opens the axis takes it before the request leaves,
 * and whichever frame commits the transition that closes the axis drops it
 * once, at its end (coherence.h).  An answer acts on the descriptor its token
 * names, and on a retired one drops that count and does nothing else.
 *
 * Pairing oracle.  Each scenario builds a descriptor whose control block
 * carries the test's one reference and a counting deleter, drives the
 * transitions through the real code (the arm's acquire, the grant plane, the
 * answers' entries, the rendezvous callbacks, a create's claim, the teardown
 * sweep), then drops its reference and requires the deleter to run exactly
 * then: a count left behind keeps it from running, a count dropped twice runs
 * it before.  The wire is stubbed: a request is captured, and its answer is
 * built from it, so every answer echoes the token its request carried.  Every
 * landing a request draws is ended exactly once, and every buffer hold a
 * woken EDT's slot takes is dropped by the release the scenario stands in for.
 *
 * Scenarios, in every VAL and INV cell:
 *   read axis   closed by its answer; an answer on a retired descriptor; a
 *               one-sided answer's pairing callback, on a live and on a
 *               retired descriptor, and a pairing the teardown discards (the
 *               count is then the sweep's); opened while the rank's write
 *               request is out, the grant serving the writer alone and an
 *               answer that moves no bytes at the write holder serving the
 *               reader against the holder's buffer; a one-sided answer at
 *               the write holder likewise, the holder's buffer staying; kept
 *               open by a create's claim and closed by its own answer after
 *               a size round; a reply of each kind (one that moves no bytes,
 *               one-sided, size round) arriving with no request out is
 *               fatal;
 *     INV:      a fetch opened while the write request is out, kill-marked
 *               by a round, whose reply serves once, purges and acks the
 *               round; a kill-marked fetch its size round re-sends, the ack
 *               kept for the re-sent fetch's reply; (OWNER) opened while
 *               the rank's install awaits its confirmation, the CONFIRM_ACK
 *               waking the writer alone;
 *   write axis  closed by its answer (HOME: the grant; OWNER: the
 *               CONFIRM_ACK its CONFIRM asks for), a size round leaving
 *               it open; by a create's claim; a size round, a grant and
 *               (OWNER) a CONFIRM_ACK on a retired descriptor; a
 *               one-sided grant's pairing, live and retired; re-opened by
 *               the rearm inside a grant (where the fuzz points exist); the
 *               home's own request, its grant run on the home (under PURGE
 *               served from the front of its queue) while the writer it
 *               wakes drops every other reference, and the same request
 *               served inside its own self-sent body once the right is idle
 *               at the home (RETAIN: the home revokes itself and ships the
 *               grant to itself; PURGE: it serves its own front);
 *   the destroy's gate at the home: the claiming creator's pre-claim write
 *               request absorbed before the destroy, which then tears down;
 *               the destroy waiting for that request, whose arrival tears
 *               down; the same request from a rank that claimed nothing,
 *               answered its size;
 *   publish flight (where releases publish) closed by its ACK; an ACK on a
 *               retired descriptor; the home's flight relaunched by its ACK
 *               and landed inside the relaunched publish (INV: by its own
 *               round's ACK, which arrives while the round is still
 *               sending);
 *   the teardown sweep: one count per open axis.
 * Wherever the last reference goes inside a frame, nothing may touch the
 * descriptor once its deleter ran, and a deleter may run only once every
 * self-sent body around it returned.
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

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#endif

#if !defined(ARTS_READ_SOURCE_OWNER) || defined(ARTS_VALIDITY_INV)
#define PUBLISHES 1 /* releases publish to the home: a flight axis exists */
#else
#define PUBLISHES 0
#endif

#define HOME 0u
#define REQ 1u
#define SIZE 64u
/* A block whose GUID bounds its size (a first touch sends a landing), and one
 * whose GUID does not (a first touch is a claimable stub). */
#define GUID_SIZED                                                             \
  ARTS_GUID_MAKE(ARTS_GUID_DB, HOME, ARTS_GUID_DB_KEY(1u, 0x51u))
#define GUID_CLAIM                                                             \
  ARTS_GUID_MAKE(ARTS_GUID_DB, HOME,                                           \
                 ARTS_GUID_DB_KEY(ARTS_GUID_DB_SZHINT_NONE, 0x52u))

unsigned int arts_global_rank_id = REQ;
unsigned int arts_global_rank_count = 2u;
uint64_t arts_guid_table_count;
struct arts_runtime_shared_s arts_node_info;
ARTS_THREAD_LOCAL_IE struct arts_runtime_private_s arts_thread_info;

#include "arts/system/shutdown.h"
#include "counter_stubs.h"

static const char *g_scenario = "setup";

static void fail(const char *what) {
  printf("FAIL token_axis_pairing_valinv: %s: %s\n", g_scenario, what);
  fflush(stdout);
  _exit(1);
}

/* Set in a scenario's child whose step must end the run. */
static bool g_expect_abort;
#define EXPECTED_ABORT 42

_Noreturn void arts_exit(int rc, enum arts_exit_origin origin) {
  (void)origin;
  if (g_expect_abort) {
    _exit(EXPECTED_ABORT);
  }
  printf("FAIL token_axis_pairing_valinv: %s: the runtime aborted\n",
         g_scenario);
  fflush(stdout);
  _exit(rc ? rc : 1);
}

#if !defined(__SANITIZE_ADDRESS__)
/* A frame that reads a descriptor whose deleter filled it follows the fill
 * as pointers and faults; the report names the scenario (stdout is
 * unbuffered). */
static void on_fault(int sig) {
  static const char head[] = "FAIL token_axis_pairing_valinv: ";
  static const char tail[] = ": a frame faulted\n";
  (void)sig;
  (void)!write(1, head, sizeof(head) - 1u);
  (void)!write(1, g_scenario, strlen(g_scenario));
  (void)!write(1, tail, sizeof(tail) - 1u);
  _exit(1);
}
#endif

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
/* A scenario acts inside a window through a hook, which stays armed until it
 * reports that it acted. */
static bool (*g_fuzz_hook)(void);
void arts_sched_fuzz_point(void) {
  bool (*h)(void) = g_fuzz_hook;
  if (h != NULL) {
    g_fuzz_hook = NULL;
    if (!h()) {
      g_fuzz_hook = h;
    }
  }
}
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
    printf("FAIL token_axis_pairing_valinv: %s: %s reached\n", g_scenario,     \
           __func__);                                                          \
    fflush(stdout);                                                            \
    _exit(1);                                                                  \
  }
UNREACHED(bool arts_db_create_install_local(arts_shared_ptr_t cb,
                                            struct arts_route_item_s *s))
UNREACHED(void arts_db_free(void *ptr))
UNREACHED(void arts_db_buf_ref_release_cb(void *arg))
UNREACHED(bool arts_net_rdzv_local(const void *p, uint64_t len, uint64_t *a,
                                   uint64_t *k))
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
UNREACHED(struct arts_db_buffer_s *arts_db_buf_install(
    struct arts_db_cache_s *c, uint64_t v, const void *d, uint64_t s))
UNREACHED(void arts_db_buf_bump_inplace(struct arts_db_cache_s *c, uint64_t v))

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
/* The version the last landed install offered the slot. */
static uint64_t g_installed_v;
struct arts_db_buffer_s *arts_db_buf_install_landed(struct arts_db_cache_s *c,
                                                    uint64_t v,
                                                    struct arts_db_buffer_s *l,
                                                    uint64_t s) {
  (void)c;
  (void)s;
  g_installed_v = v;
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

/* ---- the wire: requests are captured, answers built from them ---- */

#define MAXSENT 16
static struct {
  int rank;
  unsigned int type;
  unsigned int len;
  _Alignas(8) char buf[256];
} g_sent[MAXSENT];
static int g_nsent;

/* A scenario's stand-in for the other rank answering at once, on another
 * thread, while the sender is still running: called once, when a message of
 * the armed type leaves. */
static void (*g_send_hook)(void);
static unsigned int g_send_hook_type;

void arts_transport_send_async(int rank, char *message, unsigned int length) {
  if (g_nsent == MAXSENT || length > sizeof(g_sent[0].buf)) {
    fail("too many messages");
  }
  g_sent[g_nsent].rank = rank;
  g_sent[g_nsent].type = ((struct arts_msg_header_s *)message)->message_type;
  g_sent[g_nsent].len = length;
  memcpy(g_sent[g_nsent].buf, message, length);
  g_nsent++;
  void (*h)(void) = g_send_hook;
  if (h != NULL && g_sent[g_nsent - 1].type == g_send_hook_type) {
    g_send_hook = NULL;
    h();
  }
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

/* A self-send runs the body on the descriptor, as the engine does for an
 * installed one.  The depth lets the deleter tell whether it runs inside such
 * a body. */
static int g_dispatch_depth;

void arts_ooo_dispatch_on(arts_shared_ptr_t h, ooo_kind_t kind,
                          arts_guid_t guid, const void *args, uint32_t size) {
  char copy[256];
  if (size > sizeof(copy) || arts_shared_tag(h) != (uint64_t)guid) {
    fail("a self-send on a descriptor that is not installed");
  }
  memcpy(copy, args, size);
  void *obj = arts_shared_get(h);
  g_dispatch_depth++;
  switch (kind) {
  case OOO_DB_GRANT_REQUEST:
    arts_handler_db_grant_request(obj, copy);
    break;
#if PUBLISHES
  case OOO_DB_PUBLISH:
    arts_handler_db_publish(obj, copy);
    break;
#endif
#ifdef ARTS_VALIDITY_VAL
  case OOO_DB_SNAPSHOT_REQUEST:
#else
  case OOO_DB_INV_REQUEST:
#endif
#ifdef ARTS_VALIDITY_VAL
    arts_handler_db_snapshot_request(obj, copy);
#else
    arts_handler_db_inv_request(obj, copy);
#endif
    break;
  default:
    fail("an unexpected self-send");
  }
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
/* The next woken waiter's EDT destroys the block in its body and completes:
 * its release drops the buffer hold its slot resolved to, and the test's
 * reference, which stands for the install's and the EDT's, goes at the
 * wake. */
static bool g_wake_drops_ref;
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
/* The buffer hold the slot's resolver handed the EDT, which the EDT's release
 * drops after the arm's own release. */
static void release_buffer_hold(struct arts_edt_s *edt) {
  struct arts_db_buffer_s *b = arts_db_buf_from_data(edt->dep_acq[0].ptr);
  edt->dep_acq[0].ptr = NULL;
  if (b != NULL) {
    arts_shared_ptr_t h = b->cb;
    arts_db_buf_release(&h);
  }
}
void arts_db_acquire_account(struct arts_edt_s *edt) {
  g_wakes++;
  if (g_wake_drops_ref) {
    g_wake_drops_ref = false;
    release_buffer_hold(edt);
    arts_shared_ptr_t h = g_db->self_cb;
    arts_shared_release(&h);
  }
}
void arts_db_acquire_resolved(struct arts_edt_s *edt, unsigned int slot) {
  (void)edt;
  (void)slot;
  fail("a write turn taken without a request");
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
 * sanitizer) instead of freeing it, so a frame that touches it afterwards is
 * caught: a write by the fill check, a read by the sanitizer, or by the fill
 * read as pointers. */
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

/* A requester's first-touch cache, or (home) the block's home descriptor of a
 * block a remote creator made.  The control block holds the test's
 * reference, which stands for the install's. */
static void begin(const char *name, arts_guid_t guid, bool home) {
  g_scenario = name;
  arts_global_rank_id = home ? HOME : REQ;
  size_t sz = (sizeof(struct arts_db_s) + 63u) & ~(size_t)63u;
  g_db_bytes = sz;
  g_db = (struct arts_db_s *)aligned_alloc(64, sz);
  memset(g_db, 0, sz);
  g_db->db_type = ARTS_DB;
  arts_db_cache_init(&g_db->cache, guid, home ? SIZE : 0u,
                     home ? ARTS_DB_INIT_HOME_RECV : ARTS_DB_INIT_STUB, REQ);
  g_db->self_cb = arts_shared_make(g_db, db_deleter);
  arts_shared_set_tag(g_db->self_cb, (uint64_t)guid);
  g_deleted = 0;
  g_nsent = 0;
  g_wakes = 0;
  g_nedts = 0;
  g_expect_arg = NULL;
}

/* The block's retire, as the route table's: its control block's tag goes. */
static void retire(void) { arts_shared_set_tag(g_db->self_cb, 0u); }

/* The home's teardown retires the descriptor: the tag goes, and so does the
 * install's reference, which the test's stands for. */
bool arts_ooo_retire(arts_guid_t key, arts_shared_ptr_t cb) {
  if (cb != g_db->self_cb || arts_shared_tag(cb) != (uint64_t)key) {
    fail("a retire of another object");
  }
  arts_shared_set_tag(cb, 0u);
  arts_shared_ptr_t h = cb;
  arts_shared_release(&h);
  return true;
}

/* A woken EDT's slot holds a buffer until the release the scenario stands in
 * for drops it. */
static void free_edts(void) {
  for (int i = 0; i < g_nedts; i++) {
    if (g_edts[i]->acq[0].ptr != NULL) {
      fail("an EDT's buffer hold was never released");
    }
    free(g_edts[i]);
  }
}

static void end(int wakes) {
  if (g_wakes != wakes) {
    char m[96];
    snprintf(m, sizeof(m), "%d waiters woke, expected %d", g_wakes, wakes);
    fail(m);
  }
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
    fail("a landing was not ended exactly once");
  }
  free_edts();
  printf("ok   %s\n", g_scenario);
}

/* After a scenario whose last reference a woken writer dropped: the deleter
 * ran exactly once the frames let go, and nothing wrote the descriptor
 * after it. */
static void end_dropped(int wakes) {
  if (g_wakes != wakes) {
    char m[96];
    snprintf(m, sizeof(m), "%d waiters woke, expected %d", g_wakes, wakes);
    fail(m);
  }
  if (g_expect_arg != NULL) {
    fail("an expectation left unfired");
  }
  if (g_deleted != 1 || g_poisoned == NULL) {
    fail("the deleter did not run once the frames let go");
  }
  if (g_landings != 0) {
    fail("a landing was not ended exactly once");
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
  free_edts();
  printf("ok   %s\n", g_scenario);
}

static struct fake_edt *new_edt(arts_db_access_mode_t mode) {
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
  return f;
}

/* An EDT acquiring the block in `mode` through the arm's acquire body. */
static void acquire(arts_db_access_mode_t mode) {
  struct fake_edt *f = new_edt(mode);
  struct arts_ooo_args_db_acquire_s a = {
      .edt = &f->edt, .db_guid = g_db->cache.db_guid, .slot = 0u};
  arts_handler_db_acquire(g_db, &a);
}

/* ---- answers, built from the requests ---- */

#ifdef ARTS_VALIDITY_VAL
static const struct arts_msg_snapshot_request_packet_s *read_request(int nth) {
  return (const struct arts_msg_snapshot_request_packet_s *)sent(
      MSG_DB_SNAPSHOT_REQUEST, nth);
}

/* An answer to request r naming `version`: kind 0 nothing moved, 1 a
 * one-sided payload into `landing`, 2 a size round. */
static void read_reply(const struct arts_msg_snapshot_request_packet_s *r,
                       uint32_t kind, uint64_t version,
                       const struct arts_rdzv_landing_s *landing) {
  struct arts_db_snapshot_response_args_s a = {
      .db_guid = r->db_guid,
      .token = r->token,
      .data_present = kind,
      .version = version,
      .db_size = SIZE,
      .rdzv_txid = kind == 1u ? landing->txid : 0u,
      .rdzv_cookie = landing->cookie,
  };
  /* A read answer on the wire comes from another rank (the dispatcher's
   * serve section). */
  uint8_t census = arts_db_census_serving(true);
  arts_handler_db_snapshot_response(&a);
  arts_db_census_served(census);
}

/* The nth request's answer naming `version`, into the landing it named. */
static void read_answer_at(int nth, uint32_t kind, uint64_t version) {
  const struct arts_msg_snapshot_request_packet_s *r = read_request(nth);
  struct arts_rdzv_landing_s l = {.addr = r->rdzv.addr,
                                  .key = r->rdzv.key,
                                  .txid = r->rdzv.txid,
                                  .cookie = r->rdzv.cookie};
  read_reply(r, kind, version, &l);
}
#else
static const struct arts_msg_inv_request_packet_s *read_request(int nth) {
  return (const struct arts_msg_inv_request_packet_s *)sent(MSG_DB_INV_REQUEST,
                                                            nth);
}

/* An answer to request r naming `version`: kind 0 a deliver that moves no
 * bytes and names `landing` (the requester adopts or returns it), 1 a
 * one-sided payload into `landing`, 2 a size round. */
static void read_reply(const struct arts_msg_inv_request_packet_s *r,
                       uint32_t kind, uint64_t version,
                       const struct arts_rdzv_landing_s *landing) {
  if (kind == 2u) {
    struct arts_msg_inv_cts_packet_s p;
    memset(&p, 0, sizeof(p));
    p.header.rank = HOME;
    p.db_guid = r->db_guid;
    p.db_size = SIZE;
    p.token = r->token;
    arts_handler_db_inv_cts(&p);
    return;
  }
  struct arts_msg_inv_deliver_packet_s p;
  memset(&p, 0, sizeof(p));
  p.header.rank = HOME;
  p.db_guid = r->db_guid;
  p.version = version;
  p.data_size = SIZE;
  p.rdzv_txid = kind == 1u ? landing->txid : 0u;
  p.rdzv_cookie = landing->cookie;
  p.token = r->token;
  arts_handler_db_inv_deliver(&p, sizeof(p));
}

/* The nth request's answer naming `version`, into the landing it named. */
static void read_answer_at(int nth, uint32_t kind, uint64_t version) {
  const struct arts_msg_inv_request_packet_s *r = read_request(nth);
  struct arts_rdzv_landing_s l = {.addr = r->rdzv.addr,
                                  .key = r->rdzv.key,
                                  .txid = r->rdzv.txid,
                                  .cookie = r->rdzv.cookie};
  read_reply(r, kind, version, &l);
}
#endif

/* The nth request's answer, into the landing it named: one that moves bytes
 * names version 3, one that moves none version 0. */
static void read_answer(int nth, uint32_t kind) {
  read_answer_at(nth, kind, kind == 1u ? 3u : 0u);
}

static const struct arts_msg_grant_request_packet_s *write_request(int nth) {
  return (const struct arts_msg_grant_request_packet_s *)sent(
      MSG_DB_GRANT_REQUEST, nth);
}

/* The nth write request's cover word, the bit above its version; the version
 * below it is the one the cache held, none in every scenario here. */
static uint32_t write_cover(int nth) {
  uint64_t v = write_request(nth)->have_version;
  if ((v & ARTS_GRANT_REQ_VERSION_MASK) != ARTS_GRANT_VERSION_NONE) {
    fail("a write request names a version its cache does not hold");
  }
  return (uint32_t)(v >> ARTS_GRANT_REQ_VERSION_BITS);
}

/* The grant, from another rank: no payload, or a one-sided one. */
static void write_answer(int nth, bool payload) {
  const struct arts_msg_grant_request_packet_s *r = write_request(nth);
  struct {
    struct arts_msg_grant_response_packet_s hdr;
    uint32_t map[2];
  } m;
  memset(&m, 0, sizeof(m));
  m.hdr.header.message_type = MSG_DB_GRANT_RESPONSE;
  m.hdr.header.rank = HOME;
  m.hdr.db_guid = r->db_guid;
  m.hdr.version = payload ? 3u : 0u;
  m.hdr.data_size = SIZE;
  m.hdr.rdzv_txid = payload ? r->rdzv.txid : 0u;
  m.hdr.rdzv_cookie = r->rdzv.cookie;
  m.hdr.token = r->token;
  arts_handler_db_grant_response(&m, sizeof(m));
}

/* Under OWNER the request closes at the CONFIRM_ACK, which answers the
 * CONFIRM the grant's commit sent. */
static void confirm_ack(void) {
#ifdef ARTS_READ_SOURCE_OWNER
  const struct arts_msg_grant_confirm_packet_s *c =
      (const struct arts_msg_grant_confirm_packet_s *)sent(MSG_DB_GRANT_CONFIRM,
                                                           0);
  if (c->token != arts_db_token(g_db)) {
    fail("a CONFIRM names another descriptor");
  }
  struct arts_msg_grant_confirm_ack_packet_s p;
  memset(&p, 0, sizeof(p));
  p.db_guid = c->db_guid;
  p.token = c->token;
  p.new_owner_rank = ARTS_NO_PENDING_OWNER;
  p.new_owner_have = ARTS_GRANT_VERSION_NONE;
  arts_handler_db_grant_confirm_ack(&p);
#endif
}

/* The home's size round for a request that carried no landing. */
static void write_size_round(int nth) {
  const struct arts_msg_grant_request_packet_s *r = write_request(nth);
  if (r->rdzv.cookie != 0u) {
    fail("a size round for a request that carried a landing");
  }
  struct arts_msg_grant_cts_packet_s p;
  memset(&p, 0, sizeof(p));
  p.db_guid = r->db_guid;
  p.db_size = SIZE;
  p.token = r->token;
  arts_handler_db_grant_cts(&p);
}

/* A create's claim of the cache, in the serve section the create opens. */
static uint8_t claim(void) {
  uint8_t owed = 0u;
  uint8_t census = arts_db_census_serving(false);
  if (!arts_db_create_take_hold(&g_db->cache, &owed)) {
    fail("the claim's hold did not commit");
  }
  arts_db_census_served(census);
  return owed;
}

static int read_requests(void) {
  return count_sent(MSG_DB_SNAPSHOT_REQUEST) + count_sent(MSG_DB_INV_REQUEST);
}

/* ---- scenarios ---- */

static void read_answered(void) {
  begin("read axis closed by its answer", GUID_SIZED, false);
  acquire(DB_MODE_RO);
  read_answer(0, 0u);
  end(1);
}

static void read_retired(void) {
  begin("read answer on a retired descriptor", GUID_SIZED, false);
  acquire(DB_MODE_RO);
  retire();
  read_answer(0, 0u);
  end(0);
}

static void read_paired(bool retired_first) {
  begin(retired_first ? "read pairing callback on a retired descriptor"
                      : "read pairing callback closes the read axis",
        GUID_SIZED, false);
  acquire(DB_MODE_RO);
  read_answer(0, 1u);
  if (retired_first) {
    retire();
  }
  pairing_fire(true);
  end(retired_first ? 0 : 1);
}

static void read_discarded(void) {
  begin("read pairing discarded at teardown, the count the sweep's", GUID_SIZED,
        false);
  acquire(DB_MODE_RO);
  read_answer(0, 1u);
  /* Teardown order: the sweep releases through the word, the discard ends
   * only the landing. */
  arts_db_sweep_open_items(g_db, g_db->self_cb);
  pairing_fire(false);
  end(0);
}

static void write_answered(bool payload) {
  begin(payload ? "write axis closed by a one-sided grant's pairing"
                : "write axis closed by its answer",
        GUID_SIZED, false);
  acquire(DB_MODE_RW);
  if (write_cover(0) != 0u) {
    fail("a cache no create can claim sent a pre-claim request");
  }
  write_answer(0, payload);
  if (payload) {
    pairing_fire(true);
  }
  confirm_ack();
  end(1);
}

/* A first touch of a block whose GUID bounds no size leaves without a
 * landing; the home's size round re-sends it with one, on the same axis. */
static void write_size_rounds(void) {
  begin("write size round keeps the axis open", GUID_CLAIM, false);
  acquire(DB_MODE_RW);
  if (write_cover(0) != ARTS_COVER_PRECLAIM) {
    fail("a claimable first touch's request is not marked pre-claim");
  }
  write_size_round(0);
  if (write_request(1)->token != write_request(0)->token ||
      write_request(1)->rdzv.cookie == 0u) {
    fail("the re-sent request is not the same request with a landing");
  }
  if (write_cover(1) != 0u) {
    fail("a request re-sent after its size round is still marked pre-claim");
  }
  write_answer(1, false);
  confirm_ack();
  end(1);
}

static void write_claimed(void) {
  begin("write axis closed by a create's claim", GUID_CLAIM, false);
  acquire(DB_MODE_RW);
  if (write_cover(0) != ARTS_COVER_PRECLAIM ||
      write_request(0)->rdzv.cookie != 0u) {
    fail("the request the claim closes is not a landing-less pre-claim one");
  }
  if (claim() != ARTS_CREATE_OWED_RW) {
    fail("the claim did not announce the write request alone");
  }
  end(1);
}

static void write_retired(int how) {
  static const char *names[] = {
      "write size round on a retired descriptor",
      "write grant on a retired descriptor",
      "write pairing callback on a retired descriptor"};
  begin(names[how], how == 0 ? GUID_CLAIM : GUID_SIZED, false);
  acquire(DB_MODE_RW);
  if (how == 0) {
    retire();
    write_size_round(0);
    if (count_sent(MSG_DB_GRANT_REQUEST) != 1) {
      fail("a retired descriptor re-sent its request");
    }
  } else if (how == 1) {
    retire();
    write_answer(0, false);
  } else {
    write_answer(0, true);
    retire();
    pairing_fire(true);
  }
  end(0);
}

#ifdef ARTS_READ_SOURCE_OWNER
static void confirm_ack_retired(void) {
  begin("CONFIRM_ACK on a retired descriptor", GUID_SIZED, false);
  acquire(DB_MODE_RW);
  write_answer(0, false);
  retire();
  confirm_ack();
  end(0);
}
#endif

#ifdef ARTS_DEBUG_INJECTION
static bool write_axis_idle(void) {
#ifdef ARTS_VALIDITY_VAL
  return VREQ_RW(atomic_load_explicit(&g_db->cache.req_state,
                                      memory_order_acquire)) == VREQ_RW_IDLE;
#else
  return INV_CACHE_RW(atomic_load_explicit(
             &g_db->cache.cache_state, memory_order_acquire)) == INV_RW_IDLE;
#endif
}

/* Inside a grant, between its clear of the write axis and its look at the
 * waiters: a writer that pushed after the grant's take and lost its claim
 * to the axis the grant was about to clear. */
static bool park_after_take(void) {
  if (!write_axis_idle() ||
      arts_pending_rw_queue_pending(&g_db->cache.pending_rw)) {
    return false;
  }
  struct fake_edt *f = new_edt(DB_MODE_RW);
  struct arts_db_rw_waiter_s *w =
      (struct arts_db_rw_waiter_s *)calloc(1, sizeof(*w));
  w->edt = &f->edt;
  ARTS_DEP_WAITER_SET(w, &f->edt);
  w->slot = 0u;
  arts_pending_rw_queue_push(&g_db->cache.pending_rw, w);
  return true;
}

/* The rearm opens that writer's request with a count of its own while the
 * grant still holds its request's; the answer to it finds the block
 * retired. */
static void write_rearmed(void) {
  begin("write axis re-opened by the rearm inside a grant", GUID_SIZED, false);
  acquire(DB_MODE_RW);
  g_fuzz_hook = park_after_take;
  write_answer(0, false);
  confirm_ack();
  if (g_fuzz_hook != NULL || count_sent(MSG_DB_GRANT_REQUEST) != 2 ||
      write_request(1)->token != write_request(0)->token) {
    g_fuzz_hook = NULL;
    fail("the rearm opened no request for the writer it found");
  }
  retire();
  write_answer(1, false);
  end(1);
}
#endif

static void buf_free(void *p) { free(p); }

/* The home's buffer, holding the block's bytes at `version`. */
static void home_buffer(uint64_t version) {
  struct arts_db_buffer_s *b =
      (struct arts_db_buffer_s *)calloc(1, sizeof(*b) + SIZE);
  atomic_store_explicit(&b->version, version, memory_order_relaxed);
  b->cb = arts_shared_make(b, buf_free);
  arts_shared_slot_store(&g_db->cache.buffer, b->cb);
}

#ifdef ARTS_RETENTION_PURGE
/* The holder's hand-back, accepted at the home. */
static void hand_back(void) {
  struct arts_db_grant_return_reply_s reply = {
      .cv = 0u, .token = 0u, .version = 0u, .ack = false, .credit = false};
  arts_db_grant_return_arrived(g_db, REQ, &reply);
}
#else
/* The holder's ship, answering the nth revocation, which names the home. */
static void holder_ships_to_home(int nth) {
  const struct arts_msg_grant_invalidate_packet_s *iv =
      (const struct arts_msg_grant_invalidate_packet_s *)sent(
          MSG_DB_GRANT_INVALIDATE, nth);
  if (iv->new_owner_rank != HOME ||
      iv->new_owner_token != arts_db_token(g_db)) {
    fail("the revocation names another requester");
  }
  struct {
    struct arts_msg_grant_response_packet_s hdr;
    uint32_t map[2];
  } m;
  memset(&m, 0, sizeof(m));
  m.hdr.header.message_type = MSG_DB_GRANT_RESPONSE;
  m.hdr.header.rank = REQ;
  m.hdr.db_guid = iv->db_guid;
  m.hdr.data_size = SIZE;
  m.hdr.rdzv_cookie = iv->new_owner_rdzv.cookie;
  m.hdr.token = iv->new_owner_token;
  m.hdr.home_token = iv->home_token;
  arts_handler_db_grant_response(&m, sizeof(m));
}
#endif

/* The home's own write request: the grant is installed on the home, and
 * where the home revokes, its CONFIRM -- and under OWNER the CONFIRM_ACK
 * answering it -- runs there inline (under PURGE the home serves it from the
 * front of its queue).  The writer the grant wakes destroys the block in its
 * own body and completes, leaving the frames only their own counts. */
static void home_wake_drops_last(void) {
  begin("home's own grant: its writer drops every other reference", GUID_SIZED,
        true);
  acquire(DB_MODE_RW);
  if (count_sent(MSG_DB_GRANT_REQUEST) != 0) {
    fail("the home's own request left the rank");
  }
  g_wake_drops_ref = true;
  g_poison = true;
#ifdef ARTS_RETENTION_PURGE
  hand_back();
#else
  holder_ships_to_home(0);
#endif
  end_dropped(1);
}

/* The home's own write request served inside its self-sent body: the right
 * came home and went idle, and a home writer whose fast path ran while it was
 * away takes the slow path, whose request finds it idle.  Under RETAIN the
 * request's round revokes the home itself, which ships the grant to itself;
 * under PURGE the home serves its own queue's front.  The writer the grant
 * wakes destroys the block in its body and completes, leaving only the
 * request's count, which the grant drops, and the self-send's own: the
 * deleter may run only once the request's body has returned. */
static void home_request_served_in_body(void) {
  begin("home's own write request served inside its self-sent body", GUID_SIZED,
        true);
#ifdef ARTS_RETENTION_PURGE
  hand_back();
  int wakes = 1;
#else
  /* A first home writer's request revokes the holder, whose ship installs
   * the grant here, and that writer's release leaves it idle.  The home
   * holds the bytes the self-transfer below keeps in place. */
  home_buffer(2u);
  acquire(DB_MODE_RW);
  holder_ships_to_home(0);
  arts_db_grant_release_commit(&g_db->cache);
  release_buffer_hold(&g_edts[0]->edt);
  int wakes = 2;
#endif
  struct fake_edt *f = new_edt(DB_MODE_RW);
  g_wake_drops_ref = true;
  g_poison = true;
  (void)arts_db_acquire_remote_rw(&g_db->cache, &f->edt, 0u);
  if (count_sent(MSG_DB_GRANT_REQUEST) != 0) {
    fail("the home's own request left the rank");
  }
  end_dropped(wakes);
}

/* A write request from rank `from` that a claimable first touch opened, as
 * the engine hands it to the home: under the dispatcher's pin. */
#define FAR_TOKEN ((uint64_t)0x6b6b00u)
static void write_request_with(unsigned int from, uint32_t cover) {
  struct arts_ooo_args_db_grant_request_s a = {
      .requester = from,
      .db_guid = g_db->cache.db_guid,
      .have_version = ARTS_GRANT_VERSION_NONE,
      .cover = cover,
      .token = FAR_TOKEN,
  };
  arts_shared_ptr_t pin = arts_shared_copy(g_db->self_cb);
  arts_handler_db_grant_request(g_db, &a);
  arts_shared_release(&pin);
}

static void preclaim_write_request(unsigned int from) {
  write_request_with(from, ARTS_COVER_PRECLAIM);
}

static void destroy_arrives(void) {
  arts_shared_ptr_t pin = arts_shared_copy(g_db->self_cb);
  arts_db_destroy_arrived(g_db);
  arts_shared_release(&pin);
}

/* The destroy's gate at the home of a block whose creator claimed a
 * first-touch cache with its write request out.  The home absorbs that
 * request, queueing and answering nothing, and tears down at whichever of the
 * destroy and the request's arrival comes second, once.  The same request
 * from a rank whose create claimed nothing is a first touch like any other:
 * the home answers its size. */
static void home_gate(int how) {
  static const char *names[] = {
      "claimer's pre-claim write request absorbed, then the destroy",
      "destroy waits for the claimer's pre-claim write request",
      "pre-claim write request of a rank that claimed nothing"};
  begin(names[how], GUID_CLAIM, true);
  if (how == 2) {
    preclaim_write_request(REQ);
    if (g_nsent != 1 || count_sent(MSG_DB_GRANT_CTS) != 1 ||
        ((const struct arts_msg_grant_cts_packet_s *)sent(MSG_DB_GRANT_CTS, 0))
                ->token != FAR_TOKEN) {
      fail("a landing-less first touch was not answered its size");
    }
    end(0);
    return;
  }
  g_poison = true;
  arts_db_home_seed_owed(g_db, REQ, ARTS_CREATE_OWED_RW);
  if (how == 1) {
    destroy_arrives();
    if (g_deleted != 0 || g_nsent != 0 ||
        atomic_load_explicit(&g_db->cover_gate, memory_order_acquire) !=
            (ARTS_COVER_GATE_TD | ARTS_COVER_GATE_RWO)) {
      fail("the destroy did not wait for the request the home is owed");
    }
  }
  preclaim_write_request(REQ);
  if (count_sent(MSG_DB_GRANT_CTS) != 0 ||
      count_sent(MSG_DB_GRANT_RESPONSE) != 0) {
    fail("the home answered a request the claim had closed");
  }
  if (how == 0) {
    if (g_deleted != 0 || g_nsent != 0 ||
        !arts_home_grantreq_queue_empty(&g_db->pending_rw) ||
        atomic_load_explicit(&g_db->cover_gate, memory_order_acquire) != 0u) {
      fail("the absorbed request left something behind at the home");
    }
    destroy_arrives();
  }
  if (g_nsent != 1 || count_sent(MSG_DB_CACHE_DESTROY) != 1) {
    fail("the teardown did not tell the creator's rank exactly once");
  }
  end_dropped(0);
}

/* A write request the gate does not absorb is an ordinary first touch, and
 * leaves the gate's owed request owed: the claimer's own request sent after
 * its claim carries no PRECLAIM, and a pre-claim request of another rank is
 * not the one the claim closed. */
static void home_gate_unabsorbed(int how) {
  static const char *names[] = {
      "claimer's write request after its claim is not absorbed",
      "pre-claim write request of another rank leaves the claimer's owed"};
  const uint32_t owed = ARTS_COVER_GATE_RWO;
  begin(names[how], GUID_CLAIM, true);
  arts_db_home_seed_owed(g_db, REQ, ARTS_CREATE_OWED_RW);
  write_request_with(how == 0 ? REQ : REQ + 1u,
                     how == 0 ? 0u : ARTS_COVER_PRECLAIM);
  if (g_nsent != 1 || count_sent(MSG_DB_GRANT_CTS) != 1 ||
      ((const struct arts_msg_grant_cts_packet_s *)sent(MSG_DB_GRANT_CTS, 0))
              ->token != FAR_TOKEN) {
    fail("a request the claim had not closed was not answered its size");
  }
  if (atomic_load_explicit(&g_db->cover_gate, memory_order_acquire) != owed) {
    fail("a request the claim had not closed settled the gate");
  }
  if (how == 1) {
    preclaim_write_request(REQ);
    if (g_nsent != 1 ||
        atomic_load_explicit(&g_db->cover_gate, memory_order_acquire) != 0u) {
      fail("the claimer's own request did not settle the gate");
    }
  }
  end(0);
}

#ifdef ARTS_VALIDITY_VAL
/* The reader parked on the open window, which only the window's answer
 * serves. */
static bool reader_parked(void) {
  uint64_t w = atomic_load_explicit(&g_db->cache.req_state,
                                    memory_order_acquire);
  return VREQ_RO(w) && VREQ_GROUP(w) != 0u;
}
#else
/* The reader parked on the open fetch, which only the fetch's reply serves. */
static bool reader_parked(void) {
  uint64_t w =
      atomic_load_explicit(&g_db->cache.cache_state, memory_order_acquire);
  return INV_CACHE_INFLIGHT(w) != 0u && INV_CACHE_HEAD_RO(w) != 0u;
}
#endif

/* The buffer this rank's slot holds. */
static struct arts_db_buffer_s *slot_buffer(void) {
  arts_shared_ptr_t h = arts_shared_slot_load(&g_db->cache.buffer);
  struct arts_db_buffer_s *b = (struct arts_db_buffer_s *)arts_shared_get(h);
  arts_shared_release(&h);
  return b;
}

/* The answer at the write holder served `reader` against the holder's
 * buffer b, which the slot still holds at its version; then both EDTs
 * release their buffer holds. */
static void served_by_holder_buffer(struct arts_db_buffer_s *b, int reader) {
  if (g_wakes != 2) {
    fail("the read answer did not serve its cohort");
  }
  if (slot_buffer() != b ||
      atomic_load_explicit(&b->version, memory_order_relaxed) != 3u) {
    fail("the read answer replaced the write holder's buffer");
  }
  if (g_edts[reader]->acq[0].ptr != b->data) {
    fail("the reader was not served the write holder's buffer");
  }
  release_buffer_hold(&g_edts[0]->edt);
  release_buffer_hold(&g_edts[1]->edt);
}

/* A reader that arrives while its rank's write request is out opens its own
 * read request.  The grant serves the writer alone; the read answer, naming
 * the version the rank now holds and moving nothing, serves the reader
 * against the holder's buffer.  The slot holds the bytes the grant certifies
 * as current from the start. */
static void read_opened_under_write(void) {
  begin("read request opened while the write request is out", GUID_SIZED,
        false);
  home_buffer(3u);
  struct arts_db_buffer_s *b = slot_buffer();
  acquire(DB_MODE_RW);
  acquire(DB_MODE_RO);
  if (read_requests() != 1) {
    fail("the reader did not open a read request of its own");
  }
  write_answer(0, false);
  confirm_ack();
  if (g_wakes != 1 || !reader_parked()) {
    fail("the write grant served the reader");
  }
  read_answer_at(0, 0u, 3u);
  served_by_holder_buffer(b, 1);
  end(2);
}

/* A one-sided read answer that lands once the rank holds the write right
 * offers its bytes at their version, which is not above the holder's: the
 * holder's buffer stays, and the reader is served against it. */
static void read_put_at_holder(void) {
  begin("one-sided read answer at the write holder", GUID_SIZED, false);
  home_buffer(3u);
  struct arts_db_buffer_s *b = slot_buffer();
  acquire(DB_MODE_RO);
  acquire(DB_MODE_RW);
  write_answer(0, false);
  confirm_ack();
  if (g_wakes != 1 || !reader_parked()) {
    fail("the write grant served the reader");
  }
  g_installed_v = 0u;
  read_answer(0, 1u);
  pairing_fire(true);
  if (g_installed_v != 3u) {
    fail("the answer's bytes were not offered at their version");
  }
  served_by_holder_buffer(b, 0);
  end(2);
}

/* A create's claim takes the write axis alone: the reader whose request the
 * first-touch cache sent, landing-less as the cache knew no size, stays
 * parked, and is served by that request's own answer after a size round. */
static void read_kept_by_claim(void) {
  begin("read axis kept by a create's claim, closed by its answer",
        GUID_CLAIM, false);
  acquire(DB_MODE_RO);
  if (read_request(0)->rdzv.cookie != 0u) {
    fail("a request that knew no size carried a landing");
  }
  if (claim() != 0u) {
    fail("the claim named a request with no write request out");
  }
  if (g_wakes != 0 || !reader_parked()) {
    fail("the claim served the reader");
  }
  read_answer(0, 2u);
  if (read_requests() != 2 || read_request(1)->rdzv.cookie == 0u) {
    fail("the size round did not re-send the request with a landing");
  }
  read_answer(1, 0u);
  end(1);
}

#ifdef ARTS_VALIDITY_INV
/* The round's home token an INVALIDATE names. */
#define ROUND_TOKEN ((uint64_t)0x7a7a00u)

/* A round's INVALIDATE finds the fetch out: it marks the fetch, and the ack
 * it owes stays with the rank. */
static void round_kill_marks(void) {
  arts_handler_db_inv_invalidate(g_db, ROUND_TOKEN);
  if (count_sent(MSG_DB_INV_INVALIDATE_ACK) != 0) {
    fail("an ack owed to a kill-marked fetch left at once");
  }
}

/* The kill-marked fetch's reply served its one reader, purged the copy and
 * then sent the round's ack, once, echoing the round's home token. */
static void killed_reply_acked(void) {
  if (g_wakes != 1) {
    fail("the reply did not serve its cohort");
  }
  uint64_t w =
      atomic_load_explicit(&g_db->cache.cache_state, memory_order_acquire);
  if (INV_CACHE_RO(w) != INV_RO_IDLE || INV_CACHE_INFLIGHT(w) != 0u ||
      INV_CACHE_HEAD_RO(w) != 0u) {
    fail("the reply did not purge the copy it served");
  }
  const struct arts_msg_inv_invalidate_ack_packet_s *a =
      (const struct arts_msg_inv_invalidate_ack_packet_s *)sent(
          MSG_DB_INV_INVALIDATE_ACK, 0);
  if (count_sent(MSG_DB_INV_INVALIDATE_ACK) != 1 ||
      a->home_token != ROUND_TOKEN || a->db_guid != g_db->cache.db_guid) {
    fail("the owed ack did not leave once with the round's home token");
  }
}

/* A round reaches the rank while its write request is out and kill-marks
 * the fetch opened beside it.  The fetch's reply serves its cohort once,
 * purges the copy and only then sends the round its ack, echoing the round's
 * home token; the grant arrives after that ack, as the round orders it, and
 * serves the writer alone. */
static void read_killed_under_write(void) {
  begin("fetch opened under the write request, kill-marked: its reply serves "
        "once, purges and acks",
        GUID_SIZED, false);
  acquire(DB_MODE_RW);
  acquire(DB_MODE_RO);
  if (read_requests() != 1) {
    fail("the reader did not open a read request of its own");
  }
  round_kill_marks();
  read_answer(0, 0u);
  killed_reply_acked();
  write_answer(0, false);
  confirm_ack();
  end(2);
}

/* A round kill-marks a fetch that left without a landing.  The size round
 * that answers it re-sends the fetch with the mark kept and sends no ack;
 * the re-sent fetch's reply serves the cohort once, purges the copy and
 * sends the round its ack. */
static void read_killed_size_round(void) {
  begin("kill-marked fetch re-sent by its size round: the reply acks",
        GUID_CLAIM, false);
  acquire(DB_MODE_RO);
  round_kill_marks();
  read_answer(0, 2u);
  if (read_requests() != 2 || read_request(1)->rdzv.cookie == 0u) {
    fail("the size round did not re-send the fetch with a landing");
  }
  uint64_t w =
      atomic_load_explicit(&g_db->cache.cache_state, memory_order_acquire);
  if (count_sent(MSG_DB_INV_INVALIDATE_ACK) != 0 || !reader_parked() ||
      INV_CACHE_RO(w) != INV_RO_REQ_KILL) {
    fail("the size round did not keep the kill mark and the ack it owes");
  }
  read_answer(1, 0u);
  killed_reply_acked();
  end(1);
}
#endif

#if defined(ARTS_VALIDITY_INV) && defined(ARTS_READ_SOURCE_OWNER)
/* A reader that arrives while the rank's install awaits its confirmation is
 * no local hit: it opens a fetch of its own.  The CONFIRM_ACK wakes the
 * writer alone, and the fetch's reply, naming the version the rank holds,
 * serves the reader against the holder's buffer. */
static void read_in_unconfirmed_window(void) {
  begin("read request opened while the install awaits its confirmation",
        GUID_SIZED, false);
  home_buffer(3u);
  struct arts_db_buffer_s *b = slot_buffer();
  acquire(DB_MODE_RW);
  write_answer(0, false);
  if (g_wakes != 0) {
    fail("the writer ran before its confirmation");
  }
  acquire(DB_MODE_RO);
  if (read_requests() != 1 || !reader_parked()) {
    fail("the reader did not open a fetch of its own");
  }
  confirm_ack();
  if (g_wakes != 1 || !reader_parked()) {
    fail("the confirmation served the reader");
  }
  read_answer_at(0, 0u, 3u);
  served_by_holder_buffer(b, 1);
  end(2);
}
#endif

/* In a child: `step` must end the run; nothing it does reaches this
 * process. */
static void expect_fatal(void (*step)(void), const char *what) {
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    /* The fatal line the check prints is the expected outcome, not a verdict
     * of this test's run. */
    int devnull = open("/dev/null", O_WRONLY);
    (void)dup2(devnull, STDOUT_FILENO);
    (void)dup2(devnull, STDERR_FILENO);
    g_expect_abort = true;
    step();
    _exit(0);
  }
  int status = 0;
  if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
      WEXITSTATUS(status) != EXPECTED_ABORT) {
    fail(what);
  }
}

#ifdef ARTS_VALIDITY_VAL
/* The header-only answer again: its request carried no landing, so the
 * answer names none. */
static void again_no_bytes(void) { read_answer(0, 0u); }
#else
/* The deliver that moves no bytes again, into a landing of its own: the
 * first one ended the landing its request named. */
static void again_no_bytes(void) {
  struct arts_rdzv_landing_s l;
  (void)arts_db_buf_landing_alloc(SIZE, &l);
  read_reply(read_request(0), 0u, 0u, &l);
}
#endif
/* The one-sided answer again, into a landing of its own. */
static void again_one_sided(void) {
  struct arts_rdzv_landing_s l;
  (void)arts_db_buf_landing_alloc(SIZE, &l);
  read_reply(read_request(0), 1u, 3u, &l);
  pairing_fire(true);
}
static void again_size_round(void) { read_answer(0, 2u); }

/* A read request is answered once, and a reply carries nothing that tells one
 * request's answer from another's: a reply of any kind that finds no request
 * out ends the run.  VAL's header-only answer is for a request that carried
 * no landing; INV's deliver that moves no bytes names the landing its
 * request carried. */
static void read_reply_without_request(int kind) {
  static const char *names[] = {
#ifdef ARTS_VALIDITY_VAL
      "header-only read answer with no request out is fatal",
#else
      "read deliver that moves no bytes with no request out is fatal",
#endif
      "one-sided read answer with no request out is fatal",
      "read size round with no request out is fatal"};
#ifdef ARTS_VALIDITY_VAL
  const arts_guid_t no_bytes_guid = GUID_CLAIM;
#else
  const arts_guid_t no_bytes_guid = GUID_SIZED;
#endif
  begin(names[kind],
        kind == 0 ? no_bytes_guid : (kind == 1 ? GUID_SIZED : GUID_CLAIM),
        false);
  acquire(DB_MODE_RO);
  if (kind == 0) {
    read_answer(0, 0u);
    expect_fatal(again_no_bytes, "the second answer did not end the run");
  } else if (kind == 1) {
    read_answer(0, 1u);
    pairing_fire(true);
    expect_fatal(again_one_sided, "the second answer did not end the run");
  } else {
    read_answer(0, 2u);
    read_answer(1, 0u);
    expect_fatal(again_size_round,
                 "a size round after the answer did not end the run");
  }
  end(1);
}

#if PUBLISHES
#ifdef ARTS_VALIDITY_INV
/* REQ's ack to the INVALIDATE that just left, echoing the round's home
 * token. */
static void ack_remote_invalidate(void) {
  const struct arts_msg_inv_invalidate_packet_s *inv =
      (const struct arts_msg_inv_invalidate_packet_s *)g_sent[g_nsent - 1].buf;
  struct arts_msg_inv_invalidate_ack_packet_s p;
  memset(&p, 0, sizeof(p));
  p.header.message_type = MSG_DB_INV_INVALIDATE_ACK;
  p.header.rank = REQ;
  p.db_guid = inv->db_guid;
  p.home_token = inv->home_token;
  arts_handler_db_inv_invalidate_ack(&p);
}
#endif

/* A release landed after the home's flight left, so the flight's ACK
 * relaunches it, under the flight's count alone.  The relaunched publish's
 * body acknowledges it inline, landing the flight and dropping that count.
 * Under INV that ACK ends the relaunched round, which retires REQ's copy and
 * whose ack arrives while the round is still sending, and the round's
 * finisher still ends its hold on the descriptor afterwards.  The deleter may
 * run only once the publish's body has returned. */
static void home_flight_relaunch_lands(void) {
#ifdef ARTS_VALIDITY_INV
  begin("home's relaunched flight landed by its own round's ACK", GUID_SIZED,
        true);
#else
  begin("home's relaunched flight landed by its own ACK", GUID_SIZED, true);
#endif
  struct arts_db_cache_s *cache = &g_db->cache;
  home_buffer(2u);
#ifdef ARTS_VALIDITY_INV
  (void)arts_rank_bitset_set(&g_db->roster, REQ);
#endif
  atomic_store_explicit(&cache->pub_flight, 1u, memory_order_relaxed);
  arts_db_axis_take(g_db);
  struct arts_db_pub_waiter_s *w =
      (struct arts_db_pub_waiter_s *)calloc(1, sizeof(*w));
  w->version = 2u;
  sem_init(&w->sem, 0, 0);
  arts_lf_stack_push(&cache->pub_waiters, &w->link);
  struct arts_db_publish_ack_args_s a = {.db_guid = cache->db_guid,
                                         .token = arts_db_token(g_db),
                                         .cv = 0u,
                                         .version = 1u};
  arts_shared_ptr_t h = g_db->self_cb;
  arts_shared_release(&h);
  g_poison = true;
#ifdef ARTS_VALIDITY_INV
  g_send_hook = ack_remote_invalidate;
  g_send_hook_type = MSG_DB_INV_INVALIDATE;
#endif
  arts_handler_db_publish_ack(&a);
#ifdef ARTS_VALIDITY_INV
  if (g_send_hook != NULL) {
    g_send_hook = NULL;
    fail("the relaunched round retired no copy");
  }
#endif
  if (sem_trywait(&w->sem) != 0) {
    fail("the release behind the flight was not woken");
  }
  sem_destroy(&w->sem);
  free(w);
  end_dropped(0);
}
#endif

#ifdef ARTS_RETENTION_PURGE
/* The home's own write request, queued while another rank holds the right,
 * is served from the front of the queue by the hand-back's acceptance. */
static void write_self_front(void) {
  begin("home's own write request served from its queue's front", GUID_SIZED,
        true);
  acquire(DB_MODE_RW);
  if (count_sent(MSG_DB_GRANT_REQUEST) != 0) {
    fail("the home's own request left the rank");
  }
  struct arts_db_grant_return_reply_s reply = {
      .cv = 0u, .token = 0u, .version = 0u, .ack = false, .credit = false};
  arts_db_grant_return_arrived(g_db, REQ, &reply);
  end(1);
}
#endif

#if PUBLISHES
/* A home releaser's flight: its publish runs on the home's descriptor and is
 * acknowledged on the same descriptor, inline. */
static void flight_landed(void) {
  begin("publish flight closed by its ACK", GUID_SIZED, true);
  arts_db_publish_sync(&g_db->cache, 1u);
  if (atomic_load_explicit(&g_db->cache.pub_flight, memory_order_relaxed) !=
      0u) {
    fail("the flight did not land");
  }
  end(0);
}

static void flight_retired(void) {
  begin("publish ACK on a retired descriptor", GUID_SIZED, false);
  /* A flight in the air, as arts_db_publish_sync leaves it before its ACK. */
  atomic_store_explicit(&g_db->cache.pub_flight, 1u, memory_order_relaxed);
  arts_db_axis_take(g_db);
  retire();
  struct arts_db_publish_ack_args_s a = {.db_guid = g_db->cache.db_guid,
                                         .token = arts_db_token(g_db),
                                         .cv = 0u,
                                         .version = 1u};
  arts_handler_db_publish_ack(&a);
  end(0);
}
#endif

static void sweep_rows(void) {
  begin("teardown sweep drops one count per open axis", GUID_SIZED, false);
  acquire(DB_MODE_RO);
  acquire(DB_MODE_RW);
#if PUBLISHES
  atomic_store_explicit(&g_db->cache.pub_flight, 1u, memory_order_relaxed);
  arts_db_axis_take(g_db);
#endif
  /* Both requests carried landings, which their cut answers would have
   * ended. */
  landing_end(
      (struct arts_db_buffer_s *)(uintptr_t)write_request(0)->rdzv.cookie);
  landing_end(
      (struct arts_db_buffer_s *)(uintptr_t)read_request(0)->rdzv.cookie);
  arts_db_sweep_open_items(g_db, g_db->self_cb);
  end(0);
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
#if !defined(__SANITIZE_ADDRESS__)
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_fault;
  (void)sigaction(SIGSEGV, &sa, NULL);
  (void)sigaction(SIGBUS, &sa, NULL);
#endif
  read_answered();
  read_opened_under_write();
  read_put_at_holder();
  read_kept_by_claim();
#ifdef ARTS_VALIDITY_INV
  read_killed_under_write();
  read_killed_size_round();
#endif
#if defined(ARTS_VALIDITY_INV) && defined(ARTS_READ_SOURCE_OWNER)
  read_in_unconfirmed_window();
#endif
  read_reply_without_request(0);
  read_reply_without_request(1);
  read_reply_without_request(2);
  read_retired();
  read_paired(false);
  read_paired(true);
  read_discarded();
  write_answered(false);
  write_answered(true);
  write_size_rounds();
  write_claimed();
  write_retired(0);
  write_retired(1);
  write_retired(2);
#ifdef ARTS_READ_SOURCE_OWNER
  confirm_ack_retired();
#endif
#ifdef ARTS_DEBUG_INJECTION
  write_rearmed();
#endif
#ifdef ARTS_RETENTION_PURGE
  write_self_front();
#endif
  home_wake_drops_last();
  home_request_served_in_body();
  home_gate(0);
  home_gate(1);
  home_gate(2);
  home_gate_unabsorbed(0);
  home_gate_unabsorbed(1);
#if PUBLISHES
  home_flight_relaunch_lands();
  flight_landed();
  flight_retired();
#endif
  sweep_rows();
  printf("PASS token_axis_pairing_valinv\n");
  return 0;
}
