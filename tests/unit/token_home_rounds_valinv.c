/* SPDX-License-Identifier: Apache-2.0
 *
 * A round the home starts for answers it waits on owns one count on the
 * home's descriptor, and its answers reach that descriptor through the home
 * token its messages carry (coherence.h): the INV invalidation round (taken
 * just before its ack count is armed, dropped by the ack that ends the round,
 * after the finish), and, where the home revokes the write right, the baton's
 * tenure (taken by the claim that lifts the baton off zero, carried across
 * every pop of the tenure, dropped by the put-down that ends it).  An answer
 * that finds the home retired takes the round's counting step and nothing
 * else, and the step that ends the round drops the count.
 *
 * Pairing oracle.  Each scenario builds a descriptor whose control block
 * carries the test's one reference and a counting deleter, drives the round
 * through the real code (the publish that submits a release, the request
 * arrival that claims the baton, the answers' entries, the teardown sweep),
 * then drops its reference and requires the deleter to run exactly then: a
 * count left behind keeps it from running, a count dropped twice runs it
 * before.  The wire is stubbed: a message is captured, and its answer is
 * built from it, so every answer echoes the token its message carried.
 *
 * Scenarios.
 *   INV (every cell): a round ended by its last ack; acks on a retired home,
 *     which decrement without a finish, the last dropping the count; a round
 *     re-armed inside the finish, which takes its own count; the sweep's row
 *     for an armed round; under write-back, the home's own copy retired
 *     inline, its ack counted inline through the round's home token; a
 *     sharer's ack, sent at once with the round's home token or, owed to a
 *     kill-marked fetch, kept with it until the doomed reply or the dismiss
 *     that closes the fetch sends the ack.
 *   VAL and INV where the home revokes: a tenure that pops nothing; a tenure
 *     ended by its CONFIRM; a tenure carried across a second pop (write-
 *     through: the next INVALIDATE; write-back: the CONFIRM_ACK's
 *     piggyback); a CONFIRM that trails the home's teardown, which puts the
 *     baton down and ends the tenure; the sweep's row for a held baton; the
 *     home token riding the holder's GRANT_RESPONSE, given by the INVALIDATE
 *     or, under write-back, the CONFIRM_ACK's piggyback, and the new owner's
 *     CONFIRM (the one-sided pairing included).
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef ARTS_RELEASE_PURGE
/* The home serves the grant itself: no tenure crosses ranks. */
#define REVOKES 0
#else
#define REVOKES 1
#endif

#define HOME 0u
#define REQ 1u
#define SIZE 64u
#define GUID_SIZED                                                             \
  ARTS_GUID_MAKE(ARTS_GUID_DB, HOME, ARTS_GUID_DB_KEY(1u, 0x61u))
/* A home token from the wire, opaque to every rank but the home's. */
#define FOREIGN_HOME_TOKEN ((uint64_t)0x40c0ffee00u)

unsigned int arts_global_rank_id = HOME;
unsigned int arts_global_rank_count = 4u;
uint64_t arts_guid_table_count;
struct arts_runtime_shared_s arts_node_info;
struct arts_shutdown_abandon_s arts_shutdown_abandon;
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;

#include "counter_stubs.h"

static const char *g_scenario = "setup";

static void fail(const char *what) {
  printf("FAIL token_home_rounds_valinv: %s: %s\n", g_scenario, what);
  fflush(stdout);
  _exit(1);
}

void arts_abort(uint8_t code) {
  printf("FAIL token_home_rounds_valinv: %s: the runtime aborted\n",
         g_scenario);
  fflush(stdout);
  _exit(code ? code : 1);
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
    printf("FAIL token_home_rounds_valinv: %s: %s reached\n", g_scenario,      \
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
UNREACHED(void arts_ooo_dispatch_or_defer_guid(arts_guid_t g, ooo_kind_t k,
                                               const void *a, uint32_t s))
UNREACHED(void arts_ooo_dispatch_on(arts_shared_ptr_t h, ooo_kind_t kind,
                                    arts_guid_t guid, const void *args,
                                    uint32_t size))
UNREACHED(bool arts_ooo_retire(arts_guid_t key, arts_shared_ptr_t cb))
#if !defined(ARTS_PROTOCOL_INV) || !defined(ARTS_WRITE_POLICY_WB)
UNREACHED(void *arts_route_table_borrow_or_lookup(arts_shared_ptr_t h,
                                                  arts_guid_t g,
                                                  arts_shared_ptr_t *l))
#endif
UNREACHED(arts_shared_ptr_t arts_route_table_copy_live(arts_shared_ptr_t h,
                                                       arts_guid_t g))
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
UNREACHED(void arts_db_acquire_resolved(struct arts_edt_s *edt,
                                        unsigned int slot))

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
  (void)v;
  (void)s;
  landing_end(l);
  return NULL;
}
#ifdef ARTS_DEBUG_LANDING_BALANCE
int64_t arts_db_landings_outstanding(void) { return g_landings; }
#endif

/* ---- the rendezvous plane: the test fires a held expectation ---- */

static void (*g_expect_cb)(void *);
static void *g_expect_arg;

void arts_net_rdzv_expect(uint64_t txid, uint64_t len, void (*on_data)(void *),
                          void (*on_discard)(void *), void *arg) {
  (void)txid;
  (void)len;
  (void)on_discard;
  if (g_expect_arg != NULL) {
    fail("a second expectation while one is held");
  }
  g_expect_cb = on_data;
  g_expect_arg = arg;
}
uint64_t arts_net_rdzv_txid_next(void) { return ++g_txid; }

#if REVOKES
static void pairing_fire(void) {
  void *arg = g_expect_arg;
  g_expect_arg = NULL;
  if (arg == NULL) {
    fail("no expectation to fire");
  }
  g_expect_cb(arg);
}
#endif

/* ---- the wire: messages are captured, answers built from them ---- */

#define MAXSENT 16
static struct {
  int rank;
  unsigned int type;
  _Alignas(8) char buf[256];
} g_sent[MAXSENT];
static int g_nsent;

static void capture(int rank, const char *message, unsigned int length) {
  if (g_nsent == MAXSENT || length > sizeof(g_sent[0].buf)) {
    fail("too many messages");
  }
  g_sent[g_nsent].rank = rank;
  g_sent[g_nsent].type =
      ((const struct arts_msg_header_s *)message)->message_type;
  memcpy(g_sent[g_nsent].buf, message, length);
  g_nsent++;
}

void arts_transport_send_async(int rank, char *message, unsigned int length) {
  capture(rank, message, length);
}

/* GRANT_RESPONSE: its header is what the scenarios read; the trailing map is
 * the sender's to free. */
void arts_transport_send_payload_async_free(int rank, char *message,
                                            unsigned int length, char *payload,
                                            unsigned int offset, uint64_t size,
                                            void (*free_fn)(void *)) {
  (void)offset;
  (void)size;
  capture(rank, message, length);
  free_fn(payload);
}

static int sent_index(unsigned int type, int nth) {
  for (int i = 0; i < g_nsent; i++) {
    if (g_sent[i].type == type && nth-- == 0) {
      return i;
    }
  }
  fail("an expected message did not leave");
  return -1;
}

static const void *sent(unsigned int type, int nth) {
  return g_sent[sent_index(type, nth)].buf;
}

static int count_sent(unsigned int type) {
  int n = 0;
  for (int i = 0; i < g_nsent; i++) {
    n += (g_sent[i].type == type);
  }
  return n;
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

/* home: the block's home descriptor of a block REQ made; otherwise REQ's
 * cache of it, a dependence's first touch.  The control block holds the
 * test's reference, which stands for the install's. */
static void begin(const char *name, bool home) {
  g_scenario = name;
  arts_global_rank_id = home ? HOME : REQ;
  size_t sz = (sizeof(struct arts_db_s) + 63u) & ~(size_t)63u;
  g_db = (struct arts_db_s *)aligned_alloc(64, sz);
  memset(g_db, 0, sz);
  g_db->db_type = ARTS_DB;
  arts_db_cache_init(&g_db->cache, GUID_SIZED, home ? SIZE : 0u,
                     home ? ARTS_DB_INIT_HOME_RECV : ARTS_DB_INIT_STUB, REQ);
  g_db->self_cb = arts_shared_make(g_db, db_deleter);
  arts_shared_set_tag(g_db->self_cb, (uint64_t)GUID_SIZED);
  g_deleted = 0;
  g_nsent = 0;
  g_wakes = 0;
  g_nedts = 0;
  g_expect_arg = NULL;
}

/* The block's retire, as the route table's: its control block's tag goes. */
static void retire(void) { arts_shared_set_tag(g_db->self_cb, 0u); }

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
  for (int i = 0; i < g_nedts; i++) {
    free(g_edts[i]);
  }
  printf("ok   %s\n", g_scenario);
}

/* An EDT acquiring the block in `mode` through the arm's acquire body. */
static void acquire(arts_db_access_mode_t mode) {
  struct fake_edt *f = (struct fake_edt *)calloc(1, sizeof(*f));
  f->edt.guid = 0xed0u + (arts_guid_t)g_nedts;
  f->edt.depc = 1u;
  f->edt.dep_acq = f->acq;
  f->depv[0].guid = g_db->cache.db_guid;
  f->depv[0].mode = mode;
  f->acq[0].pin = g_db->self_cb; /* borrowed, as the firing frame's */
  g_edts[g_nedts++] = f;
  struct arts_ooo_args_db_acquire_s a = {
      .edt = &f->edt, .db_guid = g_db->cache.db_guid, .slot = 0u};
  arts_handler_db_acquire(g_db, &a);
}

#ifdef ARTS_PROTOCOL_INV
/* ===== the invalidation round ========================================== */

#ifdef ARTS_WRITE_POLICY_WB
/* The home's own INVALIDATE borrows its descriptor, which stands for an
 * installed one while its tag names the block; a retired one's lookup
 * misses. */
void *arts_route_table_borrow_or_lookup(arts_shared_ptr_t h, arts_guid_t g,
                                        arts_shared_ptr_t *l) {
  *l = NULL;
  return arts_shared_tag(h) == (uint64_t)g ? arts_shared_get(h) : NULL;
}
#endif

/* A data-less release of `releaser` reaching the home: the publish body
 * submits its entry to the round. */
static void release_at_home(unsigned int releaser, uint64_t version) {
  struct arts_ooo_args_db_publish_s a = {
      .releaser = releaser,
      .db_guid = GUID_SIZED,
      .version = version,
      .cv = 0u,
      .data_size = 0u,
      .rdzv_txid = 0u,
      .rdzv_cookie = 0u,
      .returns_grant = 0u,
      .token = 0x7000u + releaser,
  };
  arts_handler_db_publish(g_db, &a);
}

/* The sharer's ack to the nth INVALIDATE, echoing its home token. */
static void ack(int nth) {
  int i = sent_index(MSG_DB_INV_INVALIDATE, nth);
  const struct arts_msg_inv_invalidate_packet_s *inv =
      (const struct arts_msg_inv_invalidate_packet_s *)g_sent[i].buf;
  if (inv->home_token != arts_db_token(g_db)) {
    fail("an INVALIDATE does not carry the round's home token");
  }
  struct arts_msg_inv_invalidate_ack_packet_s p;
  memset(&p, 0, sizeof(p));
  p.header.message_type = MSG_DB_INV_INVALIDATE_ACK;
  p.header.rank = (unsigned int)g_sent[i].rank;
  p.db_guid = inv->db_guid;
  p.home_token = inv->home_token;
  arts_handler_db_inv_invalidate_ack(&p);
}

static uint32_t acks_out(void) {
  return atomic_load_explicit(&g_db->round_acks, memory_order_relaxed);
}

static uintptr_t round_word(void) {
  return atomic_load_explicit(&g_db->round_queue, memory_order_relaxed);
}

static void round_last_ack(void) {
  begin("a round ended by its last ack drops its count once", true);
  arts_rank_bitset_set(&g_db->roster, 1u);
  arts_rank_bitset_set(&g_db->roster, 2u);
  release_at_home(3u, 1u);
  if (acks_out() != 2u || count_sent(MSG_DB_INV_INVALIDATE) != 2) {
    fail("the round did not arm for its two sharers");
  }
  ack(0);
  if (count_sent(MSG_DB_PUBLISH_ACK) != 0 || round_word() == 0u) {
    fail("the round ended before its last ack");
  }
  ack(1);
  if (acks_out() != 0u || round_word() != 0u ||
      count_sent(MSG_DB_PUBLISH_ACK) != 1) {
    fail("the last ack did not finish the round");
  }
  end(0);
}

static void round_retired(void) {
  begin("acks on a retired home decrement without a finish", true);
  arts_rank_bitset_set(&g_db->roster, 1u);
  arts_rank_bitset_set(&g_db->roster, 2u);
  release_at_home(3u, 1u);
  retire();
  ack(0);
  if (acks_out() != 1u) {
    fail("an ack on a retired home took no step");
  }
  ack(1);
  /* The round's count went with the last step; the round itself is left as
   * the teardown finds it: held, its batch the destructor's. */
  if (acks_out() != 0u || round_word() == 0u ||
      count_sent(MSG_DB_PUBLISH_ACK) != 0) {
    fail("a retired home's round was finished");
  }
  end(0);
}

static void round_rearmed(void) {
  begin("a round re-armed inside the finish takes its own count", true);
  arts_rank_bitset_set(&g_db->roster, 1u);
  arts_rank_bitset_set(&g_db->roster, 2u);
  release_at_home(2u, 1u); /* retires rank 1's copy; rank 2 stays a sharer */
  release_at_home(3u, 2u); /* queued behind the held round */
  if (count_sent(MSG_DB_INV_INVALIDATE) != 1) {
    fail("a release behind the held round ran a round of its own");
  }
  ack(0);
  /* The finish woke rank 2, took rank 3's release and armed its round, which
   * retires rank 2's copy. */
  if (count_sent(MSG_DB_PUBLISH_ACK) != 1 ||
      count_sent(MSG_DB_INV_INVALIDATE) != 2 || acks_out() != 1u) {
    fail("the finish did not run the queued release's round");
  }
  ack(1);
  if (count_sent(MSG_DB_PUBLISH_ACK) != 2 || round_word() != 0u) {
    fail("the second round did not finish");
  }
  end(0);
}

static void round_swept(void) {
  begin("the sweep drops an armed round's count", true);
  arts_rank_bitset_set(&g_db->roster, 1u);
  release_at_home(2u, 1u);
  if (acks_out() != 1u) {
    fail("the round did not arm");
  }
  arts_db_sweep_open_items(g_db, g_db->self_cb);
  end(0);
}

#ifdef ARTS_WRITE_POLICY_WB
/* Under write-back the home's own copy is retired like any other: its
 * INVALIDATE runs inline on the home's descriptor, and so does its ack, which
 * reaches the round through the home token the INVALIDATE was given. */
static void round_home_sharer(void) {
  begin("the home's own ack counts inline through the round's home token",
        true);
  arts_rank_bitset_set(&g_db->roster, HOME);
  arts_rank_bitset_set(&g_db->roster, 1u);
  release_at_home(3u, 1u);
  if (count_sent(MSG_DB_INV_INVALIDATE) != 1 ||
      count_sent(MSG_DB_INV_INVALIDATE_ACK) != 0 || acks_out() != 1u) {
    fail("the home's own ack did not count inline");
  }
  ack(0);
  if (acks_out() != 0u || round_word() != 0u ||
      count_sent(MSG_DB_PUBLISH_ACK) != 1) {
    fail("the last ack did not finish the round");
  }
  end(0);
}
#endif

/* ---- the sharer's ack ---- */

static const struct arts_msg_inv_invalidate_ack_packet_s *sharer_ack(int nth) {
  if (count_sent(MSG_DB_INV_INVALIDATE_ACK) != nth + 1) {
    fail("an ack the sharer owes did not leave once");
  }
  int i = sent_index(MSG_DB_INV_INVALIDATE_ACK, nth);
  if (g_sent[i].rank != (int)HOME) {
    fail("an ack left for another rank than the home");
  }
  return (const struct arts_msg_inv_invalidate_ack_packet_s *)g_sent[i].buf;
}

/* A copy the INVALIDATE retires, or none to retire: the ack leaves at once,
 * echoing the round's home token. */
static void sharer_acks_at_once(void) {
  begin("a sharer acks at once with the round's home token", false);
  arts_handler_db_inv_invalidate(g_db, FOREIGN_HOME_TOKEN);
  if (sharer_ack(0)->home_token != FOREIGN_HOME_TOKEN) {
    fail("a sharer with no copy acks without the round's home token");
  }
  acquire(DB_MODE_RO);
  const struct arts_msg_inv_request_packet_s *r =
      (const struct arts_msg_inv_request_packet_s *)sent(MSG_DB_INV_REQUEST, 0);
  struct arts_msg_inv_deliver_packet_s p;
  memset(&p, 0, sizeof(p));
  p.header.rank = HOME;
  p.db_guid = r->db_guid;
  p.data_size = SIZE;
  p.rdzv_cookie = r->rdzv.cookie;
  p.token = r->token;
  arts_handler_db_inv_deliver(&p, sizeof(p));
  arts_handler_db_inv_invalidate(g_db, FOREIGN_HOME_TOKEN + 64u);
  if (sharer_ack(1)->home_token != FOREIGN_HOME_TOKEN + 64u) {
    fail("a purged copy acks without the round's home token");
  }
  end(1);
}

static void sharer_invalidated(void) {
  arts_handler_db_inv_invalidate(g_db, FOREIGN_HOME_TOKEN);
  if (count_sent(MSG_DB_INV_INVALIDATE_ACK) != 0) {
    fail("an ack owed to a kill-marked fetch left at once");
  }
}

static void sharer_acked(void) {
  if (count_sent(MSG_DB_INV_INVALIDATE_ACK) != 1) {
    fail("the owed ack did not leave once");
  }
  const struct arts_msg_inv_invalidate_ack_packet_s *a =
      (const struct arts_msg_inv_invalidate_ack_packet_s *)sent(
          MSG_DB_INV_INVALIDATE_ACK, 0);
  if (a->home_token != FOREIGN_HOME_TOKEN || a->db_guid != GUID_SIZED) {
    fail("the owed ack does not echo the round's home token");
  }
}

static void sharer_owed_deliver(void) {
  begin("a kill-marked fetch's reply sends the owed ack with its home token",
        false);
  acquire(DB_MODE_RO);
  sharer_invalidated();
  const struct arts_msg_inv_request_packet_s *r =
      (const struct arts_msg_inv_request_packet_s *)sent(MSG_DB_INV_REQUEST, 0);
  struct arts_msg_inv_deliver_packet_s p;
  memset(&p, 0, sizeof(p));
  p.header.rank = HOME;
  p.db_guid = r->db_guid;
  p.data_size = SIZE;
  p.rdzv_cookie = r->rdzv.cookie;
  p.token = r->token;
  arts_handler_db_inv_deliver(&p, sizeof(p));
  sharer_acked();
  end(1);
}

static void sharer_owed_dismiss(void) {
  begin("a dismiss that closes a kill-marked fetch sends the owed ack", false);
  acquire(DB_MODE_RO);
  acquire(DB_MODE_RW); /* the write request covers the read request */
  sharer_invalidated();
  const struct arts_msg_inv_request_packet_s *r =
      (const struct arts_msg_inv_request_packet_s *)sent(MSG_DB_INV_REQUEST, 0);
  struct arts_msg_inv_cts_packet_s c;
  memset(&c, 0, sizeof(c));
  c.header.rank = HOME;
  c.db_guid = r->db_guid;
  c.db_size = SIZE | ((uint64_t)r->cover << ARTS_INV_CTS_SIZE_BITS);
  c.rdzv_cookie = r->rdzv.cookie;
  c.token = r->token;
  arts_handler_db_inv_cts(&c);
  sharer_acked();
  /* The write turn serves both. */
  const struct arts_msg_grant_request_packet_s *w =
      (const struct arts_msg_grant_request_packet_s *)sent(MSG_DB_GRANT_REQUEST,
                                                           0);
  struct {
    struct arts_msg_grant_response_packet_s hdr;
    uint32_t map[2];
  } m;
  memset(&m, 0, sizeof(m));
  m.hdr.header.rank = HOME;
  m.hdr.db_guid = w->db_guid;
  m.hdr.data_size = SIZE;
  m.hdr.rdzv_cookie = w->rdzv.cookie;
  m.hdr.token = w->token;
  arts_handler_db_grant_response(&m, sizeof(m));
#ifdef ARTS_WRITE_POLICY_WB
  const struct arts_msg_grant_confirm_packet_s *cf =
      (const struct arts_msg_grant_confirm_packet_s *)sent(MSG_DB_GRANT_CONFIRM,
                                                           0);
  struct arts_msg_grant_confirm_ack_packet_s ca;
  memset(&ca, 0, sizeof(ca));
  ca.db_guid = cf->db_guid;
  ca.token = cf->token;
  ca.new_owner_rank = ARTS_NO_PENDING_OWNER;
  ca.new_owner_have = ARTS_GRANT_VERSION_NONE;
  arts_handler_db_grant_confirm_ack(&ca);
#endif
  end(2);
}
#endif /* ARTS_PROTOCOL_INV */

#if REVOKES
/* ===== the baton's tenure ============================================== */

static const struct arts_rdzv_landing_s g_no_landing = {0, 0, 0, 0};

static uint64_t requester_token(unsigned int rank) { return 0x9000u + rank; }

/* A write request of `rank` reaching the home: queued, then the claim. */
static void request_at_home(unsigned int rank) {
  arts_home_grantreq_queue_push(&g_db->pending_rw, rank, &g_no_landing,
                                ARTS_GRANT_VERSION_NONE, requester_token(rank));
  arts_db_grant_request_arrived(&g_db->cache, g_db, rank);
}

static uint64_t baton(void) {
  return atomic_load_explicit(&g_db->grant_baton, memory_order_relaxed);
}

static unsigned int holder(void) {
  return atomic_load_explicit(&g_db->rw_holder, memory_order_relaxed);
}

static const struct arts_msg_grant_invalidate_packet_s *invalidate(int nth) {
  return (const struct arts_msg_grant_invalidate_packet_s *)sent(
      MSG_DB_GRANT_INVALIDATE, nth);
}

/* The new owner's CONFIRM once it installed, built from what the round sent
 * it: its own token and the transfer's home token. */
static void confirm(unsigned int new_owner, uint64_t token,
                    uint64_t home_token) {
  struct arts_msg_grant_confirm_packet_s p;
  memset(&p, 0, sizeof(p));
  p.header.message_type = MSG_DB_GRANT_CONFIRM;
  p.header.rank = new_owner;
  p.db_guid = GUID_SIZED;
  p.token = token;
  p.home_token = home_token;
  arts_handler_db_grant_confirm(&p);
}

static void confirm_round(int nth) {
  const struct arts_msg_grant_invalidate_packet_s *i = invalidate(nth);
  if (i->home_token != arts_db_token(g_db) ||
      i->new_owner_token != requester_token(i->new_owner_rank)) {
    fail("an INVALIDATE does not carry the transfer's tokens");
  }
  confirm(i->new_owner_rank, i->new_owner_token, i->home_token);
}

static void tenure_pops_nothing(void) {
  /* The claim of a request an earlier tenure already served: it lifts the
   * baton off zero and finds the FIFO empty. */
  begin("a tenure that pops nothing drops its count at once", true);
  arts_db_grant_request_arrived(&g_db->cache, g_db, 2u);
  if (baton() != 0u || g_nsent != 0) {
    fail("the empty tenure did not put the baton down at once");
  }
  end(0);
}

static void tenure_confirmed(void) {
  begin("a tenure ended by its CONFIRM drops its count once", true);
  request_at_home(2u);
  if (baton() != 1u || count_sent(MSG_DB_GRANT_INVALIDATE) != 1) {
    fail("the claim did not start the transfer");
  }
  confirm_round(0);
  if (baton() != 0u || holder() != 2u) {
    fail("the CONFIRM did not end the tenure");
  }
#ifdef ARTS_WRITE_POLICY_WB
  const struct arts_msg_grant_confirm_ack_packet_s *ca =
      (const struct arts_msg_grant_confirm_ack_packet_s *)sent(
          MSG_DB_GRANT_CONFIRM_ACK, 0);
  if (ca->new_owner_rank != ARTS_NO_PENDING_OWNER || ca->home_token != 0u ||
      ca->token != requester_token(2u)) {
    fail("the plain CONFIRM_ACK is not the new owner's answer alone");
  }
#endif
  end(0);
}

static void tenure_carried(void) {
  begin("a tenure carries its count across a second pop", true);
  request_at_home(2u);
  request_at_home(3u); /* a claim behind the held baton: no tenure of its own */
  if (baton() != 2u || count_sent(MSG_DB_GRANT_INVALIDATE) != 1) {
    fail("the second claim did not queue behind the tenure");
  }
  confirm_round(0);
  if (baton() == 0u) {
    fail("the tenure ended with a request still queued");
  }
#ifdef ARTS_WRITE_POLICY_WB
  /* The next transfer rides the CONFIRM_ACK, with the home's token. */
  const struct arts_msg_grant_confirm_ack_packet_s *ca =
      (const struct arts_msg_grant_confirm_ack_packet_s *)sent(
          MSG_DB_GRANT_CONFIRM_ACK, 0);
  if (ca->new_owner_rank != 3u || ca->home_token != arts_db_token(g_db) ||
      ca->new_owner_token != requester_token(3u)) {
    fail("the piggybacked transfer does not carry its tokens");
  }
  confirm(ca->new_owner_rank, ca->new_owner_token, ca->home_token);
#else
  if (count_sent(MSG_DB_GRANT_INVALIDATE) != 2 ||
      invalidate(1)->new_owner_rank != 3u) {
    fail("the CONFIRM did not revoke for the next requester");
  }
  confirm_round(1);
#endif
  if (baton() != 0u || holder() != 3u) {
    fail("the last CONFIRM did not end the tenure");
  }
  end(0);
}

static void tenure_confirm_retired(void) {
  begin("a CONFIRM trailing the home's teardown ends the tenure", true);
  request_at_home(2u);
  int before = g_nsent;
  retire();
  confirm_round(0);
  if (baton() != 0u || holder() != REQ || g_nsent != before) {
    fail("the CONFIRM on a retired home did more than put the baton down");
  }
  end(0);
}

static void tenure_swept(void) {
  begin("the sweep drops a held baton's count", true);
  request_at_home(2u);
  arts_db_sweep_open_items(g_db, g_db->self_cb);
  end(0);
}

/* ---- the home token on its way to the CONFIRM ---- */

/* The holder ships to the target its INVALIDATE named, with both tokens. */
static void holder_ships_home_token(void) {
  begin("the holder's GRANT_RESPONSE carries the home token", false);
  /* Holding the right, every writer gone: the INVALIDATE's withdrawal is
   * the zero edge. */
  atomic_store_explicit(&g_db->cache.writer_count, 1u, memory_order_relaxed);
  struct arts_ooo_args_db_grant_invalidate_s inv = {
      .db_guid = GUID_SIZED,
      .new_owner_rank = 2u,
      .new_owner_rdzv = {0, 0, 0, 0},
      .new_owner_have = ARTS_GRANT_VERSION_NONE,
      .new_owner_token = requester_token(2u),
      .home_token = FOREIGN_HOME_TOKEN,
  };
  arts_handler_db_grant_invalidate(g_db, &inv);
  const struct arts_msg_grant_response_packet_s *r =
      (const struct arts_msg_grant_response_packet_s *)sent(
          MSG_DB_GRANT_RESPONSE, 0);
  if (g_sent[sent_index(MSG_DB_GRANT_RESPONSE, 0)].rank != 2 ||
      r->token != requester_token(2u) || r->home_token != FOREIGN_HOME_TOKEN) {
    fail("the ship does not carry the transfer's tokens");
  }
  if (g_db->cache.incoming_new_owner_home_token != 0u) {
    fail("the ship left the home token armed");
  }
  end(0);
}

/* The new owner's CONFIRM echoes the home token its grant carried. */
static void new_owner_confirms(bool payload) {
  begin(payload ? "a one-sided grant's CONFIRM echoes the home token"
                : "a grant's CONFIRM echoes the home token",
        false);
  acquire(DB_MODE_RW);
  const struct arts_msg_grant_request_packet_s *w =
      (const struct arts_msg_grant_request_packet_s *)sent(MSG_DB_GRANT_REQUEST,
                                                           0);
  struct {
    struct arts_msg_grant_response_packet_s hdr;
    uint32_t map[2];
  } m;
  memset(&m, 0, sizeof(m));
  m.hdr.header.rank = HOME;
  m.hdr.db_guid = w->db_guid;
  m.hdr.version = payload ? 3u : 0u;
  m.hdr.data_size = SIZE;
  m.hdr.rdzv_txid = payload ? w->rdzv.txid : 0u;
  m.hdr.rdzv_cookie = w->rdzv.cookie;
  m.hdr.token = w->token;
  m.hdr.home_token = FOREIGN_HOME_TOKEN;
  arts_handler_db_grant_response(&m, sizeof(m));
  if (payload) {
    pairing_fire();
  }
  const struct arts_msg_grant_confirm_packet_s *c =
      (const struct arts_msg_grant_confirm_packet_s *)sent(MSG_DB_GRANT_CONFIRM,
                                                           0);
  if (c->home_token != FOREIGN_HOME_TOKEN || c->token != w->token) {
    fail("the CONFIRM does not echo the transfer's home token");
  }
#ifdef ARTS_WRITE_POLICY_WB
  struct arts_msg_grant_confirm_ack_packet_s ca;
  memset(&ca, 0, sizeof(ca));
  ca.db_guid = c->db_guid;
  ca.token = c->token;
  ca.new_owner_rank = ARTS_NO_PENDING_OWNER;
  ca.new_owner_have = ARTS_GRANT_VERSION_NONE;
  arts_handler_db_grant_confirm_ack(&ca);
#endif
  end(1);
}

#ifdef ARTS_WRITE_POLICY_WB
/* The CONFIRM_ACK that carries the next transfer hands its home token to the
 * new owner, whose ship at the zero edge carries it on. */
static void piggyback_ships_home_token(void) {
  begin("a piggybacked transfer's GRANT_RESPONSE carries the home token",
        false);
  acquire(DB_MODE_RW);
  const struct arts_msg_grant_request_packet_s *w =
      (const struct arts_msg_grant_request_packet_s *)sent(MSG_DB_GRANT_REQUEST,
                                                           0);
  struct {
    struct arts_msg_grant_response_packet_s hdr;
    uint32_t map[2];
  } m;
  memset(&m, 0, sizeof(m));
  m.hdr.header.rank = HOME;
  m.hdr.db_guid = w->db_guid;
  m.hdr.data_size = SIZE;
  m.hdr.rdzv_cookie = w->rdzv.cookie;
  m.hdr.token = w->token;
  m.hdr.home_token = FOREIGN_HOME_TOKEN;
  arts_handler_db_grant_response(&m, sizeof(m));
  const struct arts_msg_grant_confirm_packet_s *c =
      (const struct arts_msg_grant_confirm_packet_s *)sent(MSG_DB_GRANT_CONFIRM,
                                                           0);
  struct arts_msg_grant_confirm_ack_packet_s ca;
  memset(&ca, 0, sizeof(ca));
  ca.db_guid = c->db_guid;
  ca.token = c->token;
  ca.new_owner_rank = 2u;
  ca.new_owner_have = ARTS_GRANT_VERSION_NONE;
  ca.new_owner_token = requester_token(2u);
  ca.home_token = c->home_token;
  arts_handler_db_grant_confirm_ack(&ca);
  if (count_sent(MSG_DB_GRANT_RESPONSE) != 0) {
    fail("the transfer left while the woken writer held the right");
  }
  /* The woken writer's release reaches the zero edge, which ships. */
  arts_db_grant_release_commit(&g_db->cache);
  int i = sent_index(MSG_DB_GRANT_RESPONSE, 0);
  const struct arts_msg_grant_response_packet_s *r =
      (const struct arts_msg_grant_response_packet_s *)g_sent[i].buf;
  if (g_sent[i].rank != 2 || r->token != requester_token(2u) ||
      r->home_token != FOREIGN_HOME_TOKEN) {
    fail("the piggybacked transfer's ship does not carry its tokens");
  }
  if (g_db->cache.incoming_new_owner_home_token != 0u) {
    fail("the ship left the home token armed");
  }
  end(1);
}
#endif
#endif /* REVOKES */

int main(void) {
#ifdef ARTS_PROTOCOL_INV
  round_last_ack();
  round_retired();
  round_rearmed();
  round_swept();
#ifdef ARTS_WRITE_POLICY_WB
  round_home_sharer();
#endif
  sharer_acks_at_once();
  sharer_owed_deliver();
  sharer_owed_dismiss();
#endif
#if REVOKES
  tenure_pops_nothing();
  tenure_confirmed();
  tenure_carried();
  tenure_confirm_retired();
  tenure_swept();
  holder_ships_home_token();
  new_owner_confirms(false);
  new_owner_confirms(true);
#ifdef ARTS_WRITE_POLICY_WB
  piggyback_ships_home_token();
#endif
#endif
  printf("PASS token_home_rounds_valinv\n");
  return 0;
}
