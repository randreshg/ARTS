/* SPDX-License-Identifier: Apache-2.0
 *
 * A rank the home revokes in favour of itself (its own write request was
 * queued while it held the grant, so one round names it both as the holder
 * and as the next owner) ships the grant to itself at the zero edge.  The ex-
 * owner and the new owner are one descriptor: the transfer runs the grant
 * body on it with the bytes where they are, and the parked writer is handed
 * the buffer the last write turn left, in the write policy's commit order
 * (write-through: the drain, then the CONFIRM; write-back: the CONFIRM, and
 * the drain at the CONFIRM_ACK).
 *
 * This drives the holder's INVALIDATE handler through the ship
 * (coherence/grant.c), the grant body and commit of the tree's write policy
 * (coherence/grant_wt.c or grant_wb.c), the RETAIN count edges
 * (coherence/grant_retain.c) and the waiter stack (coherence/grant_queue.c) on
 * one thread, with the wire stubbed.  Checks: no packet, lookup, send or
 * install of a copy happens for the transfer (each of those is a stub that
 * fails the test); the payload pages are inaccessible from the INVALIDATE until
 * the writer wakes, so a transfer that reads the bytes at all (to copy them
 * into a packet, or to install a snapshot of them) faults; the writer wakes
 * once, its hold counted, on the same buffer at the same version holding the
 * bytes the last turn wrote; the advertised landing goes back once; the CONFIRM
 * leaves once, in the write policy's order, naming the home's descriptor by the
 * home token the INVALIDATE carried; the descriptor's pins balance, the write
 * request's count going once with the grant that closes it; the grant stays
 * here once the writer releases; under VAL write-back the self-credit is made
 * and a concurrent advance of another rank's ledger entry survives.
 *
 * PURE test: no arts_rt, no ports, no threads.
 */
#include "coherence/grant.c"
#ifdef ARTS_WRITE_POLICY_WB
#include "coherence/grant_wb.c"
#else
#include "coherence/grant_wt.c"
#endif
#include "coherence/grant_queue.c"
#include "coherence/grant_retain.c"
#include "coherence/rank_u64_map.c"
#include "utils/shared.c"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define SELF 1u
#define HOME 0u
#define DB_GUID ((arts_guid_t)0x5a5au)
#define EDT_GUID ((arts_guid_t)0xedu)
#define WRITER_SLOT 3u
#define VERSION 7u
#define HOME_SEEN 3u     /* the version the ledger says the home holds */
#define HOME_ADVANCED 9u /* a concurrent serve's advance of that entry */
#define HOME_TOKEN ((uint64_t)0x40c0ffeeu) /* the home's, opaque here */

unsigned int arts_global_rank_id = SELF;
unsigned int arts_global_rank_count = 2u;
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;
void arts_abort(uint8_t code) { _exit(code ? code : 1); }

#include "counter_stubs.h"

void *arts_malloc(size_t n) { return malloc(n); }
void *arts_calloc(size_t n, size_t s) { return calloc(n, s); }
void arts_free(void *p) { free(p); }
unsigned int arts_guid_get_rank(arts_guid_t g) {
  (void)g;
  return HOME;
}
#ifdef ARTS_DEBUG_INJECTION
void arts_sched_fuzz_point(void) {}
#endif
#if ARTS_DB_CENSUS
ARTS_THREAD_LOCAL uint8_t arts_db_census_serve;
ARTS_THREAD_LOCAL bool arts_db_census_held;
#endif

static void fail(const char *what) {
  printf("FAIL grant_self_transfer: %s\n", what);
  fflush(stdout);
  _exit(1);
}

/* Everything a transfer to another rank or a resolution of the block would
 * reach, and the grant plane's paths this test does not drive. */
#define UNREACHED(sig)                                                         \
  sig {                                                                        \
    printf("FAIL grant_self_transfer: %s reached\n", __func__);                \
    fflush(stdout);                                                            \
    _exit(1);                                                                  \
  }
UNREACHED(arts_shared_ptr_t arts_route_table_lookup_db(arts_guid_t g))
UNREACHED(void *arts_route_table_borrow_or_lookup(arts_shared_ptr_t h,
                                                  arts_guid_t g,
                                                  arts_shared_ptr_t *l))
UNREACHED(arts_shared_ptr_t arts_route_table_copy_live(arts_shared_ptr_t h,
                                                       arts_guid_t g))
UNREACHED(arts_shared_ptr_t arts_route_table_lookup(arts_guid_t g))
UNREACHED(void arts_db_rdzv_discard_landing(uint64_t t, uint64_t s, uint64_t c))
UNREACHED(void arts_transport_send_payload_async_free(
    int r, char *m, unsigned int n, char *p, unsigned int o, uint64_t s,
    void (*f)(void *)))
#ifdef ARTS_DEBUG_INJECTION
UNREACHED(bool arts_net_send_held(int r, const char *m, unsigned int n, int k,
                                  arts_guid_t g))
#endif
UNREACHED(void arts_net_put_payload(int r, uint64_t a, uint64_t k, uint64_t t,
                                    const void *d, uint64_t s,
                                    void (*cb)(void *), void *arg))
UNREACHED(void arts_net_rdzv_expect(uint64_t txid, uint64_t len,
                                    void (*on_data)(void *),
                                    void (*on_discard)(void *), void *arg))
UNREACHED(void arts_db_buf_ref_release_cb(void *arg))
UNREACHED(bool arts_db_buf_adopt_landing(struct arts_db_cache_s *c, uint64_t v,
                                         struct arts_db_buffer_s *l,
                                         uint64_t s))
UNREACHED(struct arts_db_buffer_s *arts_db_buf_install(
    struct arts_db_cache_s *c, uint64_t v, const void *d, uint64_t s))
UNREACHED(struct arts_db_buffer_s *arts_db_buf_install_landed(
    struct arts_db_cache_s *c, uint64_t v, struct arts_db_buffer_s *l,
    uint64_t s))
UNREACHED(struct arts_db_buffer_s *arts_db_buf_landing_alloc(
    uint64_t s, struct arts_rdzv_landing_s *o))
UNREACHED(bool arts_db_buf_ensure(struct arts_db_cache_s *c, uint64_t s))
UNREACHED(void *arts_db_acquire_local(struct arts_db_cache_s *c))
UNREACHED(uint64_t arts_db_first_fetch_size(const struct arts_db_cache_s *c))
UNREACHED(bool arts_db_rw_req_claim(struct arts_db_cache_s *c, uint32_t *t))
UNREACHED(bool arts_db_rw_req_commit(struct arts_db_cache_s *c, uint32_t t,
                                     uint32_t *cover))
UNREACHED(void arts_db_rw_req_giveback(struct arts_db_cache_s *c, uint32_t t))
UNREACHED(uint32_t arts_db_rw_req_declaim(struct arts_db_cache_s *c))
UNREACHED(void arts_ooo_dispatch_on(arts_shared_ptr_t h, ooo_kind_t k,
                                    arts_guid_t g, const void *a, uint32_t s))
UNREACHED(void arts_db_grant_note_ex_holder(struct arts_db_s *db,
                                            unsigned int r))
UNREACHED(int arts_db_cover_request(struct arts_db_s *db, unsigned int r,
                                   bool w, uint32_t c, bool *t))
UNREACHED(void arts_db_home_destroy_now(struct arts_db_s *db))
UNREACHED(void arts_home_grantreq_queue_push(
    struct arts_home_grantreq_queue_s *q, unsigned int r,
    const struct arts_rdzv_landing_s *l, uint64_t h, uint64_t t))
UNREACHED(bool arts_home_grantreq_queue_pop_linked(
    struct arts_home_grantreq_queue_s *q, unsigned int *r,
    struct arts_rdzv_landing_s *l, uint64_t *h, uint64_t *t))
#ifndef NDEBUG
UNREACHED(void arts_dep_acq_resolve_once(struct arts_edt_s *e, uint32_t s))
#endif

/* ---- the descriptor, its buffer and the writer parked on it ---- */

static struct arts_db_s *g_db;
static struct arts_db_buffer_s *g_buf;
static char *g_payload; /* page-aligned: g_buf->data */
static size_t g_payload_size;
static struct arts_db_buffer_s *g_landing;
static struct arts_edt_s *g_writer;
static int g_db_deleted;
static int g_buf_deleted;

static unsigned int g_step;
static unsigned int g_landing_returned, g_confirms, g_wakes, g_acks;
static unsigned int g_confirm_step, g_wake_step, g_ack_step;

static unsigned char pattern_byte(size_t i) {
  return (unsigned char)((i * 131u + 17u) & 0xffu);
}

static void db_deleter(void *p) {
  if (p != g_db) {
    fail("the descriptor's deleter ran on another object");
  }
  g_db_deleted++;
}

static void buf_deleter(void *p) {
  if (p != g_buf) {
    fail("the buffer's deleter ran on another object");
  }
  g_buf_deleted++;
}

static void payload_protect(int prot) {
  if (mprotect(g_payload, g_payload_size, prot) != 0) {
    fail("mprotect");
  }
}

static void on_segv(int sig, siginfo_t *si, void *uc) {
  (void)sig;
  (void)uc;
  char *a = (char *)si->si_addr;
  const char *msg =
      (a >= g_payload && a < g_payload + g_payload_size)
          ? "FAIL grant_self_transfer: the transfer read or wrote the payload\n"
          : "FAIL grant_self_transfer: SIGSEGV outside the payload\n";
  ssize_t w = write(STDOUT_FILENO, msg, strlen(msg));
  (void)w;
  _exit(1);
}

static void check_buffer(const char *when) {
  arts_shared_ptr_t h = arts_shared_slot_load(&g_db->cache.buffer);
  struct arts_db_buffer_s *b = (struct arts_db_buffer_s *)arts_shared_get(h);
  char msg[160];
  if (b != g_buf) {
    snprintf(msg, sizeof(msg), "%s: the slot holds another buffer", when);
    fail(msg);
  }
  if (atomic_load_explicit(&b->version, memory_order_relaxed) != VERSION) {
    snprintf(msg, sizeof(msg), "%s: the buffer's version moved", when);
    fail(msg);
  }
  for (size_t i = 0; i < g_payload_size; i++) {
    if ((unsigned char)b->data[i] != pattern_byte(i)) {
      snprintf(msg, sizeof(msg), "%s: byte %zu is not the last turn's", when,
               i);
      fail(msg);
    }
  }
  arts_shared_release(&h);
}

/* ---- the buffer plane, as buffer.c defines it ---- */

arts_shared_ptr_t arts_db_buf_acquire(struct arts_db_cache_s *cache) {
  return arts_shared_slot_load(&cache->buffer);
}

void arts_db_buf_release(arts_shared_ptr_t *h) { arts_shared_release(h); }

/* The ledger entry of another rank advances here, as a redirect serve on
 * another thread may advance it at any point of the ship. */
void arts_db_buf_landing_return(struct arts_db_buffer_s *b) {
  if (b != g_landing) {
    fail("a landing other than the advertised one went back");
  }
  g_landing_returned++;
  g_step++;
  if (g_db->cache.cached_version != NULL) {
    arts_rank_u64_map_advance(g_db->cache.cached_version, HOME, HOME_ADVANCED);
  }
  free(b);
  g_landing = NULL;
}

/* ---- the commit's collaborators ---- */

#ifdef ARTS_WRITE_POLICY_WT
void arts_db_drain_publication_waiters(struct arts_db_cache_s *cache) {
  if (cache != &g_db->cache) {
    fail("the publication waiters drained on another cache");
  }
}
#endif
#ifdef ARTS_PROTOCOL_VAL
void arts_db_ro_combine_grant_drain(struct arts_db_cache_s *cache) {
  if (cache != &g_db->cache) {
    fail("the read combine drained on another cache");
  }
}
#endif

void arts_db_rw_req_clear(struct arts_db_cache_s *cache) {
  if (cache != &g_db->cache) {
    fail("the request word cleared on another cache");
  }
}

/* The one message the new owner owes: the CONFIRM to the home. */
void arts_transport_send_async(int rank, char *message, unsigned int length) {
  struct arts_msg_grant_confirm_packet_s *p =
      (struct arts_msg_grant_confirm_packet_s *)message;
  if (length != sizeof(*p) || p->header.message_type != MSG_DB_GRANT_CONFIRM ||
      (unsigned int)rank != HOME || p->db_guid != DB_GUID) {
    fail("a message other than the CONFIRM to the home left");
  }
  if (p->home_token != HOME_TOKEN) {
    fail("the CONFIRM does not echo the transfer's home token");
  }
  g_confirms++;
  g_confirm_step = ++g_step;
}

#ifdef ARTS_DEBUG_WAKE_GUID
void arts_dep_waiter_check(const struct arts_edt_s *edt, arts_guid_t edt_guid,
                           unsigned int slot) {
  (void)slot;
  if (edt->guid != edt_guid) {
    fail("the woken waiter names another EDT");
  }
}
#endif

/* The writer the grant serves: it reads the block the moment it is woken. */
void arts_db_resolve_slot_cached(struct arts_edt_s *edt, unsigned int slot) {
  if (edt != g_writer || slot != WRITER_SLOT) {
    fail("a waiter other than the parked writer woke");
  }
  g_wakes++;
  g_wake_step = ++g_step;
  unsigned int wc =
      atomic_load_explicit(&g_db->cache.writer_count, memory_order_relaxed);
  if (wc != 3u) {
    fail("the writer woke without sentinel, guard and its own hold counted");
  }
  payload_protect(PROT_READ | PROT_WRITE);
  check_buffer("at the writer's wake");
}

/* ---- the run ---- */

static void setup(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = on_segv;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGSEGV, &sa, NULL) != 0 || sigaction(SIGBUS, &sa, NULL) != 0) {
    fail("sigaction");
  }

  size_t db_bytes = (sizeof(struct arts_db_s) + 63u) & ~(size_t)63u;
  g_db = (struct arts_db_s *)aligned_alloc(64, db_bytes);
  memset(g_db, 0, sizeof(*g_db));
  struct arts_db_cache_s *cache = &g_db->cache;
  cache->db_guid = DB_GUID;

  /* The payload starts on a page of its own, so its pages can be closed
   * without closing the buffer's header. */
  size_t pg = (size_t)sysconf(_SC_PAGESIZE);
  g_payload_size = 2u * pg;
  char *map = (char *)mmap(NULL, pg + g_payload_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (map == MAP_FAILED) {
    fail("mmap");
  }
  g_buf = (struct arts_db_buffer_s *)(map + pg -
                                      offsetof(struct arts_db_buffer_s, data));
  g_payload = map + pg;
  atomic_init(&g_buf->version, VERSION);
  for (size_t i = 0; i < g_payload_size; i++) {
    g_payload[i] = (char)pattern_byte(i);
  }
  g_buf->cb = arts_shared_make(g_buf, buf_deleter);
  arts_shared_slot_store(&cache->buffer, g_buf->cb);
  atomic_init(&cache->db_size, (uint64_t)g_payload_size);
  atomic_init(&cache->payload_pending, (uint8_t)0);

  /* This rank holds the grant (the sentinel) and no write turn. */
  atomic_init(&cache->writer_count, 1u);
  cache->incoming_new_owner = ARTS_NO_PENDING_OWNER;
  arts_pending_rw_queue_init(&cache->pending_rw);
#if defined(ARTS_PROTOCOL_VAL) && defined(ARTS_WRITE_POLICY_WB)
  cache->cached_version = arts_rank_u64_map_create(arts_global_rank_count);
  arts_rank_u64_map_set(cache->cached_version, HOME, HOME_SEEN);
#else
  cache->cached_version = NULL;
#endif

  /* The caller's pin: the INVALIDATE's dispatcher holds the descriptor,
   * installed under the block's GUID. */
  g_db->self_cb = arts_shared_make(g_db, db_deleter);
  arts_shared_set_tag(g_db->self_cb, (uint64_t)DB_GUID);
  /* The write request the home popped is this rank's: it holds its count
   * until the grant closes it. */
  (void)arts_shared_copy(g_db->self_cb);

  /* The writer whose fast path met no grant and parked; its request is the
   * one the home popped. */
  g_writer = (struct arts_edt_s *)calloc(1, sizeof(struct arts_edt_s));
  g_writer->guid = EDT_GUID;
  struct arts_db_rw_waiter_s *w =
      (struct arts_db_rw_waiter_s *)calloc(1, sizeof(*w));
  w->edt = g_writer;
  ARTS_DEP_WAITER_SET(w, g_writer);
  w->slot = WRITER_SLOT;
  arts_pending_rw_queue_push(&cache->pending_rw, w);

  /* The landing that request advertised. */
  g_landing = (struct arts_db_buffer_s *)calloc(1, sizeof(*g_landing));
}

int main(void) {
  setup();
  struct arts_db_cache_s *cache = &g_db->cache;

  struct arts_ooo_args_db_grant_invalidate_s inv = {
      .db_guid = DB_GUID,
      .new_owner_rank = SELF,
      .new_owner_rdzv = {.addr = 0x1000u,
                         .key = 1u,
                         .txid = 42u,
                         .cookie = (uint64_t)(uintptr_t)g_landing},
      .new_owner_have = VERSION,
      .new_owner_token = (uint64_t)(uintptr_t)g_db,
      .home_token = HOME_TOKEN,
  };
  payload_protect(PROT_NONE);
  arts_handler_db_grant_invalidate(g_db, &inv);

#ifdef ARTS_WRITE_POLICY_WB
  if (g_wakes != 0u) {
    fail("the writer woke before the CONFIRM_ACK");
  }
  if (atomic_load_explicit(&cache->writer_count, memory_order_relaxed) !=
      2u + ARTS_GRANT_UNCONFIRMED) {
    fail("the install is not sentinel + guard, unconfirmed");
  }
  /* The home's answer to the CONFIRM: no next owner. */
  struct arts_msg_grant_confirm_ack_packet_s ack;
  memset(&ack, 0, sizeof(ack));
  ack.db_guid = DB_GUID;
  ack.token = (uint64_t)(uintptr_t)g_db;
  ack.new_owner_rank = ARTS_NO_PENDING_OWNER;
  g_acks++;
  g_ack_step = ++g_step;
  arts_handler_db_grant_confirm_ack(&ack);
  if (!(g_confirm_step < g_ack_step && g_ack_step < g_wake_step)) {
    fail("write-back order is not CONFIRM, CONFIRM_ACK, wake");
  }
#else
  if (!(g_wake_step < g_confirm_step)) {
    fail("write-through order is not wake, CONFIRM");
  }
#endif
  if (g_wakes != 1u || g_confirms != 1u || g_landing_returned != 1u) {
    fail("the writer, the CONFIRM and the landing are not one each");
  }
  if (atomic_load_explicit(&cache->writer_count, memory_order_relaxed) != 2u) {
    fail("after the commit the word is not sentinel + the writer's hold");
  }
  if (cache->incoming_new_owner != ARTS_NO_PENDING_OWNER) {
    fail("the transfer target was not re-armed");
  }

  /* The writer releases; the grant stays on this rank. */
  arts_db_grant_release_commit(cache);
  if (atomic_load_explicit(&cache->writer_count, memory_order_relaxed) != 1u) {
    fail("after the writer's release the word is not the sentinel");
  }
  check_buffer("after the transfer");

  if (cache->cached_version != NULL) {
    if (arts_rank_u64_map_get(cache->cached_version, SELF) != VERSION) {
      fail("the ex-owner's self-credit is missing");
    }
    if (arts_rank_u64_map_get(cache->cached_version, HOME) != HOME_ADVANCED) {
      fail("a concurrent advance of the ledger was rolled back");
    }
    arts_rank_u64_map_destroy(cache->cached_version);
  }

  /* The request's count went with the grant that closed it, once: the
   * caller's pin is the last. */
  if (g_db_deleted != 0) {
    fail("the transfer dropped a pin it did not take");
  }
  arts_shared_release(&g_db->self_cb);
  if (g_db_deleted != 1) {
    fail("the transfer kept a pin on the descriptor");
  }
  arts_shared_slot_store(&cache->buffer, NULL);
  if (g_buf_deleted != 1) {
    fail("the transfer kept a reference on the buffer");
  }
  munmap(g_payload - sysconf(_SC_PAGESIZE),
         (size_t)sysconf(_SC_PAGESIZE) + g_payload_size);
  free(g_writer);
  free(g_db);

  printf("PASS grant_self_transfer\n");
  return 0;
}
