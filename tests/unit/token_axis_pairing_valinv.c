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
 * answers' entries, the rendezvous callbacks, the home's teardown, the
 * teardown sweep), then drops its reference and requires the deleter to run
 * exactly then: a count left behind keeps it from running, a count dropped
 * twice runs it before.  The wire is stubbed: a request is captured, and its
 * answer is built from it, so every answer echoes the token its request
 * carried.  Every landing a request draws is ended exactly once, and every
 * buffer hold a woken EDT's slot takes is dropped by the release the scenario
 * stands in for.
 *
 * Scenarios, in every VAL and INV cell:
 *   read axis   closed by its answer; an answer on a retired descriptor; a
 *               one-sided answer's pairing callback, on a live and on a
 *               retired descriptor, and a pairing the teardown discards (the
 *               count is then the sweep's); opened while the rank's write
 *               request is out, the grant serving the writer alone and an
 *               answer that moves no bytes at the write holder serving the
 *               reader against the holder's buffer; a one-sided answer at
 *               the write holder likewise, the holder's buffer staying; a
 *               reply of each kind (one that moves no bytes, one-sided)
 *               arriving with no request out is fatal;
 *     INV:      a fetch opened while the write request is out, kill-marked
 *               by a round, whose reply serves once, purges and acks the
 *               round; (OWNER) opened while the rank's install awaits its
 *               confirmation, the CONFIRM_ACK waking the writer alone;
 *   write axis  closed by its answer (HOME: the grant; OWNER: the
 *               CONFIRM_ACK its CONFIRM asks for); a grant and (OWNER) a
 *               CONFIRM_ACK on a retired descriptor; a
 *               one-sided grant's pairing, live and retired; re-opened by
 *               the rearm inside a grant; claimed by a writer whose waiter a
 *               grant took inside its push window, which gives the axis back
 *               when the stack is empty and commits a second request when
 *               another writer parked since, that request's grant serving
 *               the writer (the three where the fuzz points exist); the
 *               home's own request, its grant run on the home (under PURGE
 *               served from the front of its queue) while the writer it
 *               wakes drops every other reference, and the same request
 *               served inside its own self-sent body once the right is idle
 *               at the home (RETAIN: the home revokes itself and ships the
 *               grant to itself; PURGE: it serves its own front);
 *   the home's teardown: a destroy retires the block and only then tells
 *               each rank of its roster, once; a second destroy of the same
 *               home tells no rank again;
 *   the home's refusal of a request from another rank that advertises no
 *               landing for a block with bytes, on either axis, ending the
 *               run with the plane's line; a zero-size block's request and
 *               the home's own write request taken in without one;
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
/* A third rank, in the one scenario that runs three. */
#define OTHER 2u
#define SIZE 64u
/* A block whose GUID bounds its size: a first touch sends a landing. */
#define GUID_SIZED                                                             \
  ARTS_GUID_MAKE(ARTS_GUID_DB, HOME, ARTS_GUID_DB_KEY(1u, 0x51u))
/* A zero-size block's: its name bounds its size at 0. */
#define GUID_EMPTY                                                             \
  ARTS_GUID_MAKE(ARTS_GUID_DB, HOME, ARTS_GUID_DB_KEY(0u, 0x53u))

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
    _exit(rc == 1 ? EXPECTED_ABORT : 1);
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

/* A retire through the engine detached the scenario's block. */
static bool g_retired;

void arts_transport_send_async(int rank, char *message, unsigned int length) {
  if (g_nsent == MAXSENT || length > sizeof(g_sent[0].buf)) {
    fail("too many messages");
  }
  /* A roster rank is told of the destroy only by the frame whose retire
   * detached the block, so no notice precedes that retire. */
  if (((struct arts_msg_header_s *)message)->message_type ==
          MSG_DB_CACHE_DESTROY &&
      !g_retired) {
    fail("a destroy notice left before the block's retire");
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
/* Retires that found the block retired already. */
static int g_retire_misses;
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
  g_retire_misses = 0;
  g_retired = false;
}

/* The block's retire, as the route table's: its control block's tag goes. */
static void retire(void) { arts_shared_set_tag(g_db->self_cb, 0u); }

/* The home's teardown retires the descriptor as the route table's single
 * flight does: the first retire clears the tag and drops the install's
 * reference, which the test's stands for; a retire that finds the object's
 * tag cleared detaches nothing, releases nothing and answers false.  Those
 * answers are counted (g_retire_misses). */
bool arts_ooo_retire(arts_guid_t key, arts_shared_ptr_t cb) {
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
  if (g_retire_misses != 0) {
    fail("a retire found the block retired already");
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
  if (g_retire_misses != 0) {
    fail("a retire found the block retired already");
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
 * one-sided payload into `landing`. */
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
 * one-sided payload into `landing`. */
static void read_reply(const struct arts_msg_inv_request_packet_s *r,
                       uint32_t kind, uint64_t version,
                       const struct arts_rdzv_landing_s *landing) {
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

/* The nth write request names the version its cache holds: none in every
 * scenario here. */
static void write_holds_none(int nth) {
  if (write_request(nth)->have_version != ARTS_GRANT_VERSION_NONE) {
    fail("a write request names a version its cache does not hold");
  }
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
 * CONFIRM the grant's commit sent: the nth grant's. */
static void confirm_ack_at(int nth) {
#ifdef ARTS_READ_SOURCE_OWNER
  const struct arts_msg_grant_confirm_packet_s *c =
      (const struct arts_msg_grant_confirm_packet_s *)sent(MSG_DB_GRANT_CONFIRM,
                                                           nth);
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
#else
  (void)nth;
#endif
}

static void confirm_ack(void) { confirm_ack_at(0); }

static int read_requests(void) {
  return count_sent(MSG_DB_SNAPSHOT_REQUEST) + count_sent(MSG_DB_INV_REQUEST);
}

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

/* In a child: `step` must end the run with the home's refusal of a request
 * from rank `from` that advertised no landing, printed by `plane`. */
static void expect_refused(void (*step)(void), unsigned int from,
                           const char *plane, const char *what) {
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
    step();
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
                 "%s: guid %lu: a request from rank %u advertised no landing "
                 "for a block of %u bytes",
                 plane, (unsigned long)g_db->cache.db_guid, from, SIZE);
  int status = 0;
  if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
      WEXITSTATUS(status) != EXPECTED_ABORT || strstr(out, line) == NULL) {
    fail(what);
  }
}

/* In a child: `step` must return, the run going on, and leave what `served`
 * requires of it. */
static void expect_served(void (*step)(void), bool (*served)(void),
                          const char *what) {
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    int devnull = open("/dev/null", O_WRONLY);
    (void)dup2(devnull, STDOUT_FILENO);
    (void)dup2(devnull, STDERR_FILENO);
    g_expect_abort = true;
    step();
    _exit(served() ? 0 : 1);
  }
  int status = 0;
  if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
      WEXITSTATUS(status) != 0) {
    fail(what);
  }
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
  write_holds_none(0);
  write_answer(0, payload);
  if (payload) {
    pairing_fire(true);
  }
  confirm_ack();
  end(1);
}

static void write_retired(int how) {
  static const char *names[] = {
      "write grant on a retired descriptor",
      "write pairing callback on a retired descriptor"};
  begin(names[how], GUID_SIZED, false);
  acquire(DB_MODE_RW);
  if (how == 0) {
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
#ifdef ARTS_VALIDITY_VAL
#define WRITE_AXIS_IDLE VREQ_RW_IDLE
#define WRITE_AXIS_OPEN VREQ_RW_OPEN
#define WRITE_AXIS_SENT VREQ_RW_SENT
#else
#define WRITE_AXIS_IDLE INV_RW_IDLE
#define WRITE_AXIS_OPEN INV_RW_OPEN
#define WRITE_AXIS_SENT INV_RW_SENT
#endif

/* The write axis of the cache's request word. */
static uint32_t write_axis(void) {
#ifdef ARTS_VALIDITY_VAL
  return VREQ_RW(
      atomic_load_explicit(&g_db->cache.req_state, memory_order_acquire));
#else
  return INV_CACHE_RW(
      atomic_load_explicit(&g_db->cache.cache_state, memory_order_acquire));
#endif
}

static bool write_axis_idle(void) { return write_axis() == WRITE_AXIS_IDLE; }

/* The writers parked on the cache's stack. */
static unsigned int parked_writers(void) {
  unsigned int n = 0u;
  for (arts_lf_link_t *l = atomic_load_explicit(&g_db->cache.pending_rw.head,
                                                memory_order_relaxed);
       l != NULL; l = atomic_load_explicit(&l->next, memory_order_relaxed)) {
    n++;
  }
  return n;
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

#ifdef ARTS_DEBUG_INJECTION
/* ---- a claim of the write axis won after a grant served its waiter ---- */

/* The claimer's window, between its won claim and its look at the stack, was
 * reached with the axis OPEN. */
static bool g_claim_won;
/* Where a third writer parks: nowhere, inside that window, or after the
 * claimer's look found the stack empty and before its give-back. */
enum { THIRD_NONE, THIRD_BEFORE_LOOK, THIRD_BEFORE_GIVE_BACK };
static int g_third_parks;

/* A writer that found no right on this rank before the grant installed it:
 * it pushes its waiter, and its own claim meets the claimer's OPEN. */
static void third_writer_parks(void) {
  int requests = count_sent(MSG_DB_GRANT_REQUEST);
  struct fake_edt *f = new_edt(DB_MODE_RW);
  (void)arts_db_acquire_remote_rw(&g_db->cache, &f->edt, 0u);
  if (write_axis() != WRITE_AXIS_OPEN || parked_writers() != 1u ||
      count_sent(MSG_DB_GRANT_REQUEST) != requests) {
    fail("the third writer's claim did not lose to the claimer's");
  }
}

/* The claimer's next window, between its look at the empty stack and its
 * give-back. */
static bool before_give_back(void) {
  if (write_axis() != WRITE_AXIS_OPEN || parked_writers() != 0u) {
    return false;
  }
  third_writer_parks();
  return true;
}

static bool inside_won_claim(void) {
  if (write_axis() != WRITE_AXIS_OPEN) {
    return false;
  }
  if (parked_writers() != 0u) {
    fail("the claim was won over waiters the grant did not take");
  }
  g_claim_won = true;
  if (g_third_parks == THIRD_BEFORE_LOOK) {
    third_writer_parks();
  } else if (g_third_parks == THIRD_BEFORE_GIVE_BACK) {
    g_fuzz_hook = before_give_back;
  }
  return true;
}

/* Between the second writer's push and its claim: the first request's grant
 * arrives (under OWNER with its CONFIRM_ACK), takes both waiters, clears the
 * write axis and wakes them. */
static bool grant_inside_push_window(void) {
  if (write_axis() != WRITE_AXIS_SENT || parked_writers() != 2u) {
    return false;
  }
  write_answer(0, false);
  confirm_ack();
  if (g_wakes != 2 || !write_axis_idle() || parked_writers() != 0u) {
    fail("the grant did not take both waiters, clear the write axis and wake "
         "them");
  }
  g_fuzz_hook = inside_won_claim;
  return true;
}

/* The second writer's acquire, the first request's grant landing inside its
 * push window. */
static void second_writer_meets_grant(int third_parks) {
  g_claim_won = false;
  g_third_parks = third_parks;
  g_fuzz_hook = grant_inside_push_window;
  acquire(DB_MODE_RW);
  if (g_fuzz_hook != NULL || !g_claim_won) {
    g_fuzz_hook = NULL;
    fail("the second writer did not win the claim the grant's clear freed");
  }
}

/* A writer whose push a grant took wins the claim that grant's clear freed
 * and finds the stack empty: it owes no request, so it gives the write axis
 * back and looks once more.  One request in all, the axis idle, both writers
 * woken by the one grant. */
static void write_claim_given_back(void) {
  begin("write axis given back by a claim won after the grant", GUID_SIZED,
        false);
  acquire(DB_MODE_RW);
  write_holds_none(0);
  second_writer_meets_grant(THIRD_NONE);
  if (count_sent(MSG_DB_GRANT_REQUEST) != 1) {
    fail("the claim's winner opened a request for waiters a grant had served");
  }
  if (!write_axis_idle()) {
    fail("the claim's winner did not give the write axis back");
  }
  if (parked_writers() != 0u) {
    fail("a waiter was left on the stack");
  }
  end(2);
}

#ifdef ARTS_RETENTION_PURGE
/* The home's acceptance of the hand-back that just left: its ACK wakes the
 * releaser. */
static void ack_hand_back(void) {
  const struct arts_msg_grant_return_packet_s *r =
      (const struct arts_msg_grant_return_packet_s *)g_sent[g_nsent - 1].buf;
  struct arts_db_publish_ack_args_s a = {.db_guid = r->db_guid, .cv = r->cv};
  arts_handler_db_publish_ack(&a);
}
#else
/* The home's token of a revocation round, opaque to this rank. */
#define ROUND_HOME_TOKEN ((uint64_t)0x5e1f00u)
/* The home's revocation for this rank's nth write request while this rank
 * holds the right: it names this rank the next owner too. */
static void revoked_for_own_request(int nth) {
  const struct arts_msg_grant_request_packet_s *r = write_request(nth);
  struct arts_ooo_args_db_grant_invalidate_s a = {
      .db_guid = r->db_guid,
      .new_owner_rank = REQ,
      .new_owner_rdzv = {.addr = r->rdzv.addr,
                         .key = r->rdzv.key,
                         .txid = r->rdzv.txid,
                         .cookie = r->rdzv.cookie},
      .new_owner_have = r->have_version,
      .new_owner_token = r->token,
      .home_token = ROUND_HOME_TOKEN,
  };
  arts_handler_db_grant_invalidate(g_db, &a);
}
#endif

/* The writers of this rank's first grant end their turns, and the grant of
 * its second write request arrives.  Under PURGE the last release hands the
 * right back and the home serves the request; under RETAIN the home revokes
 * this rank, the holder, naming it the next owner as well, and the last
 * release ships the grant to this rank's own descriptor (under OWNER the
 * CONFIRM_ACK then opens it). */
static void second_grant_after_turns(void) {
  release_buffer_hold(&g_edts[0]->edt);
  release_buffer_hold(&g_edts[1]->edt);
#ifdef ARTS_RETENTION_PURGE
  arts_db_grant_release_commit(&g_db->cache);
  g_send_hook = ack_hand_back;
  g_send_hook_type = MSG_DB_GRANT_RETURN;
  arts_db_grant_release_commit(&g_db->cache);
  if (g_send_hook != NULL || count_sent(MSG_DB_GRANT_RETURN) != 1) {
    g_send_hook = NULL;
    fail("the last writer's release did not hand the right back");
  }
  write_answer(1, false);
#else
  revoked_for_own_request(1);
  arts_db_grant_release_commit(&g_db->cache);
  if (g_wakes != 2) {
    fail("the grant shipped while a writer of this rank held its turn");
  }
  arts_db_grant_release_commit(&g_db->cache);
  confirm_ack_at(1);
#endif
}

/* The same claim, and a third writer whose own claim loses to the claimer's
 * OPEN.  Parked before the claimer's look at the stack, that look finds it
 * and the claimer commits.  Parked after the look found the stack empty, the
 * claimer gives the axis back, and its look after the give-back finds the
 * waiter: it claims again and commits.  Either way a second request leaves
 * under the descriptor's token with a count of its own, and its grant serves
 * the third writer.  The slot holds the bytes the first grant certifies as
 * current from the start. */
static void write_claim_committed(int third_parks) {
  begin(third_parks == THIRD_BEFORE_LOOK
            ? "write axis committed by a claim won after the grant, a writer "
              "parked since"
            : "write axis claimed again after its give-back, a writer parked "
              "before the give-back",
        GUID_SIZED, false);
  home_buffer(3u);
  acquire(DB_MODE_RW);
  second_writer_meets_grant(third_parks);
  if (count_sent(MSG_DB_GRANT_REQUEST) != 2 ||
      write_request(1)->token != write_request(0)->token) {
    fail("the claim's winner opened no request for the writer it found");
  }
  if (write_axis() != WRITE_AXIS_SENT || parked_writers() != 1u ||
      g_wakes != 2) {
    fail("the second request is not out for the third writer alone");
  }
  second_grant_after_turns();
  if (g_wakes != 3 || !write_axis_idle() || parked_writers() != 0u) {
    fail("the second request's grant did not serve the third writer");
  }
  release_buffer_hold(&g_edts[2]->edt);
  end(3);
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

/* A write request from rank `from` that advertises no landing, as the
 * engine hands it to the home: under the dispatcher's pin. */
#define FAR_TOKEN ((uint64_t)0x6b6b00u)
static void write_request_in(unsigned int from) {
  struct arts_ooo_args_db_grant_request_s a = {
      .requester = from,
      .db_guid = g_db->cache.db_guid,
      .have_version = ARTS_GRANT_VERSION_NONE,
      .token = FAR_TOKEN,
  };
  arts_shared_ptr_t pin = arts_shared_copy(g_db->self_cb);
  arts_handler_db_grant_request(g_db, &a);
  arts_shared_release(&pin);
}

/* A destroy at the home, as the engine runs it: under the dispatcher's
 * pin. */
static void destroy_arrives(void) {
  arts_shared_ptr_t pin = arts_shared_copy(g_db->self_cb);
  arts_db_destroy_arrived(g_db);
  arts_shared_release(&pin);
}

/* A block created on REQ and read by OTHER: the home's roster names both.
 * Two destroys reach the home while it is still held (each on its own
 * dispatch pin), the second a program error.  The first retires the block
 * and only then tells each roster rank, once (the wire refuses a notice that
 * precedes the retire); the second's retire detaches nothing, so it tells no
 * rank again. */
static void home_teardown_once(void) {
  arts_global_rank_count = 3u;
  begin("the home's teardown tells each roster rank once", GUID_SIZED, true);
  arts_rank_bitset_note(&g_db->cached_ranks, OTHER);
  g_poison = true;
  arts_shared_ptr_t held = arts_shared_copy(g_db->self_cb);
  destroy_arrives();
  int first = count_sent(MSG_DB_CACHE_DESTROY);
  int sent_first = g_nsent;
  bool to_req = false, to_other = false;
  for (int i = 0; i < g_nsent; i++) {
    to_req |= g_sent[i].type == MSG_DB_CACHE_DESTROY && g_sent[i].rank == REQ;
    to_other |=
        g_sent[i].type == MSG_DB_CACHE_DESTROY && g_sent[i].rank == OTHER;
  }
  destroy_arrives();
  int second = count_sent(MSG_DB_CACHE_DESTROY) - first;
  printf("     notices %d then %d, retires that found the block retired %d\n",
         first, second, g_retire_misses);
  if (first != 2 || sent_first != first || !to_req || !to_other) {
    fail("the first destroy did not tell each roster rank exactly once");
  }
  if (second != 0 || g_nsent != sent_first) {
    fail("a second destroy told the roster again");
  }
  if (g_deleted != 0) {
    fail("the descriptor went while a destroy still held it");
  }
  g_retire_misses = 0;
  arts_shared_release(&held);
  end_dropped(0);
  arts_global_rank_count = 2u;
}

/* A read request from rank `from` that advertises no landing, as the engine
 * hands it to the home: under the dispatcher's pin. */
static void read_request_bare(unsigned int from) {
#ifdef ARTS_VALIDITY_VAL
  struct arts_ooo_args_db_snapshot_request_s a = {
      .requester = from,
      .db_guid = g_db->cache.db_guid,
      .rdzv = ARTS_RDZV_NO_LANDING,
      .token = FAR_TOKEN,
  };
#else
  struct arts_ooo_args_db_inv_request_s a = {
      .requester = from,
      .db_guid = g_db->cache.db_guid,
      .mode = DB_MODE_RO,
      .rdzv = ARTS_RDZV_NO_LANDING,
      .token = FAR_TOKEN,
  };
#endif
  arts_shared_ptr_t pin = arts_shared_copy(g_db->self_cb);
#ifdef ARTS_VALIDITY_VAL
  arts_handler_db_snapshot_request(g_db, &a);
#else
  arts_handler_db_inv_request(g_db, &a);
#endif
  arts_shared_release(&pin);
}

static void far_write_request_bare(void) { write_request_in(REQ); }
static void far_read_request_bare(void) { read_request_bare(REQ); }
static void own_write_request_bare(void) { write_request_in(HOME); }

#ifdef ARTS_VALIDITY_VAL
#define READ_PLANE "val"
#else
#define READ_PLANE "inv"
#endif

/* Every requester sizes its landing from the block's name, so a request from
 * another rank that advertises none for a block with bytes is refused where
 * the home takes it in, on either axis, ending the run with the plane's
 * line. */
static void refused_far(int axis) {
  static const char *names[] = {
      "landing-less write request from another rank refused",
      "landing-less read request from another rank refused"};
  begin(names[axis], GUID_SIZED, true);
  if (axis == 0) {
    expect_refused(far_write_request_bare, REQ, "grant",
                   "the home took in a landing-less write request");
  } else {
    expect_refused(far_read_request_bare, REQ, READ_PLANE,
                   "the home took in a landing-less read request");
  }
  end(0);
}

/* The step's one message: of `type`, to `rank`. */
static const void *sent_alone(unsigned int type, unsigned int rank) {
  return g_nsent == 1 && g_sent[0].type == type && g_sent[0].rank == (int)rank
             ? g_sent[0].buf
             : NULL;
}

/* The home took in the write request `from` sent.  The block's creator, REQ,
 * holds the write right: under RETAIN the revocation the request opens names
 * it as the next owner; under PURGE it waits at the front of the home's queue
 * for the hand-back, and nothing leaves. */
static bool write_taken_in(unsigned int from) {
#ifdef ARTS_RETENTION_RETAIN
  const struct arts_msg_grant_invalidate_packet_s *iv =
      (const struct arts_msg_grant_invalidate_packet_s *)sent_alone(
          MSG_DB_GRANT_INVALIDATE, REQ);
  return iv != NULL && iv->new_owner_rank == from &&
         iv->new_owner_token == FAR_TOKEN;
#else
  unsigned int rank = ~0u;
  uint64_t token = 0u;
  return g_nsent == 0 &&
         arts_home_grantreq_queue_peek_linked(&g_db->pending_rw, &rank, NULL,
                                              NULL, &token) &&
         rank == from && token == FAR_TOKEN;
#endif
}

/* The home took in REQ's read request: under HOME it answers it, under OWNER
 * it sends it on to the holder, REQ. */
static bool read_taken_in(void) {
#if defined(ARTS_VALIDITY_VAL) && defined(ARTS_READ_SOURCE_OWNER)
  const struct arts_msg_snapshot_redirect_packet_s *m =
      (const struct arts_msg_snapshot_redirect_packet_s *)sent_alone(
          MSG_DB_SNAPSHOT_REDIRECT, REQ);
  return m != NULL && m->requester_rank == REQ && m->token == FAR_TOKEN;
#elif defined(ARTS_VALIDITY_VAL)
  const struct arts_msg_snapshot_response_packet_s *m =
      (const struct arts_msg_snapshot_response_packet_s *)sent_alone(
          MSG_DB_SNAPSHOT_RESPONSE, REQ);
  return m != NULL && m->token == FAR_TOKEN;
#elif defined(ARTS_READ_SOURCE_OWNER)
  const struct arts_msg_inv_redirect_packet_s *m =
      (const struct arts_msg_inv_redirect_packet_s *)sent_alone(
          MSG_DB_INV_REDIRECT, REQ);
  return m != NULL && m->requester == REQ && m->token == FAR_TOKEN;
#else
  const struct arts_msg_inv_deliver_packet_s *m =
      (const struct arts_msg_inv_deliver_packet_s *)sent_alone(
          MSG_DB_INV_DELIVER, REQ);
  return m != NULL && m->token == FAR_TOKEN;
#endif
}

static bool far_write_taken_in(void) { return write_taken_in(REQ); }
static bool own_write_taken_in(void) { return write_taken_in(HOME); }

/* A request without a landing that the home takes in: another rank's for a
 * zero-size block, on either axis, and the home's own write request, which
 * asks for the permission alone. */
static void taken_in_bare(int how) {
  static const char *names[] = {
      "zero-size block's landing-less write request taken in",
      "zero-size block's landing-less read request taken in",
      "home's own landing-less write request taken in"};
  begin(names[how], how == 2 ? GUID_SIZED : GUID_EMPTY, true);
  if (how != 2) {
    /* The descriptor a zero-size block's create announces. */
    atomic_store_explicit(&g_db->cache.db_size, 0u, memory_order_relaxed);
  }
  static void (*const steps[])(void) = {
      far_write_request_bare, far_read_request_bare, own_write_request_bare};
  static bool (*const served[])(void) = {far_write_taken_in, read_taken_in,
                                         own_write_taken_in};
  expect_served(steps[how], served[how],
                "the home refused a request it must take in, or did not "
                "take it in");
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

/* The answer that moves no bytes again, naming a landing of its own: the
 * first one ended the landing its request named. */
static void again_no_bytes(void) {
  struct arts_rdzv_landing_s l;
  (void)arts_db_buf_landing_alloc(SIZE, &l);
  read_reply(read_request(0), 0u, 0u, &l);
}
/* The one-sided answer again, into a landing of its own. */
static void again_one_sided(void) {
  struct arts_rdzv_landing_s l;
  (void)arts_db_buf_landing_alloc(SIZE, &l);
  read_reply(read_request(0), 1u, 3u, &l);
  pairing_fire(true);
}

/* A read request is answered once, and a reply carries nothing that tells one
 * request's answer from another's: a reply of any kind that finds no request
 * out ends the run.  An answer that moves no bytes (VAL's header-only answer,
 * INV's deliver) names the landing its request carried. */
static void read_reply_without_request(int kind) {
  static const char *names[] = {
#ifdef ARTS_VALIDITY_VAL
      "header-only read answer with no request out is fatal",
#else
      "read deliver that moves no bytes with no request out is fatal",
#endif
      "one-sided read answer with no request out is fatal"};
  begin(names[kind], GUID_SIZED, false);
  acquire(DB_MODE_RO);
  if (kind == 0) {
    read_answer(0, 0u);
    expect_fatal(again_no_bytes, "the second answer did not end the run");
  } else {
    read_answer(0, 1u);
    pairing_fire(true);
    expect_fatal(again_one_sided, "the second answer did not end the run");
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
#ifdef ARTS_VALIDITY_INV
  read_killed_under_write();
#endif
#if defined(ARTS_VALIDITY_INV) && defined(ARTS_READ_SOURCE_OWNER)
  read_in_unconfirmed_window();
#endif
  read_reply_without_request(0);
  read_reply_without_request(1);
  read_retired();
  read_paired(false);
  read_paired(true);
  read_discarded();
  write_answered(false);
  write_answered(true);
  write_retired(0);
  write_retired(1);
#ifdef ARTS_READ_SOURCE_OWNER
  confirm_ack_retired();
#endif
#ifdef ARTS_DEBUG_INJECTION
  write_rearmed();
  write_claim_given_back();
  write_claim_committed(THIRD_BEFORE_LOOK);
  write_claim_committed(THIRD_BEFORE_GIVE_BACK);
#endif
#ifdef ARTS_RETENTION_PURGE
  write_self_front();
#endif
  home_wake_drops_last();
  home_request_served_in_body();
  home_teardown_once();
  refused_far(0);
  refused_far(1);
  taken_in_bare(0);
  taken_in_bare(1);
  taken_in_bare(2);
#if PUBLISHES
  home_flight_relaunch_lands();
  flight_landed();
  flight_retired();
#endif
  sweep_rows();
  printf("PASS token_axis_pairing_valinv\n");
  return 0;
}
