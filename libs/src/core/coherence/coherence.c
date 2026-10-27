/* SPDX-License-Identifier: Apache-2.0
 *
 * Coherence cache lifecycle + acquire + release + destroy.
 *
 * This translation unit consolidates four phases of the per-rank
 * coherence cache_s:
 *
 *   1. Cache construction / destruction: the steps every arm with a home
 *      directory shares (arts_db_cache_common_init, _destroy_pre,
 *      _destroy_post).  Each arm's own arts_db_cache_init and
 *      arts_db_cache_destructor wrap them around its own fields.
 *
 *   2. Acquire path (the routines every arm's arts_handler_db_acquire body
 *      shares).  Inline comments highlight the trickier race resolutions.
 *      Wake mechanism: a slot's one resolver writes the slot's acquire
 *      record with the buffer data pointer, and counts the dep down through
 *      arts_db_acquire_account (acquire_remaining); the decrement that
 *      reaches 0 hands the payload to depv and the EDT to the scheduler.  A
 *      serialized slot handed to the resume worklist is counted down by the
 *      drain, after the walk.
 *
 *   3. Release path and the PUBLISH_ACK rendezvous.  Wait/wake mechanism: a
 *      releaser registers a heap {sem, version} waiter on the cache and waits
 *      on it (arts_db_await_ack); at most one publish flight is in the air per
 *      (DB, rank), and its final PUBLISH_ACK wakes every waiter whose version
 *      that flight covers.  The announce leg's CTS is matched by pointer
 *      identity instead: the releaser's rendezvous address rides the packet
 *      and is echoed back — no per-cache seq state, no busy-wait.
 *
 *   4. Destroy lifecycle (arts_db_destroy_remote public entry).  Final teardown
 *      is driven by the cb (shared-ptr) deferred-free model: destroy fans out,
 *      then the slot's retire (by the dispatched item) drops the install ref,
 *      and once all refs drain arts_db_free runs the cache destructor and
 *      releases the descriptor's storage.
 */

#include <errno.h>
#include <semaphore.h>
#include <stdbool.h>
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "arts/coherence/buffer.h"
#include "arts/coherence/census.h"
#include "arts/coherence/coherence.h"
#include "arts/coherence/handlers.h"
#include "arts/coherence/directory.h"
#include "arts/db.h"
#include "arts/edt.h"
#include "arts/cxl/store.h"
#include "arts/gas/guid.h" /* GUID kind extraction (quiescence debug check) */
#include "arts/gas/route_table.h"
#include "arts/memory/regpool.h"
#include "arts/ooo.h"
#include "arts/runtime_state.h"
#include "arts/runtime_types.h"
#include "arts/system/print.h"
#include "arts/system/schedfuzz.h"
#include "arts/system/threads.h"
#include "arts/transport/net.h" /* arts_net_put_payload */
#include "arts/utils/lockfree_lifo.h" /* publish flight waiter stack */
#include "arts/utils/malloc.h"
#include "arts/utils/shared.h"
#include "arts/counter/Preamble.h"

/* ================================================================== */
/* ===== Cache lifecycle ============================================ */
/* ================================================================== */

void arts_db_cache_kind_seed(struct arts_db_cache_s *c,
                             arts_db_init_kind_t kind) {
  atomic_init(&c->init_kind, (uint8_t)kind);
}

bool arts_db_create_claims_stub(struct arts_db_cache_s *cache) {
  uint8_t expected = (uint8_t)ARTS_DB_INIT_STUB;
  return atomic_compare_exchange_strong_explicit(
      &cache->init_kind, &expected, (uint8_t)ARTS_DB_INIT_CREATOR_REMOTE,
      memory_order_acq_rel, memory_order_acquire);
}

/* Protocol-agnostic cache_s field init.  Each arm's arts_db_cache_init sets
 * its own fields first — the grant-bearing arms' pending_rw stack and
 * transfer target, VAL WB's dedup ledger, INV's reader word, EXCL's cache
 * word and waiter stacks — and then calls this.
 *
 * The arms that share this shape are the ones whose cache carries a home
 * directory; an arm without one builds its cache itself. */
#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV) ||               \
    defined(ARTS_PROTOCOL_EXCL)
void arts_db_cache_common_init(struct arts_db_cache_s *c, arts_guid_t db_guid,
                               uint64_t db_size, arts_db_init_kind_t kind,
                               unsigned int creator_rank) {
  /* Caller provides a zeroed cache (embedded in a zeroed/calloc'd db_s, or
   * memset by the stub path).  We do not zero it here — the embedding db_s
   * owns the storage. */
  c->db_guid = db_guid;
  atomic_init(&c->db_size, db_size);
  /* Every cache starts without a buffer; the install path clears this the
   * moment one is in the slot, whoever put it there. */
  atomic_init(&c->payload_pending, (uint8_t)1);
#ifdef ARTS_USE_CXL
  atomic_init(&c->cxl_addr, (uint64_t)0);
#endif
  arts_db_cache_kind_seed(c, kind);
#if (defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)) &&              \
    defined(ARTS_WRITE_POLICY_WT)
  arts_lf_stack_init(&c->publication_waiters);
#if ARTS_DB_CENSUS
  /* Only a home's create-time image is ever unpublished, and the creator's
   * hold is the write that publishes it first. */
  c->census_publisher_remote = (kind == ARTS_DB_INIT_HOME_RECV &&
                                creator_rank != arts_global_rank_id);
#endif
#endif
#ifdef ARTS_PROTOCOL_VAL
  atomic_init(&c->req_state,
              (kind == ARTS_DB_INIT_STUB && ARTS_DB_STUB_CLAIMABLE(db_guid))
                  ? VREQ_MAKE(0u, 0u, 0u, 0u, 1u, 0u, 0u)
                  : (uint64_t)0);
#endif
#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)
  arts_db_reader_pool_init(&c->reader_pool);
#endif
#ifdef ARTS_DB_BUF_POOL
  arts_lf_pool_init(&c->buf_freelist);
  arts_lf_stack_init(&c->buf_retired);
  atomic_init(&c->buf_holds, 0u);
#endif
  /* Initialize the inlined home-directory fields only on the rank that owns
   * this DB's GUID home; non-home ranks leave db_self->home_initialized false
   * (and allocate only the cache-only stub, so the home fields don't exist).
   * init_kind selects the initial holder. */
  unsigned int self = arts_global_rank_id;
  unsigned int n = arts_global_rank_count;
  if (n == 0) {
    n = 1;
  }
  struct arts_db_s *db_self = arts_db_of_cache(c);
  if (kind == ARTS_DB_INIT_HOME_RECV) {
    arts_db_home_init(db_self, creator_rank, n);
    db_self->home_initialized = true;
  } else if (kind == ARTS_DB_INIT_CREATOR_HOME) {
    arts_db_home_init(db_self, self, n);
    db_self->home_initialized = true;
#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)
    /* The creator boots holding the write right, with its own create-time
     * hold under it: possession, and one live writer. */
    atomic_init(&c->writer_count, ARTS_GRANT_SEED_HOLDING);
#endif
  } else if (kind == ARTS_DB_INIT_CREATOR_REMOTE) {
#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)
    atomic_init(&c->writer_count, ARTS_GRANT_SEED_HOLDING);
#endif
  }
}
#endif /* arms sharing the common cache shape */

#ifdef ARTS_USE_CXL
bool arts_db_cxl_slot_record(struct arts_db_cache_s *cache, uint64_t addr) {
  if (cache == NULL || addr == 0) {
    return false;
  }
  uint64_t expect = 0;
  return atomic_compare_exchange_strong_explicit(&cache->cxl_addr, &expect,
                                                 addr, memory_order_release,
                                                 memory_order_acquire);
}

uint64_t arts_db_cxl_slot_mint(uint64_t db_size) {
  if (db_size == 0) {
    return 0;
  }
  return (uint64_t)(uintptr_t)arts_cxl_store_alloc((size_t)db_size);
}

bool arts_db_cxl_slot_create(struct arts_db_cache_s *cache) {
  uint64_t size = cache != NULL ? atomic_load_explicit(&cache->db_size,
                                                       memory_order_relaxed)
                                : 0u;
  if (size == 0 ||
      atomic_load_explicit(&cache->cxl_addr, memory_order_acquire) != 0) {
    return false;
  }
  uint64_t addr = arts_db_cxl_slot_mint(size);
  if (!arts_db_cxl_slot_record(cache, addr)) {
    /* Another party named the slot first; the one minted here is left to
     * the arena, which reclaims nothing within a run. */
    return false;
  }
  return true;
}

/* A discarded slot leaves nothing naming it: the cache stops naming it first,
 * so nothing can adopt it, then the descriptor that named it is withdrawn.
 * The bytes stay in the arena.
 *
 * Reached only on a cache nothing else can name: a create whose route install
 * FAILED, whose object is private and about to be freed.  That is what makes
 * both steps safe — no other party can be holding the descriptor, fetching
 * into the slot, or about to adopt it — and it is a property of the call
 * sites, not of this function, so a new caller has to establish it.  A create
 * whose object IS published never discards its slot: the slot belongs to the
 * block from the moment the cache names it. */
void arts_db_cxl_slot_discard(struct arts_db_cache_s *cache) {
  uint64_t addr = atomic_exchange_explicit(&cache->cxl_addr, (uint64_t)0,
                                           memory_order_acq_rel);
  if (addr == 0) {
    return;
  }
  (void)arts_db_buf_withdraw(cache, (const void *)(uintptr_t)addr);
}

#endif /* ARTS_USE_CXL */

/* ================================================================== */
/* ===== Acquire path =============================================== */
/* ================================================================== */

/* ===== The slot resolver ============================================ */

/* The one resolution of an owner slot the EDT's acquire phase handed to an
 * arm: the frame that fired it (an in-frame serve) or the one frame a plane's
 * take hands its waiter to.  Nothing arbitrates, because nothing can meet it:
 * every plane hands a parked slot to one runner by the RMW that takes its
 * waiter (or by the one response to its one request), and the firing frame
 * touches neither the slot nor the EDT after it published the slot.  The
 * frame that fired the slot stored the descriptor pin before the arm's
 * handler ran, so the slot's descriptor is alive here and the release reaches
 * it through the pin.
 *
 * buf_h is CONSUMED: it becomes the slot's hold (release_one_dep drops it
 * through the payload's buffer).  A NULL handle resolves a zero-size block,
 * whose defined value is NULL.
 *
 * The tail runs payload, aliases, cursor, account, and both of its orderings
 * are load-bearing:
 *
 *   payload BEFORE cursor — the cursor passing a slot is what entitles a
 *   frame to fire the next one, and such a frame reads the passed slot's
 *   payload as final.
 *
 *   cursor BEFORE account — the account may be the decrement that schedules
 *   the EDT, and a scheduled EDT's acquire state belongs to whoever runs it;
 *   nothing may read or write it afterwards.  When the cursor step handed the
 *   walk to the resume worklist, the account goes with it and the drain issues
 *   it after the walk, so the walk too runs before the EDT can be scheduled.
 *
 * Stack depth is bounded on every path: the cursor step does nothing but raise
 * a thread-local flag or append to the resume worklist, and the walk it hands
 * off to runs in the single top-level loop of the drain, never nested here. */
static void resolve_slot_body(struct arts_edt_s *edt, unsigned int slot,
                              arts_shared_ptr_t buf_h) {
  arts_edt_dep_t *depv = (arts_edt_dep_t *)arts_get_depv(edt);
  struct arts_dep_acq_s *rec = &edt->dep_acq[slot];
  bool secured = !rec->alias && arts_dep_is_serialized(depv, slot);
  struct arts_db_buffer_s *buf =
      (struct arts_db_buffer_s *)arts_shared_get(buf_h);
  rec->ptr = buf != NULL ? buf->data : NULL;
  arts_db_census_count_serve(depv[slot].guid);
  /* The block's payload is final for this EDT now: every other slot naming it
   * shares these bytes.  Before this slot's own account, which may be the one
   * that schedules the EDT. */
  arts_db_fill_aliases(edt, slot);
  if (secured && arts_db_rw_secure(edt, slot)) {
    /* The walk and this slot's account now belong to the worklist entry. */
    arts_db_drain_resume_list();
    return;
  }
  /* The last touch: do NOT touch edt after the account returns. */
  arts_db_acquire_account(edt);
}

#ifdef ARTS_PROTOCOL_FLUSH
void arts_db_resolve_slot(struct arts_edt_s *edt, unsigned int slot,
                          arts_shared_ptr_t buf_h) {
  arts_dep_acq_resolve_once(edt, slot);
  resolve_slot_body(edt, slot, buf_h);
}
#endif

#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV) ||               \
    defined(ARTS_PROTOCOL_EXCL)
/* The cache-slot form: the buffer is whatever this rank's cache holds for the
 * block, read through the descriptor the slot pins, so the pin and the ref
 * name one object by construction.  It never materializes a buffer: the arm
 * that decided to resolve the slot did that at the point it established the
 * hold. */
void arts_db_resolve_slot_cached(struct arts_edt_s *edt, unsigned int slot) {
  arts_dep_acq_resolve_once(edt, slot); /* before the pin is read */
  struct arts_db_s *db =
      (struct arts_db_s *)arts_shared_get(edt->dep_acq[slot].pin);
  resolve_slot_body(edt, slot, arts_db_buf_acquire(&db->cache));
}
#endif /* arms whose readers resume on the rank's own cache */

/* ===== Stub first-touch =========================================== */

/* Allocate a stub arts_db_s + cache_s for a DIST DB that this rank
 * has not yet touched, register it in the route_table, and return a
 * PINNED handle to the db_s whose cache it installed.  The stub has no
 * user data payload — install_buffer allocates on demand cache->user_data on
 * first wire arrival.
 *
 * Race-safe: route_table_install_if_absent rejects if another thread
 * (e.g. a concurrent wire-receive) raced us; in that case we free our
 * stub and return a pinned handle to the established db_s.
 *
 * Returns a pinned handle; the caller MUST arts_shared_release it once the
 * cache is no longer needed (on every control-flow path).  A NULL handle
 * means the DB was destroyed before the install could be observed (the
 * lost-race lookup found no live entry). */
arts_shared_ptr_t arts_db_cache_stub_install(arts_guid_t db_guid,
                                             uint64_t db_size) {
  assert(arts_guid_get_rank(db_guid) != arts_global_rank_id &&
         "a first-touch cache is never made on the block's home rank");
  /* First check if it already exists (someone else stub-installed or
   * a wire-receive fired). */
  arts_shared_ptr_t existing = arts_route_table_lookup_db(db_guid);
  if (arts_shared_get(existing) != NULL) {
    return existing;
  }
  arts_shared_release(&existing);

  /* Stub install (non-home consumer first acquire): cache-only stub — no home
   * directory (this rank is not the GUID home).  arts_db_cache_stub_size()
   * spans cache + db_type, stopping before the home fields. */
  uint64_t stub_sz = arts_db_cache_stub_size();
  struct arts_db_s *stub =
      (struct arts_db_s *)arts_malloc_aligned(stub_sz, ARTS_CACHE_LINE_SIZE);
  memset(stub, 0, stub_sz);
  stub->db_type = ARTS_DB;

  /* db_size==0 ⇒ stub install: buffer alloc deferred until first wire
   * arrival (install_buffer with the actual db_size).  Cache-only: this path
   * runs on a rank that is NOT the block's home (the caller gates it on the
   * owner not being this rank), so there are no home fields to initialize and
   * the stub's allocation deliberately stops before them. */
  arts_db_cache_init(&stub->cache, db_guid, /*db_size=*/db_size,
                     ARTS_DB_INIT_STUB,
                     /*creator_rank=*/0);

  arts_shared_ptr_t installed_h =
      arts_route_table_install_if_absent(stub, db_guid);
  bool installed = installed_h != NULL;
  arts_shared_release(&installed_h);
  if (installed) {
    /* Pin the just-installed db_s (one cb ref) for the caller. */
    return arts_route_table_lookup_db(db_guid);
  }

  /* Lost the race — another thread already installed.  Tear down our
   * stub and return a pinned handle to the established db_s. */
  arts_db_free(stub);
  return arts_route_table_lookup_db(db_guid);
}

#if ARTS_DB_CENSUS
ARTS_THREAD_LOCAL uint8_t arts_db_census_serve;
ARTS_THREAD_LOCAL bool arts_db_census_held;
#endif

/* ===== Case 1/3/5: local-buffer acquire ============================= */

void *arts_db_acquire_local(struct arts_db_cache_s *cache) {
  /* Take the EDT's strong ref on the buffer and return buf->data.  The handle
   * is intentionally NOT released here — the ref is the EDT's hold for its
   * whole lifetime; release_one_dep drops it through the block's descriptor.
   * The ref keeps the buffer alive against a concurrent destroy.
   *
   * No materialization here either: this helper is reached from every arm's
   * acquire body and cannot tell whether this rank may hold the block.  Each
   * arm calls arts_db_buf_ensure in the branch where it has already decided
   * that it may. */
  arts_shared_ptr_t h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *buf = (struct arts_db_buffer_s *)arts_shared_get(h);
  if (buf == NULL) {
    return NULL; /* h is NULL — nothing installed, nothing held */
  }
  return buf->data;
}

/* Case 2/6 (RW local fast path) and Case 4/8 (remote-RW path) live in
 * coherence/grant.c, the shared grant plane: a writer parks on pending_rw and
 * opens the write request through the request word's write axis
 * (arts_db_rw_req_claim / _commit / _giveback). */

/* ===== Case 7: remote-RO / remote-snapshot path =================== */

#if defined(ARTS_PROTOCOL_VAL)
/* ===== The read path and the request word ============================
 *
 * Every remote read-only acquire rides its cache's one window: at most one
 * read request per cache is out, and a reader that arrives while it is out
 * waits for the next one -- a reader may only ride a request sent AFTER its
 * park, because the server serves the version current when the request
 * arrives, and any release the reader is ordered after was published before
 * its park.  A reader that arrives while the rank's write request is out
 * sends nothing: the write turn covers it (RW ⊇ RO).  When a write
 * permission reaches the rank -- a grant, or the create's own hold on a cache
 * the rank made before the create -- every parked reader is served at once,
 * the window's cohort included; the window's request stays out until its own
 * answer, which then serves nobody it already served.
 *
 * The chains live in the request word (val/types.h), so parking IS the
 * acquire's decision CAS and a serve takes a whole chain in the CAS that
 * decides it: no reader can miss the request it joined or be served twice. */

/* ---- the arbiter (val/arbiters.c: a pure function of the word) ---------- */
#include "val/arbiters.c"

/* Commit `op` on the word; returns the pre-image, *post the image. */
static uint64_t val_req_step(struct arts_db_cache_s *cache, int op,
                             uint32_t idx, uint64_t *post, uint32_t *act) {
  struct arts_db_reader_node_s *node =
      op == VREQ_OP_ACQ_RO ? arts_db_reader_node(&cache->reader_pool, idx)
                           : NULL;
  uint64_t cur = atomic_load_explicit(&cache->req_state, memory_order_acquire);
  uint64_t next;
  for (;;) {
    next = arts_val_req_compute_next(cur, op, idx, act);
    if (node != NULL) {
      atomic_store_explicit(&node->next,
                            *act == VREQ_ACT_PARK ? VREQ_NEXT(cur) : 0u,
                            memory_order_relaxed);
    }
    if (next == cur ||
        atomic_compare_exchange_weak_explicit(&cache->req_state, &cur, next,
                                              memory_order_acq_rel,
                                              memory_order_acquire)) {
      break;
    }
  }
  *post = next;
  return cur;
}

static void val_send_window(struct arts_db_cache_s *cache, uint64_t post) {
  arts_send_db_snapshot_request(
      cache, (VREQ_PAR(post) ? ARTS_COVER_PAR : 0u) |
                 (VREQ_STUB(post) ? ARTS_COVER_PRECLAIM : 0u));
}

arts_db_acquire_result_t
arts_db_acquire_remote_ro(struct arts_db_cache_s *cache, struct arts_edt_s *edt,
                          unsigned int slot) {
  assert(arts_global_rank_count > 1 &&
         "a rank alone homes every block and reads it locally");
  uint32_t idx = arts_db_reader_alloc(&cache->reader_pool);
  struct arts_db_reader_node_s *w =
      arts_db_reader_node(&cache->reader_pool, idx);
  w->edt = edt;
  ARTS_DEP_WAITER_SET(w, edt);
  w->slot = slot;
  ARTS_DB_CENSUS_SET_HELD(w, false);
  uint64_t post;
  uint32_t act;
  (void)val_req_step(cache, VREQ_OP_ACQ_RO, idx, &post, &act);
  if (act == VREQ_ACT_SEND_RO) {
    INCREMENT_NUM_RO_COMBINE_WINDOW_BY(1);
    val_send_window(cache, post);
  } else {
    INCREMENT_NUM_RO_COMBINE_JOINED_BY(1);
  }
  return ARTS_DB_ACQUIRE_PARK;
}

void arts_db_ro_combine_on_terminal(struct arts_db_cache_s *cache,
                                    uint32_t cover) {
  uint64_t post;
  uint32_t act;
  uint64_t pre = val_req_step(cache, VREQ_OP_ANSWER, cover & ARTS_COVER_PAR,
                              &post, &act);
  if (act == VREQ_ACT_LATE) {
    ARTS_ERROR("val: guid %lu got a read answer (parity %u) it has no "
               "request out for (word=%llx)",
               (unsigned long)cache->db_guid, cover & ARTS_COVER_PAR,
               (unsigned long long)pre);
  }
  /* The cohort that rode this request, unless a write permission already
   * served it (then the chain is empty: the answer serves nobody). */
  arts_db_reader_serve_chain(&cache->reader_pool, VREQ_GROUP(pre));
  if (act == VREQ_ACT_SERVE_SEND) {
    INCREMENT_NUM_RO_COMBINE_WINDOW_BY(1);
    val_send_window(cache, post);
  }
}

void arts_db_ro_combine_on_cts(struct arts_db_cache_s *cache,
                               uint32_t cover) {
  uint64_t post;
  uint32_t act;
  uint64_t pre =
      val_req_step(cache, VREQ_OP_CTS, cover & ARTS_COVER_PAR, &post, &act);
  if (act == VREQ_ACT_LATE) {
    ARTS_ERROR("val: guid %lu got a read size round or dismiss (parity %u) it "
               "has no request out for (word=%llx)",
               (unsigned long)cache->db_guid, cover & ARTS_COVER_PAR,
               (unsigned long long)pre);
  }
  if (act == VREQ_ACT_RESEND) {
    val_send_window(cache, post);
  }
}

/* A write permission landed (the grant's install): every parked reader is
 * served under it, the in-flight window's cohort included.  Called with the
 * install's own hold on the grant word, which keeps the permission here for
 * the whole walk.  The buffer this rank now holds contains every release
 * completed before the transfer (the ownership chain linearizes all
 * writers), so resuming against it is correct whenever a reader parked. */
void arts_db_ro_combine_grant_drain(struct arts_db_cache_s *cache) {
  uint64_t post;
  uint32_t act;
  uint64_t pre = val_req_step(cache, VREQ_OP_COVER, 0u, &post, &act);
  arts_db_reader_serve_chain(&cache->reader_pool, VREQ_GROUP(pre));
  arts_db_reader_serve_chain(&cache->reader_pool, VREQ_NEXT(pre));
}

/* ---- the write axis, for the shared grant plane ------------------------- */
bool arts_db_rw_req_claim(struct arts_db_cache_s *cache, uint32_t *tok) {
  uint64_t post;
  uint32_t act;
  (void)val_req_step(cache, VREQ_OP_RW_CLAIM, 0u, &post, &act);
  *tok = VREQ_STUB(post);
  return act == VREQ_ACT_WON;
}

static uint32_t val_rw_cover(uint64_t s) {
  uint32_t c = 0u;
  if (VREQ_RO(s) && !VREQ_RP(s)) {
    c |= ARTS_COVER_RO_OPEN;
    if (VREQ_PAR(s)) {
      c |= ARTS_COVER_RO_PAR;
    }
  }
  if (VREQ_STUB(s)) {
    c |= ARTS_COVER_PRECLAIM;
  }
  return c;
}

bool arts_db_rw_req_commit(struct arts_db_cache_s *cache, uint32_t tok,
                           uint32_t *cover) {
  uint64_t post;
  uint32_t act;
  uint64_t pre = val_req_step(cache, VREQ_OP_RW_COMMIT, tok, &post, &act);
  *cover = val_rw_cover(pre);
  return act == VREQ_ACT_WON;
}

void arts_db_rw_req_giveback(struct arts_db_cache_s *cache, uint32_t tok) {
  uint64_t post;
  uint32_t act;
  (void)val_req_step(cache, VREQ_OP_RW_GIVE, tok, &post, &act);
  if (act == VREQ_ACT_SEND_RO) {
    INCREMENT_NUM_RO_COMBINE_WINDOW_BY(1);
  }
  if (act == VREQ_ACT_RESEND || act == VREQ_ACT_SEND_RO) {
    val_send_window(cache, post);
  }
}

void arts_db_rw_req_clear(struct arts_db_cache_s *cache) {
  uint64_t post;
  uint32_t act;
  uint64_t pre = val_req_step(cache, VREQ_OP_RW_CLEAR, 0u, &post, &act);
  if (act == VREQ_ACT_LATE) {
    ARTS_ERROR("val: guid %lu got a write grant it has no request out for "
               "(word=%llx)",
               (unsigned long)cache->db_guid, (unsigned long long)pre);
  }
  arts_db_reader_serve_chain(&cache->reader_pool, VREQ_GROUP(pre));
  arts_db_reader_serve_chain(&cache->reader_pool, VREQ_NEXT(pre));
}

uint32_t arts_db_rw_req_declaim(struct arts_db_cache_s *cache) {
  uint64_t post;
  uint32_t act;
  uint64_t pre = val_req_step(cache, VREQ_OP_DECLAIM, 0u, &post, &act);
  if (act == VREQ_ACT_LATE) {
    ARTS_ERROR("val: guid %lu got a write size round it has no request out "
               "for (word=%llx)",
               (unsigned long)cache->db_guid, (unsigned long long)pre);
  }
  return val_rw_cover(post);
}

/* The create's claim: the hold is taken on the grant word first, then every
 * parked reader is served under it and the write axis becomes the claim's.
 * Returns the owed byte the announce carries. */
uint8_t arts_db_val_claim_cover(struct arts_db_cache_s *cache) {
  uint64_t post;
  uint32_t act;
  uint64_t pre = val_req_step(cache, VREQ_OP_CLAIM, 0u, &post, &act);
  uint8_t owed = 0u;
  if (VREQ_RO(pre) && !VREQ_RP(pre)) {
    owed |= ARTS_CREATE_OWED_RO;
    if (VREQ_PAR(pre)) {
      owed |= ARTS_CREATE_OWED_RO_PAR;
    }
  }
  if (VREQ_RW(pre) == VREQ_RW_SENT) {
    owed |= ARTS_CREATE_OWED_RW;
  }
  arts_db_reader_serve_chain(&cache->reader_pool, VREQ_GROUP(pre));
  arts_db_reader_serve_chain(&cache->reader_pool, VREQ_NEXT(pre));
  return owed;
}
#endif /* the versioned-snapshot read path */

/* The 8-case acquire dispatcher arts_handler_db_acquire is protocol-specific:
 * WT and WB define it in coherence/grant.c-backed
 * each arm's own write-policy TU (single-owner GRANT_REQUEST / GRANT path,
 * differing only on the RO-has-local-data predicate).  The shared remote-RO
 * path (arts_db_acquire_remote_ro) and the local-buffer fast read
 * (arts_db_acquire_local) above are reused by both.
 */

/* Wake every publication waiter, taken in one exchange.  A waiter is a read
 * that found its write-through home's copy unpublished — the
 * create-time image at version 0 — and parked: a local read hold, or a remote
 * read whose serve was deferred.  A write-through release stamps its version
 * on the home's copy before it completes, so a read that saw version 0 is
 * ordered after no release, and any published copy (version >= 1) is a value
 * it may be given.  Every drain runs once the slot holds one, and the slot's
 * version never decreases, so waking every waiter taken is exact.  The take
 * is an acq_rel exchange paired with each parker's acq_rel push: a parker
 * re-reads the version after its push, and a publisher drains after its
 * stamp, so one of the two always sees the other. */
#if (defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)) &&              \
    defined(ARTS_WRITE_POLICY_WT)
void arts_db_drain_publication_waiters(struct arts_db_cache_s *cache) {
  arts_lf_link_t *node = arts_lf_stack_drain(&cache->publication_waiters);
  /* Every waiter here waits for the first publication, so it counts from the
   * publisher's rank whichever drain -- the publisher's own or a parker's
   * re-check -- wakes it. */
#if ARTS_DB_CENSUS
  uint8_t census = arts_db_census_serving(cache->census_publisher_remote);
#endif
  while (node != NULL) {
    struct arts_db_read_waiter_s *w = (struct arts_db_read_waiter_s *)node;
    arts_lf_link_t *next =
        atomic_load_explicit(&node->next, memory_order_relaxed);
    if (w->serve != NULL) {
      /* Deferred remote serve (home parked a SNAPSHOT_REQUEST while it had no buffer
       * yet): re-issue against the now-installed buffer.  The callback must
       * not retain w past its return. */
      w->serve(cache, w);
      arts_free(w);
    } else {
      struct arts_edt_s *edt_local = w->edt;
      unsigned int slot_local = w->slot;
      /* Checked while w is live; the free touches no EDT. */
      ARTS_DEP_WAITER_CHECK(w->edt, w->edt_guid, w->slot);
      arts_free(w);
      /* Resolved after the free: the slot reads the (now-installed) cache
       * buffer. */
      arts_db_resolve_slot_cached(edt_local, slot_local);
    }
    node = next;
  }
#if ARTS_DB_CENSUS
  arts_db_census_served(census);
#endif
}
#endif /* write-through homes that hold reads until the first publication */

/* ================================================================== */
/* ===== Release path =============================================== */
/* ================================================================== */

/* ===== release-reply wait (shared coherence service) ===============
 *
 * A release that must not return before the home has answered blocks on a
 * heap semaphore that the answer posts.  Compiled for every arm; declared in
 * coherence/coherence.h so the arm TUs can invoke it. */
bool arts_db_await_ack(sem_t *cv) {
  /* Block until the arm's reply handler posts this semaphore.  The wait state
   * is the caller's heap object, never a stack frame: a wait the shutdown
   * cuts short leaks it, so a reply that still arrives posts into live
   * memory.  No busy-wait: sem_timedwait sleeps the worker.  We re-arm on a
   * coarse cadence only to re-check the shutdown flag — once teardown starts
   * the progress threads stop draining and the reply never arrives, so the
   * EDT epilogue must not block forever (returning lets the worker exit). */
  for (;;) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1; /* shutdown re-check cadence, not a timeout on the ACK */
    if (sem_timedwait(cv, &ts) == 0) {
      return true; /* the reply arrived */
    }
    if (errno == ETIMEDOUT &&
        atomic_load_explicit(&arts_node_info.shutdown_state,
                             memory_order_acquire) != 0) {
      /* teardown: ACK will never come */
      atomic_fetch_add_explicit(&arts_shutdown_abandon.waits, 1u,
                                memory_order_relaxed);
      return false;
    }
    /* ETIMEDOUT (not shutting down) or EINTR: re-arm the blocking wait. */
  }
}

/* arts_db_release_rw is protocol-specific (the version bump is shared, but the
 * pre-decrement buffer-ref drop and the post-decrement transfer/publish
 * decision differ per protocol), so its whole body lives in each arm's own
 * TU.  VAL+WT and INV under either write policy call
 * arts_db_publish_sync below for the synchronous-PUBLISH rendezvous (INV's
 * publish doubles as its invalidation-round request). */

/* ===== publish flight machine (write-combining publish) ============
 *
 * Compiled by every arm that publishes at a release.  Under WT that is the
 * payload write-through; under WB only INV publishes at all, and its
 * publish is control-only — the round request.
 *
 * At most ONE publish is in flight per (DB, rank).  Every releaser registers
 * as a version-covered waiter, then either claims the flight (CAS 0->FLYING)
 * or joins it (CAS ->|DIRTY).  A flight ships the cache buffer's CURRENT
 * bytes stamped with the buffer's current version, so the covering ACK wakes
 * every waiter at or below that version and releases that landed mid-flight
 * coalesce into at most one trailing flight — the write-side twin of the RO
 * request-combining window.  The ACK doubles as the credit teacher: it
 * carries the home's next in-place credit {stable-buffer addr, rkey, txid},
 * consumed 1:1 by the next payload flight, so the steady state is one PUT +
 * one commit + one blocked wait, announce-free. */
#if (defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)) &&             \
    (!defined(ARTS_WRITE_POLICY_WB) || defined(ARTS_PROTOCOL_INV))

#define ARTS_PUB_FLYING 1u
#define ARTS_PUB_DIRTY 2u

struct arts_db_pub_waiter_s {
  arts_lf_link_t link; /* FIRST — Treiber membership */
  uint64_t version;    /* wake once a publish >= this version is ACKed */
  sem_t sem;
};

/* Does this rank's publish carry payload?  Write-through owners ship bytes; a
 * home-resident releaser's buffer IS the canonical copy (its publish is the
 * ordering round alone), and the write-back invalidation arm publishes
 * control only. */
/* Does a payload commit leg leaving now owe the home a hand-back of the write
 * right?  Only an arm whose write right migrates can owe one. */
static inline bool pub_leg_takes_grant_return(struct arts_db_cache_s *cache) {
#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)
  return arts_db_grant_return_claim_leg(cache);
#else
  (void)cache;
  return false;
#endif
}

static bool pub_flight_carries_payload(struct arts_db_cache_s *cache) {
#if defined(ARTS_PROTOCOL_INV) && defined(ARTS_WRITE_POLICY_WB)
  (void)cache;
  return false;
#else
  return arts_guid_get_rank(cache->db_guid) != arts_global_rank_id;
#endif
}

/* Drive ONE publish flight from the cache's current buffer state.  The caller
 * holds the FLYING claim.  may_block separates the two calling contexts: a
 * claiming releaser (worker thread — may run the credit-less announce/CTS
 * leg, which blocks) vs a completing ACK (progress thread — the ACK that
 * launched it just refilled the credit, so needing to block there means the
 * refill chain is broken: fatal outside a shutdown, the relaunch dropped at
 * one). */
static struct arts_db_pub_waiter_s *
pub_flight_drive(struct arts_db_cache_s *cache, bool may_block,
                 uint64_t fallback_version) {
  INCREMENT_NUM_PUB_FLIGHT_BY(1);
  arts_shared_ptr_t buf_h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *buf =
      (struct arts_db_buffer_s *)arts_shared_get(buf_h);
  /* The stamp is the buffer's CURRENT version (covers every registered
   * waiter); a buffer-less cache (a data-less DB, or a control-only release
   * before any payload existed) flies under the caller's version instead —
   * its waiters all registered at that same version source. */
  uint64_t version =
      (buf != NULL) ? atomic_load_explicit(&buf->version, memory_order_acquire)
                    : fallback_version;
  uint64_t db_size =
      atomic_load_explicit(&cache->db_size, memory_order_relaxed);
  unsigned int home_rank = arts_guid_get_rank(cache->db_guid);
  /* A claiming releaser gates on ITS OWN flight, not merely on a covering
   * ACK: a claim won after the claimer's registered waiter was already
   * covered (the claim-after-covered race) would otherwise launch a flight
   * nobody waits for, and the release's 0-edge — the ownership ship — could
   * overtake the still-flying PUT, letting the next owner's publish
   * interleave with this one.  The gate node is a waiter at the flight's own
   * stamp, so exactly this flight's ACK releases it.  A trailing relaunch
   * needs no gate: its uncovered waiters are blocked releasers, so the 0-edge
   * cannot fire until its ACK lands. */
  struct arts_db_pub_waiter_s *gate = NULL;
  if (may_block) {
    gate = (struct arts_db_pub_waiter_s *)arts_malloc(sizeof(*gate));
    gate->version = version;
    sem_init(&gate->sem, 0, 0);
    arts_lf_stack_push(&cache->pub_waiters, &gate->link);
  }
  if (!pub_flight_carries_payload(cache) || buf == NULL) {
    if (buf == NULL && pub_flight_carries_payload(cache)) {
      /* Unreachable: a flight only ever leaves under a hold this rank took,
       * and a hold on a sized block implies its storage. */
      ARTS_ERROR("coherence: payload publish flight with no local buffer");
    }
    arts_db_buf_release(&buf_h);
    arts_send_db_publish(home_rank, cache->db_guid, version, /*cv=*/0,
                         /*data_size=*/0, /*rdzv_txid=*/0, /*rdzv_cookie=*/0,
                         /*return_grant=*/false);
    return gate;
  }
  /* Payload flight: consume the credit.  The acquire exchange pairs with the
   * refill's release-store; addr/rkey are stable across refills (one DB has
   * one stable buffer), so reading them after the exchange is safe.  The
   * strong buffer ref transfers to the PUT's local completion — the source-
   * lifetime gate outlives even a shutdown-escaped waiter. */
  uint64_t txid =
      atomic_exchange_explicit(&cache->home_pub_txid, 0u, memory_order_acquire);
  if (txid != 0) {
    arts_net_put_payload(
        (int)home_rank,
        atomic_load_explicit(&cache->home_pub_addr, memory_order_relaxed),
        atomic_load_explicit(&cache->home_pub_rkey, memory_order_relaxed), txid,
        buf->data, db_size, arts_db_buf_ref_release_cb, (void *)buf_h);
    arts_send_db_publish(home_rank, cache->db_guid, version, /*cv=*/0, db_size,
                         txid, /*rdzv_cookie=*/0,
                         pub_leg_takes_grant_return(cache));
    return gate;
  }
  if (!may_block) {
    if (atomic_load_explicit(&arts_node_info.shutdown_state,
                             memory_order_acquire) != 0) {
      /* A shutdown escape let the releases return and the destroy that followed
       * them stripped this ACK's credit.  Every uncovered owner has already
       * escaped (its release returned only through the escape, which read the
       * same shutdown state); the flight is left FLYING and nothing is posted,
       * so each cut wait stays counted and the shutdown stays non-quiescent. */
      arts_db_buf_release(&buf_h);
      return NULL;
    }
    /* Outside a shutdown the ACK this relaunch runs in refilled the credit,
     * and only the flight's owner takes one: a home answers a payload flight
     * without one only for a block destroyed while that release was in
     * flight, which the destroy contract forbids. */
    ARTS_ERROR("coherence: guid %lu: a publish relaunch found no credit",
               (unsigned long)cache->db_guid);
  }
  INCREMENT_NUM_PUB_CTS_FALLBACK_BY(1);
  /* Credit-less first flight: announce -> blocked CTS wait -> PUT -> commit.
   * The rendezvous lives on the HEAP: the shutdown escape can abandon the
   * wait while the CTS reply is still in flight, and that reply writes the
   * landing fields through the echoed cv before posting.  A heap block
   * deliberately LEAKED on the escape keeps that late write inside live
   * memory (a bounded, teardown-only leak); a popped stack frame would be
   * corrupted. */
  struct arts_db_pub_rendezvous_s *wr =
      (struct arts_db_pub_rendezvous_s *)arts_malloc(sizeof(*wr));
  sem_init(&wr->sem, 0, 0);
  wr->landing = (struct arts_rdzv_landing_s){0, 0, 0, 0};
  arts_send_db_publish(home_rank, cache->db_guid, version,
                       (uint64_t)(uintptr_t)wr, db_size, /*rdzv_txid=*/0,
                       /*rdzv_cookie=*/0, /*return_grant=*/false);
  if (!arts_db_await_ack(&wr->sem)) {
    /* Shutdown escape: the flight is abandoned with the runtime.  A CTS
     * reply may still be writing the landing and posting through wr, so wr
     * is leaked and nothing in it is read.  The buffer ref is ours to drop —
     * nothing read the bytes. */
    arts_db_buf_release(&buf_h);
    return gate;
  }
  /* The post orders the reply's landing stores before these reads, and a
   * CTS reply always carries a landing. */
  assert(wr->landing.txid != 0);
  arts_net_put_payload((int)home_rank, wr->landing.addr, wr->landing.key,
                       wr->landing.txid, buf->data, db_size,
                       arts_db_buf_ref_release_cb, (void *)buf_h);
  arts_send_db_publish(home_rank, cache->db_guid, version, /*cv=*/0, db_size,
                       wr->landing.txid, wr->landing.cookie,
                       pub_leg_takes_grant_return(cache));
  /* The commit carries cv 0 — the final ACK completes the FLIGHT, not this
   * rendezvous.  The CTS was the only writer through wr, so it dies here. */
  sem_destroy(&wr->sem);
  arts_free(wr);
  return gate;
}

void arts_db_publish_sync(struct arts_db_cache_s *cache, uint64_t version) {
  /* The waiter node lives on the HEAP for the same reason the CTS rendezvous
   * does: a shutdown-escaped waiter leaks its node, and a late completion
   * drain may still post into it — posting into leaked live memory is safe,
   * freeing under the poster is not. */
  struct arts_db_pub_waiter_s *w =
      (struct arts_db_pub_waiter_s *)arts_malloc(sizeof(*w));
  w->version = version;
  sem_init(&w->sem, 0, 0);
  arts_lf_stack_push(&cache->pub_waiters, &w->link);
  arts_sched_fuzz_point(); /* widen the push<->flight-claim window */
  struct arts_db_pub_waiter_s *gate = NULL;
  for (;;) {
    unsigned int f =
        atomic_load_explicit(&cache->pub_flight, memory_order_relaxed);
    if (f == 0u) {
      unsigned int expected = 0u;
      if (atomic_compare_exchange_strong_explicit(
              &cache->pub_flight, &expected, ARTS_PUB_FLYING,
              memory_order_acq_rel, memory_order_relaxed)) {
        gate = pub_flight_drive(cache, /*may_block=*/true, version);
        break;
      }
    } else if (atomic_compare_exchange_strong_explicit(
                   &cache->pub_flight, &f, f | ARTS_PUB_DIRTY,
                   memory_order_acq_rel, memory_order_relaxed)) {
      INCREMENT_NUM_PUB_FLIGHT_JOIN_BY(1);
      break;
    }
  }
  /* A posted waiter was popped before its post, so nothing posts it again: it
   * is ours to free whether or not the shutdown has begun; only a wait the
   * escape cut short leaves its node to a late poster. */
  if (!arts_db_await_ack(&w->sem)) {
    /* Shutdown escape: a late drain may still post — leak w.  A wait the
     * escape cut short is already recorded by the await.  The gate's flight
     * covers w, so the gate may already be posted (a drain posts it before w,
     * or the ACK landed after the wait timed out): then it is off the stack
     * and nothing was left unawaited; otherwise its wait is recorded here and
     * it stays parked. */
    if (gate != NULL) {
      if (sem_trywait(&gate->sem) == 0) {
        sem_destroy(&gate->sem);
        arts_free(gate);
      } else {
        atomic_fetch_add_explicit(&arts_shutdown_abandon.waits, 1u,
                                  memory_order_relaxed);
      }
    }
    return;
  }
  sem_destroy(&w->sem);
  arts_free(w);
  if (gate != NULL) {
    if (!arts_db_await_ack(&gate->sem)) {
      return; /* shutdown escape — leak the gate (the escape is recorded) */
    }
    sem_destroy(&gate->sem);
    arts_free(gate);
  }
}

/* Cat-C pure body (PUBLISH_ACK), shared by every publishing arm.  cv != 0 is
 * the pointer-identity sem-post of a grant return sent on its own (PURGE;
 * cache-independent, posted on both HIT and MISS).  cv == 0 is the
 * completion of a publish flight, control-only ones included: record the
 * refilled credit, wake every waiter the ACKed version covers, and either
 * relaunch (uncovered waiters remain — their releases landed after the
 * flight's version stamp) or land the flight.  A DIRTY bit set
 * after the drain forces a re-examination before landing, so a waiter
 * registered mid-completion is never stranded. */
void arts_handler_db_publish_ack(void *item_v, void *args_v) {
  struct arts_db_publish_ack_args_s *a =
      (struct arts_db_publish_ack_args_s *)args_v;
  if (a->cv != 0) {
    sem_post((sem_t *)(uintptr_t)a->cv);
    return;
  }
  struct arts_db_s *db = (struct arts_db_s *)item_v;
  if (db == NULL) {
    /* The block was destroyed with this flight in the air: a program that
     * destroyed it before the release returned, or a shutdown whose escape
     * let the release return first.  Nothing is woken; a releaser still
     * waiting leaves through the shutdown escape. */
    return;
  }
  struct arts_db_cache_s *cache = &db->cache;
  if (a->credit_txid != 0) {
    /* Relaxed stores: two teachers (ACK refill, CREATE_RETURN) may overlap,
     * but every teacher writes the same {addr, rkey} — one DB has ONE stable
     * buffer — so the only ordering that matters is the txid release-store
     * publishing the triple to the consuming exchange. */
    atomic_store_explicit(&cache->home_pub_addr, a->credit_addr,
                          memory_order_relaxed);
    atomic_store_explicit(&cache->home_pub_rkey, a->credit_rkey,
                          memory_order_relaxed);
    atomic_store_explicit(&cache->home_pub_txid, a->credit_txid,
                          memory_order_release);
  }
  for (;;) {
    /* Waiters parked by the previous completion first, then everything that
     * queued since.  An acquire load and a release store of NULL, not an
     * exchange: under the FLYING claim exactly one completion runs at a time,
     * and only a completion touches the field (the teardown walk
     * reads it relaxed, after every thread has joined). */
    arts_lf_link_t *chain =
        atomic_load_explicit(&cache->pub_parked, memory_order_acquire);
    atomic_store_explicit(&cache->pub_parked, NULL, memory_order_release);
    arts_lf_link_t *fresh = arts_lf_stack_drain(&cache->pub_waiters);
    if (chain == NULL) {
      chain = fresh;
    } else if (fresh != NULL) {
      arts_lf_link_t *tail = chain;
      for (arts_lf_link_t *n; (n = (arts_lf_link_t *)atomic_load_explicit(
                                   &tail->next, memory_order_relaxed)) != NULL;
           tail = n) {
      }
      atomic_store_explicit(&tail->next, fresh, memory_order_relaxed);
    }
    arts_lf_link_t *uncovered = NULL;
    while (chain != NULL) {
      arts_lf_link_t *next =
          (arts_lf_link_t *)atomic_load_explicit(&chain->next,
                                                 memory_order_relaxed);
      struct arts_db_pub_waiter_s *w = (struct arts_db_pub_waiter_s *)chain;
      if (w->version <= a->version) {
        sem_post(&w->sem);
      } else {
        atomic_store_explicit(&chain->next, uncovered, memory_order_relaxed);
        uncovered = chain;
      }
      chain = next;
    }
    if (uncovered != NULL) {
      /* Releases landed after the flight's stamp: keep the claim (dropping
       * any DIRTY — this drain subsumes it) and fly again; the buffer's
       * current version now covers every parked waiter.  An exchange, not a
       * store: a join it overwrites is read by it, so that joiner's node is
       * seen by the next drain (arts_db_publish_sync pushes, then joins). */
      atomic_store_explicit(&cache->pub_parked, uncovered,
                            memory_order_release);
      (void)atomic_exchange_explicit(&cache->pub_flight, ARTS_PUB_FLYING,
                                     memory_order_acq_rel);
      INCREMENT_NUM_PUB_FLIGHT_TRAILING_BY(1);
      (void)pub_flight_drive(cache, /*may_block=*/false, a->version);
      return;
    }
    unsigned int expected = ARTS_PUB_FLYING;
    if (atomic_compare_exchange_strong_explicit(&cache->pub_flight, &expected,
                                                0u, memory_order_acq_rel,
                                                memory_order_acquire)) {
      return; /* flight landed */
    }
    /* DIRTY raced in after the drain: consume it and re-examine.  The word
     * stays in {FLYING, FLYING|DIRTY} for the whole completion (only a
     * completion clears FLYING, and only one runs).  An exchange, not a
     * store: a join it overwrites is read by it, so the joiner's push
     * happens before the loop's next drain. */
    (void)atomic_exchange_explicit(&cache->pub_flight, ARTS_PUB_FLYING,
                                   memory_order_acq_rel);
  }
}
#endif /* publishing arms */

#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)
void arts_db_release_ro(struct arts_db_cache_s *cache) {
  /* RO release is a no-op for VAL: the EDT's buf ref is dropped
   * by release_one_dep's DIST branch via release_buf (matching the
   * acquire_buf in the slot resolver / acquire_local).
   * EXCL defines its own arts_db_release_ro in coherence/excl/purge.c and
   * coherence/excl/retain.c. */
  (void)cache;
}
#endif /* arms whose RO release drops nothing but the EDT's own ref */

#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)
/* ===== The declared-arrival rule (home side) ==========================
 *
 * A request whose waiters a write turn of its own rank serves (a covered
 * request) may reach the home after the program legally destroyed the block.
 * The home learns of each one before the turn that covers it can end: a write
 * request declares the read request its rank has out (by that request's
 * parity), and a create's announce declares the requests the first-touch
 * cache it claimed had sent (marked PRECLAIM).  The cover gate counts what is
 * still coming; the teardown waits for it to reach zero.
 *
 *   cover_gate = [ td:1 (31) | rwo:1 (30) | roo:1 (29) | owed:29 ]
 *
 * A read request and its declaration each put the request's parity in the
 * requester's bit of ro_parity with one RMW; whichever comes second finds the
 * bit already there.  A declaration counts before its parity step, so the
 * arrival that reads that step finds the count.  A declared read request is
 * never processed as a read: the home settles the count and answers with a
 * dismiss (no data, no read turn), and the requester's cohort rides the
 * write turn its rank holds or has requested.  The claiming creator's
 * pre-claim requests (rwo, roo) are discarded unanswered: the claim closed
 * both axes, and the announce seeded the parity bit as if the read request
 * had arrived. */
#define COVER_TD 0x80000000u
#define COVER_RWO 0x40000000u
#define COVER_ROO 0x20000000u
#define COVER_OWED_MASK 0x1fffffffu

enum { COVER_OWE, COVER_ARRIVE, COVER_RWO_ARRIVE, COVER_ROO_ARRIVE, COVER_MARK };

/* One RMW of the gate.  True when this step leaves a marked gate with nothing
 * owed (the caller completes the teardown); *marked reports whether the gate
 * was marked when it committed. */
static bool cover_gate_step(struct arts_db_s *db, int op, bool *marked) {
  uint32_t cur = atomic_load_explicit(&db->cover_gate, memory_order_acquire);
  uint32_t next;
  bool edge;
  do {
    uint32_t owed = cur & COVER_OWED_MASK;
    uint32_t flag = (op == COVER_RWO_ARRIVE)   ? COVER_RWO
                    : (op == COVER_ROO_ARRIVE) ? COVER_ROO
                                               : 0u;
    next = cur;
    edge = false;
    if (op == COVER_OWE) {
      if (owed == COVER_OWED_MASK) {
        ARTS_ERROR("coherence: guid %lu owes more requests than its gate holds "
                   "(gate=%x)",
                   (unsigned long)db->cache.db_guid, cur);
      }
      next = cur + 1u;
    } else if (op == COVER_MARK) {
      if ((cur & COVER_TD) != 0u) {
        *marked = true;
        return false; /* a duplicate destroy */
      }
      next = cur | COVER_TD;
      edge = (owed == 0u);
    } else {
      if (owed == 0u || (flag != 0u && (cur & flag) == 0u)) {
        ARTS_ERROR("coherence: guid %lu settles a request it was never owed "
                   "(gate=%x)",
                   (unsigned long)db->cache.db_guid, cur);
      }
      next = (cur - 1u) & ~flag;
      edge = (cur & COVER_TD) != 0u && owed == 1u;
    }
  } while (!atomic_compare_exchange_weak_explicit(
      &db->cover_gate, &cur, next, memory_order_acq_rel, memory_order_acquire));
  *marked = (cur & COVER_TD) != 0u;
  return edge;
}

void arts_db_home_seed_owed(struct arts_db_s *db, unsigned int creator,
                            uint8_t owed) {
  if (owed == 0u) {
    return;
  }
  db->claim_rank = creator;
  uint32_t n = 0u, flags = 0u;
  if (owed & ARTS_CREATE_OWED_RO) {
    /* As if the pre-claim read request had arrived: the creator's next read
     * request (the opposite parity) pairs as any. */
    (void)arts_rank_bitset_put(&db->ro_parity, creator,
                               (owed & ARTS_CREATE_OWED_RO_PAR) != 0u);
    flags |= COVER_ROO;
    n++;
  }
  if (owed & ARTS_CREATE_OWED_RW) {
    flags |= COVER_RWO;
    n++;
  }
  atomic_store_explicit(&db->cover_gate, flags | n, memory_order_relaxed);
}

int arts_db_cover_request(struct arts_db_s *db, unsigned int requester,
                          bool write, uint32_t cover, bool *teardown) {
  bool marked = false;
  *teardown = false;
  if ((cover & ARTS_COVER_PRECLAIM) != 0u && requester == db->claim_rank) {
    /* The claiming creator's pre-claim request: discarded unanswered, its
     * parity not applied (the announce seeded it). */
    *teardown =
        cover_gate_step(db, write ? COVER_RWO_ARRIVE : COVER_ROO_ARRIVE, &marked);
    return ARTS_COVER_DROP;
  }
  if (write && (cover & ARTS_COVER_RO_OPEN) != 0u) {
    bool par = (cover & ARTS_COVER_RO_PAR) != 0u;
    (void)cover_gate_step(db, COVER_OWE, &marked);
    if (arts_rank_bitset_put(&db->ro_parity, requester, par) == par) {
      /* The undo is a settling step like any: if it is the one that leaves
       * a marked gate with nothing owed, it owns the teardown, and the
       * request reached a destroyed block (outside the contract): dropped. */
      if (cover_gate_step(db, COVER_ARRIVE, &marked)) {
        *teardown = true;
        return ARTS_COVER_DROP;
      }
    }
  }
  if (!write) {
    bool par = (cover & ARTS_COVER_PAR) != 0u;
    if (arts_rank_bitset_put(&db->ro_parity, requester, par) == par) {
      /* Declared first: a write turn of this rank covers the request. */
      *teardown = cover_gate_step(db, COVER_ARRIVE, &marked);
      return ARTS_COVER_DISMISS;
    }
  }
  return ARTS_COVER_PROCESS;
}

void arts_db_cover_destroy(struct arts_db_s *db) {
  bool marked = false;
  if (cover_gate_step(db, COVER_MARK, &marked)) {
    arts_db_home_destroy_now(db);
  }
}

bool arts_db_cover_marked(struct arts_db_s *db) {
  /* Acquire: a refusal to mint tells the releasing rank the block was
   * destroyed, and that rank's relaunch gate (pub_flight_drive) relies on the
   * destroy -- happens-after its escapes -- being happens-before this ACK.
   * The MARK is the head of the cover gate's release sequence (every later
   * write is an acq_rel RMW), so an acquire load that reads the mark or any
   * later value synchronizes with it. */
  return (atomic_load_explicit(&db->cover_gate, memory_order_acquire) &
          COVER_TD) != 0u;
}

void arts_db_cover_home_init(struct arts_db_s *db, unsigned int nranks) {
  arts_rank_bitset_init(&db->ro_parity, nranks);
  atomic_store_explicit(&db->cover_gate, 0u, memory_order_relaxed);
  db->claim_rank = (unsigned int)-1;
}

void arts_db_cover_home_teardown(struct arts_db_s *db) {
  arts_rank_bitset_destroy(&db->ro_parity);
}
#endif /* the declared-arrival absorb */

/* ================================================================== */
/* ===== Destroy lifecycle ========================================== */
/* ================================================================== */

/* ===== arts_db_destroy_remote public API ============================= */

/* Public destroy: send MSG_DB_DESTROY to the home (a home on this rank runs
 * it through the OoO engine inline, as the wire dispatcher does).  The caller
 * has released its own hold, and the program orders this destroy after the
 * release of every other hold on the block. */
void arts_db_destroy_remote(arts_guid_t db_guid) {
  unsigned int home_rank = arts_guid_get_rank(db_guid);
  arts_send_db_destroy(home_rank, db_guid);
}

/* ===== cache_s destructor (chained from arts_db_free) =============
 *
 * The full destructor arts_db_cache_destructor is protocol-specific — it
 * sequences the protocol's own field teardown between these two shared steps —
 * and lives in each arm's own TU.  The agnostic steps split into pre
 * (the buffer-NULL that must run first) and post (snapshot drain + home
 * teardown), so the per-arm wrapper runs pre, its own teardown, then post.
 * The descriptor's storage is released after the wrapper returns
 * (arts_db_buf_storage_release): freed at once, or, where the arm keeps a
 * per-DB free-list, once no buffer drawn from it is still out. */

#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV) ||               \
    defined(ARTS_PROTOCOL_EXCL)
/* Release the cache-hold on the buffer (store NULL into the shared slot).  If
 * no acquirer holds a ref the cb deleter ends the buffer now; otherwise it
 * survives until the last in-flight acquirer releases.  Runs first, ahead of
 * the arm's own field teardown. */
void arts_db_cache_common_destroy_pre(struct arts_db_cache_s *cache) {
  if (cache == NULL) {
    return;
  }
  arts_shared_slot_store(&cache->buffer, NULL);
  /* The slot is empty again, so the "has this block storage" byte goes back
   * with it: the two are one fact, and leaving a stale 0 would tell a use of
   * a resurrected cache that its payload is already there. */
  atomic_store_explicit(&cache->payload_pending, (uint8_t)1,
                        memory_order_release);
  /* Where the arm keeps a free-list it is not drained here: the storage
   * release that follows the destructor drains whatever is parked then, and
   * a buffer still out (a transfer's completion holding it) parks later,
   * the last of them freeing the storage. */
}
#endif /* arms sharing the common cache shape */

/* Debug-only terminal-quiescence check.  Runs once, after every runtime
 * thread has joined and before teardown frees the caches.  It reports two
 * things: states the protocols can never reach, and waiters or holds left
 * behind by a shutdown that was quiescent -- a survivor there is a lost wake
 * that the run's own success criteria may have masked (a reader that never
 * ran, a release that never completed).  Work a shutdown abandoned is the one
 * legal source of such a residue, so after a shutdown that left work undone
 * residues are not reported; the contributors say what was left undone.
 * Violations print a QUIESCENCE-DEBUG marker; tests whose shutdown is
 * quiescent turn that marker into a failure.
 *
 * The walk exists only to produce diagnostic messages, so it is gated on
 * the log level that compiles those messages: below DEBUG the whole check
 * is compiled out — its cost (a full route-table scan at teardown) never
 * lands in a measurement build. */
#if ARTS_LOG_LEVEL >= 3
/* The rank's route tables in one index space: one per thread, then the
 * remote shards. */
static arts_route_table_t *quiescence_table(unsigned int t) {
  return t < (unsigned int)arts_guid_table_count
             ? arts_node_info.route_table[t]
             : arts_node_info
                   .remote_route_table[t - (unsigned int)arts_guid_table_count];
}
#endif

#if ARTS_LOG_LEVEL >= 3 && defined(ARTS_PROTOCOL_EXCL)
/* One axis row of the EXCL cache word: report `bad` always, or only after a
 * quiescent shutdown when `quiet_only`. */
static unsigned int excl_axis_row(const struct arts_db_cache_s *c, uint64_t cw,
                                  const char *bad, bool quiet_only,
                                  bool quiescent) {
  if (bad == NULL || (quiet_only && !quiescent)) {
    return 0u;
  }
  ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu %s (word=%llx)",
             (unsigned long)c->db_guid, bad, (unsigned long long)cw);
  return 1u;
}

/* The cache word judged per axis, on that axis's own count.  Write axis:
 * IDLE with writers counted is always lost (nothing covers a writer); at REQ
 * with none it is a request no writer opened; non-idle with writers is an
 * unfinished turn (quiescent only).  Read axis: at REQ with no reader is a
 * covered read request whose answer was in flight (legal); IDLE with readers
 * counted is legal only under the write axis's cover (quiescent only), else
 * lost; non-idle with readers is unfinished (quiescent only).  PURGE: a grant
 * with nothing counted under it is a lost zero edge (the edge returns the
 * grant in the same CAS).  RETAIN: a resting grant with nothing counted is the
 * policy's normal state, and a resident owner also covers its readers. */
static unsigned int excl_cache_word_rows(const struct arts_db_cache_s *c,
                                         uint64_t cw, bool quiescent) {
  uint32_t rws = CACHE_RW_ST(cw), ros = CACHE_RO_ST(cw);
  uint32_t wcn = CACHE_RW_CNT(cw), rcn = CACHE_RO_CNT(cw);
#ifdef ARTS_RELEASE_RETAIN
  bool read_cover =
      rws == CACHE_ST_REQ || rws == CACHE_ST_GRANT || CACHE_OWNER(cw) == 1u;
  bool purge = false;
#else
  bool read_cover = rws == CACHE_ST_REQ || rws == CACHE_ST_GRANT;
  bool purge = true;
#endif
  unsigned int v = 0u;
  if (rws == CACHE_ST_IDLE && wcn != 0u) {
    v += excl_axis_row(c, cw, "rw axis rests idle with writers counted", false,
                       quiescent);
  } else if (rws == CACHE_ST_REQ && wcn == 0u) {
    v += excl_axis_row(c, cw,
                       "rw axis rests at its request with no writer counted",
                       false, quiescent);
  } else if (purge && rws == CACHE_ST_GRANT && wcn == 0u && rcn == 0u) {
    v += excl_axis_row(c, cw, "rw axis rests granted with nothing counted",
                       false, quiescent);
  } else if (rws != CACHE_ST_IDLE && wcn != 0u) {
    v += excl_axis_row(c, cw,
                       "rw axis rests non-idle with writers still counted",
                       true, quiescent);
  }
  if (ros == CACHE_ST_IDLE && rcn != 0u) {
    v += excl_axis_row(c, cw,
                       read_cover ? "ro readers rest under the write axis"
                                  : "ro axis rests idle with readers counted",
                       read_cover, quiescent);
  } else if (purge && ros == CACHE_ST_GRANT && rcn == 0u) {
    v += excl_axis_row(c, cw, "ro axis rests granted with no reader counted",
                       false, quiescent);
  } else if (ros != CACHE_ST_IDLE && ros != CACHE_ST_REQ && rcn != 0u) {
    v += excl_axis_row(c, cw,
                       "ro axis rests non-idle with readers still counted",
                       true, quiescent);
  } else if (ros == CACHE_ST_REQ && rcn != 0u) {
    v += excl_axis_row(c, cw, "ro readers rest on an open read request", true,
                       quiescent);
  }
  return v;
}
#endif

void arts_db_debug_quiescence_check(void) {
#if ARTS_LOG_LEVEL >= 3
  /* Every read below assumes the runtime's threads are gone: the joins order
   * every write before it, so each load is relaxed.  A thread the shutdown
   * gave up joining may still be mid transition, so the walk cannot judge
   * and says so instead. */
  unsigned int unjoined = atomic_load_explicit(
      &arts_shutdown_abandon.unjoined_threads, memory_order_relaxed);
  if (unjoined != 0) {
    ARTS_DEBUG("quiescence check skipped: %u thread(s) never joined",
               unjoined);
    return;
  }
#ifdef ARTS_DEBUG_LANDING_BALANCE
  /* Informational: a reply cut by the shutdown (no delivery barrier) leaves
   * its landing out; a test waits for the count to reach zero instead. */
  if (arts_db_landings_outstanding() != 0) {
    ARTS_DEBUG("LANDING-INFO: %lld landing(s) outstanding at shutdown",
               (long long)arts_db_landings_outstanding());
  }
#endif
  unsigned int viol = 0;
  /* Homes a destroy marked that still wait for a declared request: legal at
   * any shutdown, printed so a campaign can watch it. */
  unsigned int marked_owed = 0;
  const unsigned int nt =
      (unsigned int)arts_guid_table_count + ARTS_REMOTE_ROUTE_SHARDS;
  /* Whether this shutdown was quiescent, named by contributor so a log's
   * reader knows why a residue went unreported.  Every one is pending work
   * the shutdown left undone: runnable EDTs dropped from a deque, release
   * waits cut short, EDTs admitted (every dependence satisfied, acquisition
   * begun) that never finished -- these cover an EDT parked mid acquisition,
   * which sits in no queue a discard site sees, and overlap the first -- and
   * messages parked on a route slot's OoO list: a create waiting for a
   * destroy, or any other message waiting for an install, that never came.
   * A parked message is residue whatever parked it -- a late message for a
   * destroyed GUID included -- so a program whose shutdown leaves one was not
   * quiescent.  Each is read after every runtime thread has joined, so no
   * poster can add to what is counted, and before anything frees it.  A
   * message from another rank still undelivered is not one: a release in
   * flight to a home strands no waiter, and a grant still owed to a requester
   * leaves that requester's EDT admitted and unfinished. */
  unsigned int ab_queued = atomic_load_explicit(
      &arts_shutdown_abandon.queued_edts, memory_order_relaxed);
  unsigned int ab_waits = atomic_load_explicit(&arts_shutdown_abandon.waits,
                                               memory_order_relaxed);
  unsigned int ab_ooo = 0;
  for (unsigned int t = 0; t < nt; t++) {
    arts_route_table_t *table = quiescence_table(t);
    if (table != NULL) {
      ab_ooo += arts_route_table_parked_count(table);
    }
  }
  unsigned int ab_admitted = 0;
  for (unsigned int t = 0; t < nt; t++) {
    arts_route_table_t *table = quiescence_table(t);
    if (table == NULL) {
      continue;
    }
    arts_route_table_iterator_t iter;
    arts_reset_route_table_iterator(&iter, table);
    for (arts_route_item_t *item = arts_route_table_iterate(&iter);
         item != NULL; item = arts_route_table_iterate(&iter)) {
      if (ARTS_GUID_GET_TYPE(atomic_load_explicit(
              &item->key, memory_order_relaxed)) != ARTS_GUID_EDT) {
        continue;
      }
      arts_shared_ptr_t h = arts_shared_slot_load(&item->value);
      if (arts_edt_admitted(h)) {
        ab_admitted++;
      }
      arts_shared_release(&h);
    }
  }
  bool quiescent = (ab_queued | ab_waits | ab_ooo | ab_admitted) == 0u;
  for (unsigned int t = 0; t < nt; t++) {
    arts_route_table_t *table = quiescence_table(t);
    if (table == NULL) {
      continue;
    }
    arts_route_table_iterator_t iter;
    arts_reset_route_table_iterator(&iter, table);
    for (arts_route_item_t *item = arts_route_table_iterate(&iter);
         item != NULL; item = arts_route_table_iterate(&iter)) {
      if (ARTS_GUID_GET_TYPE(atomic_load_explicit(
              &item->key, memory_order_relaxed)) != ARTS_GUID_DB) {
        continue;
      }
      arts_shared_ptr_t h = arts_shared_slot_load(&item->value);
      struct arts_db_s *db = (struct arts_db_s *)(h ? arts_shared_get(h)
                                                    : NULL);
      if (db != NULL && db->db_type == ARTS_DB) {
#ifdef ARTS_PROTOCOL_FLUSH
        /* FLUSH keeps no wait structure on a descriptor — a blocked release
         * owns a heap semaphore that the home's ACK posts — so a walk has
         * nothing to report. */
#else
        struct arts_db_cache_s *c = &db->cache;
#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)
        /* What each rule can see.  A waiter or a releaser that is still
         * blocked belongs to an EDT that has not finished; that EDT makes its
         * own rank's shutdown non-quiescent, and in a program whose shutdown
         * waits for it a lost wake shows as the run never ending, not as a
         * residue here.  So the rules gated on a quiescent shutdown catch
         * what completed work left behind: a node no drain removed, a hold
         * whose EDT finished without releasing it.  The ungated rules are
         * states no interleaving and no message still in flight can leave,
         * and they hold after any shutdown. */
#endif
#if (defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)) &&             \
    (!defined(ARTS_WRITE_POLICY_WB) || defined(ARTS_PROTOCOL_INV))
        if (quiescent && (!arts_lf_stack_empty(&c->pub_waiters) ||
                          atomic_load_explicit(&c->pub_parked,
                                               memory_order_relaxed) != NULL)) {
          ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu has parked publish "
                     "waiters after a quiescent shutdown",
                     (unsigned long)c->db_guid);
          viol++;
        }
#endif
#ifdef ARTS_PROTOCOL_VAL
        {
          uint64_t rq =
              atomic_load_explicit(&c->req_state, memory_order_relaxed);
          /* A window whose request is out with nothing parked on it is a
           * covered request whose answer was in flight: legal.  A parked
           * reader is an unfinished acquire: reported after a quiescent
           * shutdown.  A write axis left OPEN has lost its writer thread; a
           * rider with no request of either kind to serve it, or a window
           * parked behind a write request that is not out, will never be
           * served: always reported. */
          if (quiescent && (VREQ_GROUP(rq) != 0u || VREQ_NEXT(rq) != 0u)) {
            ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu has a parked read after a "
                       "quiescent shutdown (word=%llx)",
                       (unsigned long)c->db_guid, (unsigned long long)rq);
            viol++;
          }
          if (VREQ_RW(rq) == VREQ_RW_OPEN ||
              (VREQ_NEXT(rq) != 0u && !VREQ_RO(rq) &&
               VREQ_RW(rq) == VREQ_RW_IDLE) ||
              (VREQ_RP(rq) && VREQ_RW(rq) == VREQ_RW_IDLE)) {
            ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu request word rests where "
                       "nothing serves it (word=%llx)",
                       (unsigned long)c->db_guid, (unsigned long long)rq);
            viol++;
          }
        }
#endif
#if (defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)) &&              \
    defined(ARTS_WRITE_POLICY_WT)
        if (quiescent) {
          /* Only a read of this rank's own: a deferred serve belongs to an
           * EDT on the requesting rank, whose shutdown judges it. */
          unsigned int local = 0;
          for (arts_lf_link_t *n = atomic_load_explicit(
                   &c->publication_waiters.head, memory_order_relaxed);
               n != NULL;
               n = atomic_load_explicit(&n->next, memory_order_relaxed)) {
            if (((struct arts_db_read_waiter_s *)n)->serve == NULL) {
              local++;
            }
          }
          if (local != 0) {
            ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu has %u publication "
                       "waiter(s) after a quiescent shutdown",
                       (unsigned long)c->db_guid, local);
            viol++;
          }
        }
#endif
#ifdef ARTS_PROTOCOL_EXCL
        if (db->home_initialized) {
          /* The cover gate: marked with nothing owed (and, on PURGE, nothing
           * counted) means the teardown was skipped; a pre-claim request owed
           * with nothing owed is a broken count.  Something owed is legal and
           * only counted. */
#ifdef ARTS_RELEASE_PURGE
          uint64_t ls =
              atomic_load_explicit(&db->lock_state, memory_order_relaxed);
          bool gtd = EXCL_STATE_TEARDOWN(ls) != 0u;
          uint32_t gowed = EXCL_STATE_OWED(ls);
          bool gskip = gtd && gowed == 0u && EXCL_STATE_W(ls) == 0u &&
                       EXCL_STATE_R(ls) == 0u;
          bool grwo =
              (EXCL_STATE_RWO(ls) != 0u || EXCL_STATE_ROO(ls) != 0u) &&
              gowed == 0u;
#else
          uint32_t gw =
              atomic_load_explicit(&db->cover_gate, memory_order_relaxed);
          bool gtd = (gw & 0x80000000u) != 0u;
          uint32_t gowed = gw & 0x1fffffffu;
          bool gskip = gtd && gowed == 0u;
          bool grwo = (gw & 0x60000000u) != 0u && gowed == 0u;
#endif
          if (gskip || grwo) {
            ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu cover gate rests where no "
                       "step leaves it (skipped teardown=%d rwo=%d)",
                       (unsigned long)c->db_guid, (int)gskip, (int)grwo);
            viol++;
          }
          if (gtd && gowed != 0u) {
            marked_owed++;
          }
          /* ro_queued: a set bit names a read request waiting on ro_waiters
           * to be served as a read.  The request sets it before queueing its
           * node, and the drain that pops the node or the same rank's write
           * request takes it, so once every handler has joined a set bit with
           * no node of its rank queued is a lost take; reported after a
           * quiescent shutdown. */
          if (quiescent) {
            /* The cursor's acquire loads order nothing here: every handler
             * has joined. */
            struct arts_rank_bitset_cursor_s qc =
                arts_rank_bitset_cursor(&db->ro_queued);
            unsigned int qr;
            while (arts_rank_bitset_next(&qc, &qr)) {
              bool node = false;
              for (arts_lf_link_t *n = atomic_load_explicit(
                       &db->ro_waiters.head, memory_order_relaxed);
                   n != NULL && !node;
                   n = atomic_load_explicit(&n->next, memory_order_relaxed)) {
                node = ARTS_CONTAINER_OF(n, struct arts_lock_ro_node_s, link)
                           ->rank == qr;
              }
              if (!node) {
                ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu marks rank %u's read "
                           "request queued with none of its nodes on the "
                           "read queue",
                           (unsigned long)c->db_guid, qr);
                viol++;
              }
            }
          }
        }
#endif
#ifdef ARTS_PROTOCOL_INV
        {
          uint64_t cs =
              atomic_load_explicit(&c->cache_state, memory_order_relaxed);
          /* A covered fetch whose reply is in flight (inflight with nobody
           * on the chain) is legal.  A writer thread lost between claim and
           * commit, or readers no fetch, write request or valid copy can
           * reach, are reported always; a parked reader after a quiescent
           * shutdown is an unfinished acquire. */
          if (INV_CACHE_RW(cs) == INV_RW_OPEN ||
              (INV_CACHE_HEAD_RO(cs) != 0u && INV_CACHE_RW(cs) == INV_RW_IDLE &&
               INV_CACHE_INFLIGHT(cs) == 0u &&
               INV_CACHE_RO(cs) != INV_RO_VALID)) {
            ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu reader word rests where "
                       "nothing serves it (word=%llx)",
                       (unsigned long)c->db_guid, (unsigned long long)cs);
            viol++;
          }
          if (quiescent && INV_CACHE_HEAD_RO(cs) != 0u) {
            ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu has a parked read after a "
                       "quiescent shutdown (word=%llx)",
                       (unsigned long)c->db_guid, (unsigned long long)cs);
            viol++;
          }
        }
#endif
#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)
        if (quiescent && !arts_lf_stack_empty(&c->pending_rw)) {
          ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu has parked RW waiters after "
                     "a quiescent shutdown",
                     (unsigned long)c->db_guid);
          viol++;
        }
        {
          /* A write request opens only for a parked writer, and only its
           * grant's take (or a create's claim) empties the stack, which also
           * closes the axis: a write request out with no writer is lost. */
#ifdef ARTS_PROTOCOL_VAL
          bool rw_sent = VREQ_RW(atomic_load_explicit(
                             &c->req_state, memory_order_relaxed)) ==
                         VREQ_RW_SENT;
#else
          bool rw_sent = INV_CACHE_RW(atomic_load_explicit(
                             &c->cache_state, memory_order_relaxed)) ==
                         INV_RW_SENT;
#endif
          if (rw_sent && arts_lf_stack_empty(&c->pending_rw)) {
            ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu has a write request out "
                       "with no writer waiting on it",
                       (unsigned long)c->db_guid);
            viol++;
          }
        }
        if (db->home_initialized) {
          /* The cover gate: marked with nothing owed means the teardown was
           * skipped; the creator's write request owed with nothing owed is a
           * broken count.  Something owed is legal (a declared request may be
           * undelivered at any shutdown) and only counted. */
          uint32_t gate =
              atomic_load_explicit(&db->cover_gate, memory_order_relaxed);
          if (((gate & 0x80000000u) != 0u && (gate & 0x1fffffffu) == 0u) ||
              ((gate & 0x60000000u) != 0u && (gate & 0x1fffffffu) == 0u)) {
            ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu cover gate rests where no "
                       "step leaves it (gate=%x)",
                       (unsigned long)c->db_guid, gate);
            viol++;
          }
          if ((gate & 0x80000000u) != 0u && (gate & 0x1fffffffu) != 0u) {
            marked_owed++;
          }
        }
        /* The home directory.  A remote holder that has not yet handed the
         * right back, the requests queued behind it and, where the home
         * revokes, an open round (the baton) are each explained by a message
         * still in flight when the network stopped, so none is reported by
         * itself.  Home fields only — a cache-only stub does not carry
         * them. */
        if (db->home_initialized) {
          bool baton = atomic_load_explicit(&db->invalidate_in_flight,
                                            memory_order_relaxed) != 0;
#ifdef ARTS_RELEASE_PURGE
          unsigned int hw =
              atomic_load_explicit(&c->writer_count, memory_order_relaxed);
          /* The home names no holder: its own possession is the directory.
           * The baton is held only by a thread inside the serving tail, and
           * every way out of the tail puts it down, so once every thread has
           * joined a held baton has lost its holder and every later entry to
           * the tail is refused. */
          if (baton) {
            ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu home rests with its "
                       "transfer baton held (word=%u)",
                       (unsigned long)c->db_guid, hw);
            viol++;
          }
          /* A hand-back parked in the latch is taken by its own arrival or by
           * the thread inside the tail on its way out, so one still parked
           * with the baton down has no consumer left, and the right it
           * carries never comes home. */
          bool named = ARTS_GRANT_OWN_OF(hw);
          if (!baton && atomic_load_explicit(&db->pending_return_from,
                                             memory_order_relaxed) !=
                            ARTS_GRANT_NO_RETURNER) {
            ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu home has a hand-back "
                       "parked with no tail to take it (word=%u)",
                       (unsigned long)c->db_guid, hw);
            viol++;
          }
#else
          bool named =
              atomic_load_explicit(&db->rw_holder, memory_order_relaxed) ==
              arts_global_rank_id;
          unsigned int hw =
              atomic_load_explicit(&c->writer_count, memory_order_relaxed);
          /* Directory and word must agree outside a round.  Inside one the
           * home has shipped the right and names the new holder only at its
           * confirmation, so the two legitimately disagree until the baton
           * is put down; with no round open, naming this rank as the holder
           * while the word possesses nothing is a lost hand-over, and every
           * later requester queues behind a server with nothing to serve. */
          if (!baton && named && !ARTS_GRANT_OWN_OF(hw)) {
            ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu home is named the holder "
                       "but its word holds nothing (word=%u)",
                       (unsigned long)c->db_guid, hw);
            viol++;
          }
#endif
          /* A request is served by its own arrival or by whatever next puts
           * the right in reach of the home's server — the previous round's
           * close where the home revokes, the accepted hand-back where grants
           * come back unasked — and the home serves synchronously when it
           * holds the right idle.  So a request queued with the baton down
           * behind a home that holds the right with no hold under it is one
           * nothing will ever serve: stranded, whatever else the shutdown
           * abandoned. */
          if (!baton && named && ARTS_GRANT_OWN_OF(hw) &&
              ARTS_GRANT_COUNT_OF(hw) == 0u &&
              !arts_home_grantreq_queue_empty(&db->pending_rw)) {
            ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu has a request queued "
                       "behind an idle home with no round open (word=%u)",
                       (unsigned long)c->db_guid, hw);
            viol++;
          }
        }
        {
          unsigned int w =
              atomic_load_explicit(&c->writer_count, memory_order_relaxed);
          /* Every hold has a named releaser, so after a quiescent shutdown
           * nothing may rest holding one. */
          if (quiescent && ARTS_GRANT_COUNT_OF(w) != 0u) {
            ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu rests with %u unreleased "
                       "hold(s) after a quiescent shutdown (word=%u)",
                       (unsigned long)c->db_guid, ARTS_GRANT_COUNT_OF(w), w);
            viol++;
          }
#ifndef ARTS_WRITE_POLICY_WB
          /* Without the unconfirmed-install marker nothing sets the word's
           * sign bit, so a negative word is a count that underflowed: a
           * release with no matching acquire.  (Under write-back the marker
           * makes a negative word an install awaiting its confirmation.) */
          if ((int)w < 0) {
            ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu rests with an "
                       "underflowed hold count (word=%u)",
                       (unsigned long)c->db_guid, w);
            viol++;
          }
#endif
#ifdef ARTS_RELEASE_PURGE
          /* Where the right goes back unasked, the edge that drops a
           * non-home holder's last hold returns the right in the same step,
           * so possession with no hold under it and no return owed is a
           * state that edge cannot leave. */
          if (arts_guid_get_rank(c->db_guid) != arts_global_rank_id &&
              arts_global_rank_count > 1 && w == ARTS_GRANT_OWN &&
              atomic_load_explicit(&c->pending_grant_return,
                                   memory_order_relaxed) == 0u) {
            ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu rests holding the write "
                       "right with no hold under it and no return owed",
                       (unsigned long)c->db_guid);
            viol++;
          }
#endif
        }
#ifdef ARTS_RELEASE_PURGE
        if (atomic_load_explicit(&c->pending_grant_return,
                                 memory_order_relaxed) != 0u) {
          ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu rests owing a return of the "
                     "write right",
                     (unsigned long)c->db_guid);
          viol++;
        }
#endif
#endif
#if defined(ARTS_PROTOCOL_EXCL) && defined(ARTS_RELEASE_RETAIN)
        /* A read grant marked to return owes exactly one return, payable at
         * the read count's zero edge.  Resting at that edge still marked is
         * the signature of a promise made after the edge it named had already
         * passed: the count covers waiters as well as holders, so nothing is
         * left to reach the edge again, and the directory waits on a return
         * no one will send. */
        {
          uint64_t cw =
              atomic_load_explicit(&c->cache_state, memory_order_relaxed);
          if (CACHE_RO_ST(cw) == CACHE_ST_GRANT_PURGE &&
              CACHE_RO_CNT(cw) == 0u) {
            ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu rests owing a read-grant "
                       "return with no reader to pay it (word=%llx)",
                       (unsigned long)c->db_guid, (unsigned long long)cw);
            viol++;
          }
          viol += excl_cache_word_rows(c, cw, quiescent);
        }
#endif
#if defined(ARTS_PROTOCOL_EXCL) && defined(ARTS_RELEASE_PURGE) &&             \
    !defined(ARTS_USE_CXL)
        viol += excl_cache_word_rows(
            c, atomic_load_explicit(&c->cache_state, memory_order_relaxed),
            quiescent);
#endif
#ifdef ARTS_USE_CXL
        /* The arm's own quiescence, beside the RETAIN block above: what a
         * data block whose storage lives in the CXL store looks like once
         * every runtime thread has joined.  Cache fields need no
         * guard; a home field is read only where db->home_initialized says a
         * home directory exists, exactly like the VAL/INV block above — a
         * cache-only stub ends at lock_state, and reading past it on a
         * non-home rank would be a heap-buffer overflow on every cached
         * block of every non-home rank. */
        {
          uint64_t cw =
              atomic_load_explicit(&c->cache_state, memory_order_relaxed);
          uint8_t pending =
              atomic_load_explicit(&c->payload_pending, memory_order_relaxed);
          uint64_t addr = arts_db_cxl_slot_addr(c);
          uint64_t size =
              atomic_load_explicit(&c->db_size, memory_order_relaxed);
          /* 1. No slot missing where a working copy exists.  A sentinel-
           * sized block has no slot on either residency and legitimately
           * rests at address 0. */
          if (size != 0 && pending == 0u && addr == 0) {
            ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu has a working copy but no "
                       "slot for it (size=%llu)",
                       (unsigned long)c->db_guid, (unsigned long long)size);
            viol++;
          }
          /* 2. Each axis rests only where the single-CAS acquire / grant /
           * release discipline can leave it (excl_cache_word_rows). */
          viol += excl_cache_word_rows(c, cw, quiescent);
#ifdef ARTS_CXL_DIRECT
          /* 4. Nothing stands between the store and an EDT: where a holder's
           * working bytes ARE the block's store, a descriptor naming anything
           * else is storage some site materialized instead of adopting the
           * slot.  A home a destroy has marked is left out: a destroy that
           * found holders leaves the block undefined. */
          if (!(db->home_initialized &&
                EXCL_STATE_TEARDOWN(atomic_load_explicit(
                    &db->lock_state, memory_order_relaxed)) != 0u)) {
            arts_shared_ptr_t bh = arts_db_buf_acquire(c);
            const struct arts_db_buffer_s *b =
                (const struct arts_db_buffer_s *)(bh ? arts_shared_get(bh)
                                                     : NULL);
            if (b != NULL && !arts_cxl_store_contains(b->data)) {
              ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu rests with a payload "
                         "that is not its slot",
                         (unsigned long)c->db_guid);
              viol++;
            }
            if (bh) {
              arts_db_buf_release(&bh);
            }
          }
#endif
          /* 5. No waiter is stranded at the home.  A count left on the word
           * with nobody queued behind it is a hold whose fire-and-forget
           * release was still in flight when the network stopped -- a program
           * may shut down while holding -- and nothing can ever wait on it;
           * a count WITH a waiter queued is a release that will never come
           * for someone who asked. */
          if (db->home_initialized) {
            uint64_t ls =
                atomic_load_explicit(&db->lock_state, memory_order_relaxed);
            bool rwq = !arts_home_grantreq_queue_empty(&db->rw_waiters);
            bool roq = !arts_lf_stack_empty(&db->ro_waiters);
            if ((EXCL_STATE_W(ls) != 0u || EXCL_STATE_R(ls) != 0u) &&
                (rwq || roq)) {
              ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu home lock_state not "
                         "idle at teardown with a waiter queued "
                         "(w=%u r=%u teardown=%u)",
                         (unsigned long)c->db_guid, EXCL_STATE_W(ls),
                         EXCL_STATE_R(ls), EXCL_STATE_TEARDOWN(ls));
              viol++;
            }
            if (rwq) {
              ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu has queued RW waiters "
                         "at teardown (w=%u r=%u teardown=%u)",
                         (unsigned long)c->db_guid, EXCL_STATE_W(ls),
                         EXCL_STATE_R(ls), EXCL_STATE_TEARDOWN(ls));
              viol++;
            }
            if (roq) {
              ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu has queued RO waiters "
                         "at teardown (w=%u r=%u teardown=%u)",
                         (unsigned long)c->db_guid, EXCL_STATE_W(ls),
                         EXCL_STATE_R(ls), EXCL_STATE_TEARDOWN(ls));
              viol++;
            }
          }
        }
#endif /* ARTS_USE_CXL */
#endif /* ARTS_PROTOCOL_FLUSH */
      }
      if (h) {
        arts_shared_release(&h);
      }
    }
  }
  /* Every contributor is printed, so "nothing was left undone" and "work was
   * left undone, and it is why a residue went unreported" read apart -- the
   * two look identical from the violation count alone. */
  if (marked_owed != 0) {
    ARTS_DEBUG("COVER-INFO: %u home(s) marked for teardown still wait for a "
               "declared request (quiescent=%d)",
               marked_owed, (int)quiescent);
  }
  if (viol != 0 || !quiescent) {
    ARTS_DEBUG("QUIESCENCE-DEBUG: %u violation(s) (shutdown left undone: "
               "queued_edts=%u waits=%u ooo=%u "
               "admitted_edts=%u)",
               viol, ab_queued, ab_waits, ab_ooo, ab_admitted);
  }
#endif /* ARTS_LOG_LEVEL >= 3 */
}

#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV) ||               \
    defined(ARTS_PROTOCOL_EXCL)
/* Free the read waiters a destroy found still parked, then tear down the
 * inlined home-directory sub-resources.  Runs AFTER the protocol
 * field-destroy. */
void arts_db_cache_common_destroy_post(struct arts_db_cache_s *cache) {
  if (cache == NULL) {
    return;
  }
#if (defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)) &&              \
    defined(ARTS_WRITE_POLICY_WT)
  {
    arts_lf_link_t *n = arts_lf_stack_drain(&cache->publication_waiters);
    while (n != NULL) {
      arts_lf_link_t *next =
          atomic_load_explicit(&n->next, memory_order_relaxed);
      arts_free(n);
      n = next;
    }
  }
#endif
#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)
  /* Readers still parked at destroy are freed with the pool, not woken. */
  arts_db_reader_pool_destroy(&cache->reader_pool);
#endif
  {
    struct arts_db_s *db_self = arts_db_of_cache(cache);
    if (db_self->home_initialized) {
      arts_db_home_teardown(db_self);
      db_self->home_initialized = false;
    }
  }
}
#endif /* arms sharing the common cache shape */
