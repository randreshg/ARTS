/* SPDX-License-Identifier: Apache-2.0
 *
 * The migrating sentinel grant — the ownership plane shared by every arm whose
 * write right MOVES between owners rather than being arbitrated per acquire.
 *
 * The grant lives on writer_count.  Under RETAIN it is a per-rank sentinel: an
 * ownership install adds +1, and nothing withdraws it except an INVALIDATE.
 * A grant therefore OUTLIVES the writer that opened it (sticky), and
 * consecutive local writers cost no wire traffic at all.  The home is a pure
 * directory: it queues requesters (pending_rw, Vyukov MPSC), admits one
 * transfer round at a time (invalidate_in_flight baton), and under RETAIN
 * names the holder a revocation targets (rw_holder).  Under RETAIN the
 * payload itself never routes through the home on a transfer — the
 * INVALIDATE carries the next owner, and whichever actor drives writer_count
 * to exactly 0 (the INVALIDATE handler, or the last release) ships owner ->
 * owner directly.  Under PURGE the grant is a possession bit beside the
 * count: the holder hands it back to the home at its own idle edge, and the
 * home grants it onward from there (coherence/grant_purge.c).
 *
 * Nothing here is protocol-specific: what a release does BESIDES decrementing
 * the count (publish the payload to home, run an invalidation round, both, or
 * neither) belongs to the arm, not to the grant.  VAL and INV link this TU;
 * EXCL and FLUSH do not.  EXCL serializes through a single home-side
 * lock_state word instead of a migrating grant, and FLUSH has no write right
 * to move: its programs keep a block's write acquisitions exclusive.
 *
 * Write-policy-divergent steps are delegated to seams the write policy
 * defines (coherence/grant_wt.c, coherence/grant_wb.c):
 * arts_db_start_grant_round and the GRANT_INVALIDATE, GRANT_RESPONSE and
 * CONFIRM handlers, where WT drains immediately and WB gates on CONFIRM_ACK
 * (the round start, INVALIDATE and CONFIRM exist only under RETAIN).
 * Four write-policy guards remain here.  Under WB the ship credits the
 * ex-owner in the dedup ledger (which only VAL WB keeps) and moves no bytes
 * to a new owner that already holds this version; under WT the home
 * materializes its buffer when a request reaches it, and asks for the
 * permission alone when it requests the grant itself.  The INVALIDATE
 * self-send is a direct call under both write policies.
 *
 * Release-policy-divergent steps — who initiates the grant's return, and
 * therefore what a queued request, a round close and a count-dropping edge
 * mean — are delegated to a second seam set (coherence/grant_retain.c,
 * coherence/grant_purge.c) declared in coherence.h.
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "arts/coherence/buffer.h" /* arts_db_buf_acquire (invalidate xfer) */
#include "arts/coherence/coherence.h"
#include "arts/coherence/handlers.h"
#include "arts/coherence/directory.h" /* arts_home_grantreq_queue_push */
#include "arts/db.h"
#include "arts/edt.h"
#include "arts/gas/route_table.h" /* arts_route_table_lookup_db (Cat-C self-send) */
#include "arts/ooo.h"
#include "arts/runtime_state.h"
#include "arts/runtime_types.h"
#include "arts/system/print.h" /* ARTS_ERROR */
#include "arts/system/schedfuzz.h"
#include "arts/system/threads.h"     /* arts_global_rank_id */
#include "arts/transport/net.h"   /* arts_transport_send_async */
#include "arts/transport/protocol.h" /* arts_fill_packet_header, MSG_* */
#include "arts/utils/malloc.h" /* arts_malloc / arts_free (transfer ship) */
#include "arts/counter/Preamble.h"

/* ===== Case 2/6: RW local fast path ================================
 * Shared by every grant-bearing arm's arts_handler_db_acquire body
 * (coherence/val/, coherence/inv/).  CAS-increments writer_count "if
 * runnable"; on success resolves the slot (acquire_local) and returns true;
 * returns false when this rank may not take a write turn, so the caller falls
 * through to arts_db_acquire_remote_rw. */
bool arts_db_acquire_rw_local_fast(struct arts_db_cache_s *cache,
                                   struct arts_edt_s *edt, unsigned int slot) {
  /* The CAS below is the WHOLE test, not a commit behind one.  writer_count
   * carries both facts a fresh write turn needs — a positive count IS "this
   * rank holds the grant", and ARTS_GRANT_UNCONFIRMED makes an install whose
   * directory flip the home has not yet published read back negative — so a
   * turn is taken only if, AT THE INSTANT THE CAS COMMITS, the word is a
   * positive count.  Testing the two facts as separate loads cannot achieve
   * this: a grant lost and re-granted (unconfirmed) between them is
   * indistinguishable from one never lost, and the turn lands on an install
   * that may not run.  Here that interleaving changes the word, the CAS fails,
   * and the loop re-reads.  The predicate is a property of the VALUE, not of
   * its identity, so the value's reuse (ABA) is harmless.
   *
   * `(int)wc <= 0` therefore rejects two distinct states: 0 (no grant — never
   * held, or invalidated) and negative (held but unconfirmed, where a write
   * would be observable while the directory still names the previous owner and
   * a reader is redirected to that rank's retained, now stale copy).  Both go
   * remote-RW, which parks: the in-flight round's drain wakes a waiter pushed
   * before its take, and one pushed after it opens a request of its own. */
  while (1) {
    unsigned int wc =
        atomic_load_explicit(&cache->writer_count, memory_order_acquire);
    if ((int)wc <= 0) {
      arts_db_census_note_held(wc != 0u);
      return false; /* no grant, or one not yet confirmed — go remote-RW. */
    }
    if (atomic_compare_exchange_strong_explicit(&cache->writer_count, &wc,
                                                wc + 1u, memory_order_acq_rel,
                                                memory_order_relaxed)) {
      /* writer_count bumped; release_rw decrements the matching bump.
       * acquire_local may only come back NULL for a zero-sized (sentinel)
       * block: holding the grant on a SIZED block means holding its buffer,
       * and a buffer installed but never published is present at version 0,
       * not absent.  The one exception is a block whose create took no hold:
       * its storage waits for a first user, and the CAS just proved this
       * rank is entitled to be it. */
      (void)arts_db_buf_ensure(
          cache, atomic_load_explicit(&cache->db_size, memory_order_relaxed));
      /* A write turn taken on a grant this rank already held: the round trip
       * the sticky grant removed. */
      INCREMENT_NUM_GRANT_LOCAL_REUSE_BY(1);
      INCREMENT_NUM_DB_ACQUIRE_LOCAL_HIT_BY(1);
      arts_db_acquire_local_hit(edt, slot, arts_db_acquire_local(cache));
      return true;
    }
  }
}

/* ===== Case 4/8: remote-RW path ==================================== */

arts_db_acquire_result_t
arts_db_acquire_remote_rw(struct arts_db_cache_s *cache, struct arts_edt_s *edt,
                          unsigned int slot) {
  /* No destroy precheck: a destroy retires the block's route slot, taking the
   * slot's value to NULL, before anything frees the block, so an acquire
   * issued after the retire misses in the route table and never reaches this
   * cache.  Destroying a DB an EDT still has a pending acquire on is undefined
   * per OCR; a waiter left parked at that point is never woken, and its
   * slot's descriptor pin keeps the descriptor, and so the waiter, until the
   * process ends. */
  /* Park the waiter on the cache's pending-RW Treiber stack.  A grant takes
   * the whole stack at once and wakes every waiter on it, newest first; order
   * is immaterial, since every waiter is woken. */
  struct arts_db_rw_waiter_s *w =
      (struct arts_db_rw_waiter_s *)arts_malloc(sizeof(*w));
  w->edt = edt;
  ARTS_DEP_WAITER_SET(w, edt);
  w->slot = slot;
  ARTS_DB_CENSUS_SET_HELD(w, arts_db_census_take_held());
  arts_pending_rw_queue_push(&cache->pending_rw, w);
  arts_sched_fuzz_point(); /* widen the push<->coalescing-flag-CAS window */
  /* seq_cst fence: store->load pair with arts_db_grant_take_waiters (and
   * this function's own give-back below), whose clear of the flag is
   * followed by a fence and a look at the stack.  Of the two fences in S,
   * the later one's side sees the other's store: either this CAS reads the
   * clear, or that look sees this push. */
  atomic_thread_fence(memory_order_seq_cst);

  /* Open a request only for a waiter no grant covers.  Winning the flag
   * proves the last grant's take already ran (the consumer takes before it
   * clears, and the clear's release pairs with this claim), so an empty
   * stack means a grant served this waiter and nothing is owed; a non-empty
   * one holds only waiters pushed after that take.  With nothing to ask for
   * the flag goes back down, and the stack is looked at once more behind a
   * seq_cst fence: a waiter that pushed meanwhile and lost its CAS to this
   * transient flag is then either seen here or wins the flag itself.  One
   * request per grant, so this counts grants that had to move rather than
   * waiters. */
  uint32_t tok = 0u;
  if (arts_db_rw_req_claim(cache, &tok)) {
    for (;;) {
      arts_sched_fuzz_point(); /* widen the flag-win<->pending-check window */
      if (arts_pending_rw_queue_pending(&cache->pending_rw)) {
        /* The commit is the request: it reads the read request this rank
         * has out in the same atom, and fails if a create's claim reset this
         * OPEN (the token), whose take then served this waiter. */
        uint32_t cover = 0u;
        if (arts_db_rw_req_commit(cache, tok, &cover)) {
          INCREMENT_NUM_GRANT_MIGRATE_BY(1);
          arts_send_db_grant_request(cache, cover);
        }
        break;
      }
      arts_db_rw_req_giveback(cache, tok);
      /* seq_cst fence: the give-back's side of the pair whose other side is
       * a pusher's fence above. */
      atomic_thread_fence(memory_order_seq_cst);
      if (!arts_pending_rw_queue_pending(&cache->pending_rw) ||
          !arts_db_rw_req_claim(cache, &tok)) {
        break;
      }
    }
  }
  return ARTS_DB_ACQUIRE_PARK;
}

/* The arts_handler_db_acquire 8-case body lives per-write-policy in
 * coherence/val/wt.c and coherence/val/wb.c — the two builds differ only on the
 * RO-has-local-data predicate (WT: is_home||is_owner; WB: is_owner), which
 * the C-preprocessor seam forbids in a shared TU.  Both call the shared
 * arts_db_acquire_rw_local_fast / arts_db_acquire_remote_rw above and
 * arts_db_acquire_remote_ro (coherence/coherence.c).
 */

/* ===== GRANT drain (called where a write grant commits on this rank) ===== */

/* Drain callback context for the walk over a taken pending-RW chain. */
struct rw_drain_ctx_s {
  struct arts_db_cache_s *cache;
};

static void rw_drain_cb(const struct arts_db_rw_waiter_s *w, void *vctx) {
  struct rw_drain_ctx_s *ctx = (struct rw_drain_ctx_s *)vctx;
  /* Each woken waiter claims exactly one writer_count slot. */
  atomic_fetch_add_explicit(&ctx->cache->writer_count, 1u,
                            memory_order_acq_rel);
  /* A waiter woken here is being handed the write turn, so it must find
   * storage of the DB's declared size; the one buffer-less case is a
   * zero-sized (sentinel) block, whose defined value is the NULL pointer this
   * wake stamps (still accounting the dep, still advancing the walk). */
  ARTS_DEP_WAITER_CHECK(w->edt, w->edt_guid, w->slot);
  arts_db_resolve_slot_cached(w->edt, w->slot);
}

/* The coalescing flag's consumer half.  The chain is TAKEN before the flag
 * is cleared, and the clear is a release the next claim acquires, so a
 * producer that wins the flag afterwards finds every waiter this grant
 * serves already gone and requests nothing for them; what it does find was
 * pushed after the take, and no grant covers it.  The clear is then
 * followed by a seq_cst fence and one more look at the stack: a waiter that
 * pushed after the take while the flag was still up lost its CAS and relies
 * on this re-check to open its request.  The pusher passes a seq_cst fence
 * between its push and its CAS (arts_db_acquire_remote_rw), so of the two
 * fences in S the later one's side sees the other's store. */
void arts_db_grant_rearm(struct arts_db_cache_s *cache) {
  /* seq_cst fence: the write axis went IDLE (a grant's clear, a claim's
   * CLAIM) before this look at the stack; a pusher's push precedes its own
   * fence and claim.  Of the two fences in S the later one's side sees the
   * other's store, so a waiter the take missed is either seen here or wins
   * the axis itself. */
  atomic_thread_fence(memory_order_seq_cst);
  arts_sched_fuzz_point(); /* widen the clear<->re-check window */
  uint32_t tok = 0u;
  if (arts_pending_rw_queue_pending(&cache->pending_rw) &&
      arts_db_rw_req_claim(cache, &tok)) {
    uint32_t cover = 0u;
    if (arts_db_rw_req_commit(cache, tok, &cover)) {
      INCREMENT_NUM_GRANT_MIGRATE_BY(1);
      arts_send_db_grant_request(cache, cover);
    }
  }
}

arts_lf_link_t *arts_db_grant_take_waiters(struct arts_db_cache_s *cache) {
  arts_lf_link_t *chain = arts_pending_rw_queue_take(&cache->pending_rw);
  arts_db_rw_req_clear(cache);
  arts_db_grant_rearm(cache);
  return chain;
}

static void claim_wake_cb(const struct arts_db_rw_waiter_s *w, void *vctx) {
  struct arts_db_cache_s *cache = (struct arts_db_cache_s *)vctx;
  /* Each writer the create's hold serves takes a hold of its own under it. */
  atomic_fetch_add_explicit(&cache->writer_count, 1u, memory_order_acq_rel);
  ARTS_DEP_WAITER_CHECK(w->edt, w->edt_guid, w->slot);
  arts_db_resolve_slot_cached(w->edt, w->slot);
}

void arts_db_grant_claim_writers(struct arts_db_cache_s *cache) {
  arts_pending_rw_chain_wake(arts_pending_rw_queue_take(&cache->pending_rw),
                             claim_wake_cb, cache);
}

void arts_db_drain_pending_rw_after_grant(struct arts_db_cache_s *cache) {
  struct rw_drain_ctx_s ctx = {.cache = cache};
  arts_pending_rw_chain_wake(arts_db_grant_take_waiters(cache), rw_drain_cb,
                             &ctx);
}

/* ===== Shared owner→owner transfer ship (WT + WB) =============
 * Ship the current buffer to new_owner via the GRANT_RESPONSE wire.  VAL WB
 * serializes its owner-side dedup map; WT and INV keep none (cached_version
 * == NULL) and emit an empty map (count=0): the receiver unconditionally
 * reconstructs map_size >= 8, so an omitted header would underflow data_size
 * and corrupt the install.
 *
 * The target is an argument, not a field read: where it comes from is the
 * release policy's, not the ship's.  A revocation round publishes it on the
 * holder's cache ahead of the withdrawal (arts_db_grant_ship_pending below);
 * a home serving its own queue names the requester it just popped. */
void arts_db_send_grant_response(struct arts_db_cache_s *cache,
                                 unsigned int new_owner,
                                 const struct arts_rdzv_landing_s *new_rdzv,
                                 bool data_less) {
  struct arts_rdzv_landing_s rdzv = *new_rdzv;
  arts_shared_ptr_t buf_h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *buf =
      (struct arts_db_buffer_s *)arts_shared_get(buf_h);

  /* Serialize the owner-side dedup map — small, and it rides INLINE in the
   * response packet (the control plane); only the buffer payload moves
   * one-sided.  An empty count-header is emitted when there is no map: the
   * receiver unconditionally parses map_size >= 8, so omitting it would
   * corrupt the parse. */
  size_t map_max =
      (sizeof(uint32_t) * 2) + ((size_t)arts_global_rank_count *
                                sizeof(struct arts_msg_rank_version_pair_s));
  void *map_buf = arts_malloc(map_max);
  size_t map_size;
#ifdef ARTS_WRITE_POLICY_WB
  /* Self-credit before the map migrates: this (ex-)owner keeps its buffer
   * after the ship, and its self-produced version was never "sent" by anyone —
   * without this entry the inherited ledger would re-ship a payload the
   * ex-owner already holds on its next read.  The slot CAS is safe against the
   * redirect handler. */
  if (buf != NULL && cache->cached_version != NULL) {
    arts_rank_u64_map_advance(
        cache->cached_version, arts_global_rank_id,
        atomic_load_explicit(&buf->version, memory_order_acquire));
  }
#endif
  if (buf != NULL && cache->cached_version != NULL) {
    map_size = arts_rank_u64_map_serialize(cache->cached_version, map_buf);
  } else {
    uint32_t *mp = (uint32_t *)map_buf;
    mp[0] = 0u;
    mp[1] = 0u;
    map_size = sizeof(uint32_t) * 2;
  }

  uint64_t version =
      (buf != NULL) ? atomic_load_explicit(&buf->version, memory_order_acquire)
                    : 0;
  uint64_t db_size =
      atomic_load_explicit(&cache->db_size, memory_order_relaxed);
  /* data_size carries the DB's size ALWAYS (descriptor state — known even
   * when no buffer exists yet): a hinted first-touch requester may have
   * skipped the size CTS, and a data-less grant is its only chance to learn
   * the exact size before its release publishes one.  Payload presence is
   * signaled by rdzv_txid (wire) / trailing bytes (self), never by
   * data_size. */
  uint64_t payload_size = (buf != NULL) ? db_size : 0;
  struct arts_msg_grant_response_packet_s hdr;
  arts_fill_packet_header(&hdr.header, sizeof(hdr) + map_size,
                          MSG_DB_GRANT_RESPONSE);
  hdr.header.rank = arts_global_rank_id;
  hdr.db_guid = cache->db_guid;
  hdr.version = version;
  hdr.map_entry_count = (map_size >= sizeof(uint32_t) * 2)
                            ? ((const uint32_t *)map_buf)[0]
                            : 0u;
  hdr.pad = 0;

#ifdef ARTS_RELEASE_PURGE
  /* Every grant here is issued BY the home, and a home serving itself takes
   * the shortcut that ships nothing at all — so the self-transfer arm below,
   * which copies the whole block through a packet, is unreachable.  It is the
   * only owner→owner data leg this file has left under this release policy,
   * and it must stay unreachable: reaching it would be two full-size copies
   * on the path whose whole claim is that it moves no bytes. */
  assert(new_owner != arts_global_rank_id &&
         "a home that serves itself hands out no bytes");
#endif
  if (new_owner == arts_global_rank_id) {
    /* Self-transfer: no wire, no RDMA.  Ex-owner and new owner are the same
     * cache, so the landing this rank advertised in its own request carries
     * nothing it does not already have — unless there is nothing to have.
     * With a buffer present the bytes are already in place and the landing
     * goes back to the pool; with none, this rank is about to take the write
     * turn on a sized block, so the landing BECOMES its storage (nothing else
     * on this path would give it any).  The handler then runs inline on a
     * contiguous same-rank buffer (map + data trailing). */
    if (rdzv.cookie != 0) {
      struct arts_db_buffer_s *landing =
          (struct arts_db_buffer_s *)(uintptr_t)rdzv.cookie;
      if (buf != NULL) {
        arts_db_buf_landing_return(landing);
      } else {
        /* A first image is stamped at version 1, never 0: on the wire 0 is
         * ARTS_GRANT_VERSION_NONE, "holds nothing", so a live image carrying
         * it could not be told from an absent one — and this rank's first
         * release would then mint 1 for REAL bytes, the same stamp an
         * invented first image elsewhere already carries, which makes a
         * genuine install retreat as stale. */
        (void)arts_db_buf_adopt_landing(cache, version ? version : 1u, landing,
                                        db_size);
      }
    }
    uint64_t total = sizeof(hdr) + map_size + payload_size;
    hdr.header.size = total;
    hdr.data_size = db_size;
    hdr.rdzv_txid = 0;
    hdr.rdzv_cookie = 0;
    char *pkt = (char *)arts_malloc((size_t)total);
    memcpy(pkt, &hdr, sizeof(hdr));
    memcpy(pkt + sizeof(hdr), map_buf, map_size);
    if (buf != NULL && payload_size > 0) {
      memcpy(pkt + sizeof(hdr) + map_size, buf->data, (size_t)payload_size);
    }
    arts_free(map_buf);
    if (buf != NULL) {
      arts_db_buf_release(&buf_h);
    }
    arts_handler_db_grant_response(pkt, (size_t)total);
    arts_free(pkt);
    return;
  }

  if (data_less) {
    /* The new owner already holds these exact bytes, so only the permission
     * needs to move.  Decided here, at the send, because the point is the
     * bytes on the wire: a receiver-side skip would still have paid for
     * them.  The advertised landing is echoed back unused, and the receiver
     * recycles it against the copy it kept. */
    INCREMENT_NUM_GRANT_REGRANT_DEDUP_BY(1);
  }
  if (!data_less && buf != NULL && payload_size > 0 && rdzv.txid != 0) {
    /* One-sided ship: PUT straight from the live buffer into the new owner's
     * landing — zero copy at the source.  The strong buffer ref transfers to
     * the PUT's local completion, keeping the bytes valid until the fabric no
     * longer reads them (a revoking home additionally keeps this cache's
     * buffer slot untouched until the new owner CONFIRMs, but the ref makes
     * the lifetime explicit rather than assumed). */
    hdr.data_size = db_size;
    hdr.rdzv_txid = rdzv.txid;
    hdr.rdzv_cookie = rdzv.cookie;
    arts_net_put_payload((int)new_owner, rdzv.addr, rdzv.key, rdzv.txid,
                         buf->data, payload_size, arts_db_buf_ref_release_cb,
                         (void *)buf_h);
    buf_h = NULL; /* transferred to the PUT completion */
  } else {
    /* Nothing to ship: this ex-owner holds no buffer, or the block is
     * zero-sized.  Echo the advertised landing (if any) back — for a sized
     * block it is the new owner's only storage, and the receiving handler
     * adopts it rather than returning it to the pool; data_size tells that
     * handler the exact size either way. */
    hdr.data_size = db_size;
    hdr.rdzv_txid = 0;
    hdr.rdzv_cookie = rdzv.cookie;
    if (buf != NULL) {
      arts_db_buf_release(&buf_h);
    }
  }
  arts_transport_send_payload_async_free((int)new_owner, (char *)&hdr,
                                         sizeof(hdr), (char *)map_buf,
                                         /*offset=*/0, (uint64_t)map_size,
                                         arts_free);
}

/* Ship to the target a revocation round published on this cache, re-arming
 * the field to the sentinel BEFORE the send: a self-transfer dispatches the
 * new owner's install inline, which can recursively republish the field for
 * the next round, and clearing it up front leaves that fresh publish intact.
 * The field is the channel only where the home revokes a holder; a policy
 * whose grants move through the home never writes it. */
#ifndef ARTS_RELEASE_PURGE
void arts_db_grant_ship_pending(struct arts_db_cache_s *cache) {
  unsigned int new_owner = cache->incoming_new_owner;
  struct arts_rdzv_landing_s rdzv = cache->incoming_new_owner_rdzv;
  uint64_t have = cache->incoming_new_owner_have;
  cache->incoming_new_owner = ARTS_NO_PENDING_OWNER;
  cache->incoming_new_owner_rdzv = (struct arts_rdzv_landing_s){0, 0, 0, 0};
  cache->incoming_new_owner_have = ARTS_GRANT_VERSION_NONE;
  bool data_less = false;
#ifdef ARTS_WRITE_POLICY_WB
  /* The new owner's copy is current when it carries this buffer's version:
   * under write-back the owner's buffer version is the one axis every copy
   * is stamped on, each release advances it before any copy is retired or
   * re-served, and nothing advances it on the 0-edge this ship runs on.  A
   * requester that reported no version holds nothing worth keeping.  Under
   * write-through the copies are stamped on the home's own axis, which this
   * buffer does not carry, so the comparison is not made there. */
  if (have != ARTS_GRANT_VERSION_NONE) {
    arts_shared_ptr_t h = arts_db_buf_acquire(cache);
    struct arts_db_buffer_s *b = (struct arts_db_buffer_s *)arts_shared_get(h);
    data_less = (b != NULL && atomic_load_explicit(
                                  &b->version, memory_order_acquire) == have);
    arts_db_buf_release(&h);
  }
#else
  (void)have;
#endif
  arts_db_send_grant_response(cache, new_owner, &rdzv, data_less);
}
#endif /* !ARTS_RELEASE_PURGE */

/* ===== Home-side ownership handlers (VAL, INV) =======================
 * GRANT_REQUEST exists only in the arms that link this TU.  The
 * home-directory machinery these handlers touch (pending_rw and
 * invalidate_in_flight, and under RETAIN rw_holder) is shared by both write
 * policies; where they diverge is delegated to the seams in
 * coherence/grant_wt.c / grant_wb.c and coherence/grant_retain.c /
 * grant_purge.c. */

/* Cat-B pure body (OoO g_ooo_table[OOO_DB_GRANT_REQUEST]): the OoO engine
 * has already acquired the home db_s for db_guid and pinned a ref across this
 * call, so there is no lookup / NULL-check / defer here.  cache is the FIRST
 * member of arts_db_s (offset 0), so the slot object the engine hands us IS the
 * cache.  The wire dispatcher decodes GRANT_REQUEST into the args struct
 * and routes through the engine via OOO_DB_GRANT_REQUEST; a missing home
 * db_s defers the args and re-issues this body once DB_CREATE installs and
 * drains. */
void arts_handler_db_grant_request(void *item_v, void *args_v) {
  struct arts_db_cache_s *cache = &((struct arts_db_s *)item_v)->cache;
  struct arts_ooo_args_db_grant_request_s *a =
      (struct arts_ooo_args_db_grant_request_s *)args_v;
  unsigned int requester = a->requester;

  struct arts_db_s *db = arts_db_of_cache(cache);
  /* The declared-arrival rule: a write request a create's claim covered is
   * absorbed, and one that declares its rank's read request counts it. */
  {
    bool td = false;
    int r = arts_db_cover_request(db, requester, true, a->cover, &td);
    if (r != ARTS_COVER_PROCESS) {
      /* A write request a claim covered was committed with PRECLAIM, which
       * sends it without a landing: nothing to give back. */
      assert((a->cover & ARTS_COVER_PRECLAIM) == 0u || a->rdzv.cookie == 0u);
      if (td) {
        arts_db_home_destroy_now(db);
      }
      return;
    }
  }
  /* A requester of the write right holds a cache of the block from here on,
   * so the destroy must reach it: its cache would otherwise outlive the
   * block, keeping its storage and its rank's route slot for the GUID. */
  if (requester != arts_global_rank_id) {
    (void)arts_rank_bitset_set(&db->cached_ranks, requester);
  }
  uint64_t db_size =
      atomic_load_explicit(&cache->db_size, memory_order_relaxed);
#ifdef ARTS_WRITE_POLICY_WT
  /* The write-through home holds this block's canonical bytes for its whole
   * lifetime, and from here on it owes them to someone: the grant it is
   * about to hand out carries them, and the holder's release publishes back
   * into them.  A block whose create took no hold has not been given storage
   * yet, and this request is its first use — so materialize it here, where
   * the home is provably entitled to the block's first image. */
  (void)arts_db_buf_ensure(cache, db_size);
#endif
  if (a->rdzv.txid == 0 && requester != arts_global_rank_id && db_size > 0 &&
      arts_global_rank_count > 1) {
    /* First-touch request without a landing: the requester did not know
     * db_size.  Answer with the size (CTS) and do NOT enqueue — home only
     * queues requests that carry a landing (or target a sentinel DB, whose
     * transfers are data-less).  The requester re-issues with a landing.
     * The home's OWN landing-less request is not a size probe: it wants the
     * permission without payload and queues like any other. */
    arts_send_db_grant_cts(requester, cache->db_guid, db_size);
    return;
  }
  arts_home_grantreq_queue_push(&db->pending_rw, requester, &a->rdzv,
                                a->have_version);
  arts_sched_fuzz_point(); /* widen the push<->baton-CAS window */
  /* seq_cst fence: store->load pair with every baton release (the round
   * start's and the round close's under RETAIN, the serving tail's exits
   * under PURGE), each a store of 0 followed by a fence and a probe of this
   * FIFO or of the home's possession.  Of the two fences in S the later
   * one's side sees the other's store: either the releaser's probe finds this
   * request, or the release policy's check below (the baton CAS, or the
   * home's possession) sees the release. */
  atomic_thread_fence(memory_order_seq_cst);

  /* The requester is queued; what the home does about it is the release
   * policy's answer (coherence/grant_retain.c / grant_purge.c). */
  arts_db_grant_request_arrived(cache, db, requester);
}

/* ===== Ownership wire senders (VAL, INV) ===============================
 * GRANT_REQUEST / GRANT_INVALIDATE exist only in the arms that link this TU.
 * The grant itself travels in a GRANT_RESPONSE
 * (arts_db_send_grant_response): owner→owner when the home revokes it
 * (RETAIN), home→requester once the holder has handed it back (PURGE).
 * Self-sends dispatch the matching handler inline.
 */

void arts_send_db_grant_request(struct arts_db_cache_s *cache,
                                uint32_t cover) {
  arts_guid_t db_guid = cache->db_guid;
  unsigned int home_rank = arts_guid_get_rank(db_guid);
  /* Advertise a fresh transfer landing sized by the exact size when known,
   * else by the GUID's szhint bound — a first touch usually carries a
   * landing already, and home's GRANT_CTS round survives only as the
   * sentinel fallback (its handler re-enters this sender with
   * cache->db_size learned).  The rendezvous plane exists only when a peer
   * could PUT (multi-rank run): a single-rank run has no fabric, every
   * transfer is a same-rank inline dispatch, and the registered pool
   * carries no MRs to advertise. */
  struct arts_rdzv_landing_s rdzv = {0, 0, 0, 0};
  /* Under write-back the landing is advertised unconditionally, even though a
   * transfer of a DB nobody has published yet carries no bytes: whether the
   * current owner HAS bytes is remote state, so the only way to know before
   * asking is to ask — a round trip added to every acquire to save one pooled
   * buffer on the rare pre-publication path.  A transfer that ships no data
   * echoes the advertisement back, and the requester keeps it as this block's
   * storage (it is entitled to a pointer of the declared size whether or not
   * anyone has written the block) or returns it to the pool when it already
   * holds a buffer. */
#ifdef ARTS_WRITE_POLICY_WT
  /* The write-through home already holds the newest bytes (every release
   * published them), so its own request needs the PERMISSION only: no
   * landing, and the ex-owner's transfer takes the data-less arm.  The
   * stable home buffer is never re-installed through the grant plane. */
  bool wants_payload = (home_rank != arts_global_rank_id);
#else
  bool wants_payload = true;
#endif
  uint64_t fetch_size = arts_db_first_fetch_size(cache);
  /* A request opened before a create claimed this first-touch cache leaves
   * without a landing, whatever size the cache has learned since: the home
   * absorbs it if that claim was this rank's, and answers a size round
   * otherwise, whose re-send carries the landing. */
  if ((cover & ARTS_COVER_PRECLAIM) != 0u) {
    fetch_size = 0;
  }
  if (wants_payload && fetch_size > 0 && arts_global_rank_count > 1) {
    (void)arts_db_buf_landing_alloc(cache, fetch_size, &rdzv);
  } else if (!wants_payload) {
    INCREMENT_NUM_GRANT_HOME_DATALESS_BY(1);
  }
  /* What this rank already holds.  An unpublished buffer reports the same
   * "nothing worth keeping" as no buffer at all: its contents were never
   * anyone's, so a server must send the bytes either way. */
  uint64_t have_version = ARTS_GRANT_VERSION_NONE;
  {
    arts_shared_ptr_t hv = arts_db_buf_acquire(cache);
    struct arts_db_buffer_s *hb = (struct arts_db_buffer_s *)arts_shared_get(hv);
    if (hb != NULL) {
      have_version = atomic_load_explicit(&hb->version, memory_order_acquire);
    }
    arts_db_buf_release(&hv);
  }
  struct arts_msg_grant_request_packet_s p;
  arts_fill_packet_header(&p.header, sizeof(p), MSG_DB_GRANT_REQUEST);
  p.header.rank = arts_global_rank_id;
  p.db_guid = db_guid;
  if (have_version > ARTS_GRANT_REQ_VERSION_MASK) {
    ARTS_ERROR("grant: guid %lu reached version %llu, past the request's "
               "60-bit version field",
               (unsigned long)db_guid, (unsigned long long)have_version);
  }
  p.have_version =
      have_version | ((uint64_t)cover << ARTS_GRANT_REQ_VERSION_BITS);
  p.rdzv.addr = rdzv.addr;
  p.rdzv.key = rdzv.key;
  p.rdzv.txid = rdzv.txid;
  p.rdzv.cookie = rdzv.cookie;
  if (home_rank == arts_global_rank_id) {
    /* Self-send: route through the OoO engine exactly as the wire RX
     * dispatcher does — HIT runs the GRANT_REQUEST body inline, MISS defers
     * the args and replays once the home db_s is installed + drained.  (The
     * handler is a pure (item, args) body that does no lookup-or-defer of
     * its own, so the inline shortcut must enter through
     * dispatch_or_defer.) */
    struct arts_ooo_args_db_grant_request_s args = {
        .requester = p.header.rank,
        .db_guid = db_guid,
        .rdzv = rdzv,
        .have_version = have_version,
        .cover = cover,
    };
    arts_ooo_dispatch_or_defer_guid(db_guid, OOO_DB_GRANT_REQUEST, &args,
                                    sizeof(args));
    return;
  }
  if (!arts_net_send_held((int)home_rank, (char *)&p, sizeof(p),
                          ARTS_HELD_WRITE, db_guid)) {
    arts_transport_send_async((int)home_rank, (char *)&p, sizeof(p));
  }
}

/* GRANT_CTS sender (home → remote first-touch requester) + requester-side
 * body.  The requester learns db_size and re-issues the in-flight request with
 * a landing; the coalescing flag stays held (same round continuing). */
void arts_send_db_grant_cts(unsigned int requester_rank,
                                arts_guid_t db_guid, uint64_t db_size) {
  INCREMENT_NUM_GRANT_SIZE_CTS_BY(1);
  struct arts_msg_grant_cts_packet_s p;
  arts_fill_packet_header(&p.header, sizeof(p), MSG_DB_GRANT_CTS);
  p.header.rank = arts_global_rank_id;
  p.db_guid = db_guid;
  p.db_size = db_size;
  arts_transport_send_async((int)requester_rank, (char *)&p, sizeof(p));
}

void arts_handler_db_grant_cts(void *item_v, void *args_v) {
  struct arts_db_cache_s *cache = &((struct arts_db_s *)item_v)->cache;
  struct arts_msg_grant_cts_packet_s *p =
      (struct arts_msg_grant_cts_packet_s *)args_v;
  arts_db_cache_learn_size(cache, p->db_size);
  /* Only a request the home did not absorb gets a size round, so this cache
   * is no claim's and never will be: the size round clears its stub bit in
   * the same atom that reads the re-send's cover word, and every later
   * request of this cache carries a landing. */
  arts_send_db_grant_request(cache, arts_db_rw_req_declaim(cache));
}

#ifndef ARTS_RELEASE_PURGE
void arts_send_db_grant_invalidate(
    unsigned int owner_rank, arts_guid_t db_guid, unsigned int new_owner_rank,
    const struct arts_rdzv_landing_s *new_owner_rdzv, uint64_t new_owner_have) {
  struct arts_rdzv_landing_s rdzv =
      (new_owner_rdzv != NULL) ? *new_owner_rdzv
                               : (struct arts_rdzv_landing_s){0, 0, 0, 0};
  struct arts_msg_grant_invalidate_packet_s p;
  arts_fill_packet_header(&p.header, sizeof(p), MSG_DB_GRANT_INVALIDATE);
  p.header.rank = arts_global_rank_id;
  p.db_guid = db_guid;
  p.new_owner_rank = new_owner_rank;
  memset(p.pad, 0, sizeof(p.pad));
  p.new_owner_rdzv.addr = rdzv.addr;
  p.new_owner_rdzv.key = rdzv.key;
  p.new_owner_rdzv.txid = rdzv.txid;
  p.new_owner_rdzv.cookie = rdzv.cookie;
  p.new_owner_have = new_owner_have;
  if (owner_rank == arts_global_rank_id) {
    /* Self-send: under both write policies the INVALIDATE handler runs
     * directly.
     *
     * The home publishes the invalidate target (rw_holder) only AFTER that
     * rank's cache install — the post-install CONFIRM owner-swap or the
     * DB_CREATE on the creator — so an INVALIDATE always targets an
     * already-installed cache and never needs deferring through the OoO
     * engine.
     *
     * Direct (non-deferred) self-dispatch is also safe in the new_owner==home
     * self-CONFIRM chain: GRANT_RESPONSE installs a sentinel(+1)+guard(+1)
     * on writer_count, then self-sends CONFIRM, whose drain loop self-sends
     * THIS INVALIDATE inline.  Because the guard +1 is still present at that
     * point, this inline -1 cannot drive writer_count to the 0-edge
     * (writer_count >= 2 before it) — the commutative signed counter's unique
     * 0-crossing remains the RESPONSE handler's guard-removal -1, which then
     * ships exactly one owner→owner transfer.  Inline vs deferred only moves
     * WHICH actor observes the 0-edge; the guard guarantees it is still exactly
     * one.  Do not re-introduce a defer here. */
    struct arts_ooo_args_db_grant_invalidate_s args = {
        .db_guid = db_guid,
        .new_owner_rank = new_owner_rank,
        .new_owner_rdzv = rdzv,
        .new_owner_have = new_owner_have,
    };
    /* Pin the db_s for the handler's duration (cache is its FIRST member,
     * offset 0) — keeps it alive against a concurrent DESTROY.  Target is the
     * rw_holder, published only post-install, so it is provably present. */
    arts_shared_ptr_t db_h = arts_route_table_lookup_db(db_guid);
    struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(db_h);
    assert(db != NULL); /* target is rw_holder, published post-install */
    arts_handler_db_grant_invalidate(db, &args);
    arts_shared_release(&db_h);
    return;
  }
  arts_transport_send_async((int)owner_rank, (char *)&p, sizeof(p));
}

/* CONFIRM (new owner C → home A), both write policies.  The home flips
 * rw_holder and advances the next round; under WB its handler also replies
 * CONFIRM_ACK, under WT it does not.  The handler reads pending_install_owner
 * and takes no args. */
void arts_send_db_grant_confirm(unsigned int home_rank, arts_guid_t db_guid) {
  struct arts_msg_grant_confirm_packet_s p;
  arts_fill_packet_header(&p.header, sizeof(p), MSG_DB_GRANT_CONFIRM);
  p.header.rank = arts_global_rank_id;
  p.db_guid = db_guid;
  if (home_rank == arts_global_rank_id) {
    /* Self-send: mirror the wire RX dispatcher's Cat-C lookup-acquire-or-drop.
     * HIT advances the transfer round on the ref-pinned home db_s; MISS (DB
     * destroyed) silently drops. */
    arts_shared_ptr_t h = arts_route_table_lookup_db(db_guid);
    struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(h);
    if (db != NULL) {
      arts_handler_db_grant_confirm(db, NULL);
    }
    arts_shared_release(&h);
    return;
  }
  arts_transport_send_async((int)home_rank, (char *)&p, sizeof(p));
}
#endif /* !ARTS_RELEASE_PURGE */
