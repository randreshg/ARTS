/* SPDX-License-Identifier: Apache-2.0
 *
 * The WT write policy's half of the migrating grant.
 *
 * coherence/grant.c holds what every grant-bearing arm shares (the local fast
 * path, the home FIFO, the transfer baton, the grant's ship).  This TU holds
 * what the WT write policy decides on top of it: a new owner installs, drains,
 * and RUNS immediately — there is no confirm gate, because the home serves
 * reads, so no reader can be sent to a stale copy while the directory catches
 * up.  (The WB write policy gates on CONFIRM_ACK for exactly that reason; see
 * its own TU.)  Where the home revokes (RETAIN), the new owner then CONFIRMs
 * its install so the home can name it as the holder a later revocation
 * targets; where the holder hands the right back unasked (PURGE), the home
 * revokes no one and names no holder, and the hand-back is the only message
 * the new owner owes it.
 *
 * Nothing here is protocol-specific — VAL and INV link this same TU.  What a
 * transfer means for the ex-holder's copy is the protocol's business, and
 * none of it happens here: the ex-holder keeps the bytes it wrote, VAL's
 * readers re-check a version at every acquire so that copy is harmless, and
 * INV keeps every releaser registered as a sharer in the round it releases
 * through (inv/engine.c), so the new owner's first release already retires
 * it — nothing this TU could add at the transfer or the CONFIRM would be
 * earlier than that, and a registration made here was erased by the
 * ex-holder's own in-flight release round.  The arts_db_grant_note_ex_holder
 * seam belongs to the policies that do have a point of their own between
 * the ex-holder's last round and the new owner's first: WB (its gated
 * CONFIRM) and PURGE (the hand-back).
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "arts/coherence/buffer.h"
#include "arts/coherence/coherence.h"
#include "arts/coherence/directory.h"
#include "arts/coherence/handlers.h"
#include "arts/db.h"
#include "arts/gas/route_table.h"
#include "arts/ooo.h"
#include "arts/runtime_state.h"
#include "arts/runtime_types.h"
#include "arts/system/print.h"
#include "arts/system/schedfuzz.h"
#include "arts/system/threads.h"
#include "arts/transport/net.h"
#include "arts/transport/protocol.h"
#include "arts/utils/malloc.h"
#include "arts/counter/Preamble.h"

/* ===== Ownership-round seams (called from coherence/grant.c) ==
 * family→protocol: the ownership-family GRANT_REQUEST handler
 * delegates the WT/WB-divergent steps here.  Only the revoking release policy
 * opens a round. */

#ifndef ARTS_RELEASE_PURGE
void arts_db_start_grant_round(struct arts_db_cache_s *cache,
                                   struct arts_db_s *db,
                                   unsigned int requester) {
  (void)requester;
  /* Pop the OLDEST requester (FIFO) to be the transfer target, record it in
   * pending_install_owner, and INVALIDATE the current holder carrying the new
   * owner so the holder ships the owner→owner GRANT_RESPONSE directly.
   * Structurally identical to the WB start round; the WT / WB divergence is
   * only the drain point in the response/confirm handlers. */
  unsigned int next_owner;
  struct arts_rdzv_landing_s next_rdzv;
  while (!arts_home_grantreq_queue_pop(&db->pending_rw, &next_owner,
                                       &next_rdzv, NULL)) {
    /* Empty despite our own push: an earlier round already served it (rounds
     * can complete between the push and this claim).  Release the baton with
     * the SAME re-check discipline as the round close: a requester that
     * pushed and lost the baton race while we held it would otherwise stay
     * queued forever with no baton holder to serve it. */
    arts_sched_fuzz_point(); /* widen the pop-empty<->release window */
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
  unsigned int current_owner =
      atomic_load_explicit(&db->rw_holder, memory_order_acquire);
  arts_send_db_grant_invalidate(current_owner, cache->db_guid, next_owner,
                                    &next_rdzv, ARTS_GRANT_VERSION_NONE);
}
#endif /* !ARTS_RELEASE_PURGE */

/* ===== WT GRANT_RESPONSE handler (new owner C) ================ */

/* WT converged onto the WB owner→owner transfer, with the CRITICAL
 * divergence: WT drains pending_rw + runs its RW EDTs IMMEDIATELY here (no
 * CONFIRM_ACK gate — home serves RO, so there is no stale-RO window to close),
 * then, where the home revokes, sends CONFIRM to home (flips rw_holder +
 * advances the next round). */
/* Post-install tail of the GRANT_RESPONSE (GRANT) handler — everything
 * that must run only once the transferred bytes are in place.  db_h is the
 * caller's pin on the db_s; consumed (released) here. */
static void home_response_commit(arts_shared_ptr_t db_h) {
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(db_h);
  struct arts_db_cache_s *cache = &db->cache;

  /* Possession + drain guard in one transition: the guard keeps a hold on the
   * word across the drains below so nothing can reach the zero edge mid-drain
   * and hand the grant on prematurely.  Where the home revokes, the guard is
   * removed at the commit's close; where the right goes back unasked, the
   * writer drain turns it into the woken writers' holds. */
  arts_db_grant_install(cache);

  /* Readers parked before this grant resume against the bytes it installed,
   * and must do so while the install's own hold is still on the word: that
   * hold keeps the right on this rank, so no write elsewhere can be ordered
   * before a reader resumed here.  The writer drain below may turn the hold
   * into the woken writers' holds, and where a holder hands the right back
   * unasked the last of them can give it away while this thread is still
   * running — so the reader drains come first. */
  arts_db_drain_publication_waiters(cache);
#ifdef ARTS_PROTOCOL_VAL
  /* Read waiters batched while this rank had no local copy resume here
   * against the buffer it now holds instead of re-fetching remotely — the
   * bytes that arrived with the grant, or the ones already here that a
   * permission-only grant certified as current. */
  arts_db_ro_combine_grant_drain(cache);
#endif

  /* WT drains + runs NOW (no confirm-ack gate): home serves RO so there is
   * no stale-RO window. */
  unsigned int drained = arts_db_grant_commit_drain(cache);

#ifndef ARTS_RELEASE_PURGE
  /* Tell the home we installed: it names this rank as the holder only now,
   * so a revocation never reaches a rank that has not installed, and the
   * round it advances may revoke this rank at once. */
  arts_send_db_grant_confirm(arts_guid_get_rank(cache->db_guid),
                             cache->db_guid);
#endif

  /* Close the commit.  Whether that means dropping a hold the commit kept for
   * itself, or nothing at all because the hold already became the waiters',
   * is the release policy's — as is what reaching the edge with no local
   * writer left implies: shipping onward to a target a round already named, or
   * handing the right straight back. */
  arts_db_grant_commit_finish(cache, drained);

  arts_shared_release(&db_h);
}

/* Rendezvous continuation: the transfer payload has fully landed in the
 * advertised landing buffer ("imm seen => landing valid"); install it without
 * a copy and run the commit tail.  Fires on {response packet, write
 * completion} pairing, in either arrival order, on a dispatch-path progress
 * thread. */
struct home_response_landed_ctx_s {
  arts_shared_ptr_t db_h; /* pin transferred from the handler */
  struct arts_db_buffer_s *landing;
  uint64_t version;
  uint64_t data_size;
};

static void home_response_landed_cb(void *arg) {
  struct home_response_landed_ctx_s *ctx =
      (struct home_response_landed_ctx_s *)arg;
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(ctx->db_h);
  arts_db_buf_install_landed(&db->cache, ctx->version, ctx->landing,
                             ctx->data_size);
  home_response_commit(ctx->db_h); /* releases */
  arts_free(ctx);
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

  /* Pin the db_s across the install + commit (and, on the rendezvous path,
   * across the pairing wait): the embedded cache is its FIRST member (offset
   * 0), so one cb ref keeps it alive against a concurrent DESTROY on another
   * progress thread.  Released by the commit tail. */
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
      arts_db_rdzv_discard_landing(hdr->rdzv_txid, hdr->rdzv_cookie);
      return; /* the install lost to an object already retired — drop. */
    }
  }
  struct arts_db_cache_s *cache = &db->cache;

  /* A hinted first touch may reach its first grant with the size still
   * unlearned (no CTS leg ran); the response's data_size is the sender's
   * descriptor size, valid payload or not. */
  arts_db_cache_learn_size(cache, hdr->data_size);

  /* Wire layout: header | map (count pairs, inline).  WT ignores the map
   * (it dedups RO via home->cached_version), but parses past it. */
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

  if (hdr->rdzv_txid != 0) {
    /* The payload travels one-sided: pair this packet with the write
     * completion (either may arrive first) and install+commit when both are
     * in.  The landing is our own buffer, named by the echoed cookie. */
    struct home_response_landed_ctx_s *ctx =
        (struct home_response_landed_ctx_s *)arts_malloc(sizeof(*ctx));
    ctx->db_h = db_h;
    ctx->landing =
        (struct arts_db_buffer_s *)(uintptr_t)hdr->rdzv_cookie;
    ctx->version = hdr->version;
    ctx->data_size = hdr->data_size;
    arts_net_rdzv_expect(hdr->rdzv_txid, home_response_landed_cb, ctx);
    return;
  }

  /* No PUT.  The grant still arrives, so this rank must end with storage of
   * the DB's declared size — an acquire that completes owes its EDT an
   * addressable pointer, and "nobody has written the block" is a statement
   * about its contents.  A landing was advertised exactly when this rank
   * wanted payload (the home rank asks for the permission alone and echoes no
   * cookie, because it already holds the canonical buffer), so it is the
   * storage to use: adopt it when the cache has none, recycle it when the
   * cache already holds a buffer.  Then install any inline same-rank payload
   * and commit. */
  if (hdr->rdzv_cookie != 0) {
    /* Stamped at version 1 when the server held nothing (it sent
     * ARTS_GRANT_VERSION_NONE): a live image may never carry the version that
     * means "holds nothing". */
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
  home_response_commit(db_h); /* releases db_h */
}

/* ===== WT CONFIRM handler (home A) =============================== */

#ifndef ARTS_RELEASE_PURGE
/* Cat-C pure body (CONFIRM, home side).  The wire dispatcher / self-send
 * shortcut has already looked the home db_s up with a held ref and passes it as
 * item_v (cache is its FIRST member).  args_v is unused — the new owner is read
 * from db->pending_install_owner (stored by the baton holder).  WT does
 * NOT send CONFIRM_ACK (the new owner already drained + ran at
 * GRANT_RESPONSE); it only flips rw_holder + advances the next round. */
void arts_handler_db_grant_confirm(void *item_v, void *args_v) {
  (void)args_v;
  struct arts_db_cache_s *cache = &((struct arts_db_s *)item_v)->cache;
  struct arts_db_s *db = arts_db_of_cache(cache);

  unsigned int new_owner =
      atomic_load_explicit(&db->pending_install_owner, memory_order_relaxed);
  atomic_store_explicit(&db->rw_holder, new_owner, memory_order_release);

  /* Advance the queue by revoking the new owner again, or put the baton
   * down. */
  arts_db_grant_round_close(cache, db);
}

/* ===== WT GRANT_INVALIDATE handler (pure body) =================== */

/* Pure (cache, args) body.  The wire dispatcher / self-send shortcut has
 * already looked the cache up (the target is rw_holder, published only at the
 * post-install CONFIRM owner-swap, so the cache is provably installed when
 * INVALIDATE arrives) and passes the db_s as item_v — cache is its FIRST member
 * (offset 0).  No OoO defer is needed: rw_holder flips post-install in both
 * write policies, so no INVALIDATE can reach a rank ahead of the GRANT it
 * revokes.  GRANT(+sentinel) strictly precedes INVALIDATE(-1); the install
 * guard (+2) keeps writer_count non-negative even if the next round's
 * INVALIDATE lands mid-install on another progress thread. */
void arts_handler_db_grant_invalidate(void *item_v, void *args_v) {
  struct arts_db_cache_s *cache = &((struct arts_db_s *)item_v)->cache;
  struct arts_ooo_args_db_grant_invalidate_s *a =
      (struct arts_ooo_args_db_grant_invalidate_s *)args_v;
  /* Publish the transfer target BEFORE withdrawing the sentinel: whichever
   * actor drives writer_count to 0 (this handler, or a concurrent last
   * release_rw) then reads the same new_owner and ships the grant to it.  Same
   * store-before-decrement discipline the WB arm uses for incoming_new_owner;
   * the WT INVALIDATE carries new_owner (home embeds the FIFO front),
   * making the WT and WB transfer paths structurally identical. */
  cache->incoming_new_owner_rdzv = a->new_owner_rdzv;
  cache->incoming_new_owner_have = a->new_owner_have;
  cache->incoming_new_owner = a->new_owner_rank;
  /* Sentinel withdrawal (writer_count -= 1).  Home's invalidate_in_flight gate
   * sends AT MOST ONE GRANT_INVALIDATE to this rank per transfer round, after
   * rw_holder has been advanced to a rank that already holds the sentinel (+1).
   * The decrement that drives writer_count to 0 is the unique actor that
   * performs the ownership transfer; while local writers are still active
   * (rest > 0) the last release_rw drives it instead.
   *
   * writer_count is non-negative.  The post-install rw_holder flip means this
   * notice targets a rank whose sentinel +1 is already installed (GRANT
   * precedes INVALIDATE); the install guard (+2) holds the count >= 0 if the
   * next round's INVALIDATE lands mid-install on another progress thread.  It
   * bottoms out at exactly 0, so the (int) cast is defensive — only the
   * decrement that drives writer_count from a positive value to exactly 0 owns
   * the transfer. */
  int rest = (int)(atomic_fetch_sub_explicit(&cache->writer_count, 1u,
                                             memory_order_acq_rel) -
                   1u);
  if (rest == 0) {
    /* The decrement that drives writer_count to exactly 0 is the unique actor
     * that ships the owner→owner transfer to incoming_new_owner (published
     * above, before the sentinel withdrawal). */
    arts_db_grant_ship_pending(cache);
  }
}
#endif /* !ARTS_RELEASE_PURGE */
