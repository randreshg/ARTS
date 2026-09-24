/* SPDX-License-Identifier: Apache-2.0
 *
 * Coherence protocol wire-message handlers (receive side) + home-side
 * dedup / ownership-transfer helpers.
 *
 * The matching arts_send_db_* SENDERS (packet-fill + outbox enqueue) live
 * in coherence_senders.c.
 *
 * Lookup discipline.  Two handler categories, both lookup-then-operate but
 * differing on the MISS action:
 *   - Cat-B (deferrable home-side: GRANT_REQUEST / SNAPSHOT_REQUEST / PUBLISH /
 * DESTROY): the wire dispatcher routes through the OoO engine, which acquires
 * the home db_s (ref-pinned) and hands a pure (item, args) body the live cache
 *     on a HIT, or DEFERS the args and replays them once DB_CREATE installs.
 *   - Cat-C (non-deferrable: SNAPSHOT_RESPONSE / DESTROY_NOTIFY / PUBLISH_ACK /
 *     SNAPSHOT_REDIRECT / CONFIRM / CONFIRM_ACK): the wire
 * dispatcher (and the matching self-send shortcut) does the ref-pinned
 *     lookup-acquire; on a HIT it calls the pure (item, args) body, and on a
 *     MISS it applies that handler's exact miss-action (silent drop,
 *     DESTROY_NOTIFY reply, or the PUBLISH_ACK sem-post — see each body).
 * Either way the handler body itself performs NO route-table lookup; it
 * operates on the already-acquired, ref-pinned cache (the FIRST member of the
 * db_s the caller hands it).
 *
 * Memory ordering: ARTS atomics are __sync_*-based (full fence) and
 * the per-rank network thread (S1) is the sole writer of home.*
 * state, so the trickier orderings are confined to:
 *   - cache.writer_count (worker ↔ network handler)
 *   - cache.buffer       (worker ↔ network handler installs)
 *   - cache.pending_count (worker push ↔ marker mark)
 * All accessed via arts_atomic_*, all matching the algorithm in the
 * coherence design plan.
 */

#include "arts/coherence/handlers.h"

#include <assert.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "arts.h" /* ARTS_DB_PROP_NO_ACQUIRE */
#include "arts/coherence/buffer.h"
#include "arts/coherence/coherence.h"
#include "arts/coherence/directory.h"
#include "arts/db.h"
#include "arts/fam/pool.h" /* arts_fam_contains / arts_fam_free (ARTS_FAM only) */
#include "arts/gas/route_table.h"
#include "arts/memory/regpool.h" /* arts_regpool_free (orphaned landing) */
#include "arts/ooo.h"
#include "arts/system/print.h"
#include "arts/system/threads.h"
#include "arts/transport/net.h" /* arts_net_rdzv_expect */
#include "arts/utils/atomics.h"
#include "arts/utils/malloc.h"
#include "arts/utils/shared.h"

/* ===== Home-side handlers ========================================== */

/* arts_handler_db_grant_request lives in coherence/grant.c (VAL only). */

/* arts_handler_db_snapshot_request (SNAPSHOT_REQUEST) is protocol-specific —
 * WT serves from home's canonical buffer (dedup), WB records the
 * sharer + REDIRECTs to the owner — so its whole body lives in
 * each arm's own write-policy TU. */

/* arts_handler_db_publish (+_ack) is protocol-specific — WT installs
 * + ACKs (pure: ownership transfer is a separate owner→owner GRANT_RESPONSE
 * ship), WB has no synchronous publish (no-op fillers preserve the
 * OoO-table / link parity) — so their whole bodies live in
 * each arm's own write-policy TU. */

/* arts_handler_db_destroy is protocol-specific — each arm keeps its own exact
 * destroy roster (the creator, the readers, every write requester) — so its
 * whole body lives in each arm's own write-policy TU.  Every body runs the
 * roster fan-out, then retires the slot by the dispatched item; any waiter
 * left parked at destroy time (UB per OCR) is cleaned up by the refcount-0
 * cache destructor. */

/* NO_ACQUIRE home normalization.  The creator neither acquires nor releases,
 * so the home is the sole idle owner.  Every home create path seeds a
 * create-time RW hold (cache_init CREATOR_HOME / arts_db_home_init) that, for
 * NO_ACQUIRE, no EDT will ever release.  That seed must be undone so the first
 * real acquirer is granted rather than blocked behind a hold nothing releases.
 * Mirrors the local-create path: under a single-writer lock the unreleased hold
 * deadlocks every future writer; the ownership protocols collapse the seed to
 * the sentinel (writer_count = 1).  Idempotent and safe to call on every create
 * path. */
static inline void db_create_no_acquire_idle(struct arts_db_s *db,
                                             bool no_acquire) {
  if (!no_acquire) {
    return;
  }
  /* The creator was given no storage, so whatever the create-time seed
   * claimed about this rank holding a reader copy is false and must go with
   * the write hold. */
  arts_db_create_retract_creator_copy(db);
#if defined(ARTS_PROTOCOL_FLUSH)
  /* An arm that keeps no permission state has no seed to collapse: the
   * retraction above is the whole normalization. */
#elif defined(ARTS_PROTOCOL_EXCL)
#if defined(ARTS_RELEASE_RETAIN)
  /* RETAIN: data lives with the owner, not the home.  With no creator hold there
   * is no owner unless we make one — so the home rank (this rank; the create
   * handler runs only on the GUID home, see the assert in
   * arts_handler_db_create) becomes the IDLE data owner with owner-bit set but
   * rw_st=IDLE, wc=0.  Its buffer does not exist yet: a create that acquires
   * nothing has no first user to place it for, so the first user allocates it
   * (arts_db_buf_ensure) — and the first writer's REQUEST then migrates it
   * from here, exactly like a sticky owner that has finished its writers.
   * lock_state is the idle directory naming this rank as owner. */
  atomic_store_explicit(&db->cache.cache_state,
                        CACHE_MAKE_FULL(1u, CACHE_ST_IDLE, CACHE_ST_IDLE,
                                        ARTS_EXCL_NO_TARGET, 0u, 0u),
                        memory_order_relaxed);
  atomic_store_explicit(&db->lock_state,
                        LOCK_MAKE(EXCL_PHASE_IDLE, arts_global_rank_id, 0u, 0u),
                        memory_order_relaxed);
#else  /* ARTS_RELEASE_PURGE */
  /* PURGE: the home holds the canonical buffer and grants from it; the creator
   * is a non-owner.  Idle both words (the first EXCL_REQUEST is granted, not
   * blocked behind the unreleased creator hold). */
  atomic_store_explicit(&db->cache.cache_state, 0ULL, memory_order_relaxed);
  atomic_store_explicit(&db->lock_state, 0ULL, memory_order_relaxed);
#endif /* ARTS_RELEASE_* */
#else
  /* Grant-bearing arms: with no creator hold this rank is the idle owner —
   * possession, and no hold under it.  The store is atomic because this word
   * is what a concurrent acquire's admission CAS and, where grants come back
   * unasked, a returning holder's claim both arbitrate against. */
  (void)arts_atomic_swap(&db->cache.writer_count, ARTS_GRANT_SEED_IDLE);
#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)
  /* The directory must name THIS rank too.  A creator that never acquires
   * never releases, so it can never reach an idle edge: named as the holder
   * it would absorb the first requester's round and keep the block forever.
   * A transition, not a field write — demand may already be queued behind
   * the holder it replaces. */
  arts_db_grant_home_idle_transition(db);
#endif
#endif
}

/* There is deliberately no arrival-time check that the creator's claim
 * agrees with the directory.  The claim is about the moment the create ran on
 * its own rank, and the home cannot reconstruct that moment: DB_CREATE and
 * GRANT_RETURN carry no ordering against each other, so a first creator's
 * legitimate claim can arrive after its own hand-back has been accepted and
 * after a later create of the label has installed the block — leaving
 * exactly the directory state an illegitimate claim leaves.  Any test here
 * would therefore abort correct programs.  What does hold the word is
 * checked where it is committed: a grant installs only on a rank that holds
 * nothing, a hand-back is accepted only onto a home that holds nothing, and
 * one write right yields one outstanding hand-back.  A rank that took a right
 * the home never issued reaches one of those, and those states are reachable
 * no other way. */


/* The OOO_DB_CREATE body.  The engine runs it only while the GUID's slot on
 * this rank is empty; a create that finds the slot occupied is parked there
 * and dispatched by the occupant's destroy.  Two shapes:
 *   - the announce (arts_ooo_args_db_create_s): the home builds the block's
 *     directory for a creator on another rank, or for a creator here that
 *     acquires nothing;
 *   - a creator's own descriptor (arts_ooo_args_create_local_s), built
 *     privately on this rank: it is installed as is, and a creator that is
 *     not the block's home announces the block once its descriptor is
 *     installed.
 * A lost install re-enters the engine with the same args, which parks. */
void arts_handler_db_create(void *item_v, void *args_v) {
  (void)item_v;
  if (((const struct arts_ooo_args_create_local_s *)args_v)->size ==
      ARTS_OOO_CREATE_ADOPT) {
    const struct arts_ooo_args_create_local_s *l =
        (const struct arts_ooo_args_create_local_s *)args_v;
    if (!arts_db_create_install_local((arts_shared_ptr_t)l->descriptor)) {
      arts_ooo_dispatch_or_defer_guid(l->guid, OOO_DB_CREATE, l, sizeof(*l));
    }
    return;
  }
  const struct arts_ooo_args_db_create_s *p =
      (const struct arts_ooo_args_db_create_s *)args_v;
  unsigned int creator_rank = p->creator;
  arts_guid_t db_guid = p->db_guid;
  uint64_t db_size = p->db_size;
  /* NO_ACQUIRE: the creator neither acquires nor publishes, so the home is
   * the sole idle owner (not a non-owner awaiting a creator publish). */
  bool no_acquire = (p->flags & ARTS_DB_PROP_NO_ACQUIRE) != 0;

  /* The block's home directory is built only on the GUID's home rank, where
   * the descriptor is a full allocation.  Every descriptor installed on a
   * home rank is one a create built: an acquire of a block its own rank homes
   * parks on the empty slot instead of installing a cache-only stub. */
  assert((unsigned int)arts_guid_get_rank(db_guid) == arts_global_rank_id &&
         "home-directory init must run on the GUID home rank");

  /* Whether the home installs a buffer here is the arm's decision
   * (arts_db_create_install_home_buffer): an arm whose home holds the
   * canonical copy installs one now, an arm whose payload lives with its
   * owner leaves the home metadata-only.  A create that acquires nothing
   * installs none on any arm — it has no first user to place the payload
   * for, so the first use allocates it.  Either way a read that arrives
   * before the block has been published does NOT resolve to a NULL pointer —
   * the arm holds it (on the snapshot reorder buffer, until the first publish
   * drains it), routes it to the rank that does hold the bytes, or, where no
   * publish is ever coming because nobody took a write hold, materializes the
   * block's first image on the spot.  What is undefined before the first
   * publish is the block's CONTENTS (OCR ch2: "value of the created data
   * block is undefined"), never whether it has storage. */
  struct arts_db_s *stub = (struct arts_db_s *)arts_malloc_aligned(
      sizeof(struct arts_db_s), ARTS_CACHE_LINE_SIZE);
  memset(stub, 0, sizeof(struct arts_db_s));
  stub->db_type = (arts_db_types_t)p->db_type;
  if (no_acquire) {
    /* Home is the idle RW owner from creation — identical to a locally created
     * DB that has already been released by its creator.  HOME_RECV (rw_holder =
     * creator) would route the first GRANT_INVALIDATE to a creator that holds
     * no cache — a phantom holder — and the acquire would stall forever.
     *
     * CREATOR_HOME sets writer_count = sentinel(1) + creator hold(1) = 2, but
     * NO_ACQUIRE means no EDT will ever release the creator hold.  Decrement
     * to 1 (sentinel only) so the first GRANT_REQUEST's INVALIDATE-to-self
     * drives
     * writer_count to 0, triggering advance_chain and the GRANT. */
    arts_db_cache_init(&stub->cache, db_guid, db_size,
                       ARTS_DB_INIT_CREATOR_HOME, creator_rank);
    /* Collapse the create-time creator hold to the idle/sentinel state:
     * VAL drops writer_count 2 -> 1 (sentinel only); EXCL frees the
     * lock+cache state so the first GRANT_REQUEST / EXCL_REQUEST is granted
     * rather than blocked behind a hold no EDT will ever release. */
    db_create_no_acquire_idle(stub, no_acquire);
    /* No buffer here: a create that acquires nothing has no first user to
     * place the payload for, so its storage is allocated by whoever first
     * uses it (arts_db_buf_ensure) —
     * this rank when the first request it serves needs the bytes, on the
     * acquiring thread's node when the first user is local. */
  } else {
    arts_db_cache_init(&stub->cache, db_guid, db_size, ARTS_DB_INIT_HOME_RECV,
                       creator_rank);
  }
#if (!defined(ARTS_PROTOCOL_EXCL) && defined(ARTS_WRITE_POLICY_WT)) ||       \
    defined(ARTS_PROTOCOL_FLUSH)
  /* Kept for the return below, which echoes it to the creator. */
  stub->cache.create_token = p->create_token;
#endif
#ifdef ARTS_FAM
  /* The creator minted the block's store before sending this create; the home
   * records it and allocates none.  Before the home's buffer install, which
   * adopts the slot on the residency that keeps no copy, and before the route
   * install, whose drain can serve a request out of it. */
  (void)arts_db_fam_slot_record(&stub->cache, p->fam_addr);
#endif
  /* The home's create-time buffer is the target a publish lands in, at
   * version 0 — "unpublished", which is what every serve and park predicate
   * on these arms keys on.  A create confers the block's write right on some
   * rank (its own, or the home under NO_ACQUIRE), so a publish is coming
   * wherever the creator acquires, and the target must exist before it can
   * arrive.  Which arms need one at all is the leaf's own decision: a
   * write-back home holds no payload and its leaf is a no-op. */
  if (!no_acquire) {
    arts_db_create_install_home_buffer(&stub->cache, db_size);
  }

  arts_shared_ptr_t installed_h = arts_route_table_install_if_absent(
      stub, db_guid, arts_global_rank_id, /*used=*/true);
  if (installed_h == NULL) {
    /* The descriptor was never published; the store stays the creator's,
     * named again by the replay. */
#ifdef ARTS_FAM
    __atomic_store_n(&stub->cache.fam_addr, (uint64_t)0, __ATOMIC_RELEASE);
#endif
    arts_db_free(stub);
    arts_ooo_dispatch_or_defer_guid(db_guid, OOO_DB_CREATE, p, sizeof(*p));
    return;
  }
#if (!defined(ARTS_PROTOCOL_EXCL) && defined(ARTS_WRITE_POLICY_WT)) ||       \
    defined(ARTS_PROTOCOL_FLUSH)
  /* Off the critical path: the credit flies while the creator EDT is still
   * writing, so a create -> write -> release sequence publishes with no
   * announce round.  A creator that took no right never publishes — no
   * credit.  Only while this descriptor is still the one installed: the
   * install's drain can run a deferred destroy of it, and a retired block
   * must not advertise its buffer to the creator of whatever the slot holds
   * next. */
  if (!no_acquire) {
    arts_shared_ptr_t live_h = arts_route_table_lookup_db(db_guid);
    if (arts_shared_get(live_h) == (void *)stub) {
      arts_send_db_create_return(creator_rank, &stub->cache);
    }
    arts_shared_release(&live_h);
  }
#endif
  arts_shared_release(&installed_h);
}

/* ===== Sharer-side response handlers =============================== */

/* The WT GRANT_RESPONSE handler arts_handler_db_grant_response lives in
 * coherence/grant_wt.c; WB's overload lives in
 * coherence/grant_wb.c. */

/* Cat-C pure body (SNAPSHOT_RESPONSE).  The wire dispatcher / self-send shortcut
 * has already looked the home db_s up with a held ref and passes it as item_v
 * (cache is its FIRST member, offset 0, so item_v IS the cache).  No
 * lookup/NULL-check here — the dispatcher's MISS branch SILENTLY DROPS (this
 * 1:1 response resumes a parked EDT; a missing cache means it was torn down).
 *
 * No acquire-time list registration: this 1:1 response resumes the parked EDT
 * (a->edt_guid, a->slot) directly.  Three cases over a monotonic version:
 *   1. a->version <= buf->version : nothing newer to install — resume self
 *      against the live buffer.
 *   2. data + a->version > buf->version : install + drain-all the reorder
 *      buffer + resume self.
 *   3. NO_DATA + a->version > buf->version : the with-data reply was
 *      reordered behind us — push self onto pending_snapshot (a future
 *      case-2 install drains us) + re-check (race recovery).
 * Shared verbatim by WT/WB. */
/* Consume an in-flight rendezvous whose receiver-side object is gone: the
 * metadata packet arrived for a destroyed target, so nobody will ever expect
 * the txid — register a discard continuation that returns the landing's
 * storage to the registered pool once (or as soon as) the write completion
 * arrives, keeping the pairing table leak-free. */
struct rdzv_discard_ctx_s {
  struct arts_db_buffer_s *landing;
};

static void rdzv_discard_cb(void *arg) {
  struct rdzv_discard_ctx_s *ctx = (struct rdzv_discard_ctx_s *)arg;
  arts_regpool_free(ctx->landing);
  arts_free(ctx);
}

void arts_db_rdzv_discard_landing(uint64_t txid, uint64_t cookie) {
  if (txid == 0) {
    return;
  }
  struct rdzv_discard_ctx_s *ctx =
      (struct rdzv_discard_ctx_s *)arts_malloc(sizeof(*ctx));
  ctx->landing = (struct arts_db_buffer_s *)(uintptr_t)cookie;
  arts_net_rdzv_expect(txid, rdzv_discard_cb, ctx);
}

#if defined(ARTS_PROTOCOL_VAL)
/* Rendezvous continuation for a data-bearing SNAPSHOT_RESPONSE: the snapshot
 * payload has fully landed in our advertised landing ("imm seen => landing
 * valid").  Install it without a copy (version-conditional; a stale landing
 * recycles), drain the reorder buffer, and resume the parked EDT.  Fires on
 * {packet, write-completion} pairing, either arrival order, on a
 * dispatch-path progress thread. */
struct snapshot_landed_ctx_s {
  arts_shared_ptr_t db_h; /* own pin taken by the handler (may be NULL) */
  struct arts_db_buffer_s *landing;
  uint64_t version;
  uint64_t data_size;
  arts_guid_t edt_guid;
  uint32_t slot;
};

static void snapshot_landed_cb(void *arg) {
  struct snapshot_landed_ctx_s *ctx = (struct snapshot_landed_ctx_s *)arg;
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(ctx->db_h);
  if (db == NULL) {
    /* DB destroyed while the pairing was outstanding (destroy-during-pending-
     * acquire is app UB): the cache — and its recycle pool — are gone, so
     * return the landing's storage straight to the registered pool and drop
     * (the parked EDT was torn down with the DB). */
    arts_regpool_free(ctx->landing);
    arts_shared_release(&ctx->db_h);
    arts_free(ctx);
    return;
  }
  struct arts_db_cache_s *cache = &db->cache;
  arts_db_buf_install_landed(cache, ctx->version, ctx->landing,
                             ctx->data_size);
  arts_db_drain_pending_snapshot(cache);
  mark_edt_ready_by_guid(ctx->edt_guid, ctx->slot);
#ifdef ARTS_RO_COMBINING_LIVE
  arts_db_ro_combine_on_terminal(cache, true, ctx->version);
#endif
  arts_shared_release(&ctx->db_h);
  arts_free(ctx);
}

void arts_handler_db_snapshot_response(void *item_v, void *args_v) {
  struct arts_db_cache_s *cache = &((struct arts_db_s *)item_v)->cache;
  struct arts_db_snapshot_response_args_s *a =
      (struct arts_db_snapshot_response_args_s *)args_v;
  arts_guid_t edt_guid = a->edt_guid;
  uint32_t slot = a->slot;
  arts_db_cache_note_answered(cache); /* before the answer takes effect */

  /* Every response kind carries the server's descriptor size; a hinted
   * first touch may reach any of them with the size still unlearned. */
  if (cache->db_size == 0 && a->db_size > 0) {
    cache->db_size = a->db_size;
  }

  if (a->data_present == 2) {
    /* Size-only CTS: the server holds data but our request carried no landing
     * (first touch — db_size unknown).  Learn the size and re-issue the
     * request; the re-request advertises a landing and is served for real. */
    if (cache->db_size == 0) {
      cache->db_size = a->db_size;
    }
    arts_send_db_snapshot_request(cache, edt_guid, slot);
    return;
  }

  if (a->data_present == 1 && a->rdzv_txid != 0) {
    /* The payload travels one-sided: pair this packet with the write
     * completion (either may arrive first); install+resume when both are in.
     * Take our own descriptor pin for the pairing window (the dispatcher's
     * ref ends when this handler returns). */
    struct snapshot_landed_ctx_s *ctx =
        (struct snapshot_landed_ctx_s *)arts_malloc(sizeof(*ctx));
    ctx->db_h = arts_route_table_lookup_db(cache->db_guid);
    ctx->landing = (struct arts_db_buffer_s *)(uintptr_t)a->rdzv_cookie;
    ctx->version = a->version;
    ctx->data_size = a->db_size;
    ctx->edt_guid = edt_guid;
    ctx->slot = slot;
    arts_net_rdzv_expect(a->rdzv_txid, snapshot_landed_cb, ctx);
    return;
  }

  /* No PUT consumed the advertised landing.  Two readings, told apart by the
   * version the reply carries, and the landing's fate differs:
   *   - version 0 (ARTS_GRANT_VERSION_NONE, "the server holds nothing"):
   *     nobody has written the block, so there are no bytes to fetch, and
   *     this rank is entitled to storage of the declared size whatever its
   *     contents — adopt the landing it already allocated, stamped 1 rather
   *     than the 0 that means "nothing";
   *   - any other version is the server's dedup verdict (known_v >= cur_v)
   *     or a same-rank inline serve: bytes at that version reached this rank
   *     (the ledger credits a rank only where a serve PUT them or it
   *     published them itself) — recycle.  Never adopt here: the credited
   *     install may still be in flight behind this reply (the reorder case
   *     parked below), and an invented image stamped with its version would
   *     make that install retreat as stale and stand in for the data. */
  if (a->rdzv_cookie != 0) {
    struct arts_db_buffer_s *landing =
        (struct arts_db_buffer_s *)(uintptr_t)a->rdzv_cookie;
    if (a->version == 0) {
      (void)arts_db_buf_adopt_landing(cache, 1u, landing, cache->db_size);
    } else {
      arts_db_buf_landing_recycle(cache, landing);
    }
  }

  arts_shared_ptr_t buf_h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *buf =
      (struct arts_db_buffer_s *)arts_shared_get(buf_h);
  uint64_t buf_v = buf ? buf->version : 0;
  if (buf != NULL) {
    arts_db_buf_release(&buf_h);
  }

  if (a->version <= buf_v) {
    /* Case 1: nothing newer to install — resume self. */
    mark_edt_ready_by_guid(edt_guid, slot);
#ifdef ARTS_RO_COMBINING_LIVE
    arts_db_ro_combine_on_terminal(cache, true, a->version);
#endif
    return;
  }
  if (a->data_present) {
    /* Case 2: install (version-conditional publish inside install_buffer;
     * stale installs retreat) + drain-all + resume self. */
    arts_db_buf_install(cache, a->version, a->data, a->data_size);
    arts_db_drain_pending_snapshot(cache);
    mark_edt_ready_by_guid(edt_guid, slot);
#ifdef ARTS_RO_COMBINING_LIVE
    arts_db_ro_combine_on_terminal(cache, true, a->version);
#endif
    return;
  }
  /* Case 3: NO_DATA arrived ahead of the with-data reply.  Park a
   * reorder-buffer node; a later case-2 install drains it. */
#ifdef ARTS_RO_COMBINING_LIVE
  /* The in-flight batch shares the leader's fate.  Park it on the reorder
   * buffer BEFORE the leader's own park so the recovery recheck below covers
   * the whole batch. */
  arts_db_ro_combine_on_terminal(cache, false, a->version);
#endif
  struct arts_db_snapshot_waiter_s *w =
      (struct arts_db_snapshot_waiter_s *)arts_malloc(sizeof(*w));
  w->edt_guid = edt_guid;
  w->slot = slot;
  w->target_version = a->version;
  w->serve = NULL; /* local waiter: drain resumes the parked EDT */
  arts_lf_stack_push(&cache->pending_snapshot, &w->link);
  /* Race recovery: a concurrent case-2 install may have published the buffer
   * between our version read and the push.  If so, drain (our own node
   * included) so we don't park forever.  The atomic_exchange drain is the
   * single-actor primitive — a concurrent installer's drain and ours cannot
   * both claim the same node. */
  arts_shared_ptr_t rch = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *rbuf =
      (struct arts_db_buffer_s *)arts_shared_get(rch);
  uint64_t rv = rbuf ? rbuf->version : 0;
  if (rbuf != NULL) {
    arts_db_buf_release(&rch);
  }
  if (rv >= a->version) {
    arts_db_drain_pending_snapshot(cache);
  }
}
#endif /* the versioned-snapshot read path */

/* arts_handler_db_grant_invalidate (GRANT_INVALIDATE) lives per write policy:
 * coherence/grant_wt.c (commutative signed counter) and coherence/grant_wb.c
 * (publish-target-then-withdraw). */

/* arts_handler_db_publish_ack is the shared flight-completion body, defined
 * ONCE in coherence/coherence.c for every publishing arm (gated
 * !EXCL && (!WB || INV)); the exclusion arm has no synchronous publish and
 * its dispatcher fatals on the message. */

/* Cat-C pure body (DESTROY_NOTIFY).  The wire dispatcher / self-send shortcut
 * has already looked the cache up with a held ref and passes the db_s as item_v
 * (cache is its FIRST member, offset 0).  No lookup/NULL-check here — the
 * dispatcher's MISS branch SILENTLY DROPS (already torn down on this rank;
 * cb-NULL = idempotent, a second DESTROY_NOTIFY is a no-op).
 *
 * Destroy is just the route-slot detach of the object the message was
 * dispatched on (CAS value→NULL + drop the install ref); the cb deleter
 * (arts_db_cache_destructor) runs at refcount 0 and does the cleanup (free
 * the parked-waiter nodes).  Destroying a DB that an EDT
 * still has a pending dependence on is undefined per OCR (ocrDbDestroy: the
 * user ensures the DB is not in use), so no parked-EDT wake is attempted. */
void arts_handler_db_cache_destroy(void *item_v, void *args_v) {
  struct arts_db_cache_destroy_args_s *a =
      (struct arts_db_cache_destroy_args_s *)args_v;
  (void)arts_ooo_retire_item(a->db_guid, item_v);
#if (defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)) &&             \
    (!defined(ARTS_WRITE_POLICY_WB) || defined(ARTS_PROTOCOL_INV))
  /* AFTER the slot withdrawal, so a releaser registering concurrently either
   * lands in this drain or sees the NULL slot on its own recheck.  The
   * dispatcher's held ref keeps the cache alive across the drain. */
  if (item_v != NULL) {
    arts_db_pub_flight_abandon(&((struct arts_db_s *)item_v)->cache);
  }
#else
  (void)item_v;
#endif
}

#ifdef ARTS_FAM
/* FAM_FREE handler: the address alone names the slot, so there is no
 * route-table lookup and no cache pinned by a dispatcher — this is a Cat-E
 * (state-less) message, ordered by construction (a home sends it only from
 * the teardown whose retire detached the block, and only the home ever
 * frees a slot it handed out). */
void arts_handler_db_fam_free(struct arts_msg_db_fam_free_packet_s *p) {
  void *slot = (void *)(uintptr_t)p->fam_addr;
  if (slot == NULL) {
    return;
  }
  if (!arts_fam_contains(slot)) {
    ARTS_ERROR("fam: rank %u asked this rank to free an address outside its "
               "pool",
               p->header.rank);
  }
  arts_fam_free(slot);
}
#endif /* ARTS_FAM */

/* The WB SNAPSHOT_REDIRECT handler arts_handler_db_snapshot_redirect lives in
 * coherence/val/wb.c (owner-side, REDIRECT only exists under WB). */
