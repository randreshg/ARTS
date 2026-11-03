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

#include "arts/coherence/buffer.h"
#include "arts/coherence/census.h"
#include "arts/coherence/coherence.h"
#include "arts/coherence/directory.h"
#include "arts/coherence/handlers.h"
#include "arts/counter/Preamble.h"
#include "arts/db.h"
#include "arts/memory/regpool.h"
#include "arts/ooo.h"
#include "arts/runtime_state.h"
#include "arts/runtime_types.h"
#include "arts/system/print.h"
#include "arts/system/threads.h"
#include "arts/transport/net.h"
#include "arts/transport/protocol.h"
#include "arts/utils/malloc.h"

/* Rendezvous continuation state for a transfer whose payload travels
 * one-sided: the packet and the write completion may arrive in either order,
 * so both are paired before the install + commit run.  The context borrows
 * the write request's count. */
struct owner_response_landed_ctx_s {
  struct arts_db_s *db; /* the token's descriptor */
  struct arts_db_buffer_s *landing;
  uint64_t version;
  uint64_t data_size;
  uint64_t home_token; /* the transfer's, for the CONFIRM */
};
static void owner_response_commit(struct arts_db_s *db, uint64_t home_token);
static void owner_response_landed_cb(void *arg);
static void owner_response_landed_discard(void *arg);

void arts_handler_db_grant_response(void *payload, size_t size) {
  struct arts_msg_grant_response_packet_s *hdr =
      (struct arts_msg_grant_response_packet_s *)payload;
  if (size < sizeof(*hdr)) {
    ARTS_ERROR("GRANT_RESPONSE of %zu bytes is shorter than its %zu-byte "
               "header",
               size, sizeof(*hdr));
  }
  arts_guid_t db_guid = hdr->db_guid;

  /* Wire layout: header | map (count pairs, inline), and nothing after it: the
   * buffer payload travels one-sided into our advertised landing. */
  char *map_start = (char *)payload + sizeof(*hdr);
  size_t map_size =
      (sizeof(uint32_t) * 2) + ((size_t)hdr->map_entry_count *
                                sizeof(struct arts_msg_rank_version_pair_s));
  if (map_size != size - sizeof(*hdr)) {
    ARTS_ERROR("GRANT_RESPONSE for DB[Guid:%lu] from rank %u: its header "
               "declares a %zu-byte version map, but %zu bytes follow it",
               (unsigned long)db_guid, hdr->header.rank, map_size,
               size - sizeof(*hdr));
  }

  /* An answer: the body runs on the requester's descriptor the token names,
   * under its write request's count.  A retired one (a destroy while the
   * acquire was pending, outside the destroy contract) only ends the landing
   * and drops that count: no CONFIRM will ask for the CONFIRM_ACK that would
   * have closed the request. */
  struct arts_db_s *db = arts_db_from_token(hdr->token, db_guid);
  if (arts_db_token_retired(db, db_guid)) {
    arts_db_rdzv_discard_landing(hdr->rdzv_txid, hdr->data_size,
                                 hdr->rdzv_cookie);
    arts_db_axis_drop(db, 1u);
    return;
  }

  /* The owner-side dedup ledger moves with the grant, so this rank can skip
   * redundant SNAPSHOT_RESPONSE sends to readers that already hold a
   * sufficiently fresh copy.  Loaded here, from the packet, ahead of the
   * install.  A redirect served meanwhile from an earlier tenure's buffer can
   * advance an entry this load overwrites; that only understates what a
   * reader holds, so it costs a payload send, never a stale reply. */
  struct arts_db_cache_s *cache = &db->cache;
  if (!arts_rank_u64_map_load(cache->cached_version, map_start, map_size)) {
    ARTS_ERROR("GRANT_RESPONSE for DB[Guid:%lu] from rank %u: its version map "
               "names more entries than the %u its header declares",
               (unsigned long)db_guid, hdr->header.rank, hdr->map_entry_count);
  }

  struct arts_db_grant_response_args_s a = {
      .sender = hdr->header.rank,
      .version = hdr->version,
      .data_size = hdr->data_size,
      .rdzv_txid = hdr->rdzv_txid,
      .rdzv_cookie = hdr->rdzv_cookie,
      .home_token = hdr->home_token,
  };
  arts_db_grant_response_on(db, &a);
}

void arts_db_grant_response_on(struct arts_db_s *db,
                               const struct arts_db_grant_response_args_s *a) {
  struct arts_db_cache_s *cache = &db->cache;

  /* A hinted first touch may reach its first grant with the size still
   * unlearned (no CTS leg ran); the response's data_size is the sender's
   * descriptor size, valid payload or not. */
  arts_db_cache_learn_size(cache, a->data_size);

  if (a->rdzv_txid != 0) {
    /* Payload travels one-sided: pair this packet with the write completion
     * (either order) and install+commit when both are in. */
    struct owner_response_landed_ctx_s *ctx =
        (struct owner_response_landed_ctx_s *)arts_malloc(sizeof(*ctx));
    ctx->db = db;
    ctx->landing = (struct arts_db_buffer_s *)(uintptr_t)a->rdzv_cookie;
    ctx->version = a->version;
    ctx->data_size = a->data_size;
    ctx->home_token = a->home_token;
    arts_net_rdzv_expect(a->rdzv_txid, a->data_size, owner_response_landed_cb,
                         owner_response_landed_discard, ctx);
    return;
  }

  /* No PUT.  The grant still arrives, so this rank must end with storage of
   * the DB's declared size — an acquire that completes owes its EDT an
   * addressable pointer, and "nobody has written the block" is a statement
   * about its contents.  A landing was advertised exactly when this rank
   * wanted payload, so it is the storage to use: adopt it when the cache has
   * none, return it to the pool when the cache already holds a buffer (its
   * bytes are at least as good as the silence that just arrived).  Then
   * commit. */
  if (a->rdzv_cookie != 0) {
    /* Stamped at version 1 when the server held nothing (it sent
     * ARTS_GRANT_VERSION_NONE): a live image may never carry the version that
     * means "holds nothing", or this rank's first release would mint 1 for
     * real bytes against an invented first image at the same stamp and the
     * genuine install would retreat as stale. */
    (void)arts_db_buf_adopt_landing(
        cache, a->version ? a->version : 1u,
        (struct arts_db_buffer_s *)(uintptr_t)a->rdzv_cookie,
        atomic_load_explicit(&cache->db_size, memory_order_relaxed));
  }
  uint8_t census = arts_db_census_serving_from(a->sender);
  owner_response_commit(db, a->home_token);
  arts_db_census_served(census);
}

/* Post-install tail of the GRANT_RESPONSE handler — everything that must
 * run only once the transferred bytes are in place.  It runs under the write
 * request's count and closes nothing: the CONFIRM_ACK this CONFIRM asks for
 * does, inline in the send on the home rank, so the send is its last access.
 * home_token is the transfer's, which the CONFIRM answers. */
static void owner_response_commit(struct arts_db_s *db, uint64_t home_token) {
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
   * edge across the drain, which counts every parked writer before waking
   * any, so the transfer cannot ship before they are counted and scheduled.
   * An absolute swap would clobber such a decrement and lose the transfer
   * (distributed hang). */
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

  /* No racing INVALIDATE can have reached us yet: the home holds this round's
   * baton until this CONFIRM arrives and starts the next round only from it
   * (the CONFIRM_ACK's piggyback or its own advance).  So incoming_new_owner
   * == NONE here — send CONFIRM unconditionally.  The request word's write
   * axis stays SENT until CONFIRM_ACK's drain clears it, so a fresh RW acquire
   * in the gate window loses the axis claim and parks without issuing a
   * duplicate request. */
  arts_send_db_grant_confirm(db, home_token);
}

/* Rendezvous continuation: transfer payload fully landed — install without a
 * copy, then run the commit tail.  Fires on {packet, write-completion}
 * pairing, either arrival order, on a dispatch-path progress thread. */
static void owner_response_landed_cb(void *arg) {
  struct owner_response_landed_ctx_s *ctx =
      (struct owner_response_landed_ctx_s *)arg;
  struct arts_db_s *db = ctx->db;
  if (arts_db_token_retired(db, db->cache.db_guid)) {
    /* Retired while the pairing was outstanding: end the landing, commit
     * nothing (no CONFIRM, so no CONFIRM_ACK), drop the request's count. */
    arts_db_buf_landing_return(ctx->landing);
    arts_free(ctx);
    arts_db_axis_drop(db, 1u);
    return;
  }
  arts_db_buf_install_landed(&db->cache, ctx->version, ctx->landing,
                             ctx->data_size);
  uint64_t home_token = ctx->home_token;
  arts_free(ctx);
  /* A one-sided payload comes from another rank. */
  uint8_t census = arts_db_census_serving(true);
  owner_response_commit(db, home_token);
  arts_db_census_served(census);
}

/* The teardown found the payload never landed: end the landing, the
 * context's one possession, and commit nothing. */
static void owner_response_landed_discard(void *arg) {
  struct owner_response_landed_ctx_s *ctx =
      (struct owner_response_landed_ctx_s *)arg;
  arts_db_buf_landing_return(ctx->landing);
  arts_free(ctx);
}

/* ===== WB CONFIRM handler (home A) =============================== */

/* CONFIRM at the home, the answer to its transfer: on the home descriptor the
 * home token names, under the baton tenure's count.  The packet's token is
 * the new owner's, which the CONFIRM_ACK answers with (the new owner itself
 * is read from db->pending_install_owner, stored by the baton holder; CONFIRM
 * only confirms the install completed).  The new owner's writers wait for
 * the CONFIRM_ACK, so under the destroy contract the home is installed here;
 * on a retired one it only ends the tenure, and no CONFIRM_ACK resumes
 * writers a teardown freed. */
void arts_handler_db_grant_confirm(
    const struct arts_msg_grant_confirm_packet_s *p) {
  struct arts_db_s *db = arts_db_from_token(p->home_token, p->db_guid);
  if (arts_db_token_retired(db, p->db_guid)) {
    arts_db_grant_end_tenure(db);
    return;
  }
  struct arts_db_cache_s *cache = &db->cache;

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
   * If a next requester is linked, the round advances in the SAME message —
   * the CONFIRM_ACK piggybacks the next transfer target, and the new owner's
   * handler applies the INVALIDATE effect (publish incoming_new_owner +
   * withdraw the sentinel) itself.  With none the ack carries
   * ARTS_NO_PENDING_OWNER and the baton holder's step below decides; a
   * requester that claims after this look gets a standalone INVALIDATE,
   * which can overtake the ack and is absorbed by the unconfirmed marker
   * (arts_handler_db_grant_invalidate).  This handler inherits the hold, so
   * it reads the claim count before it looks. */
  uint64_t seen = atomic_load_explicit(&db->grant_baton, memory_order_acquire);
  unsigned int piggyback = ARTS_NO_PENDING_OWNER;
  struct arts_rdzv_landing_s piggyback_rdzv = {0, 0, 0, 0};
  uint64_t piggyback_have = ARTS_GRANT_VERSION_NONE;
  uint64_t piggyback_token = 0u;
  {
    unsigned int next_owner;
    if (arts_home_grantreq_queue_pop_linked(&db->pending_rw, &next_owner,
                                            &piggyback_rdzv, &piggyback_have,
                                            &piggyback_token)) {
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
  /* The token answers the request of the rank the baton holder named, so
   * only that rank's CONFIRM may carry it. */
  assert(p->header.rank == new_owner &&
         "a CONFIRM comes from the new owner the round named");
  arts_send_db_grant_confirm_ack(db, new_owner, p->token, piggyback,
                                 &piggyback_rdzv, piggyback_have,
                                 piggyback_token);
  if (piggyback != ARTS_NO_PENDING_OWNER) {
    /* Round advanced via the merged ack; the baton stays held until the new
     * owner releases (transfer-commit), as in the standalone-INVALIDATE case.
     * The tenure goes on with its count, which the next CONFIRM may already
     * have dropped: nothing of the descriptor is touched after the send. */
    return;
  }

  /* The ack carried no transfer, so the tenure, with its count, is still
   * this frame's. */
  arts_db_grant_advance(cache, db, true, seen);
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

/* CONFIRM_ACK (new-owner side), the answer that closes this rank's write
 * request: on the descriptor the token names, under that request's count.
 * Home has flipped rw_holder to this rank; it is now safe for this rank's RW
 * EDTs to run and make their writes observable. Clear ARTS_GRANT_UNCONFIRMED
 * from the count, drain the RW waiters deferred at TRANSFER (which closes the
 * request), apply the piggybacked invalidate effect (if the round advanced —
 * see arts_handler_db_grant_confirm), remove the drain guard (the relocated
 * 0-edge ship-check), and drop the request's count last.
 *
 * The packet's new_owner_rank carries the next transfer target when the
 * round advanced, or ARTS_NO_PENDING_OWNER for a plain ack.  Merging the
 * INVALIDATE into this message removes the reorder for a round that advances
 * at the CONFIRM: both effects happen here, in a fixed order, under the drain
 * guard.  A standalone INVALIDATE sent after a plain ack can still overtake
 * it, and the unconfirmed marker absorbs it
 * (arts_handler_db_grant_invalidate).
 */
void arts_handler_db_grant_confirm_ack(
    struct arts_msg_grant_confirm_ack_packet_s *p) {
  struct arts_db_s *db = arts_db_from_token(p->token, p->db_guid);
  if (arts_db_token_retired(db, p->db_guid)) {
    /* Torn down meanwhile: the gated waiters are acquires the destroy
     * contract excludes, freed with the cache and never resumed. */
    arts_db_axis_drop(db, 1u);
    return;
  }
  struct arts_db_cache_s *cache = &db->cache;

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
  if (p->new_owner_rank != ARTS_NO_PENDING_OWNER) {
    cache->incoming_new_owner_rdzv.addr = p->new_owner_rdzv.addr;
    cache->incoming_new_owner_rdzv.key = p->new_owner_rdzv.key;
    cache->incoming_new_owner_rdzv.txid = p->new_owner_rdzv.txid;
    cache->incoming_new_owner_rdzv.cookie = p->new_owner_rdzv.cookie;
    cache->incoming_new_owner_have = p->new_owner_have;
    cache->incoming_new_owner_token = p->new_owner_token;
    cache->incoming_new_owner_home_token = p->home_token;
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
  arts_db_axis_drop(db, 1u);
}

/* ===== Per-model wire-handler bodies =============================== */

/* Cat-B pure body (OoO g_ooo_table[OOO_DB_SNAPSHOT_REQUEST]): the OoO engine
 * has already acquired the home db_s and pinned a ref across this call (cache
 * is its FIRST member), so there is no lookup / NULL-check / defer here. */

/* ===== WB GRANT_INVALIDATE handler (pure body, DIRECT-call) ====== */

/* Pure (item, args) body.  INVALIDATE has no OoO kind: the invalidate target
 * is always the rw_holder, whose CACHE the requester installs before it ever
 * sends GRANT_REQUEST, so the wire dispatcher and the self-send call this
 * body directly.  The dispatcher drops a miss (a cache destroyed outside the
 * contract); the self-send asserts it finds the cache.  cache is the FIRST
 * member of arts_db_s (offset 0), so the item_v handed in IS the cache.
 *
 * When this INVALIDATE arrives the ownership sentinel (+1) is already
 * installed: the post-install rw_holder flip is CONFIRM-driven, so home names
 * a rank as holder only after its GRANT install, and targets it only after
 * that — GRANT(+sentinel) precedes INVALIDATE(-1), they never reorder.  Once
 * confirmed, the decrement that drives the count from positive to EXACTLY 0
 * ships the owner->owner transfer.
 *
 * It can land inside the unconfirmed window, though: a requester that claims
 * the home's baton after the CONFIRM's look gets a standalone INVALIDATE,
 * which nothing orders after the CONFIRM_ACK.  The marker then makes the word
 * negative and the -1 cannot produce 0, so no transfer is shipped early; the
 * guard removal at CONFIRM_ACK owns the edge and reads the target published
 * here, before this decrement. */
void arts_handler_db_grant_invalidate(void *item_v, void *args_v) {
  struct arts_db_cache_s *cache = &((struct arts_db_s *)item_v)->cache;
  struct arts_ooo_args_db_grant_invalidate_s *a =
      (struct arts_ooo_args_db_grant_invalidate_s *)args_v;
  /* Sentinel withdrawal (writer_count -= 1).  The home's baton sends AT MOST
   * ONE GRANT_INVALIDATE to this rank per transfer round, after
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
  cache->incoming_new_owner_token = a->new_owner_token;
  cache->incoming_new_owner_home_token = a->home_token;
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
    struct arts_db_s *db, unsigned int new_owner_rank, uint64_t new_owner_token,
    unsigned int piggyback_new_owner,
    const struct arts_rdzv_landing_s *piggyback_rdzv, uint64_t piggyback_have,
    uint64_t piggyback_token) {
  arts_guid_t db_guid = db->cache.db_guid;
  struct arts_msg_grant_confirm_ack_packet_s p;
  arts_fill_packet_header(&p.header, sizeof(p), MSG_DB_GRANT_CONFIRM_ACK);
  p.header.rank = arts_global_rank_id;
  p.db_guid = db_guid;
  p.token = new_owner_token;
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
  p.new_owner_token = piggyback_token;
  p.home_token =
      piggyback_new_owner != ARTS_NO_PENDING_OWNER ? arts_db_token(db) : 0u;
  if (new_owner_rank == arts_global_rank_id) {
    /* Self-send: the answer, as on the wire, on the descriptor the token
     * names (the home's, this rank's one descriptor of the block), with the
     * packet so the piggybacked invalidate effect (if any) is applied. */
    arts_handler_db_grant_confirm_ack(&p);
    return;
  }
  arts_transport_send_async((int)new_owner_rank, (char *)&p, sizeof(p));
}

