/* SPDX-License-Identifier: Apache-2.0
 *
 * WT write-policy translation unit: defines the WT-specific
 * arts_handler_db_* / arts_db_* bodies directly (CMake links exactly this TU
 * for a VAL+WT build) plus the WT-only wire handlers/senders.
 * Compiled only for ARTS_COHERENCE_PROTOCOL=VAL with
 * ARTS_WRITE_POLICY=WT (selected in libs/src/core/CMakeLists.txt).
 * Contains NO protocol/write-policy preprocessor logic.
 */
#include <assert.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "arts/coherence/buffer.h"
#include "arts/coherence/coherence.h"
#include "arts/coherence/handlers.h"
#include "arts/coherence/directory.h"
#include "arts/counter/Preamble.h"
#include "arts/db.h"
#include "arts/edt.h"             /* arts_edt_dep_t (acquire body) */
#include "arts/gas/guid.h"
#include "arts/gas/route_table.h" /* arts_route_table_lookup_db (pin db_s) */
#include "arts/ooo.h"             /* OOO_DB_* args (handler bodies) */
#include "arts/runtime_state.h"
#include "arts/runtime_types.h"
#include "arts/system/print.h"       /* ARTS_ERROR */
#include "arts/system/threads.h"     /* arts_global_rank_id */
#include "arts/transport/net.h"   /* arts_transport_send_async */
#include "arts/transport/protocol.h" /* arts_fill_packet_header, MSG_* */
#include "arts/utils/malloc.h" /* pairing ctx */

/* ===== 8-case acquire dispatch (WT) ================================
 * Whole arts_handler_db_acquire body for the WT build.  Diverges from WB only
 * on the RO-has-local-data predicate (WT: is_home||is_owner — home always
 * holds current data via synchronous PUBLISH).  The remote-RO,
 * RW-local-fast, and remote-RW paths are the shared helpers
 * (coherence/coherence.c / coherence/grant.c). */
void arts_handler_db_acquire(void *item, void *args) {
  struct arts_db_s *db = (struct arts_db_s *)item;
  struct arts_ooo_args_db_acquire_s *a =
      (struct arts_ooo_args_db_acquire_s *)args;
  struct arts_edt_s *edt = a->edt;
  unsigned int slot = a->slot;
  struct arts_db_cache_s *cache = &db->cache;
  arts_edt_dep_t *dep = &((arts_edt_dep_t *)arts_get_depv(edt))[slot];
  arts_db_access_mode_t mode = dep->mode;
  bool is_home = (arts_guid_get_rank(cache->db_guid) == arts_global_rank_id);
  /* writer_count is non-negative (install guard; under RETAIN also the
   * post-install rw_holder flip), so > 0 means owner and <= 0 means
   * not-owner; the (int) cast is defensive.
   * An owned read here is safe to serve RO from the local buffer. */
  bool is_owner = ((int)atomic_load_explicit(&cache->writer_count,
                                             memory_order_acquire) > 0);
  uint64_t db_size =
      atomic_load_explicit(&cache->db_size, memory_order_relaxed);

  if (is_home) {
    /* First use of a block whose create took no hold: its storage was left
     * for whoever uses it first, and on this arm only the home may hold the
     * canonical copy — so if the block has none, this acquiring thread
     * allocates it, on its own node.  A no-op for every other block (the
     * home's buffer exists from create, at version 0 until published). */
    (void)arts_db_buf_ensure(cache, db_size);
  }

  if (mode == DB_MODE_RO) {
    if (is_home ||
        is_owner) { /* WT RO predicate (home holds current data) */
      /* Pre-check the publication signal so an unpublished (v0, installed
       * at create) stable buffer parks WITHOUT taking the EDT's acquire
       * hold — the resume re-derives the slot's pointer from the published
       * buffer. */
      bool unpublished = false;
      {
        arts_shared_ptr_t ph = arts_db_buf_acquire(cache);
        struct arts_db_buffer_s *pb =
            (struct arts_db_buffer_s *)arts_shared_get(ph);
        unpublished =
            (pb != NULL &&
             atomic_load_explicit(&pb->version, memory_order_acquire) == 0);
        arts_db_buf_release(&ph);
      }
      void *payload =
          (unpublished && db_size > 0) ? NULL : arts_db_acquire_local(cache);
      if (payload == NULL && db_size > 0) {
        /* Home before the creator's first PUBLISH: a cross-rank create
         * installs home metadata only — no buffer exists until the creator's
         * release publishes one.  A dependence satisfied before that release
         * (add-dependence satisfies immediately) may legally race here; the
         * RELEASE is the publication point, so PARK among the publication
         * waiters — the first publish install drains us and the resume
         * re-derives the slot's pointer from the installed buffer.
         * (db_size == 0 is the sentinel DB: NULL is its defined value.) */
        struct arts_db_read_waiter_s *w =
            (struct arts_db_read_waiter_s *)arts_malloc(sizeof(*w));
        w->edt = edt;
        ARTS_DEP_WAITER_SET(w, edt);
        w->slot = slot;
        w->cover = 0u;
        w->serve = NULL;
        arts_lf_stack_push(&cache->publication_waiters, &w->link);
        /* Race recovery: a publication may have landed between the version
         * read and the push — drain (our own node included) so nobody parks
         * forever.  Same-word pairing on publication_waiters closes the
         * store->load shape with every publisher, which stamps the version
         * and then drains (arts_db_drain_publication_waiters): of this acq_rel
         * push and that acq_rel drain, the later one in the stack's
         * modification order reads the other, so either the drain takes this
         * node or this re-read sees the stamp. */
        arts_shared_ptr_t rh = arts_db_buf_acquire(cache);
        struct arts_db_buffer_s *rb =
            (struct arts_db_buffer_s *)arts_shared_get(rh);
        bool published =
            (rb != NULL &&
             atomic_load_explicit(&rb->version, memory_order_acquire) > 0);
        arts_db_buf_release(&rh);
        if (published) {
          arts_db_drain_publication_waiters(cache);
        }
        return;
      }
      INCREMENT_NUM_DB_ACQUIRE_LOCAL_HIT_BY(1);
      arts_db_acquire_local_hit(edt, slot, payload);
      arts_db_acquire_resolved(edt, slot);
      return;
    }
    arts_db_acquire_remote_ro(cache, edt, slot); /* parks (SNAPSHOT) */
    return;
  }
  /* RW */
  if (is_owner && arts_db_acquire_rw_local_fast(cache, edt, slot)) {
    arts_db_acquire_resolved(edt, slot); /* data here, writer_count bumped */
    return;
  }
  arts_db_acquire_remote_rw(cache, edt, slot); /* parks (GRANT_REQUEST) */
}

bool arts_db_acquire_is_serialized(arts_db_access_mode_t mode) {
  return mode == DB_MODE_RW;
}

/* ===== release_rw (WT arm) ========================================
 * The WT write policy keeps home's RO copy fresh with a pure synchronous
 * PUBLISH on every non-home release (home owners already hold the canonical
 * buffer).  Ownership moves on the grant plane: under RETAIN a separate
 * owner→owner ship (arts_db_send_grant_response), fired on the 0-edge when a
 * transfer target is pending, as in the WB arm; under PURGE the right goes
 * back to the home, on the publish's commit leg when it can.  The drain-now /
 * confirm-ack divergence lives only in the GRANT_RESPONSE / CONFIRM
 * handlers, not here. */
/* A create's hold is this arm's ordinary write hold, taken when the block was
 * made; the bytes under it are the cache's own, so the release needs no
 * pointer to them. */
void arts_db_release_created(struct arts_db_cache_s *cache) {
  arts_db_release_rw(cache, NULL);
}

void arts_db_release_rw(struct arts_db_cache_s *cache, void *payload) {
  (void)payload;
  /* Defensive: no local hold to drop means our acquire never bumped ownership;
   * decrementing would underflow.  Which word states carry no hold is the
   * release policy's — possession alone is one of them under a
   * voluntary-return policy and not under a revoked one. */
  if (arts_db_grant_release_skip(cache)) {
    return;
  }
  /* Acquire current buffer for the version bump + PUBLISH send.  Local ref
   * scoped to release_rw — the EDT's own ref is dropped by release_one_dep. */
  arts_shared_ptr_t buf_h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *buf =
      (struct arts_db_buffer_s *)arts_shared_get(buf_h);
  uint64_t new_version = 0;
  if (buf != NULL) {
    new_version =
        atomic_fetch_add_explicit(&buf->version, 1u, memory_order_acq_rel) + 1u;
  }
  bool is_home = (arts_guid_get_rank(cache->db_guid) == arts_global_rank_id);
  /* WT keeps home's RO copy fresh: pure synchronous publish every release
   * (a non-home owner; home owners already hold the canonical buffer).  Under
   * RETAIN the publish moves no ownership (that is the separate owner→owner
   * ship); under PURGE its commit leg may carry the hand-back.  Derived ONCE:
   * the hand-back rides this decision, and deriving it twice is how the two
   * drift apart. */
  bool will_publish = (!is_home && buf != NULL);
  /* Where the write right goes back unasked it can travel with these bytes
   * instead of behind them, which is the whole saving — so the claim is taken
   * BEFORE the publish, and a won claim replaces the count-dropping edge. */
  uint64_t hand_back = arts_db_grant_release_claim(cache, will_publish);
  if (will_publish) {
    /* The flight pins its own source ref; this caller's ref only covers the
     * version bump above. */
    arts_db_publish_sync(cache, new_version);
  }
  /* WT: release buffer ref after the publish (which reads buf->data). */
  if (buf != NULL) {
    arts_db_buf_release(&buf_h);
  }

  /* The count-dropping edge, and what this rank owes at it, belong to the
   * release policy — a revoked grant ships onward to the target the round
   * named, a voluntarily returned one goes back to the home.  Sequenced after
   * the publish above either way: the bytes must be at the home before the
   * write right can move.  A claim taken above already dropped the count, and
   * only has to settle which vehicle carried it. */
  if (hand_back != 0u) {
    arts_db_grant_release_settle(cache, hand_back);
  } else {
    arts_db_grant_release_commit(cache);
  }
}

/* ===== cache_s lifecycle (WT) ======================================
 * Construct: the WT write policy's own fields (the pending_rw Treiber stack,
 * the transfer target, no dedup ledger), then arts_db_cache_common_init.
 * Destruct order: buffer-NULL (pre) → pending_rw destroy → snapshot drain +
 * home teardown (post). */
void arts_db_cache_init(struct arts_db_cache_s *c, arts_guid_t db_guid,
                        uint64_t db_size, arts_db_init_kind_t kind,
                        unsigned int creator_rank) {
  arts_pending_rw_queue_init(&c->pending_rw);
#ifndef ARTS_RELEASE_PURGE
  /* Target of the grant's next ship, published by each round's INVALIDATE
   * before it withdraws the sentinel.  Starts at the sentinel: no transfer
   * pending. */
  c->incoming_new_owner = ARTS_NO_PENDING_OWNER;
  c->incoming_new_owner_rdzv = (struct arts_rdzv_landing_s){0, 0, 0, 0};
  c->incoming_new_owner_have = ARTS_GRANT_VERSION_NONE;
#endif
  /* WT has no owner-side dedup map (home serves RO via SNAPSHOT_REQUEST): the
   * owner→owner transfer always ships an empty map.  NULL so the shared ship
   * helper's map-build gate takes its empty-map branch. */
  c->cached_version = NULL;
#ifdef ARTS_RELEASE_PURGE
  atomic_init(&c->pending_grant_return, 0u);
  atomic_init(&c->grant_generation, 0u);
#endif
  arts_db_cache_common_init(c, db_guid, db_size, kind, creator_rank);
}

void arts_db_cache_destructor(struct arts_db_cache_s *cache) {
  if (cache == NULL) {
    return;
  }
  arts_db_cache_common_destroy_pre(cache); /* buffer-NULL FIRST */
  arts_pending_rw_queue_destroy(&cache->pending_rw);
  arts_db_cache_common_destroy_post(cache); /* snapshot drain → home teardown */
}

/* ===== home-directory lifecycle (inlined in arts_db_s) ============= */

void arts_db_home_init(struct arts_db_s *db, unsigned int rw_holder,
                       unsigned int nranks) {
  /* A revoking home names the holder it will revoke; a home whose grants
   * come back unasked names none — its own possession is its directory. */
#ifndef ARTS_RELEASE_PURGE
  atomic_store_explicit(&db->rw_holder, rw_holder, memory_order_relaxed);
#endif
  arts_home_grantreq_queue_init(&db->pending_rw);
  atomic_store_explicit(&db->invalidate_in_flight, 0, memory_order_relaxed);
  db->cached_version = arts_rank_u64_map_create(nranks);
  arts_rank_bitset_init(&db->cached_ranks, nranks);
  arts_db_cover_home_init(db, nranks);
  /* A creator on another rank holds a cache of the block from the moment it
   * made it, so the destroy must reach that rank too: its cache would
   * otherwise outlive the block, keeping its storage and the rank's route
   * slot for the GUID (where a later create of the GUID would park). */
  if (rw_holder != arts_global_rank_id && rw_holder < nranks) {
    arts_rank_bitset_set(&db->cached_ranks, rw_holder);
  }
#ifndef ARTS_RELEASE_PURGE
  atomic_store_explicit(&db->pending_install_owner, 0u, memory_order_relaxed);
#endif
#ifdef ARTS_RELEASE_PURGE
  atomic_store_explicit(&db->pending_return_from, ARTS_GRANT_NO_RETURNER,
                        memory_order_relaxed);
#endif
}

void arts_db_home_teardown(struct arts_db_s *db) {
  if (db == NULL) {
    return;
  }
  arts_home_grantreq_queue_destroy(&db->pending_rw);
  arts_rank_u64_map_destroy(db->cached_version);
  arts_rank_bitset_destroy(&db->cached_ranks);
  arts_db_cover_home_teardown(db);
  /* No free: home fields are inlined in the arts_db_s. */
}

/* ===== SNAPSHOT_REQUEST reply (home.cached_version atomic-monotonic) ===== */

/* SNAPSHOT_REQUEST reply path: decide CTS / no-data / one-sided-data from the home
 * watermark and the requester's landing, then advance the watermark only when
 * payload actually moves.  Consumes master_h (the pinned canonical buffer). */
static void update_cached_version_max(struct arts_db_cache_s *cache,
                                 unsigned int requester, uint64_t master_v,
                                 arts_shared_ptr_t master_h, uint32_t cover,
                                 const struct arts_rdzv_landing_s *rdzv) {
  struct arts_db_s *db = arts_db_of_cache(cache);
  /* Monotonic dedup — if the requester already holds this version
   * (cur >= master_v), no payload moves: reply no-data, echoing the unused
   * landing for recycling.  The ledger is credited by serves AND by the
   * requester's own publishs, so a self-produced copy dedups too. */
  uint64_t db_size =
      atomic_load_explicit(&cache->db_size, memory_order_relaxed);
  uint64_t cur = arts_rank_u64_map_get(db->cached_version, requester);
  if (cur >= master_v) {
    INCREMENT_NUM_SNAPSHOT_HEADER_ONLY_BY(1);
    arts_db_buf_release(&master_h);
    arts_send_db_snapshot_response(requester, db, cache->db_guid, master_v,
                                   cover, /*kind=*/0, db_size, rdzv, NULL);
    return;
  }
  if (rdzv->txid == 0) {
    /* Data must move but the requester advertised no landing (first touch —
     * db_size unknown there).  Size-only CTS; the re-request carries a
     * landing.  The watermark does NOT advance on this leg. */
    arts_db_buf_release(&master_h);
    INCREMENT_NUM_SNAPSHOT_SIZE_CTS_BY(1);
    arts_send_db_snapshot_response(requester, db, cache->db_guid, master_v,
                                   cover, /*kind=*/2, db_size, rdzv, NULL);
    return;
  }
  arts_rank_u64_map_advance(db->cached_version, requester, master_v);
  /* master_h transfers into the sender (PUT source-lifetime pin). */
  arts_send_db_snapshot_response(requester, db, cache->db_guid, master_v,
                                 cover, /*kind=*/1, db_size, rdzv, master_h);
}

/* Deferred SNAPSHOT_REQUEST serve (a read waiter's `serve` arm): the request
 * arrived at home before the creator's first PUBLISH, and a drain re-issues it
 * once the copy is published (every drain that serves runs at version >= 1;
 * teardown frees a waiter without serving it).  Runs the same serve tail as
 * the request handler.  The waiter is freed by the drain after this returns. */
static void home_serve_deferred_read(struct arts_db_cache_s *cache,
                                     struct arts_db_read_waiter_s *w) {
  arts_shared_ptr_t master_h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *master =
      (struct arts_db_buffer_s *)arts_shared_get(master_h);
  /* Acquire pairs with the publish commit's release-store version stamp: a
   * serve must never stamp a version whose bytes it cannot yet see. */
  uint64_t master_v =
      master != NULL
          ? atomic_load_explicit(&master->version, memory_order_acquire)
          : 0u;
  assert(master_v >= 1 && "a deferred read is served only once published");
  /* master_h transfers into the reply path (consumed there). */
  update_cached_version_max(cache, w->requester, master_v, master_h, w->cover,
                            &w->rdzv);
}

/* ===== Per-protocol wire-handler bodies =============================== */

/* Cat-B pure body (OoO g_ooo_table[OOO_DB_SNAPSHOT_REQUEST]): the OoO engine
 * has already acquired the home db_s for db_guid and pinned a ref across this
 * call (cache is its FIRST member), so there is no lookup / NULL-check / defer
 * here.  WT serves from home's canonical buffer with cached_version dedup. */
void arts_handler_db_snapshot_request(void *item_v, void *args_v) {
  struct arts_db_cache_s *cache = &((struct arts_db_s *)item_v)->cache;
  struct arts_ooo_args_db_snapshot_request_s *a =
      (struct arts_ooo_args_db_snapshot_request_s *)args_v;
  unsigned int requester = a->requester;
  uint32_t cover = a->cover;
  {
    bool td = false;
    int r = arts_db_cover_request(arts_db_of_cache(cache), requester, false,
                                  cover, &td);
    if (r == ARTS_COVER_DISMISS) {
      /* Declared: a write turn of the requester's rank covers this request.
       * Dismiss it -- no data, no read turn -- returning the landing. */
      arts_send_db_snapshot_response(
          requester, arts_db_of_cache(cache), cache->db_guid, /*version=*/0,
          cover, /*kind=*/2,
          atomic_load_explicit(&cache->db_size, memory_order_relaxed),
          &a->rdzv, NULL);
    }
    if (td) {
      arts_db_home_destroy_now(arts_db_of_cache(cache));
    }
    if (r != ARTS_COVER_PROCESS) {
      return;
    }
  }
  /* A reader holds a cache of the block from here on, so the destroy must
   * reach it (a size-0 block's reply credits no version to the ledger). */
  if (requester != arts_global_rank_id) {
    (void)arts_rank_bitset_set(&arts_db_of_cache(cache)->cached_ranks,
                               requester);
  }

  /* A remote first use of a block whose create took no hold: the home is the
   * canonical holder on this arm and the reply must carry the block's first
   * image, so it is materialized here — on a progress thread, hence on
   * whatever node that thread sits on rather than the requester's. */
  uint64_t db_size =
      atomic_load_explicit(&cache->db_size, memory_order_relaxed);
  (void)arts_db_buf_ensure(cache, db_size);

  arts_shared_ptr_t master_h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *master =
      (struct arts_db_buffer_s *)arts_shared_get(master_h);
  if (master != NULL &&
      atomic_load_explicit(&master->version, memory_order_acquire) == 0) {
    /* Present-but-unpublished (v0, installed at create): park exactly as
     * if no buffer existed — the creator's first publish is the
     * publication point. */
    arts_db_buf_release(&master_h);
    master = NULL;
    master_h = NULL;
  }
  if (master == NULL) {
    /* Two cases produce master==NULL post-precheck:
     *   (a) Sentinel DB (db_size==0): no buffer is ever installed — respond
     *       with version=0, NULL data (NULL is the sentinel's defined value).
     *   (b) HOME_RECV pre-PUBLISH (db_size>0): cross-rank create has
     *       happened but the creator's first PUBLISH has not landed.  The
     *       creator's RELEASE is the publication point a satisfied consumer
     *       is entitled to observe, so PARK the serve among the
     *       publication waiters; the first publish install drains it and
     *       re-issues the serve with the original requester/landing.
     * NOT a destroy condition: a destroyed block's slot is withdrawn, so this
     * body never runs for one. */
    if (db_size > 0) {
      struct arts_db_read_waiter_s *w =
          (struct arts_db_read_waiter_s *)arts_malloc(sizeof(*w));
      w->edt = NULL;
      w->slot = 0u;
      w->cover = cover;
      w->serve = home_serve_deferred_read;
      w->requester = requester;
      w->rdzv = a->rdzv;
      arts_lf_stack_push(&cache->publication_waiters, &w->link);
      /* Race recovery: a publish may have landed between the version read
       * and the push — drain (our own node included) so no serve parks
       * forever.  Same-word pairing on publication_waiters with the
       * publisher's stamp-then-drain, as at the acquire's parker above. */
      arts_shared_ptr_t rh = arts_db_buf_acquire(cache);
      struct arts_db_buffer_s *rb =
          (struct arts_db_buffer_s *)arts_shared_get(rh);
      bool published =
          (rb != NULL &&
           atomic_load_explicit(&rb->version, memory_order_acquire) > 0);
      arts_db_buf_release(&rh);
      if (published) {
        arts_db_drain_publication_waiters(cache);
      }
      return;
    }
    arts_send_db_snapshot_response(requester, arts_db_of_cache(cache),
                                   cache->db_guid, /*version=*/0, cover,
                                   /*kind=*/0, db_size, &a->rdzv, NULL);
    return;
  }
  /* Acquire pairs with the publish commit's release-store version stamp: a
   * serve must never stamp a version whose bytes it cannot yet see. */
  uint64_t master_v =
      atomic_load_explicit(&master->version, memory_order_acquire);
  /* master_h transfers into the reply path (consumed there). */
  update_cached_version_max(cache, requester, master_v, master_h, cover,
                            &a->rdzv);
}

/* Cat-B pure body (OoO g_ooo_table[OOO_DB_PUBLISH]): the OoO engine has
 * already acquired the home db_s and pinned a ref across this call (cache is
 * its FIRST member).  A PUBLISH carries no payload: the bytes travel one-sided
 * into the stable buffer.  A PUBLISH that races ahead of DB_CREATE MISSes the
 * engine, which defers the args and re-issues this body on the install's
 * drain.
 *
 * ACK-exactly-once: the body answers only when it runs (cache live) — the
 * announce with its PUBLISH_CTS, every other leg with its PUBLISH_ACK.  A
 * deferred PUBLISH does NOT ack on the defer — the single PUBLISH_ACK is
 * sent on the drain replay.  It stamps the version of the bytes already in
 * place and acks; ownership moves on the grant plane (under PURGE a commit
 * may carry the hand-back, accepted once the stamp is in place). */
/* Rendezvous continuation for a committed publish: the dirty bytes have
 * fully landed IN PLACE in the stable home buffer; publication is the version
 * stamp alone (release-store, sequenced after the {commit packet, write
 * completion} pairing — "landing valid" precedes the stamp).  The ACK fires
 * even if the DB was destroyed while the pairing was outstanding — a
 * torn-down home cache must never strand the blocked releaser (the
 * PUBLISH_ACK-on-MISS rule). */
struct pub_landed_ctx_s {
  arts_shared_ptr_t db_h;
  uint64_t version;
  unsigned int releaser;
  arts_guid_t db_guid;
  uint64_t cv;
  bool returns_grant;
};

static void pub_landed_cb(void *arg) {
  struct pub_landed_ctx_s *ctx = (struct pub_landed_ctx_s *)arg;
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(ctx->db_h);
  if (db != NULL) {
    arts_db_buf_bump_inplace(&db->cache, ctx->version);
    /* The releaser holds the version it just published — credit the ledger
     * so its own next acquire dedups to no-data. */
    arts_rank_u64_map_advance(db->cached_version, ctx->releaser, ctx->version);
    /* First-publish publication (bump from version 0): wake home-parked RO
     * acquires and re-issue home-parked SNAPSHOT_REQUEST serves. */
    arts_db_drain_publication_waiters(&db->cache);
  }
  if (ctx->returns_grant && db != NULL) {
    /* The releaser handed the write right back with these bytes.  Accepting
     * here and not a moment earlier is what makes the two one event: the
     * stamp above is already in place, so a block handed straight on carries
     * a version this release has not moved past.  The publish's
     * acknowledgement rides the hand-back and leaves once the home has
     * applied it.  A block destroyed while the bytes were in flight owes
     * nothing but the acknowledgement — its directory went with it. */
    struct arts_db_grant_return_reply_s reply = {.cv = ctx->cv,
                                                 .version = ctx->version,
                                                 .ack = true,
                                                 .credit = true};
    arts_db_grant_return_arrived(db, ctx->releaser, &reply);
  } else {
    arts_send_db_publish_ack(ctx->releaser, ctx->db_guid, ctx->cv,
                             ctx->version, db != NULL ? &db->cache : NULL);
  }
  arts_shared_release(&ctx->db_h);
  arts_free(ctx);
}

void arts_handler_db_publish(void *item_v, void *args_v) {
  struct arts_db_cache_s *cache = &((struct arts_db_s *)item_v)->cache;
  struct arts_ooo_args_db_publish_s *a =
      (struct arts_ooo_args_db_publish_s *)args_v;

  if (a->data_size == 0) {
    /* Data-less ordering round (sentinel DB): install nothing, ACK.  A
     * hand-back rides payload legs only. */
    assert(a->returns_grant == 0u &&
           "a hand-back never rides a data-less publish");
    arts_send_db_publish_ack(a->releaser, a->db_guid, a->cv, a->version,
                               cache);
    return;
  }
  if (a->rdzv_txid == 0) {
    /* Announce: the releaser holds a->data_size dirty bytes and no home
     * address yet.  Advertise the STABLE buffer as the landing (the PUT lands
     * in place; a concurrent snapshot serve may ship a torn old/new word mix,
     * which the memory model already permits for unordered readers) and mint
     * the pairing txid.  Nothing is allocated; nothing installs at commit. */
    uint64_t db_size =
        atomic_load_explicit(&cache->db_size, memory_order_relaxed);
    if (a->data_size > db_size) {
      ARTS_ERROR("coherence: publish announce of %llu bytes exceeds the "
                 "DB's %llu-byte stable buffer",
                 (unsigned long long)a->data_size, (unsigned long long)db_size);
    }
    struct arts_rdzv_landing_s landing = {0, 0, 0, 0};
    arts_shared_ptr_t mh = arts_db_buf_acquire(cache);
    struct arts_db_buffer_s *master =
        (struct arts_db_buffer_s *)arts_shared_get(mh);
    if (master == NULL || !arts_net_rdzv_local(master->data, db_size,
                                               &landing.addr, &landing.key)) {
      /* Unreachable: the releaser obtained its write right from this home,
       * and serving that request is one of the points that materializes the
       * home's buffer. */
      ARTS_ERROR("coherence: publish announce found no stable home buffer");
    }
    landing.txid = arts_net_rdzv_txid_next();
    arts_db_buf_release(&mh);
    arts_send_db_publish_cts(a->releaser, a->db_guid, &landing, a->cv);
    return;
  }
  /* Commit: the dirty bytes were PUT in place into the stable buffer.  Pair
   * with the write completion — either arrival order — then stamp the
   * version and ACK the blocked releaser. */
  struct pub_landed_ctx_s *ctx =
      (struct pub_landed_ctx_s *)arts_malloc(sizeof(*ctx));
  ctx->db_h = arts_route_table_lookup_db(cache->db_guid);
  ctx->version = a->version;
  ctx->releaser = a->releaser;
  ctx->db_guid = a->db_guid;
  ctx->cv = a->cv;
  ctx->returns_grant = (a->returns_grant != 0u);
  arts_net_rdzv_expect(a->rdzv_txid, a->data_size, pub_landed_cb, ctx);
}

/* Cat-B pure body (OoO g_ooo_table[OOO_DB_DESTROY]): the OoO engine has already
 * acquired the home db_s and pinned a ref across this call (cache is its FIRST
 * member).  Order: roster fan-out, then arts_ooo_retire_item LAST (detach the
 * slot cb + drop the install ref); a waiter left parked at destroy (UB) is
 * never woken and keeps the descriptor through its slot's pin, and the cb
 * deleter frees the cache once every ref drains.  A second DESTROY_REQ parks on the empty
 * slot (a second destroy is a program error).  WT roster source =
 * home->cached_ranks.  The teardown is immediate: every release that sends
 * the home anything (a publish, a hand-back) blocks until the home ACKs it,
 * so under the destroy contract none arrives after the destroy; a transfer's
 * CONFIRM that does finds no home and is dropped. */
void arts_handler_db_destroy(void *item_v, void *args_v) {
  (void)args_v;
  struct arts_db_s *db = (struct arts_db_s *)item_v;
  if (db == NULL) {
    return;
  }
  arts_db_cover_destroy(db);
}

/* The teardown, run by whichever of the destroy and the last request the home
 * was told is coming reaches the gate's edge.  Roster fan-out, then the slot
 * return.  The queued ownership requesters are NOT drained for the roster:
 * the FIFO has exactly one consumer — the rank holding the transfer baton —
 * and the roster is complete without them. */
void arts_db_home_destroy_now(struct arts_db_s *db) {
  arts_guid_t guid = db->cache.db_guid;
  struct arts_rank_bitset_cursor_s c =
      arts_rank_bitset_cursor(&db->cached_ranks);
  unsigned int rank;
  while (arts_rank_bitset_next(&c, &rank)) {
    if (rank != arts_global_rank_id) {
      arts_send_db_cache_destroy(rank, guid);
    }
  }
  (void)arts_ooo_retire_item(guid, db);
}

/* Retract nothing: this arm's readers hold no durable copy claim — every
 * acquire re-validates against a version, so a creator that was given no
 * storage simply has nothing to assert. */
/* The hold is possession plus this create's own writer, from a word that
 * holds nothing.  It is a write permission landing on this rank, so every EDT
 * parked on the first-touch cache is served under it -- readers whose read
 * request is still out included -- and the requests that cache sent are the
 * home's to absorb (the announce names them). */
bool arts_db_create_take_hold(struct arts_db_cache_s *cache, uint8_t *owed) {
  *owed = 0u;
  unsigned int none = 0u;
  if (!atomic_compare_exchange_strong_explicit(
          &cache->writer_count, &none, ARTS_GRANT_SEED_HOLDING,
          memory_order_acq_rel, memory_order_relaxed)) {
    return false;
  }
  /* Writers first, then the axis: a writer that pushed before the take is
   * served by it, and one whose request commits before CLAIM is absorbed
   * (PRECLAIM); one that pushed after the take is re-armed below.  The
   * creator's own hold keeps the right here throughout. */
  arts_db_grant_claim_writers(cache);
  *owed = arts_db_val_claim_cover(cache);
  arts_db_grant_rearm(cache);
  return true;
}

void arts_db_create_retract_creator_copy(struct arts_db_s *db) { (void)db; }

/* Claim nothing either: a reader here re-validates against a version at
 * every acquire, so a copy asserts nothing that has to be recorded. */
void arts_db_create_claim_creator_copy(struct arts_db_s *db) { (void)db; }


void arts_db_create_install_home_buffer(struct arts_db_cache_s *cache,
                                        uint64_t db_size) {
  /* The stable home buffer exists from create so its address can travel as
   * a durable publish credit — but at VERSION 0: "unpublished".  The
   * has-the-creator-published predicate that used to be buffer PRESENCE is
   * version > 0 everywhere on this arm; the first publish's 0->v bump is
   * the publication point that drains the parked readers/serves. */
  if (db_size > 0) {
    (void)arts_db_buf_install(cache, /*new_version=*/0, /*data_payload=*/NULL,
                              db_size);
  }
}

/* The home's installed buffer IS the canonical copy under this write policy,
 * and every publish stamps it in place, so its version is the axis a serve is
 * judged against. */
uint64_t arts_db_grant_serve_version(struct arts_db_s *db) {
  arts_shared_ptr_t h = arts_db_buf_acquire(&db->cache);
  struct arts_db_buffer_s *b = (struct arts_db_buffer_s *)arts_shared_get(h);
  uint64_t v = (b != NULL)
                   ? atomic_load_explicit(&b->version, memory_order_acquire)
                   : ARTS_GRANT_VERSION_NONE;
  arts_db_buf_release(&h);
  return v;
}

/* Readers re-check a version at every acquire, so an ex-holder's retained
 * buffer can never be mistaken for current: nothing to register. */
void arts_db_grant_note_ex_holder(struct arts_db_s *db, unsigned int rank) {
  (void)db;
  (void)rank;
}
