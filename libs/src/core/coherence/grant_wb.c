/* SPDX-License-Identifier: Apache-2.0
 *
 * The WB write policy's half of the migrating sentinel grant.
 *
 * coherence/grant.c holds what every grant-bearing arm shares (the sentinel,
 * the home FIFO, the transfer baton, the owner->owner ship).  This TU holds
 * what the WB write policy decides on top of it: a new owner installs but may
 * NOT run until the home confirms the directory flip (CONFIRM -> CONFIRM_ACK).
 * The gate is load-bearing here and absent under WT for one reason — with no
 * bytes at the home, a reader registered after the install would be redirected
 * to whichever rank the directory still names, and that rank's retained copy is
 * stale the instant the new owner stores.  WT has no such window because the
 * home serves reads itself.
 *
 * Nothing here is protocol-specific: VAL and INV link this same TU.  What a
 * transfer means for the ex-holder's copy IS protocol-specific and is the
 * arts_db_grant_note_ex_holder seam.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "arts/coherence/buffer.h"
#include "arts/coherence/census.h"
#include "arts/coherence/coherence.h"
#include "arts/coherence/directory.h"
#include "arts/coherence/handlers.h"
#include "arts/db.h"
#include "arts/gas/route_table.h"
#include "arts/memory/regpool.h"
#include "arts/ooo.h"
#include "arts/runtime_state.h"
#include "arts/runtime_types.h"
#include "arts/system/print.h"
#include "arts/system/threads.h"
#include "arts/transport/net.h"
#include "arts/transport/protocol.h"
#include "arts/utils/malloc.h"
#include "arts/counter/Preamble.h"

/* Rendezvous continuation state for a transfer whose payload travels
 * one-sided: the packet and the write completion may arrive in either order,
 * so both are paired before the install + commit run. */
struct owner_response_landed_ctx_s {
  arts_shared_ptr_t db_h; /* pin transferred from the handler */
  arts_guid_t db_guid;
  struct arts_db_buffer_s *landing;
  uint64_t version;
  uint64_t data_size;
};
static void owner_response_commit(arts_shared_ptr_t db_h, arts_guid_t db_guid);
static void owner_response_landed_cb(void *arg);

/* ===== start the transfer round =======================================
 * The INVALIDATE target is always rw_holder, which the home names only after
 * that rank's cache install (the creator at DB_CREATE, a new owner at
 * CONFIRM).  The target's cache is therefore provably installed by the time
 * the INVALIDATE arrives, so it is never deferred: the dispatcher and the
 * self-send call the handler body directly. */
void arts_db_owner_start_invalidate_round(
    struct arts_db_cache_s *cache, unsigned int new_owner,
    const struct arts_rdzv_landing_s *new_owner_rdzv, uint64_t new_owner_have) {
  struct arts_db_s *db = arts_db_of_cache(cache);
  unsigned int current_owner =
      atomic_load_explicit(&db->rw_holder, memory_order_acquire);
  /* INVALIDATE target is always rw_holder, which home publishes only after that
   * rank's cache install (creator at DB_CREATE, or the new owner at
   * CONFIRM).  So the target's cache is provably already installed when the
   * INVALIDATE arrives — the WB write policy never defers INVALIDATE; do not
   * route it through dispatch_or_defer (the dispatcher / self-send call the
   * handler body directly, guarded by assert(cache != NULL)). */
  arts_send_db_grant_invalidate(current_owner, cache->db_guid, new_owner,
                                    new_owner_rdzv, new_owner_have);
}


void arts_handler_db_grant_response(void *payload, size_t size) {
  struct arts_msg_grant_response_packet_s *hdr =
      (struct arts_msg_grant_response_packet_s *)payload;
  if (size < sizeof(*hdr)) {
    ARTS_ERROR("GRANT_RESPONSE of %zu bytes is shorter than its %zu-byte "
               "header",
               size, sizeof(*hdr));
  }
  arts_guid_t db_guid = hdr->db_guid;

  /* Pin the db_s for the whole handler body (map deserialize + buffer install +
   * snapshot drains + writer_count RMW): the embedded cache is its FIRST
   * member (offset 0), so one cb ref keeps it alive against a concurrent
   * DESTROY on another progress thread.  Released on every return path. */
  arts_shared_ptr_t db_h = arts_route_table_lookup_db(db_guid);
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(db_h);
  if (db == NULL) {
    /* Only a program outside the destroy contract gets here: the cache this
     * rank installed when it requested was retired by a destroy while the
     * acquire was pending.  The grant then goes into a fresh cache of the
     * destroyed block, which nothing waits on and which outlives the block;
     * it is dropped only when that install loses to an object already
     * retired. */
    db_h = arts_db_cache_stub_install(db_guid, /*db_size=*/0);
    db = (struct arts_db_s *)arts_shared_get(db_h);
    if (db == NULL) {
      arts_shared_release(&db_h);
      /* Consume any in-flight one-sided payload so the pairing table stays
       * leak-free; the landing ends when that payload arrives, or at once
       * when none was sent. */
      arts_db_rdzv_discard_landing(hdr->rdzv_txid, hdr->data_size,
                                   hdr->rdzv_cookie);
      return; /* the install lost to an object already retired — drop. */
    }
  }
  struct arts_db_cache_s *cache = &db->cache;

  /* A hinted first touch may reach its first grant with the size still
   * unlearned (no CTS leg ran); the response's data_size is the sender's
   * descriptor size, valid payload or not. */
  arts_db_cache_learn_size(cache, hdr->data_size);

  /* Wire layout: header | map (count pairs, inline).  The buffer payload does
   * not trail on the wire — it travels one-sided into our advertised landing
   * (a same-rank self-transfer still trails it inline). */
  char *map_start = (char *)payload + sizeof(*hdr);
  size_t map_size =
      (sizeof(uint32_t) * 2) + ((size_t)hdr->map_entry_count *
                                sizeof(struct arts_msg_rank_version_pair_s));
  if (map_size > size - sizeof(*hdr)) {
    ARTS_ERROR("GRANT_RESPONSE for DB[Guid:%lu] from rank %u: its header "
               "declares a %zu-byte version map, but %zu bytes follow it",
               (unsigned long)db_guid, hdr->header.rank, map_size,
               size - sizeof(*hdr));
  }

  /* Reconstruct the owner-side dedup map so this rank can skip redundant
   * SNAPSHOT_RESPONSE sends to readers that already hold a sufficiently fresh
   * copy.  Must happen HERE (the map bytes live in the packet, which is freed
   * after dispatch) — harmless ahead of the install: the map is only consulted
   * once this rank serves REDIRECTs, which requires the ownership this round
   * is still delivering. */
  if (!arts_rank_u64_map_load(cache->cached_version, map_start, map_size)) {
    ARTS_ERROR("GRANT_RESPONSE for DB[Guid:%lu] from rank %u: its version map "
               "names more entries than the %u its header declares",
               (unsigned long)db_guid, hdr->header.rank, hdr->map_entry_count);
  }

  if (hdr->rdzv_txid != 0) {
    /* Payload travels one-sided: pair this packet with the write completion
     * (either order) and install+commit when both are in. */
    struct owner_response_landed_ctx_s *ctx =
        (struct owner_response_landed_ctx_s *)arts_malloc(sizeof(*ctx));
    ctx->db_h = db_h;
    ctx->db_guid = db_guid;
    ctx->landing = (struct arts_db_buffer_s *)(uintptr_t)hdr->rdzv_cookie;
    ctx->version = hdr->version;
    ctx->data_size = hdr->data_size;
    arts_net_rdzv_expect(hdr->rdzv_txid, hdr->data_size,
                         owner_response_landed_cb, ctx);
    return;
  }

  /* No PUT.  The grant still arrives, so this rank must end with storage of
   * the DB's declared size — an acquire that completes owes its EDT an
   * addressable pointer, and "nobody has written the block" is a statement
   * about its contents.  A landing was advertised exactly when this rank
   * wanted payload, so it is the storage to use: adopt it when the cache has
   * none, recycle it when the cache already holds a buffer (its bytes are at
   * least as good as the silence that just arrived).  Then install any inline
   * same-rank payload and commit. */
  if (hdr->rdzv_cookie != 0) {
    /* Stamped at version 1 when the server held nothing (it sent
     * ARTS_GRANT_VERSION_NONE): a live image may never carry the version that
     * means "holds nothing", or this rank's first release would mint 1 for
     * real bytes against an invented first image at the same stamp and the
     * genuine install would retreat as stale. */
    (void)arts_db_buf_adopt_landing(
        cache, hdr->version ? hdr->version : 1u,
        (struct arts_db_buffer_s *)(uintptr_t)hdr->rdzv_cookie,
        atomic_load_explicit(&cache->db_size, memory_order_relaxed));
  }
  char *data_start = map_start + map_size;
  size_t data_size = size - sizeof(*hdr) - map_size;
  if (data_size > 0) {
    arts_db_buf_install(cache, hdr->version, data_start, data_size);
    arts_db_cache_learn_size(cache, data_size);
  }
  uint8_t census = arts_db_census_serving_from(hdr->header.rank);
  owner_response_commit(db_h, db_guid); /* releases db_h */
  arts_db_census_served(census);
}

/* Post-install tail of the GRANT_RESPONSE handler — everything that must
 * run only once the transferred bytes are in place.  db_h is the caller's pin
 * on the db_s; consumed (released) here. */
static void owner_response_commit(arts_shared_ptr_t db_h, arts_guid_t db_guid) {
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(db_h);
  struct arts_db_cache_s *cache = &db->cache;

  /* Install the grant and gate it in ONE atomic publication:
   *   +1  ownership sentinel — this rank holds the grant;
   *   +1  transient DRAIN GUARD, released by the CONFIRM_ACK handler;
   *   ARTS_GRANT_UNCONFIRMED — the home has not flipped rw_holder to us yet,
   *       so this rank holds the data and the sentinel for accounting but no
   *       RW EDT may run: its stores would be observable while the directory
   *       still names the previous owner, and a reader registered in that
   *       window is redirected to that rank's retained, now stale copy.
   *
   * One RMW rather than a flag store plus a count bump is what makes the
   * acquire fast path's CAS a COMPLETE test — there is no instant at which the
   * word says "runnable" while the confirmation says otherwise, and no pair of
   * loads a lose-and-regain round trip can slip between.
   *
   * The jump also never passes through a value a racing INVALIDATE(-1) could
   * catch at 0: with the marker set the word cannot be exactly 0 at all, and
   * after CONFIRM_ACK clears it the guard still holds the count off the zero
   * edge across the per-waiter +1 drain, so the transfer cannot ship before
   * this rank's parked writers are counted and scheduled.  An absolute swap
   * would clobber such a decrement and lose the transfer (distributed hang). */
  atomic_fetch_add_explicit(&cache->writer_count, 2u + ARTS_GRANT_UNCONFIRMED,
                            memory_order_acq_rel);
#if ARTS_DB_CENSUS
  cache->census_install_remote = arts_db_census_serving_remote();
#endif

  /* RW drain is DEFERRED to the CONFIRM_ACK handler (home has not flipped
   * rw_holder to us yet).  Reads are not: a read waiter resumed here gets
   * the transferred (pre-write) version, which is correct. */
#ifdef ARTS_PROTOCOL_VAL
  /* Read waiters batched while this rank had no local copy resume here
   * against the transferred buffer instead of re-fetching remotely. */
  arts_db_ro_combine_grant_drain(cache);
#endif

  /* No racing INVALIDATE can have reached us yet: home targets this rank as an
   * INVALIDATE recipient only after the rw_holder flip, which needs this
   * CONFIRM. So incoming_new_owner == NONE here — send CONFIRM
   * unconditionally.  The request word's write axis stays SENT until
   * CONFIRM_ACK's drain clears it, so a fresh RW acquire in the gate window
   * loses the axis claim and parks without issuing a duplicate request. */
  unsigned int home_rank = arts_guid_get_rank(db_guid);
  arts_send_db_grant_confirm(home_rank, db_guid);

  arts_shared_release(&db_h);
}

/* Rendezvous continuation: transfer payload fully landed — install without a
 * copy, then run the commit tail.  Fires on {packet, write-completion}
 * pairing, either arrival order, on a dispatch-path progress thread. */
static void owner_response_landed_cb(void *arg) {
  struct owner_response_landed_ctx_s *ctx =
      (struct owner_response_landed_ctx_s *)arg;
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(ctx->db_h);
  arts_db_buf_install_landed(&db->cache, ctx->version, ctx->landing,
                             ctx->data_size);
  /* A one-sided payload comes from another rank. */
  uint8_t census = arts_db_census_serving(true);
  owner_response_commit(ctx->db_h, ctx->db_guid); /* releases */
  arts_db_census_served(census);
  arts_free(ctx);
}

/* ===== WB CONFIRM handler (home A) =============================== */

/* Cat-C pure body (CONFIRM, home side).  The wire dispatcher / self-send
 * shortcut has already looked the home db_s up with a held ref and passes it as
 * item_v (cache is its FIRST member, offset 0).  No lookup/NULL-check here —
 * the dispatcher's MISS branch SILENTLY DROPS (DB destroyed).  args_v is unused
 * (the new owner is read from db->pending_install_owner, stored by the baton
 * holder; CONFIRM only confirms the install completed). */
void arts_handler_db_grant_confirm(void *item_v, void *args_v) {
  (void)args_v;
  struct arts_db_cache_s *cache = &((struct arts_db_s *)item_v)->cache;
  struct arts_db_s *db = arts_db_of_cache(cache);

  /* Publish the new rw_holder (visible to the read redirect path). */
  unsigned int new_owner =
      atomic_load_explicit(&db->pending_install_owner, memory_order_relaxed);
  unsigned int prev_holder =
      atomic_load_explicit(&db->rw_holder, memory_order_acquire);
  atomic_store_explicit(&db->rw_holder, new_owner, memory_order_release);
  /* Protocol seam: the ex-holder keeps the bytes it wrote.  Whether that
   * retained copy needs registering is the protocol's question, not the
   * write policy's. */
  arts_db_grant_note_ex_holder(db, prev_holder);

  /* The directory now names the new owner: tell it to run its gated RW EDTs.
   * If a next requester is queued, the round advances in the SAME message —
   * the CONFIRM_ACK piggybacks the next transfer target, and the new owner's
   * handler applies the INVALIDATE effect (publish incoming_new_owner +
   * withdraw the sentinel) itself.  One message rather than a CONFIRM_ACK
   * and a separate INVALIDATE, so the two can never reach the new owner
   * reordered.  With no pending requester the ack carries
   * ARTS_NO_PENDING_OWNER and the baton is released below. */
  unsigned int piggyback = ARTS_NO_PENDING_OWNER;
  struct arts_rdzv_landing_s piggyback_rdzv = {0, 0, 0, 0};
  uint64_t piggyback_have = ARTS_GRANT_VERSION_NONE;
  {
    unsigned int next_owner;
    if (arts_home_grantreq_queue_pop(&db->pending_rw, &next_owner,
                                    &piggyback_rdzv, &piggyback_have)) {
      atomic_store_explicit(&db->pending_install_owner, next_owner,
                            memory_order_relaxed);
      piggyback = next_owner;
      /* The next owner's waiters are not woken here: they wake when the grant
       * reaches it — the current owner ships it at its writer_count 0-edge
       * (arts_db_send_grant_response), the earliest moment the next owner's
       * acquisition of this db is irrevocably committed, and the next
       * owner's CONFIRM_ACK drains them.  Waking them at round start, before
       * the current owner has released, could strand them in a hold-and-wait
       * cycle. */
    }
  }
  arts_send_db_grant_confirm_ack(new_owner, cache->db_guid, piggyback,
                                     &piggyback_rdzv, piggyback_have);
  if (piggyback != ARTS_NO_PENDING_OWNER) {
    /* Round advanced via the merged ack; the baton stays held until the new
     * owner releases (transfer-commit), as in the standalone-INVALIDATE case.
     */
    return;
  }

  /* No pending requester — what the home does next is the release policy's. */
  arts_db_grant_round_close(cache, db);
}

/* ===== WB CONFIRM_ACK handler (new owner C) ==================== */

/* Whether the grant this rank installed last came from another rank. */
static bool install_was_remote(const struct arts_db_cache_s *cache) {
#if ARTS_DB_CENSUS
  return cache->census_install_remote;
#else
  (void)cache;
  return false;
#endif
}

/* Cat-C pure body (CONFIRM_ACK, new-owner side). Home has flipped
 * rw_holder to this rank; it is now safe for this rank's RW EDTs to run and
 * make their writes observable. Clear ARTS_GRANT_UNCONFIRMED from the count,
 * drain the RW waiters deferred at TRANSFER, apply the piggybacked invalidate
 * effect (if the round advanced — see arts_handler_db_grant_confirm), and
 * remove the drain guard (the relocated 0-edge ship-check).
 *
 * args_v is the CONFIRM_ACK packet: its new_owner_rank carries the next
 * transfer target when the round advanced, or ARTS_NO_PENDING_OWNER for a
 * plain ack.  Merging the INVALIDATE into this message removes the former
 * CONFIRM_ACK↔INVALIDATE reorder window: both effects now happen here, in a
 * fixed order, under the drain guard.
 */
void arts_handler_db_grant_confirm_ack(void *item_v, void *args_v) {
  struct arts_db_cache_s *cache = &((struct arts_db_s *)item_v)->cache;
  struct arts_msg_grant_confirm_ack_packet_s *p =
      (struct arts_msg_grant_confirm_ack_packet_s *)args_v;

  /* Open the gate: home has flipped rw_holder to this rank, so the hold it
   * already owns becomes runnable and fresh RW acquires may take the fast
   * path.  Clearing the marker is an atomic SUB of ARTS_GRANT_UNCONFIRMED, not
   * a masking store: it is exact mod 2^32 whatever the low bits hold and it
   * commutes with any concurrent ±1, so the count underneath survives
   * untouched. */
  atomic_fetch_sub_explicit(&cache->writer_count, ARTS_GRANT_UNCONFIRMED,
                            memory_order_acq_rel);

  /* Drain the RW waiters that the GRANT_RESPONSE handler deferred (this is the
   * work moved out of arts_handler_db_grant_response); the drain takes them
   * and then releases the coalescing flag. */
  uint8_t census = arts_db_census_serving(install_was_remote(cache));
  arts_db_drain_pending_rw_after_grant(cache);
  arts_db_census_served(census);

  /* Piggybacked INVALIDATE effect (merged from the former standalone
   * GRANT_INVALIDATE).  Mirror the INVALIDATE handler's discipline: publish
   * the transfer target BEFORE withdrawing the sentinel (the publish-before-sub
   * is the dedup against a concurrent release_rw that observes the 0-edge).
   * Do NOT ship on this sub's edge: the drain guard (+1) is still held, so the
   * count is >= 1 here and the 0-crossing is deferred to the guard-removal
   * below — exactly the invariant the held guard always provided. */
  if (p != NULL && p->new_owner_rank != ARTS_NO_PENDING_OWNER) {
    cache->incoming_new_owner_rdzv.addr = p->new_owner_rdzv.addr;
    cache->incoming_new_owner_rdzv.key = p->new_owner_rdzv.key;
    cache->incoming_new_owner_rdzv.txid = p->new_owner_rdzv.txid;
    cache->incoming_new_owner_rdzv.cookie = p->new_owner_rdzv.cookie;
    cache->incoming_new_owner_have = p->new_owner_have;
    cache->incoming_new_owner = p->new_owner_rank;
    /* sentinel withdrawal */
    atomic_fetch_sub_explicit(&cache->writer_count, 1u, memory_order_acq_rel);
  }

  /* Remove the drain guard held across the CONFIRM→CONFIRM_ACK round trip. If
   * the round advanced (sentinel withdrawn above) and that drives the count to
   * 0 with no local writer remaining, we are the unique actor that ships
   * GRANT_RESPONSE to the next owner.  Otherwise this rank retains
   * ownership and its drained EDTs ship on their own release 0-edge. */
  if (atomic_fetch_sub_explicit(&cache->writer_count, 1u,
                                memory_order_acq_rel) == 1u &&
      cache->incoming_new_owner != ARTS_NO_PENDING_OWNER) {
    arts_db_grant_ship_pending(cache);
  }
}

/* ===== Per-model wire-handler bodies =============================== */

/* Cat-B pure body (OoO g_ooo_table[OOO_DB_SNAPSHOT_REQUEST]): the OoO engine
 * has already acquired the home db_s and pinned a ref across this call (cache
 * is its FIRST member), so there is no lookup / NULL-check / defer here. */

/* ===== Ownership-round seams (called from coherence/grant.c) ==
 * family→protocol: the ownership-family GRANT_REQUEST handler
 * delegates the WT/WB-divergent steps here. */

void arts_db_start_grant_round(struct arts_db_cache_s *cache,
                                   struct arts_db_s *db,
                                   unsigned int requester) {
  (void)requester;
  /* WB: pop the OLDEST requester (FIFO) to be the transfer target and
   * embed its rank in the GRANT_INVALIDATE so the current holder ships
   * GRANT_RESPONSE directly, without a home round-trip. */
  unsigned int next_owner;
  struct arts_rdzv_landing_s next_rdzv;
  uint64_t next_have = ARTS_GRANT_VERSION_NONE;
  while (!arts_home_grantreq_queue_pop(&db->pending_rw, &next_owner,
                                       &next_rdzv, &next_have)) {
    /* Empty despite our own push: an earlier round already served it (rounds
     * can complete between the push and this claim).  Release the baton with
     * the SAME re-check discipline as the round close: a requester that
     * pushed and lost the baton race while we held it would otherwise stay
     * queued forever with no baton holder to serve it. */
    atomic_store_explicit(&db->invalidate_in_flight, 0u, memory_order_release);
    /* seq_cst fence: store->load pair with arts_handler_db_grant_request,
     * which pushes onto this FIFO and passes a fence before its baton
     * check.  Of the two fences in S the later one's side sees the other's
     * store: either the probe below finds that request, or that handler's
     * baton CAS reads this release. */
    atomic_thread_fence(memory_order_seq_cst);
    if (!arts_home_grantreq_queue_pending(&db->pending_rw)) {
      return;
    }
    unsigned int expected = 0u;
    if (!atomic_compare_exchange_strong_explicit(
            &db->invalidate_in_flight, &expected, 1u, memory_order_acq_rel,
            memory_order_acquire)) {
      return; /* another claimant owns the queue now */
    }
    INCREMENT_NUM_GRANT_BATON_RECLAIM_BY(1);
  }
  atomic_store_explicit(&db->pending_install_owner, next_owner,
                        memory_order_relaxed);
  arts_db_owner_start_invalidate_round(cache, next_owner, &next_rdzv,
                                       next_have);
  /* The next owner's waiters are not woken at round start: they wake at that
   * owner's CONFIRM_ACK, after the grant the current owner ships at its
   * 0-edge (arts_db_send_grant_response) has reached it. */
}

/* ===== WB GRANT_INVALIDATE handler (pure body, DIRECT-call) ====== */

/* Pure (item, args) body.  The WB write policy does NOT route INVALIDATE through
 * the OoO engine
 * (its engine slot is an inert no-op): the invalidate target is always the
 * rw_holder, whose CACHE the requester stub-installs before it ever sends
 * GRANT_REQUEST, so the cache is present and the wire dispatcher /
 * self-send shortcut call this body directly (guarded by assert(cache !=
 * NULL)).  cache is the FIRST member of arts_db_s (offset 0), so the item_v
 * handed in IS the cache.
 *
 * When this INVALIDATE arrives the ownership sentinel (+1) is already
 * installed and CONFIRMED: the post-install rw_holder flip is CONFIRM-driven,
 * so home names a rank as holder only after its GRANT install, and targets it
 * only after that — GRANT(+sentinel) precedes INVALIDATE(-1), they never
 * reorder.  The count is therefore a positive one here, and the decrement that
 * drives it from positive to EXACTLY 0 ships the owner->owner transfer.
 *
 * Should this ever land inside the unconfirmed window instead, the marker
 * makes the word negative and the -1 cannot produce 0, so no transfer is
 * shipped early; the guard removal at CONFIRM_ACK owns the edge either way. */
void arts_handler_db_grant_invalidate(void *item_v, void *args_v) {
  struct arts_db_cache_s *cache = &((struct arts_db_s *)item_v)->cache;
  struct arts_ooo_args_db_grant_invalidate_s *a =
      (struct arts_ooo_args_db_grant_invalidate_s *)args_v;
  /* Sentinel withdrawal (writer_count -= 1).  Home's invalidate_in_flight gate
   * sends AT MOST ONE GRANT_INVALIDATE to this rank per transfer round, after
   * rw_holder has been advanced to a rank that already holds the sentinel (+1).
   * The decrement that drives writer_count to 0 is the unique actor that
   * performs the ownership transfer; while local writers are still active
   * (rest > 0) the last release_rw drives it instead.
   *
   * WB: publish the transfer target BEFORE withdrawing the sentinel.  This
   * ordering is the dedup (no separate transfer_pending flag): a concurrent
   * release_rw that observes rest==0 is guaranteed to see incoming_new_owner
   * already published, so exactly one of {this handler, the last releaser}
   * ships.  Only one GRANT_INVALIDATE is in flight per round (home baton
   * gate), so there is no concurrent writer to incoming_new_owner. */
  cache->incoming_new_owner_rdzv = a->new_owner_rdzv;
  cache->incoming_new_owner_have = a->new_owner_have;
  cache->incoming_new_owner = a->new_owner_rank;
  /* Ship ONLY on the exact 0 edge (see the handler header). */
  int rest = (int)(atomic_fetch_sub_explicit(&cache->writer_count, 1u,
                                             memory_order_acq_rel) -
                   1u);
  if (rest != 0) {
    /* Local writers still active — the last release_rw, seeing rest==0 with
     * incoming_new_owner already published, ships instead. */
    return;
  }
  /* rest == 0: we are the unique transfer actor. */
  arts_db_grant_ship_pending(cache);
}

/* ===== WB wire senders =================================================
 * The GRANT_RESPONSE wire sender, the owner→owner ship and the CONFIRM
 * sender are shared (coherence/grant.c); only the WB-only CONFIRM_ACK sender
 * is here.  The versioned-snapshot redirect, handler and sender, is VAL's
 * (coherence/val/wb.c). */

void arts_send_db_grant_confirm_ack(
    unsigned int new_owner_rank, arts_guid_t db_guid,
    unsigned int piggyback_new_owner,
    const struct arts_rdzv_landing_s *piggyback_rdzv, uint64_t piggyback_have) {
  struct arts_msg_grant_confirm_ack_packet_s p;
  arts_fill_packet_header(&p.header, sizeof(p), MSG_DB_GRANT_CONFIRM_ACK);
  p.header.rank = arts_global_rank_id;
  p.db_guid = db_guid;
  p.new_owner_rank = piggyback_new_owner;
  if (piggyback_rdzv != NULL) {
    p.new_owner_rdzv.addr = piggyback_rdzv->addr;
    p.new_owner_rdzv.key = piggyback_rdzv->key;
    p.new_owner_rdzv.txid = piggyback_rdzv->txid;
    p.new_owner_rdzv.cookie = piggyback_rdzv->cookie;
  } else {
    p.new_owner_rdzv = (struct arts_msg_rdzv_landing_s){0, 0, 0, 0};
  }
  p.new_owner_have = piggyback_have;
  if (new_owner_rank == arts_global_rank_id) {
    /* Self-send: mirror the wire RX dispatcher's Cat-C lookup-acquire-or-drop.
     * HIT runs the confirm_ack body on the ref-pinned db_s, passing the packet
     * so the piggybacked invalidate effect (if any) is applied; MISS (DB
     * destroyed) silently drops: the gated waiters are acquires the destroy
     * contract excludes, freed with the cache and never resumed. */
    arts_shared_ptr_t h = arts_route_table_lookup_db(db_guid);
    struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(h);
    if (db != NULL) {
      arts_handler_db_grant_confirm_ack(db, &p);
    }
    arts_shared_release(&h);
    return;
  }
  arts_transport_send_async((int)new_owner_rank, (char *)&p, sizeof(p));
}

