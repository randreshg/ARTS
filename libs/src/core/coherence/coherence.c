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
 *      releaser registers a {sem, version} waiter in its own frame on the
 *      cache and waits on it (arts_db_await_ack); at most one publish flight
 *      is in the air per (DB, rank), and its final PUBLISH_ACK wakes every
 *      waiter whose version that flight covers.  The announce leg's CTS is
 *      matched by pointer identity instead: the releaser's rendezvous address
 *      rides the packet and is echoed back — no per-cache seq state, no
 *      busy-wait.  A waiter returns only after its post, and no poster
 *      touches a node after posting it, so every rendezvous outlives every
 *      write into it.
 *
 *   4. Destroy lifecycle (the cache destructor).  Final teardown
 *      is driven by the cb (shared-ptr) deferred-free model: destroy fans out,
 *      then the slot's retire (through the descriptor's self_cb) drops the
 *      install ref, and once all refs drain arts_db_free runs the cache
 *      destructor and releases the descriptor's storage.
 */

#include <errno.h>
#include <semaphore.h>
#include <stdbool.h>
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "arts/coherence/buffer.h"
#include "arts/coherence/census.h"
#include "arts/coherence/coherence.h"
#include "arts/coherence/cover_gate.h"
#include "arts/coherence/directory.h"
#include "arts/coherence/handlers.h"
#include "arts/counter/Preamble.h"
#include "arts/cxl/store.h"
#include "arts/db.h"
#include "arts/edt.h"
#include "arts/gas/guid.h" /* GUID kind extraction (quiescence debug check) */
#include "arts/gas/route_table.h"
#include "arts/memory/regpool.h"
#include "arts/ooo.h"
#include "arts/runtime_state.h"
#include "arts/runtime_types.h"
#include "arts/system/print.h"
#include "arts/system/schedfuzz.h"
#include "arts/system/threads.h"
#include "arts/transport/net.h"       /* arts_net_put_payload */
#include "arts/utils/lockfree_lifo.h" /* publish flight waiter stack */
#include "arts/utils/malloc.h"
#include "arts/utils/shared.h"

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
 * transfer target, VAL·OWNER's dedup ledger, INV's reader word, EXCL's cache
 * word and waiter stacks — and then calls this.
 *
 * The arms that share this shape are the ones whose cache carries a home
 * directory; an arm without one builds its cache itself. */
#if defined(ARTS_VALIDITY_VAL) || defined(ARTS_VALIDITY_INV) ||               \
    defined(ARTS_VALIDITY_EXCL)
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
#if (defined(ARTS_VALIDITY_VAL) || defined(ARTS_VALIDITY_INV)) &&              \
    defined(ARTS_READ_SOURCE_HOME)
  arts_lf_stack_init(&c->publication_waiters);
#if ARTS_DB_CENSUS
  /* Only a home's create-time image is ever unpublished, and the creator's
   * hold is the write that publishes it first. */
  c->census_publisher_remote = (kind == ARTS_DB_INIT_HOME_RECV &&
                                creator_rank != arts_global_rank_id);
#endif
#endif
#ifdef ARTS_VALIDITY_VAL
  atomic_init(&c->req_state,
              (kind == ARTS_DB_INIT_STUB && ARTS_DB_STUB_CLAIMABLE(db_guid))
                  ? VREQ_MAKE(0u, 0u, 1u, 0u, 0u)
                  : (uint64_t)0);
#endif
#if defined(ARTS_VALIDITY_VAL) || defined(ARTS_VALIDITY_INV)
  arts_db_reader_pool_init(&c->reader_pool);
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
#if defined(ARTS_VALIDITY_VAL) || defined(ARTS_VALIDITY_INV)
    /* The creator boots holding the write right, with its own create-time
     * hold under it: possession, and one live writer. */
    atomic_init(&c->writer_count, ARTS_GRANT_SEED_HOLDING);
#endif
  } else if (kind == ARTS_DB_INIT_CREATOR_REMOTE) {
#if defined(ARTS_VALIDITY_VAL) || defined(ARTS_VALIDITY_INV)
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

#ifdef ARTS_VALIDITY_FLUSH
void arts_db_resolve_slot(struct arts_edt_s *edt, unsigned int slot,
                          arts_shared_ptr_t buf_h) {
  arts_dep_acq_resolve_once(edt, slot);
  resolve_slot_body(edt, slot, buf_h);
}
#endif

#if defined(ARTS_VALIDITY_VAL) || defined(ARTS_VALIDITY_INV) ||               \
    defined(ARTS_VALIDITY_EXCL)
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

/* A cache-only stub arts_db_s + cache_s for a DIST DB this rank has not yet
 * touched, built privately, its self_cb armed: its control block, whose one
 * ref is the caller's.  No home directory (this rank is not the GUID home):
 * arts_db_cache_stub_size() spans cache + db_type, stopping before the home
 * fields.  No user data payload either, and no size: the first install
 * allocates the payload, and the first answer teaches the size. */
static arts_shared_ptr_t db_cache_stub_make(arts_guid_t db_guid) {
  assert(arts_guid_get_rank(db_guid) != arts_global_rank_id &&
         "a first-touch cache is never made on the block's home rank");
  uint64_t stub_sz = arts_db_cache_stub_size();
  struct arts_db_s *stub =
      (struct arts_db_s *)arts_malloc_aligned(stub_sz, ARTS_CACHE_LINE_SIZE);
  memset(stub, 0, stub_sz);
  stub->db_type = ARTS_DB;
  arts_db_cache_init(&stub->cache, db_guid, /*db_size=*/0, ARTS_DB_INIT_STUB,
                     /*creator_rank=*/0);
  arts_shared_ptr_t cb = arts_route_table_make_handle(stub, db_guid);
  stub->self_cb = cb;
  return cb;
}

/* Free a stub whose control block no install took. */
static void db_cache_stub_discard(arts_shared_ptr_t cb) {
  struct arts_db_s *stub = (struct arts_db_s *)arts_shared_get(cb);
  stub->self_cb = NULL;
  arts_shared_abandon(&cb);
  arts_db_free(stub);
}

arts_shared_ptr_t
arts_db_cache_stub_install_into(struct arts_route_item_s *slot,
                                arts_guid_t db_guid) {
  arts_shared_ptr_t cb = db_cache_stub_make(db_guid);
  arts_shared_ptr_t got = arts_route_table_install_or_get(slot, cb, db_guid);
  if (got != cb) {
    db_cache_stub_discard(cb);
  }
  return got;
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

#if defined(ARTS_VALIDITY_VAL)
/* ===== The read path and the request word ============================
 *
 * Every remote read-only acquire joins its cache's one window: at most one
 * read request per cache is out, and a reader that arrives while it is out
 * waits for the next one -- a reader may only be served by a request sent
 * AFTER its park, because the server serves the version current when the
 * request arrives, and any release the reader is ordered after was published
 * before its park.  The read axis never waits on the write axis: a reader
 * opens its window whatever write request the rank has out, and only the
 * window's own answer serves its cohort, so a parked reader waits for
 * messages in flight and for answers no EDT's acquire holds up.
 *
 * The chains live in the request word (val/types.h), so parking IS the
 * acquire's decision CAS and the answer takes the whole cohort in the CAS
 * that decides it: no reader can miss the request it joined or be served
 * twice. */

/* ---- the arbiter (val/arbiters.c: a pure function of the word) ---------- */
#include "val/arbiters.c"

/* Commit `op` on the word; returns the pre-image, *post the image. */
static ARTS_ALWAYS_INLINE uint64_t val_req_step(struct arts_db_cache_s *cache,
                                                int op, uint32_t idx,
                                                uint64_t *post, uint32_t *act) {
  struct arts_db_reader_node_s *node =
      op == VREQ_OP_ACQ_RO ? arts_db_reader_node(&cache->reader_pool, idx)
                           : NULL;
  uint64_t cur = atomic_load_explicit(&cache->req_state, memory_order_acquire);
  uint64_t next;
  for (;;) {
    next = val_req_compute_next(cur, op, idx, act);
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
    arts_db_axis_take(arts_db_of_cache(cache));
    arts_send_db_snapshot_request(cache);
  } else {
    INCREMENT_NUM_RO_COMBINE_JOINED_BY(1);
  }
  return ARTS_DB_ACQUIRE_PARK;
}

bool arts_db_ro_combine_on_terminal(struct arts_db_cache_s *cache) {
  uint64_t post;
  uint32_t act;
  uint64_t pre = val_req_step(cache, VREQ_OP_ANSWER, 0u, &post, &act);
  if (act == VREQ_ACT_LATE) {
    ARTS_ERROR("val: guid %lu got a read answer it has no request out for "
               "(word=%llx)",
               (unsigned long)cache->db_guid, (unsigned long long)pre);
  }
  arts_db_reader_serve_chain(&cache->reader_pool, VREQ_GROUP(pre));
  if (act == VREQ_ACT_SERVE_SEND) {
    INCREMENT_NUM_RO_COMBINE_WINDOW_BY(1);
    arts_send_db_snapshot_request(cache);
    return false;
  }
  return true;
}

void arts_db_ro_combine_on_cts(struct arts_db_cache_s *cache) {
  uint64_t post;
  uint32_t act;
  uint64_t pre = val_req_step(cache, VREQ_OP_CTS, 0u, &post, &act);
  if (act == VREQ_ACT_LATE) {
    ARTS_ERROR("val: guid %lu got a read size round it has no request out "
               "for (word=%llx)",
               (unsigned long)cache->db_guid, (unsigned long long)pre);
  }
  arts_send_db_snapshot_request(cache);
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
  return VREQ_STUB(s) ? ARTS_COVER_PRECLAIM : 0u;
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
}

void arts_db_rw_req_declaim(struct arts_db_cache_s *cache) {
  uint64_t post;
  uint32_t act;
  uint64_t pre = val_req_step(cache, VREQ_OP_DECLAIM, 0u, &post, &act);
  if (act == VREQ_ACT_LATE) {
    ARTS_ERROR("val: guid %lu got a write size round it has no request out "
               "for (word=%llx)",
               (unsigned long)cache->db_guid, (unsigned long long)pre);
  }
}

/* The create's claim of the write axis, after its hold is taken on the grant
 * word: a write request out is the claimer's pre-claim request, which the
 * announce names for the home to absorb, and an OPEN is reset (its commit
 * then loses on the token).  A read request out stays out for its own
 * answer.  Returns the owed byte the announce carries. */
uint8_t arts_db_val_claim_rw_axis(struct arts_db_cache_s *cache) {
  uint64_t post;
  uint32_t act;
  uint64_t pre = val_req_step(cache, VREQ_OP_CLAIM, 0u, &post, &act);
  return VREQ_RW(pre) == VREQ_RW_SENT ? ARTS_CREATE_OWED_RW : 0u;
}
#endif /* the versioned-snapshot read path */

/* The 8-case acquire dispatcher arts_handler_db_acquire is defined per read
 * source, in each arm's own read-source TU: both run the single-owner
 * GRANT_REQUEST / GRANT path of coherence/grant.c and differ only on the
 * RO-has-local-data predicate.  The shared remote-RO path
 * (arts_db_acquire_remote_ro) and the local-buffer fast read
 * (arts_db_acquire_local) above are reused by both. */

/* Wake every publication waiter, taken in one exchange.  A waiter is a read
 * that found its home's copy unpublished under HOME — the
 * create-time image at version 0 — and parked: a local read hold, or a remote
 * read whose serve was deferred.  A release under HOME stamps its version
 * on the home's copy before it completes, so a read that saw version 0 is
 * ordered after no release, and any published copy (version >= 1) is a value
 * it may be given.  Every drain runs once the slot holds one, and the slot's
 * version never decreases, so waking every waiter taken is exact.  The take
 * is an acq_rel exchange paired with each parker's acq_rel push: a parker
 * re-reads the version after its push, and a publisher drains after its
 * stamp, so one of the two always sees the other. */
#if (defined(ARTS_VALIDITY_VAL) || defined(ARTS_VALIDITY_INV)) &&              \
    defined(ARTS_READ_SOURCE_HOME)
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
#endif /* homes under HOME that hold reads until the first publication */

/* ================================================================== */
/* ===== Release path =============================================== */
/* ================================================================== */

/* ===== release-reply wait (shared coherence service) ===============
 *
 * A release that must not return before the home has answered blocks on a
 * semaphore that the answer posts.  Compiled for every arm; declared in
 * coherence/coherence.h so the arm TUs can invoke it. */
void arts_db_await_ack(sem_t *cv) {
  while (sem_wait(cv) != 0) {
    if (errno != EINTR) {
      ARTS_ERROR("coherence: a release wait failed: %s", strerror(errno));
    }
  }
}

/* arts_db_release_rw and arts_db_publish_rw are protocol-specific (the
 * version bump is shared, but a release's buffer-ref drop before its
 * decrement and its transfer/publish decision after it differ per protocol),
 * so their whole bodies live in each arm's own TU.  The write half of VAL·HOME and of
 * INV under either read source calls arts_db_publish_sync below for the
 * synchronous-PUBLISH rendezvous (INV's publish doubles as its
 * invalidation-round request). */

/* ===== publish flight machine (write-combining publish) ============
 *
 * Compiled by every arm that publishes at a release's write half, which a
 * release and an arts_db_publish_rw both run.  Under HOME that is the payload
 * write-through; under OWNER only INV publishes at all, and its publish is
 * control-only — the round request.
 *
 * At most ONE publish is in flight per (DB, rank).  Every releaser (the
 * caller of a write half) registers as a version-covered waiter, then either
 * claims the flight (CAS 0->FLYING) or joins it (CAS ->|DIRTY).  A flight
 * ships the cache buffer's CURRENT bytes stamped with the buffer's current
 * version, so the covering ACK wakes every waiter at or below that version
 * and write halves that landed mid-flight coalesce into at most one trailing
 * flight — the write-side twin of the RO request-combining window.  The ACK
 * doubles as the credit teacher: it carries the home's next in-place credit
 * {stable-buffer addr, rkey, txid}, consumed 1:1 by the next payload flight,
 * so the steady state is one PUT + one commit + one blocked wait,
 * announce-free. */
#if (defined(ARTS_VALIDITY_VAL) || defined(ARTS_VALIDITY_INV)) &&             \
    (!defined(ARTS_READ_SOURCE_OWNER) || defined(ARTS_VALIDITY_INV))

#define ARTS_PUB_FLYING 1u
#define ARTS_PUB_DIRTY 2u

struct arts_db_pub_waiter_s {
  arts_lf_link_t link; /* FIRST — Treiber membership */
  uint64_t version;    /* wake once a publish >= this version is ACKed */
  sem_t sem;
};

/* INV·OWNER publishes control only: none of its
 * flights carries payload, so none consumes a credit or leaves a payload leg
 * that could carry a hand-back. */
#if defined(ARTS_VALIDITY_INV) && defined(ARTS_READ_SOURCE_OWNER)
#define ARTS_PUB_CONTROL_ONLY 1
#else
#define ARTS_PUB_CONTROL_ONLY 0
#endif

#if !ARTS_PUB_CONTROL_ONLY
/* Does a payload commit leg leaving now owe the home a hand-back of the write
 * right?  Only an arm whose write right migrates can owe one. */
static inline bool pub_leg_takes_grant_return(struct arts_db_cache_s *cache) {
#if defined(ARTS_VALIDITY_VAL) || defined(ARTS_VALIDITY_INV)
  return arts_db_grant_return_claim_leg(cache);
#else
  (void)cache;
  return false;
#endif
}
#endif

/* Does this rank's publish carry payload?  Owners under HOME ship bytes; a
 * home-resident releaser's buffer IS the canonical copy (its publish is the
 * ordering round alone), and INV·OWNER publishes control only. */
static bool pub_flight_carries_payload(struct arts_db_cache_s *cache) {
#if defined(ARTS_VALIDITY_INV) && defined(ARTS_READ_SOURCE_OWNER)
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
 * refill chain is broken, which is fatal).  `gate` is the claiming releaser's
 * storage for this flight's gate node (below), in a frame that outlives the
 * gate's post; NULL exactly when may_block is false. */
static void pub_flight_drive(struct arts_db_cache_s *cache, bool may_block,
                             uint64_t fallback_version,
                             struct arts_db_pub_waiter_s *gate) {
  assert(may_block == (gate != NULL));
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
#if !ARTS_PUB_CONTROL_ONLY
  uint64_t db_size =
      atomic_load_explicit(&cache->db_size, memory_order_relaxed);
  unsigned int home_rank = arts_guid_get_rank(cache->db_guid);
#endif
  /* A claiming releaser gates on ITS OWN flight, not merely on a covering
   * ACK: a claim won after the claimer's registered waiter was already
   * covered (the claim-after-covered race) would otherwise launch a flight
   * nobody waits for, and the release's 0-edge — the ownership ship — could
   * overtake the still-flying PUT, letting the next owner's publish
   * interleave with this one.  The gate node is a waiter at the flight's own
   * stamp, so exactly this flight's ACK releases it.  A trailing relaunch
   * needs no gate: its uncovered waiters are blocked releasers, so the 0-edge
   * cannot fire until its ACK lands. */
  if (gate != NULL) {
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
    arts_send_db_publish(arts_db_of_cache(cache), version, /*cv=*/0,
                         /*data_size=*/0, /*rdzv_txid=*/0, /*rdzv_cookie=*/0,
                         /*return_grant=*/false);
    return;
  }
#if !ARTS_PUB_CONTROL_ONLY
  /* Payload flight: consume the credit.  The acquire exchange pairs with the
   * refill's release-store; addr/rkey are stable across refills (one DB has
   * one stable buffer), so reading them after the exchange is safe.  The
   * strong buffer ref transfers to the PUT's local completion, which pins the
   * source bytes until the fabric has drained them. */
  uint64_t txid =
      atomic_exchange_explicit(&cache->home_pub_txid, 0u, memory_order_acquire);
  if (txid != 0) {
    arts_net_put_payload(
        (int)home_rank,
        atomic_load_explicit(&cache->home_pub_addr, memory_order_relaxed),
        atomic_load_explicit(&cache->home_pub_rkey, memory_order_relaxed), txid,
        buf->data, db_size, arts_db_buf_ref_release_cb, (void *)buf_h);
    arts_send_db_publish(arts_db_of_cache(cache), version, /*cv=*/0, db_size,
                         txid, /*rdzv_cookie=*/0,
                         pub_leg_takes_grant_return(cache));
    return;
  }
  if (!may_block) {
    /* The ACK this relaunch runs in refilled the credit, and only the
     * flight's owner takes one. */
    ARTS_ERROR("coherence: guid %lu: a publish relaunch found no credit",
               (unsigned long)cache->db_guid);
  }
  INCREMENT_NUM_PUB_CTS_FALLBACK_BY(1);
  /* Credit-less first flight: announce -> blocked CTS wait -> PUT -> commit.
   * The CTS reply writes the landing through the echoed cv and then posts;
   * the wait returns only after that post, so the rendezvous lives in this
   * frame. */
  struct arts_db_pub_rendezvous_s wr;
  sem_init(&wr.sem, 0, 0);
  wr.landing = ARTS_RDZV_NO_LANDING;
  arts_send_db_publish(arts_db_of_cache(cache), version,
                       (uint64_t)(uintptr_t)&wr, db_size, /*rdzv_txid=*/0,
                       /*rdzv_cookie=*/0, /*return_grant=*/false);
  arts_db_await_ack(&wr.sem);
  /* The post orders the reply's landing stores before these reads, and a
   * CTS reply always carries a landing. */
  assert(wr.landing.txid != 0);
  arts_net_put_payload((int)home_rank, wr.landing.addr, wr.landing.key,
                       wr.landing.txid, buf->data, db_size,
                       arts_db_buf_ref_release_cb, (void *)buf_h);
  arts_send_db_publish(arts_db_of_cache(cache), version, /*cv=*/0, db_size,
                       wr.landing.txid, wr.landing.cookie,
                       pub_leg_takes_grant_return(cache));
  /* The commit carries cv 0 — the final ACK completes the FLIGHT, not this
   * rendezvous. */
  sem_destroy(&wr.sem);
#endif
}

void arts_db_publish_sync(struct arts_db_cache_s *cache, uint64_t version) {
  /* The waiter node and the gate live in this frame: a drain pops a node
   * before it posts it and touches it no more, and both waits below return
   * only after their posts. */
  struct arts_db_pub_waiter_s w;
  struct arts_db_pub_waiter_s gate;
  bool gated = false;
  w.version = version;
  sem_init(&w.sem, 0, 0);
  arts_lf_stack_push(&cache->pub_waiters, &w.link);
  arts_sched_fuzz_point(); /* widen the push<->flight-claim window */
  for (;;) {
    unsigned int f =
        atomic_load_explicit(&cache->pub_flight, memory_order_relaxed);
    if (f == 0u) {
      unsigned int expected = 0u;
      if (atomic_compare_exchange_strong_explicit(
              &cache->pub_flight, &expected, ARTS_PUB_FLYING,
              memory_order_acq_rel, memory_order_relaxed)) {
        /* The flight's count, released by the completion that lands it. */
        arts_db_axis_take(arts_db_of_cache(cache));
        pub_flight_drive(cache, /*may_block=*/true, version, &gate);
        gated = true;
        break;
      }
    } else if (atomic_compare_exchange_strong_explicit(
                   &cache->pub_flight, &f, f | ARTS_PUB_DIRTY,
                   memory_order_acq_rel, memory_order_relaxed)) {
      INCREMENT_NUM_PUB_FLIGHT_JOIN_BY(1);
      break;
    }
  }
  arts_db_await_ack(&w.sem);
  sem_destroy(&w.sem);
  if (gated) {
    arts_db_await_ack(&gate.sem);
    sem_destroy(&gate.sem);
  }
}

/* The PUBLISH_ACK answer, shared by every publishing arm (the wire and the
 * home's own releaser).  cv != 0 is the pointer-identity sem-post of a grant
 * return sent on its own (PURGE): it names no descriptor and resolves
 * nothing.  cv == 0 is the completion of a publish flight, control-only ones
 * included, on the descriptor its token names: record the refilled credit,
 * wake every waiter the ACKed version covers, and either relaunch (uncovered
 * waiters remain — their releases landed after the flight's version stamp)
 * or land the flight.  A DIRTY bit set after the drain forces a
 * re-examination before landing, so a waiter registered mid-completion is
 * never stranded. */
void arts_handler_db_publish_ack(const struct arts_db_publish_ack_args_s *a) {
  if (a->cv != 0) {
    sem_post((sem_t *)(uintptr_t)a->cv);
    return;
  }
  struct arts_db_s *db = arts_db_from_token(a->token, a->db_guid);
  if (arts_db_token_retired(db, a->db_guid)) {
    /* The block was destroyed with this flight in the air, by a program that
     * destroyed it before the release returned, which the destroy contract
     * forbids.  Nothing is woken (a releaser still waiting stays blocked);
     * the flight's count goes. */
    arts_db_axis_drop(db, 1u);
    return;
  }
  struct arts_db_cache_s *cache = &db->cache;
  if (a->credit_txid != 0) {
#if ARTS_PUB_CONTROL_ONLY
    ARTS_ERROR("coherence: guid %lu got a publish credit — this arm's homes "
               "mint none; binary mode mismatch?",
               (unsigned long)cache->db_guid);
#else
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
#endif
  }
  for (;;) {
    /* Waiters parked by the previous completion first, then everything that
     * queued since.  An acquire load and a release store of NULL, not an
     * exchange: under the FLYING claim exactly one completion runs at a time,
     * and only a completion touches the field (the teardown walk
     * reads it relaxed, after every other thread has stopped). */
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
      pub_flight_drive(cache, /*may_block=*/false, a->version, NULL);
      return;
    }
    unsigned int expected = ARTS_PUB_FLYING;
    if (atomic_compare_exchange_strong_explicit(&cache->pub_flight, &expected,
                                                0u, memory_order_acq_rel,
                                                memory_order_acquire)) {
      /* Flight landed: its count goes, this frame's last access. */
      arts_db_axis_drop(db, 1u);
      return;
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

#if defined(ARTS_VALIDITY_VAL) || defined(ARTS_VALIDITY_INV)
void arts_db_release_ro(struct arts_db_cache_s *cache) {
  /* A VAL or INV read hold is the EDT's buffer ref alone, which the caller
   * drops: no protocol state counts readers. */
  (void)cache;
}

void arts_db_join_read(struct arts_db_cache_s *cache) { (void)cache; }
#endif /* arms whose read hold is nothing but the EDT's own ref */

#if defined(ARTS_VALIDITY_VAL) || defined(ARTS_VALIDITY_INV)
/* ===== The destroy's gate (home side) =================================
 *
 * One request can reach the home after the program legally destroyed the
 * block: the write request a first-touch cache sent before a create on its
 * rank claimed that cache.  The claim served the request's writers, so no
 * acquire waits for its answer, and the claim's announce tells the home that
 * it is still coming.  The gate (arts/coherence/cover_gate.h) holds it as
 * rwo; the destroy marks the gate, and the teardown runs at the step that
 * leaves the gate marked with rwo clear: the destroy, or that request's
 * arrival.  The home absorbs the request, queueing and answering nothing,
 * since the claim closed the requester's write axis.
 *
 * No read request passes the gate.  Its readers stay parked until its own
 * answer, and a destroy issued while an acquire is in progress is the
 * program's error, so no read request is out when a legal destroy is
 * issued. */
void arts_db_home_seed_owed(struct arts_db_s *db, unsigned int creator,
                            uint8_t owed) {
  if (owed == 0u) {
    return;
  }
  assert(owed == ARTS_CREATE_OWED_RW &&
         "a claim closes its cache's write request and nothing else");
  db->claim_rank = creator;
  atomic_store_explicit(&db->cover_gate, ARTS_COVER_GATE_RWO,
                        memory_order_relaxed);
}

bool arts_db_preclaim_write_absorb(struct arts_db_s *db, unsigned int requester,
                                   uint32_t cover, bool *teardown) {
  *teardown = false;
  if ((cover & ARTS_COVER_PRECLAIM) == 0u || requester != db->claim_rank) {
    return false;
  }
  *teardown = arts_cover_gate_step(db, ARTS_COVER_GATE_RWO_ARRIVE);
  return true;
}

void arts_db_cover_destroy(struct arts_db_s *db) {
  if (arts_cover_gate_step(db, ARTS_COVER_GATE_MARK)) {
    arts_db_home_destroy_now(db);
  }
}
#endif /* the destroy's gate */

/* ================================================================== */
/* ===== Destroy lifecycle ========================================== */
/* ================================================================== */

/* ===== cache_s destructor (chained from arts_db_free) =============
 *
 * The full destructor arts_db_cache_destructor is protocol-specific — it
 * sequences the protocol's own field teardown between these two shared steps —
 * and lives in each arm's own TU.  The agnostic steps split into pre
 * (the buffer-NULL that must run first) and post (snapshot drain + home
 * teardown), so the per-arm wrapper runs pre, its own teardown, then post.
 * The descriptor's storage is freed after the wrapper returns. */

#if defined(ARTS_VALIDITY_VAL) || defined(ARTS_VALIDITY_INV) ||               \
    defined(ARTS_VALIDITY_EXCL)
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
}
#endif /* arms sharing the common cache shape */

/* Debug-only terminal-quiescence check.  Runs once, after every other
 * runtime thread has stopped and before teardown frees the caches.  It
 * reports two things: states the protocols can never reach, and waiters or
 * holds left behind by a shutdown that was quiescent -- a survivor there is a
 * lost wake that the run's own success criteria may have masked (a reader
 * that never ran).  Work a shutdown abandoned is the one legal source of such
 * a residue, so after a shutdown that left work undone residues are not
 * reported; the contributors say what was left undone.  Violations print a
 * QUIESCENCE-DEBUG marker; tests whose shutdown is quiescent turn that marker
 * into a failure.
 *
 * The walk exists only in a tree with the teardown, and only to produce
 * diagnostic messages, so it is also gated on the log level that compiles
 * those messages: below DEBUG its body is empty. */
#if defined(ARTS_DEBUG_TEARDOWN) && ARTS_LOG_LEVEL >= 3 &&                     \
    defined(ARTS_VALIDITY_EXCL)
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
 * unfinished turn (quiescent only).  PURGE: a grant with nothing counted
 * under it is a lost zero edge (the edge returns the grant in the same CAS).
 * RETAIN: a resting grant with nothing counted is the retention's normal
 * state.
 * Read axis: the read request's readers are parked, in the word's count
 * while nothing here serves a read or in ro_aside, so a request out with
 * none parked, readers set aside with no request out, and readers counted
 * with neither a request out nor anything serving them are lost; readers
 * parked on the request, or holding under a grant or (RETAIN) the resident
 * copy, are unfinished (quiescent only).  What serves a read is a held grant
 * on either axis or (RETAIN) the owner bit.  PURGE: a read grant with no
 * reader counted is a lost zero edge; RETAIN: a resting read grant is the
 * retention's normal state. */
static unsigned int excl_cache_word_rows(const struct arts_db_cache_s *c,
                                         uint64_t cw, bool quiescent) {
  uint32_t rws = CACHE_RW_ST(cw), ros = CACHE_RO_ST(cw);
  uint32_t wcn = CACHE_RW_CNT(cw), rcn = CACHE_RO_CNT(cw);
#ifdef ARTS_RETENTION_RETAIN
  bool purge = false;
#else
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
  /* Relaxed: the walk runs once every other thread has stopped. */
  uint32_t aside = atomic_load_explicit(&c->ro_aside, memory_order_relaxed);
#ifdef ARTS_RETENTION_RETAIN
  bool write_held = rws == CACHE_ST_GRANT || CACHE_OWNER(cw) == 1u;
  if (aside != 0u && ros != CACHE_ST_REQ) {
    v += excl_axis_row(c, cw,
                       "ro readers are set aside with no read request out",
                       false, quiescent);
  }
  if (ros == CACHE_ST_REQ) {
    bool parked = aside != 0u || (!write_held && rcn != 0u);
    v += excl_axis_row(c, cw,
                       parked ? "ro readers rest on an open read request"
                              : "ro axis rests at its request with no reader "
                                "parked on it",
                       parked, quiescent);
  } else if ((ros == CACHE_ST_GRANT || ros == CACHE_ST_GRANT_PURGE) &&
             rcn != 0u) {
    v += excl_axis_row(c, cw,
                       "ro axis rests non-idle with readers still counted",
                       true, quiescent);
  } else if (ros == CACHE_ST_IDLE && rcn != 0u && !write_held) {
    v += excl_axis_row(c, cw, "ro axis rests idle with readers counted", false,
                       quiescent);
  }
  if (rcn != 0u && write_held) {
    v += excl_axis_row(c, cw,
                       "ro readers rest under the write grant or the resident "
                       "copy",
                       true, quiescent);
  }
#else
  bool write_held = rws == CACHE_ST_GRANT;
  if (aside != 0u && ros != CACHE_ST_REQ) {
    v += excl_axis_row(c, cw,
                       "ro readers are set aside with no read request out",
                       false, quiescent);
  }
  if (ros == CACHE_ST_REQ) {
    bool parked = aside != 0u || (!write_held && rcn != 0u);
    v += excl_axis_row(c, cw,
                       parked ? "ro readers rest on an open read request"
                              : "ro axis rests at its request with no reader "
                                "parked on it",
                       parked, quiescent);
  } else if (ros == CACHE_ST_GRANT) {
    v += excl_axis_row(c, cw,
                       rcn != 0u
                           ? "ro axis rests non-idle with readers still counted"
                           : "ro axis rests granted with no reader counted",
                       rcn != 0u, quiescent);
  } else if (rcn != 0u && !write_held) {
    v += excl_axis_row(c, cw, "ro axis rests idle with readers counted", false,
                       quiescent);
  }
  if (rcn != 0u && write_held) {
    v += excl_axis_row(c, cw, "ro readers rest under the write grant", true,
                       quiescent);
  }
#endif
  return v;
}
#endif

#if defined(ARTS_DEBUG_TEARDOWN) && ARTS_LOG_LEVEL >= 3
static bool walk_exempt_edt(const struct arts_teardown_exempt_s *ex,
                            const void *edt) {
  for (unsigned int i = 0; ex != NULL && i < ex->edt_count; i++) {
    if ((const void *)ex->edts[i] == edt) {
      return true;
    }
  }
  return false;
}

static bool walk_exempt_db(const struct arts_teardown_exempt_s *ex,
                           const struct arts_db_s *db) {
  for (unsigned int i = 0; ex != NULL && i < ex->db_count; i++) {
    if (ex->dbs[i] == db) {
      return true;
    }
  }
  return false;
}
#endif

#ifdef ARTS_DEBUG_TEARDOWN
void arts_db_debug_quiescence_check(const struct arts_teardown_exempt_s *ex,
                                    unsigned int ab_queued) {
#if ARTS_LOG_LEVEL >= 3
  /* Runs only once every other runtime thread has stopped: the stopper's
   * acquire of each thread's stop orders every write before it, so each load
   * is relaxed. */
#ifdef ARTS_DEBUG_LANDING_BALANCE
  /* Informational: a reply cut by the shutdown (no delivery barrier) leaves
   * its landing out; a test waits for the count to reach zero instead. */
  if (arts_db_landings_outstanding() != 0) {
    ARTS_DEBUG("LANDING-INFO: %lld landing(s) outstanding at shutdown",
               (long long)arts_db_landings_outstanding());
  }
#endif
  unsigned int viol = 0;
  /* Homes a destroy marked that still wait for the claimer's pre-claim write
   * request the create's announce said is coming: legal at any shutdown,
   * printed so a campaign can watch it. */
  unsigned int marked_owed = 0;
  const unsigned int nt = (unsigned int)arts_guid_table_count;
  /* Whether this shutdown was quiescent, named by contributor so a log's
   * reader knows why a residue went unreported.  Every one is pending work
   * the shutdown left undone: runnable EDTs dropped from a deque, EDTs
   * admitted (every dependence satisfied, acquisition begun) that never
   * finished -- these cover an EDT parked mid acquisition, which sits in no
   * queue a discard site sees, and overlap the first -- and messages parked
   * on a route slot's OoO list: a create waiting for a destroy, or any other
   * message waiting for an install, that never came.
   * A parked message is residue whatever parked it -- a late message for a
   * destroyed GUID included -- so a program whose shutdown leaves one was not
   * quiescent.  Each is read after every other runtime thread has stopped,
   * so no poster can add to what is counted, and before anything frees it.  A
   * message from another rank still undelivered is not one: a release in
   * flight to a home strands no waiter, and a grant still owed to a requester
   * leaves that requester's EDT admitted and unfinished. */
  unsigned int ab_ooo = 0;
  for (unsigned int t = 0; t < nt; t++) {
    ab_ooo += arts_route_table_parked_count(arts_node_info.route_table[t]);
  }
  unsigned int ab_admitted = 0;
  for (unsigned int t = 0; t < nt; t++) {
    arts_route_table_iterator_t iter;
    arts_route_table_iterator_reset(&iter, arts_node_info.route_table[t]);
    for (arts_route_item_t *item = arts_route_table_iterate(&iter);
         item != NULL; item = arts_route_table_iterate(&iter)) {
      if (ARTS_GUID_GET_TYPE(atomic_load_explicit(
              &item->key, memory_order_relaxed)) != ARTS_GUID_EDT) {
        continue;
      }
      arts_shared_ptr_t h = arts_shared_slot_load(&item->value);
      /* The EDT that called the end never returns, by design. */
      if (arts_edt_admitted(h) &&
          !walk_exempt_edt(ex, h ? arts_shared_get(h) : NULL)) {
        ab_admitted++;
      }
      arts_shared_release(&h);
    }
  }
  bool walk_quiescent = (ab_queued | ab_ooo | ab_admitted) == 0u;
  /* Blocks the exempt EDTs hold, which the walk judges as after a shutdown
   * that left work undone: their holds are never released. */
  unsigned int caller_held = 0;
  for (unsigned int t = 0; t < nt; t++) {
    arts_route_table_iterator_t iter;
    arts_route_table_iterator_reset(&iter, arts_node_info.route_table[t]);
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
        const bool exempt = walk_exempt_db(ex, db);
        caller_held += exempt ? 1u : 0u;
        const bool quiescent = walk_quiescent && !exempt;
        (void)quiescent;
#ifdef ARTS_VALIDITY_FLUSH
        /* FLUSH keeps no wait structure on a descriptor — a blocked release
         * waits on a semaphore in its own frame that the home's answer posts
         * — so a walk has nothing to report. */
#else
        struct arts_db_cache_s *c = &db->cache;
#if defined(ARTS_VALIDITY_VAL) || defined(ARTS_VALIDITY_INV)
        /* What each rule can see.  A parked waiter belongs to an EDT that has
         * not finished, which makes the shutdown non-quiescent; a blocked
         * releaser keeps its thread from stopping, so the walk does not run
         * and the skip line reports it.  So the rules gated on a quiescent
         * shutdown catch what completed work left behind: a node no drain
         * removed, a hold whose EDT finished without releasing it.  The
         * ungated rules are states no interleaving and no message still in
         * flight can leave, and they hold after any shutdown. */
#endif
#if (defined(ARTS_VALIDITY_VAL) || defined(ARTS_VALIDITY_INV)) &&             \
    (!defined(ARTS_READ_SOURCE_OWNER) || defined(ARTS_VALIDITY_INV))
        if (quiescent && (!arts_lf_stack_empty(&c->pub_waiters) ||
                          atomic_load_explicit(&c->pub_parked,
                                               memory_order_relaxed) != NULL)) {
          ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu has parked publish "
                     "waiters after a quiescent shutdown",
                     (unsigned long)c->db_guid);
          viol++;
        }
#endif
#ifdef ARTS_VALIDITY_VAL
        {
          uint64_t rq =
              atomic_load_explicit(&c->req_state, memory_order_relaxed);
          /* A parked reader is an unfinished acquire: reported after a
           * quiescent shutdown.  A write axis left OPEN has lost its writer
           * thread; a window out with nobody in it, and a reader parked for
           * a next window with no window out whose answer opens it, are words
           * no transition makes: always reported. */
          if (quiescent && (VREQ_GROUP(rq) != 0u || VREQ_NEXT(rq) != 0u)) {
            ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu has a parked read after a "
                       "quiescent shutdown (word=%llx)",
                       (unsigned long)c->db_guid, (unsigned long long)rq);
            viol++;
          }
          if (VREQ_RW(rq) == VREQ_RW_OPEN ||
              (VREQ_RO(rq) && VREQ_GROUP(rq) == 0u) ||
              (VREQ_NEXT(rq) != 0u && !VREQ_RO(rq))) {
            ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu request word rests where "
                       "nothing serves it (word=%llx)",
                       (unsigned long)c->db_guid, (unsigned long long)rq);
            viol++;
          }
        }
#endif
#if (defined(ARTS_VALIDITY_VAL) || defined(ARTS_VALIDITY_INV)) &&              \
    defined(ARTS_READ_SOURCE_HOME)
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
#ifdef ARTS_VALIDITY_EXCL
        if (db->home_initialized) {
#ifdef ARTS_RETENTION_PURGE
          /* A marked home with nothing counted and no pre-claim write
           * request awaited is a teardown that was skipped.  One still
           * awaited is legal and only counted. */
          uint64_t ls =
              atomic_load_explicit(&db->lock_state, memory_order_relaxed);
          if (EXCL_STATE_TEARDOWN(ls) != 0u) {
            if (EXCL_STATE_RWO(ls) != 0u) {
              marked_owed++;
            } else if (EXCL_STATE_W(ls) == 0u && EXCL_STATE_R(ls) == 0u) {
              ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu home rests marked for "
                         "teardown with nothing counted or awaited "
                         "(lock_state=%llx)",
                         (unsigned long)c->db_guid, (unsigned long long)ls);
              viol++;
            }
          }
#else
          /* Marked with no pre-claim write request awaited means the
           * teardown was skipped.  One still awaited is legal at any
           * shutdown (it may be undelivered) and only counted. */
          uint32_t gate =
              atomic_load_explicit(&db->cover_gate, memory_order_relaxed);
          if ((gate & ARTS_COVER_GATE_TD) != 0u) {
            if ((gate & ARTS_COVER_GATE_RWO) != 0u) {
              marked_owed++;
            } else {
              ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu home rests marked for "
                         "teardown with nothing awaited (gate=%x)",
                         (unsigned long)c->db_guid, gate);
              viol++;
            }
          }
#endif /* ARTS_RETENTION_PURGE */
        }
#endif
#ifdef ARTS_VALIDITY_INV
        {
          uint64_t cs =
              atomic_load_explicit(&c->cache_state, memory_order_relaxed);
          /* A parked reader after a quiescent shutdown is an unfinished
           * acquire.  A writer thread lost between claim and commit, readers
           * parked with no fetch out, and a fetch out with nobody parked on
           * it are words no transition makes: always reported. */
          if (INV_CACHE_RW(cs) == INV_RW_OPEN ||
              (INV_CACHE_HEAD_RO(cs) != 0u) !=
                  (INV_CACHE_INFLIGHT(cs) != 0u)) {
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
#if defined(ARTS_VALIDITY_VAL) || defined(ARTS_VALIDITY_INV)
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
#ifdef ARTS_VALIDITY_VAL
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
          /* Marked with no pre-claim write request awaited means the
           * teardown was skipped.  One still awaited is legal at any
           * shutdown (it may be undelivered) and only counted. */
          uint32_t gate =
              atomic_load_explicit(&db->cover_gate, memory_order_relaxed);
          if ((gate & ARTS_COVER_GATE_TD) != 0u) {
            if ((gate & ARTS_COVER_GATE_RWO) != 0u) {
              marked_owed++;
            } else {
              ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu home rests marked for "
                         "teardown with nothing awaited (gate=%x)",
                         (unsigned long)c->db_guid, gate);
              viol++;
            }
          }
        }
#ifdef ARTS_VALIDITY_INV
        if (db->home_initialized) {
          /* The invalidation round.  Unarmed, it is held only by a thread
           * running or finishing it, so once every thread has stopped a held
           * round or a batch left with no ack to finish it has lost its
           * holder, and every later release queues behind it for ever.  An
           * armed round is not reported by itself: its acks may still be in
           * flight when the network stopped. */
          uintptr_t rq =
              atomic_load_explicit(&db->round_queue, memory_order_relaxed);
          uint32_t ra =
              atomic_load_explicit(&db->round_acks, memory_order_relaxed);
          if (ra == 0u && (rq != 0u || db->round_entries != NULL)) {
            ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu round held with no thread "
                       "to end it (queue=%llx)",
                       (unsigned long)c->db_guid, (unsigned long long)rq);
            viol++;
          }
          if (ra != 0u && rq == 0u) {
            ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu round armed with %u acks "
                       "but not held",
                       (unsigned long)c->db_guid, ra);
            viol++;
          }
        }
#endif
        /* The home directory.  A remote holder that has not yet handed the
         * right back, the requests queued behind it and, where the home
         * revokes, an open round (the baton) are each explained by a message
         * still in flight when the network stopped, so none is reported by
         * itself.  Home fields only — a cache-only stub does not carry
         * them. */
        if (db->home_initialized) {
          bool baton = atomic_load_explicit(&db->grant_baton,
                                            memory_order_relaxed) != 0u;
#ifdef ARTS_RETENTION_PURGE
          unsigned int hw =
              atomic_load_explicit(&c->writer_count, memory_order_relaxed);
          /* The home names no holder: its own possession is the directory.
           * The baton is held only by a thread inside the serving tail, and
           * every way out of the tail puts it down, so once every thread has
           * stopped a held baton has lost its holder and every later entry to
           * the tail is refused. */
          if (baton) {
            ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu home rests with its "
                       "transfer baton held (word=%u)",
                       (unsigned long)c->db_guid, hw);
            viol++;
          }
          /* A hand-back parked in the latch is taken by its own arrival or by
           * the baton holder whose put-down its claim makes fail, so one
           * still parked with the baton down has no consumer left, and the
           * right it carries never comes home. */
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
#ifndef ARTS_READ_SOURCE_OWNER
          /* Without the unconfirmed-install marker nothing sets the word's
           * sign bit, so a negative word is a count that underflowed: a
           * release with no matching acquire.  (Under OWNER the marker
           * makes a negative word an install awaiting its confirmation.) */
          if ((int)w < 0) {
            ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu rests with an "
                       "underflowed hold count (word=%u)",
                       (unsigned long)c->db_guid, w);
            viol++;
          }
#endif
#ifdef ARTS_RETENTION_PURGE
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
#ifdef ARTS_RETENTION_PURGE
        if (atomic_load_explicit(&c->pending_grant_return,
                                 memory_order_relaxed) != 0u) {
          ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu rests owing a return of the "
                     "write right",
                     (unsigned long)c->db_guid);
          viol++;
        }
#endif
#endif
#if defined(ARTS_VALIDITY_EXCL) && defined(ARTS_RETENTION_RETAIN)
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
#if defined(ARTS_VALIDITY_EXCL) && defined(ARTS_RETENTION_PURGE) &&            \
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
          /* 5. The home's queues against its lock word.  A request queued
           * with no count on its axis is a state no stopped rank rests in: a
           * write request's node is counted by the transition that follows
           * its push, a read request's node is pushed only once it is
           * counted, and a count goes only with the release of a turn whose
           * grant took its node.  After a quiescent shutdown no waiter is
           * stranded either: a count left on the word with nobody queued
           * behind it is a hold whose fire-and-forget release was still in
           * flight when the network stopped -- a program may shut down while
           * holding -- and nothing can ever wait on it; a count WITH a waiter
           * queued is a release that will never come for someone who asked.
           * A shutdown that left work undone may leave a requester queued
           * behind a turn still held. */
          if (db->home_initialized) {
            uint64_t ls =
                atomic_load_explicit(&db->lock_state, memory_order_relaxed);
            bool rwq = !arts_home_grantreq_queue_empty(&db->rw_waiters);
            bool roq = !arts_lf_stack_empty(&db->ro_waiters);
            if ((rwq && EXCL_STATE_W(ls) == 0u) ||
                (roq && EXCL_STATE_R(ls) == 0u)) {
              ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu has a request queued at "
                         "its home that the lock word does not count "
                         "(rw queued=%d ro queued=%d w=%u r=%u teardown=%u)",
                         (unsigned long)c->db_guid, (int)rwq, (int)roq,
                         EXCL_STATE_W(ls), EXCL_STATE_R(ls),
                         EXCL_STATE_TEARDOWN(ls));
              viol++;
            }
            if (quiescent) {
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
        }
#endif /* ARTS_USE_CXL */
#endif /* ARTS_VALIDITY_FLUSH */
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
               "claimer's pre-claim write request (quiescent=%d)",
               marked_owed, (int)walk_quiescent);
  }
  if (caller_held != 0) {
    ARTS_DEBUG("CALLER-INFO: %u block(s) held by the shutdown's caller(s), "
               "exempt",
               caller_held);
  }
  if (viol != 0 || !walk_quiescent) {
    ARTS_DEBUG("QUIESCENCE-DEBUG: %u violation(s) (shutdown left undone: "
               "queued_edts=%u ooo=%u admitted_edts=%u)",
               viol, ab_queued, ab_ooo, ab_admitted);
  }
#else
  (void)ex;
  (void)ab_queued;
#endif /* ARTS_LOG_LEVEL >= 3 */
}
#endif /* ARTS_DEBUG_TEARDOWN */

/* After every other thread stopped, the words read relaxed.  Each item below
 * is an open request axis or an open round of the home (coherence.h): its
 * count is the one its cut answer would have dropped.  The home's rows read
 * home fields, which a cache-only stub does not carry. */
void arts_db_sweep_open_items(struct arts_db_s *db, arts_shared_ptr_t cb) {
  unsigned int open = 0u;
#if defined(ARTS_VALIDITY_VAL)
  uint64_t s = atomic_load_explicit(&db->cache.req_state, memory_order_relaxed);
  open += VREQ_RO(s);
  open += (VREQ_RW(s) == VREQ_RW_SENT) ? 1u : 0u;
#ifdef ARTS_READ_SOURCE_HOME
  open +=
      atomic_load_explicit(&db->cache.pub_flight, memory_order_relaxed) != 0u
          ? 1u
          : 0u;
#endif
#elif defined(ARTS_VALIDITY_INV)
  uint64_t s =
      atomic_load_explicit(&db->cache.cache_state, memory_order_relaxed);
  open += INV_CACHE_INFLIGHT(s);
  open += (INV_CACHE_RW(s) == INV_RW_SENT) ? 1u : 0u;
  open +=
      atomic_load_explicit(&db->cache.pub_flight, memory_order_relaxed) != 0u
          ? 1u
          : 0u;
  if (db->home_initialized) {
    /* An armed invalidation round. */
    open += atomic_load_explicit(&db->round_acks, memory_order_relaxed) != 0u
                ? 1u
                : 0u;
  }
#elif defined(ARTS_VALIDITY_EXCL)
  uint64_t s =
      atomic_load_explicit(&db->cache.cache_state, memory_order_relaxed);
  open += (CACHE_RO_ST(s) == CACHE_ST_REQ) ? 1u : 0u;
  open += (CACHE_RW_ST(s) == CACHE_ST_REQ) ? 1u : 0u;
#ifdef ARTS_RETENTION_RETAIN
  /* The home's count, held while w is above zero: after the stop, a write
   * request the home took in whose CONFIRM the shutdown cut. */
  if (db->home_initialized &&
      LOCK_W(atomic_load_explicit(&db->lock_state, memory_order_relaxed)) !=
          0u) {
    open += 1u;
  }
#endif
#else
  (void)db;
#endif
#if (defined(ARTS_VALIDITY_VAL) || defined(ARTS_VALIDITY_INV)) &&              \
    !defined(ARTS_RETENTION_PURGE)
  if (db->home_initialized) {
    /* A held baton: once every thread has stopped, a tenure whose transfer
     * awaits its CONFIRM. */
    open += atomic_load_explicit(&db->grant_baton, memory_order_relaxed) != 0u
                ? 1u
                : 0u;
  }
#endif
  while (open-- != 0u) {
    arts_shared_ptr_t h = cb;
    arts_shared_release(&h);
  }
}

#if defined(ARTS_VALIDITY_VAL) || defined(ARTS_VALIDITY_INV) ||               \
    defined(ARTS_VALIDITY_EXCL)
/* Free the read waiters a destroy found still parked, then tear down the
 * inlined home-directory sub-resources.  Runs AFTER the protocol
 * field-destroy. */
void arts_db_cache_common_destroy_post(struct arts_db_cache_s *cache) {
  if (cache == NULL) {
    return;
  }
#if (defined(ARTS_VALIDITY_VAL) || defined(ARTS_VALIDITY_INV)) &&              \
    defined(ARTS_READ_SOURCE_HOME)
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
#if defined(ARTS_VALIDITY_VAL) || defined(ARTS_VALIDITY_INV)
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
