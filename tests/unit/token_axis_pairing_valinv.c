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
 *   read axis   closed by its answer, by a dismiss while the rank's write
 *               request is out, by a dismiss after the write turn served
 *               the window (no write request out), by a create's claim; an
 *               answer and a dismiss on a retired descriptor; a one-sided
 *               answer's pairing callback, on a live and on a retired
 *               descriptor, and a pairing the teardown discards (the count
 *               is then the sweep's); opened by a writer's give-back, for
 *               riders and for a window a size round closed while the write
 *               axis was open;
 *   write axis  closed by its answer (write-through: the grant; write-back:
 *               the CONFIRM_ACK its CONFIRM asks for), a size round leaving
 *               it open; by a create's claim; a size round, a grant and
 *               (write-back) a CONFIRM_ACK on a retired descriptor; a
 *               one-sided grant's pairing, live and retired; re-opened by
 *               the rearm inside a grant (where the fuzz points exist); the
 *               home's own request, its grant run on the home (under PURGE
 *               served from the front of its queue) while the writer it
 *               wakes drops every other reference, and the same request
 *               served inside its own self-sent body once the right is idle
 *               at the home (RETAIN: the home revokes itself and ships the
 *               grant to itself; PURGE: it serves its own front);
 *   publish flight (where releases publish) closed by its ACK; an ACK on a
 *               retired descriptor; the home's flight relaunched by its ACK
 *               and landed inside the relaunched publish (INV: by its own
 *               round's ACK, which arrives while the round is still
 *               sending);
 *   the home's own read request (WB) dismissed inside its self-send with
 *               the teardown due, and (INV) served by the home on a
 *               descriptor retired since the request was processed;
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
#ifdef ARTS_WRITE_POLICY_WB
#include "coherence/grant_wb.c"
#else
#include "coherence/grant_wt.c"
#endif
#ifdef ARTS_RELEASE_PURGE
#include "coherence/grant_purge.c"
#else
#include "coherence/grant_retain.c"
#endif
#ifdef ARTS_PROTOCOL_INV
#include "coherence/inv/directory.c"
#include "coherence/inv/engine.c"
#ifdef ARTS_WRITE_POLICY_WB
#include "coherence/inv/wb.c"
#else
#include "coherence/inv/wt.c"
#endif
#else
#ifdef ARTS_WRITE_POLICY_WB
#include "coherence/val/wb.c"
#else
#include "coherence/val/wt.c"
#endif
#endif
#include "utils/shared.c"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#endif

#if !defined(ARTS_WRITE_POLICY_WB) || defined(ARTS_PROTOCOL_INV)
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

_Noreturn void arts_exit(int rc, enum arts_exit_origin origin) {
  (void)origin;
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
    printf("FAIL token_axis_pairing_valinv: %s: %s reached\n", g_scenario,     \
           __func__);                                                          \
    fflush(stdout);                                                            \
    _exit(1);                                                                  \
  }
UNREACHED(bool arts_db_create_install_local(arts_shared_ptr_t cb,
                                            struct arts_route_item_s *s,
                                            bool f))
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
/* Another thread acting at the next look at a buffer slot (called once). */
static void (*g_buf_hook)(void);

arts_shared_ptr_t arts_db_buf_acquire(struct arts_db_cache_s *cache) {
  void (*h)(void) = g_buf_hook;
  if (h != NULL) {
    g_buf_hook = NULL;
    h();
  }
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
  (void)v;
  (void)s;
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
 * a body; the hook stands for another thread acting between a self-sent
 * read request's send and its body (called once, before the next one). */
static int g_dispatch_depth;
static void (*g_dispatch_hook)(void);

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
#ifdef ARTS_PROTOCOL_VAL
  case OOO_DB_SNAPSHOT_REQUEST:
#else
  case OOO_DB_INV_REQUEST:
#endif
    if (g_dispatch_hook != NULL) {
      void (*hook)(void) = g_dispatch_hook;
      g_dispatch_hook = NULL;
      hook();
    }
#ifdef ARTS_PROTOCOL_VAL
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

#ifdef ARTS_PROTOCOL_VAL
static const struct arts_msg_snapshot_request_packet_s *read_request(int nth) {
  return (const struct arts_msg_snapshot_request_packet_s *)sent(
      MSG_DB_SNAPSHOT_REQUEST, nth);
}

/* kind: 0 nothing moved, 1 a one-sided payload, 2 a size round or dismiss. */
static void read_answer(int nth, uint32_t kind) {
  const struct arts_msg_snapshot_request_packet_s *r = read_request(nth);
  struct arts_db_snapshot_response_args_s a = {
      .db_guid = r->db_guid,
      .token = r->token,
      .cover = r->cover,
      .data_present = kind,
      .version = kind == 1u ? 3u : 0u,
      .db_size = SIZE,
      .rdzv_txid = kind == 1u ? r->rdzv.txid : 0u,
      .rdzv_cookie = r->rdzv.cookie,
  };
  /* A read answer on the wire comes from another rank (the dispatcher's
   * serve section). */
  uint8_t census = arts_db_census_serving(true);
  arts_handler_db_snapshot_response(&a);
  arts_db_census_served(census);
}
#else
static const struct arts_msg_inv_request_packet_s *read_request(int nth) {
  return (const struct arts_msg_inv_request_packet_s *)sent(MSG_DB_INV_REQUEST,
                                                            nth);
}

static void read_answer(int nth, uint32_t kind) {
  const struct arts_msg_inv_request_packet_s *r = read_request(nth);
  if (kind == 2u) {
    struct arts_msg_inv_cts_packet_s p;
    memset(&p, 0, sizeof(p));
    p.header.rank = HOME;
    p.db_guid = r->db_guid;
    p.db_size = SIZE | ((uint64_t)r->cover << ARTS_INV_CTS_SIZE_BITS);
    p.rdzv_cookie = r->rdzv.cookie;
    p.token = r->token;
    arts_handler_db_inv_cts(&p);
    return;
  }
  struct arts_msg_inv_deliver_packet_s p;
  memset(&p, 0, sizeof(p));
  p.header.rank = HOME;
  p.db_guid = r->db_guid;
  p.version = kind == 1u ? 3u : 0u;
  p.data_size = SIZE;
  p.rdzv_txid = kind == 1u ? r->rdzv.txid : 0u;
  p.rdzv_cookie = r->rdzv.cookie;
  p.token = r->token;
  arts_handler_db_inv_deliver(&p, sizeof(p));
}
#endif

static const struct arts_msg_grant_request_packet_s *write_request(int nth) {
  return (const struct arts_msg_grant_request_packet_s *)sent(
      MSG_DB_GRANT_REQUEST, nth);
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

/* Under write-back the request closes at the CONFIRM_ACK, which answers the
 * CONFIRM the grant's commit sent. */
static void confirm_ack(void) {
#ifdef ARTS_WRITE_POLICY_WB
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

static void read_dismissed(void) {
  begin("read axis closed by a dismiss, the write request out", GUID_SIZED,
        false);
  acquire(DB_MODE_RO);
  acquire(DB_MODE_RW);
  /* The home dismisses the read request the write request declared; the
   * reader rides the write turn. */
  read_answer(0, 2u);
  if (count_sent(MSG_DB_SNAPSHOT_REQUEST) + count_sent(MSG_DB_INV_REQUEST) !=
      1) {
    fail("the dismiss re-sent the read request");
  }
  write_answer(0, false);
  confirm_ack();
  end(2);
}

/* The write turn serves the window first; the dismiss of its read request,
 * still out, then closes the axis with no write request out. */
static void read_dismissed_after_grant(void) {
  begin("read axis closed by a dismiss after the write turn served it",
        GUID_SIZED, false);
  acquire(DB_MODE_RO);
  acquire(DB_MODE_RW);
  write_answer(0, false);
  confirm_ack();
  read_answer(0, 2u);
  if (read_requests() != 1) {
    fail("the dismiss re-sent the read request");
  }
  end(2);
}

/* A reader that parks while a writer holds the write axis OPEN rides the
 * request that OPEN would send; the writer gives the axis back unsent, and
 * the riders open the read axis themselves. */
static void read_given_riders(void) {
  begin("read axis opened by a writer's give-back, for its riders", GUID_SIZED,
        false);
  uint32_t tok = 0u;
  if (!arts_db_rw_req_claim(&g_db->cache, &tok)) {
    fail("the write axis was not idle");
  }
  acquire(DB_MODE_RO);
  if (read_requests() != 0) {
    fail("a rider sent a read request");
  }
  arts_db_rw_req_giveback(&g_db->cache, tok);
  read_answer(0, 0u);
  end(1);
}

/* A first touch's size round answers a window while a writer holds the
 * write axis OPEN: the window closes, its cohort riding the write request.
 * The writer gives the axis back unsent, and the window asks again -- still
 * a claimable stub's request, so a second size round re-sends it with a
 * landing. */
static void read_given_resend(void) {
  begin("read axis re-opened by a writer's give-back after a size round",
        GUID_CLAIM, false);
  acquire(DB_MODE_RO);
  uint32_t tok = 0u;
  if (!arts_db_rw_req_claim(&g_db->cache, &tok)) {
    fail("the write axis was not idle");
  }
  read_answer(0, 2u);
  arts_db_rw_req_giveback(&g_db->cache, tok);
  if (read_requests() != 2 ||
      read_request(1)->token != read_request(0)->token) {
    fail("the give-back did not ask again");
  }
  read_answer(1, 2u);
  if (read_requests() != 3 || read_request(2)->rdzv.cookie == 0u) {
    fail("the size round did not re-send the request with a landing");
  }
  read_answer(2, 0u);
  end(1);
}

static void read_claimed(void) {
  begin("read axis closed by a create's claim", GUID_CLAIM, false);
  acquire(DB_MODE_RO);
  if ((claim() & ARTS_CREATE_OWED_RO) == 0u) {
    fail("the claim did not close the read request");
  }
  end(1);
}

static void read_retired(void) {
  begin("read answer on a retired descriptor", GUID_SIZED, false);
  acquire(DB_MODE_RO);
  retire();
  read_answer(0, 0u);
  end(0);
}

/* The write turn served the window and the block's teardown retired the
 * descriptor before the dismiss of the read request, still out, arrived: the
 * dismiss only returns the landing and drops the count. */
static void read_dismissed_retired(void) {
  begin("read dismiss on a retired descriptor", GUID_SIZED, false);
  acquire(DB_MODE_RO);
  acquire(DB_MODE_RW);
  write_answer(0, false);
  confirm_ack();
  retire();
  read_answer(0, 2u);
  if (read_requests() != 1) {
    fail("a retired descriptor re-sent its read request");
  }
  end(2);
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
  write_size_round(0);
  if (write_request(1)->token != write_request(0)->token ||
      write_request(1)->rdzv.cookie == 0u) {
    fail("the re-sent request is not the same request with a landing");
  }
  write_answer(1, false);
  confirm_ack();
  end(1);
}

static void write_claimed(void) {
  begin("write axis closed by a create's claim", GUID_CLAIM, false);
  acquire(DB_MODE_RW);
  if ((claim() & ARTS_CREATE_OWED_RW) == 0u) {
    fail("the claim did not close the write request");
  }
  end(1);
}

static void write_retired(int how) {
  static const char *names[] = {"write size round on a retired descriptor",
                                "write grant on a retired descriptor",
                                "write pairing callback on a retired "
                                "descriptor"};
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

#ifdef ARTS_WRITE_POLICY_WB
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
#ifdef ARTS_PROTOCOL_VAL
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

#ifdef ARTS_RELEASE_PURGE
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
 * where the home revokes, its CONFIRM -- and under write-back the CONFIRM_ACK
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
#ifdef ARTS_RELEASE_PURGE
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
#ifdef ARTS_RELEASE_PURGE
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

#ifdef ARTS_WRITE_POLICY_WB
/* Between a self-sent read request's send and its body at the home: this
 * rank's writer commits its write request, which names the read request out;
 * the home takes that declaration in; and the block's destroy marks the cover
 * gate, which now waits for the declared read request alone. */
static void declare_then_destroy(void) {
  uint32_t tok = 0u;
  uint32_t cover = 0u;
  if (!arts_db_rw_req_claim(&g_db->cache, &tok) ||
      !arts_db_rw_req_commit(&g_db->cache, tok, &cover) ||
      (cover & ARTS_COVER_RO_OPEN) == 0u) {
    fail("the write request does not name the read request out");
  }
  bool td = false;
  if (arts_db_cover_request(g_db, HOME, true, cover, &td) !=
          ARTS_COVER_PROCESS ||
      td) {
    fail("the home did not take the declaration in");
  }
  arts_db_cover_destroy(g_db);
}

/* The home's own read request, dismissed inside its self-send with the
 * teardown due: the dismiss closes the read axis and drops its count, the
 * teardown retires the descriptor, and the deleter may run only once the
 * request's body has returned.  Under VAL the home redirects its request to
 * the owner, whose size round has the window ask again, and the re-send is
 * the one dismissed; INV's home sends no size round for its own fetch, so
 * the first request is. */
static void home_read_dismissed_teardown(void) {
  begin("home's own read request dismissed with its teardown due", GUID_SIZED,
        true);
#ifdef ARTS_PROTOCOL_VAL
  acquire(DB_MODE_RO);
  const struct arts_msg_snapshot_redirect_packet_s *rd =
      (const struct arts_msg_snapshot_redirect_packet_s *)sent(
          MSG_DB_SNAPSHOT_REDIRECT, 0);
  struct arts_db_snapshot_response_args_s a = {
      .db_guid = rd->db_guid,
      .token = rd->token,
      .cover = rd->cover,
      .data_present = 2u,
      .version = 0u,
      .db_size = SIZE,
      .rdzv_txid = 0u,
      .rdzv_cookie = rd->rdzv.cookie,
  };
  g_dispatch_hook = declare_then_destroy;
  g_poison = true;
  uint8_t census = arts_db_census_serving(true);
  arts_handler_db_snapshot_response(&a);
  arts_db_census_served(census);
#else
  g_dispatch_hook = declare_then_destroy;
  g_poison = true;
  acquire(DB_MODE_RO);
#endif
  if (g_dispatch_hook != NULL) {
    fail("no read request reached the home");
  }
  end_dropped(0);
}

#ifdef ARTS_PROTOCOL_INV
/* At the serve of the home's own read request, after the home processed it
 * and before the deliver's look at the token: the same declaration and
 * destroy, which now find nothing owed, so the teardown retires the
 * descriptor at once. */
static void retire_at_serve(void) {
  if (g_dispatch_depth == 0) {
    fail("the serve ran outside the home's own read request");
  }
  declare_then_destroy();
}

/* The home's own read request, served by the home as the holder its
 * directory names, on a descriptor retired since the request was processed:
 * the self-serve only drops the read request's count, and the deleter may run
 * only once the request's body has returned. */
static void home_read_served_retired(void) {
  begin("home's own read served on a retired descriptor", GUID_SIZED, true);
  /* The directory names the home the holder while the home has neither a
   * valid copy nor a write turn: its grant is on its way to a rank that has
   * not confirmed it yet. */
  atomic_store_explicit(&g_db->rw_holder, HOME, memory_order_relaxed);
  g_buf_hook = retire_at_serve;
  g_poison = true;
  acquire(DB_MODE_RO);
  if (g_buf_hook != NULL) {
    g_buf_hook = NULL;
    fail("the home did not serve its own read request");
  }
  end_dropped(0);
}
#endif
#endif

#if PUBLISHES
#ifdef ARTS_PROTOCOL_INV
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
#ifdef ARTS_PROTOCOL_INV
  begin("home's relaunched flight landed by its own round's ACK", GUID_SIZED,
        true);
#else
  begin("home's relaunched flight landed by its own ACK", GUID_SIZED, true);
#endif
  struct arts_db_cache_s *cache = &g_db->cache;
  home_buffer(2u);
#ifdef ARTS_PROTOCOL_INV
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
#ifdef ARTS_PROTOCOL_INV
  g_send_hook = ack_remote_invalidate;
  g_send_hook_type = MSG_DB_INV_INVALIDATE;
#endif
  arts_handler_db_publish_ack(&a);
#ifdef ARTS_PROTOCOL_INV
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

#ifdef ARTS_RELEASE_PURGE
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
  read_dismissed();
  read_dismissed_after_grant();
  read_given_riders();
  read_given_resend();
  read_claimed();
  read_retired();
  read_dismissed_retired();
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
#ifdef ARTS_WRITE_POLICY_WB
  confirm_ack_retired();
#endif
#ifdef ARTS_DEBUG_INJECTION
  write_rearmed();
#endif
#ifdef ARTS_RELEASE_PURGE
  write_self_front();
#endif
  home_wake_drops_last();
  home_request_served_in_body();
#ifdef ARTS_WRITE_POLICY_WB
  home_read_dismissed_teardown();
#ifdef ARTS_PROTOCOL_INV
  home_read_served_retired();
#endif
#endif
#if PUBLISHES
  home_flight_relaunch_lands();
  flight_landed();
  flight_retired();
#endif
  sweep_rows();
  printf("PASS token_axis_pairing_valinv\n");
  return 0;
}
