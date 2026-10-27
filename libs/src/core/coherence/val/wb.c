/* SPDX-License-Identifier: Apache-2.0
 *
 * WB write-policy translation unit: defines the WB-specific
 * arts_handler_db_* / arts_db_* bodies directly (CMake links exactly this TU
 * for a VAL+WB build) plus the WB-only wire handlers/senders.
 * Compiled only for ARTS_COHERENCE_PROTOCOL=VAL with
 * ARTS_WRITE_POLICY=WB (selected in libs/src/core/CMakeLists.txt).
 * Contains NO protocol/write-policy preprocessor logic.
 */
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
#include "arts/gas/route_table.h" /* arts_route_table_lookup_db (Cat-C self-send) */
#include "arts/ooo.h"             /* OOO_DB_* args (handler bodies) */
#include "arts/runtime_state.h"
#include "arts/runtime_types.h"
#include "arts/system/threads.h"   /* arts_global_rank_id */
#include "arts/transport/net.h" /* arts_transport_send_async */
#include "arts/transport/protocol.h"
#include "arts/utils/malloc.h"  /* arts_malloc / arts_free (transfer sender) */

/* ===== 8-case acquire dispatch (WB arm) ==========================
 * Whole arts_handler_db_acquire body for the WB build.  Diverges from WT
 * only on the RO-has-local-data predicate (WB: is_owner — the home rank does
 * NOT hold the canonical copy; only the current owner has an installed buffer,
 * so a home-but-not-owner rank goes through acquire_remote_ro and home forwards
 * to the owner via SNAPSHOT_REDIRECT). */
void arts_handler_db_acquire(void *item, void *args) {
  struct arts_db_s *db = (struct arts_db_s *)item;
  struct arts_ooo_args_db_acquire_s *a =
      (struct arts_ooo_args_db_acquire_s *)args;
  struct arts_edt_s *edt = a->edt;
  unsigned int slot = a->slot;
  struct arts_db_cache_s *cache = &db->cache;
  arts_edt_dep_t *dep = &((arts_edt_dep_t *)arts_get_depv(edt))[slot];
  arts_db_access_mode_t mode = dep->mode;
  /* A NON-ZERO writer_count means this rank holds the grant and therefore the
   * canonical buffer — an install the home has not confirmed yet included,
   * which ARTS_GRANT_UNCONFIRMED makes negative rather than zero.  Reading in
   * that window is safe precisely because no RW EDT can have run here: the
   * bytes are exactly the ones the transfer delivered.  A zero count is the
   * only state in which this rank's copy may be stale. */
  bool has_canonical_copy =
      (atomic_load_explicit(&cache->writer_count, memory_order_acquire) != 0u);

  if (mode == DB_MODE_RO) {
    if (has_canonical_copy) { /* WB RO predicate (only a grant holder has the
                                 canonical copy; home holds no bytes) */
      /* First use of a block whose create took no hold: its storage was left
       * for whoever uses it first, and holding the block's ownership is what
       * entitles this rank to be it — so allocate here, on the acquiring
       * thread's node.  A no-op for every other block: a grant install
       * always brings storage with it. */
      (void)arts_db_buf_ensure(
          cache, atomic_load_explicit(&cache->db_size, memory_order_relaxed));
      INCREMENT_NUM_DB_ACQUIRE_LOCAL_HIT_BY(1);
      arts_db_acquire_local_hit(edt, slot, arts_db_acquire_local(cache));
      arts_db_acquire_resolved(edt, slot);
      return;
    }
    arts_db_acquire_remote_ro(cache, edt->guid, slot); /* parks (SNAPSHOT) */
    return;
  }
  /* RW: the fast path's CAS is the whole test.  It takes a turn only against a
   * grant this rank holds AND the home has already confirmed, in one atom — an
   * install whose rw_holder flip is still unpublished reads back negative and
   * is refused, because writing now would be observable while the directory
   * names the previous owner (the stale-RO window).  Everything else parks;
   * the in-flight round holds the request word's write axis (SENT until
   * CONFIRM_ACK's drain clears it), so acquire_remote_rw parks without
   * issuing a duplicate request, and CONFIRM_ACK drains it. */
  if (arts_db_acquire_rw_local_fast(cache, edt, slot)) {
    arts_db_acquire_resolved(edt, slot); /* data here, writer_count bumped */
    return;
  }
  arts_db_acquire_remote_rw(cache, edt->guid,
                            slot); /* parks (GRANT_REQUEST) */
}

bool arts_db_acquire_is_serialized(arts_db_access_mode_t mode) {
  return mode == DB_MODE_RW;
}

/* ===== release_rw (WB arm) =======================================
 * The WB write policy drops the buffer ref BEFORE decrementing writer_count, so
 * the slot's cache-hold is the only ref that can keep the buffer alive past
 * writer_count==0 (a concurrent teardown then ends it via the cb deleter with
 * no dangling local ref).  In the WT write policy this window does not exist
 * (local_transfer_now restores the sentinel); the WB write policy has no sentinel
 * restoration, so it must close the window by releasing the ref before exposing
 * writer_count==0. */
/* A create's hold is this arm's ordinary write hold, taken when the block was
 * made; the bytes under it are the cache's own, so the release needs no
 * pointer to them. */
void arts_db_release_created(struct arts_db_cache_s *cache) {
  arts_db_release_rw(cache, NULL);
}

void arts_db_release_rw(struct arts_db_cache_s *cache, void *payload) {
  (void)payload;
  /* Defensive: no local hold to drop means our acquire never bumped ownership;
   * decrementing would underflow. */
  if (arts_db_grant_release_skip(cache)) {
    return;
  }
  /* Version bump on the current buffer (local ref scoped to release_rw). */
  arts_shared_ptr_t buf_h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *buf =
      (struct arts_db_buffer_s *)arts_shared_get(buf_h);
  if (buf != NULL) {
    uint64_t new_version =
        atomic_fetch_add_explicit(&buf->version, 1u, memory_order_acq_rel) + 1u;
    /* Self-credit: this rank now holds the bumped version.  Keeps the
     * owner-side ledger's invariant ("floor of what each rank's cache holds")
     * true for the producer itself; the ship path re-credits before the map
     * migrates. */
    if (cache->cached_version != NULL) {
      arts_rank_u64_map_advance(cache->cached_version, arts_global_rank_id,
                                new_version);
    }
  }
  /* Drop the buffer ref BEFORE the writer_count decrement (close the
   * writer_count==0 teardown window). */
  if (buf != NULL) {
    arts_db_buf_release(&buf_h);
  }

  /* The count-dropping edge: the last releaser ships the grant onward when a
   * GRANT_INVALIDATE has already named a target; otherwise the home keeps
   * ownership until a GRANT_REQUEST and a non-home owner quiesces. */
  arts_db_grant_release_commit(cache);
}

/* ===== cache_s lifecycle (WB) ======================================
 * Construct: the WB write policy's own fields (the pending_rw Treiber stack,
 * the owner-side dedup ledger, the transfer target), then
 * arts_db_cache_common_init.  Destruct order: buffer-NULL (pre) → pending_rw
 * destroy → ledger free → snapshot drain + home teardown (post). */
void arts_db_cache_init(struct arts_db_cache_s *c, arts_guid_t db_guid,
                        uint64_t db_size, arts_db_init_kind_t kind,
                        unsigned int creator_rank) {
  arts_pending_rw_queue_init(&c->pending_rw);
  /* WB owner-side fields.  The dedup ledger is allocated HERE, once, rather
   * than on first use: its readers hold no reference to it, so a ledger that
   * could be created (or replaced) later would have to carry a lifetime
   * protocol to be read safely.  Allocated with the cache and freed with it,
   * a reader can only ever meet the live object. */
  c->cached_version = arts_rank_u64_map_create(arts_global_rank_count);
  c->incoming_new_owner = ARTS_NO_PENDING_OWNER;
  c->incoming_new_owner_rdzv = (struct arts_rdzv_landing_s){0, 0, 0, 0};
  c->incoming_new_owner_have = ARTS_GRANT_VERSION_NONE;
  arts_db_cache_common_init(c, db_guid, db_size, kind, creator_rank);
}

void arts_db_cache_destructor(struct arts_db_cache_s *cache) {
  if (cache == NULL) {
    return;
  }
  arts_db_cache_common_destroy_pre(cache); /* buffer-NULL FIRST */
  arts_pending_rw_queue_destroy(&cache->pending_rw);
  arts_rank_u64_map_destroy(cache->cached_version);
  cache->cached_version = NULL;
  arts_db_cache_common_destroy_post(cache); /* snapshot drain → home teardown */
}

/* ===== home-directory lifecycle (inlined in arts_db_s) ============= */

void arts_db_home_init(struct arts_db_s *db, unsigned int rw_holder,
                       unsigned int nranks) {
  atomic_store_explicit(&db->rw_holder, rw_holder, memory_order_relaxed);
  arts_home_grantreq_queue_init(&db->pending_rw);
  atomic_store_explicit(&db->invalidate_in_flight, 0, memory_order_relaxed);
  arts_rank_bitset_init(&db->cached_ranks, nranks);
  arts_db_cover_home_init(db, nranks);
  /* A creator on another rank holds a cache of the block from the moment it
   * made it, so the destroy must reach that rank too: its cache would
   * otherwise outlive the block, keeping its storage and the rank's route
   * slot for the GUID (where a later create of the GUID would park). */
  if (rw_holder != arts_global_rank_id && rw_holder < nranks) {
    arts_rank_bitset_set(&db->cached_ranks, rw_holder);
  }
  atomic_store_explicit(&db->pending_install_owner, 0u, memory_order_relaxed);
}

void arts_db_home_teardown(struct arts_db_s *db) {
  if (db == NULL) {
    return;
  }
  arts_home_grantreq_queue_destroy(&db->pending_rw);
  arts_rank_bitset_destroy(&db->cached_ranks);
  arts_db_cover_home_teardown(db);
  /* No free: home fields are inlined in the arts_db_s. */
}

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

  /* WB home-side RO routing.
   *
   * Under the WB write policy home does not hold the canonical data copy — the
   * current owner does.  Home's job is to redirect the requester to the owner
   * (via SNAPSHOT_REDIRECT) so the owner can send SNAPSHOT_RESPONSE directly,
   * applying the owner-side cached_version dedup.
   *
   * Record the requester in the cached-ranks set, then redirect to the current
   * owner.  A destroyed DB is handled by the route_table lookup miss (slot
   * value NULL-stored before destroy) + the handler single-actor invariant —
   * no per-home destroy flag. */
  struct arts_db_s *db = arts_db_of_cache(cache);
  arts_rank_bitset_set(&db->cached_ranks, requester);
  /* Forward to the current owner unconditionally: even mid-transfer, rw_holder
   * still names the OLD owner, which retains its buffer + cached_version
   * permanently and serves the REDIRECT from its own copy.  Client-side
   * monotonic version compare keeps stale snapshots safe.  No defer queue. */
  uint64_t db_size =
      atomic_load_explicit(&cache->db_size, memory_order_relaxed);
  if (a->rdzv.txid == 0 && db_size > 0) {
    /* First-touch request without a landing: the requester did not know
     * db_size.  Home is the size authority even though the data lives with
     * the owner — answer a size-only CTS directly; the re-request carries a
     * landing and is then redirected for real. */
    INCREMENT_NUM_SNAPSHOT_SIZE_CTS_BY(1);
    arts_send_db_snapshot_response(requester, db, cache->db_guid,
                                   /*version=*/0, cover, /*kind=*/2, db_size,
                                   &a->rdzv, NULL);
    return;
  }
  unsigned int owner =
      atomic_load_explicit(&db->rw_holder, memory_order_acquire);
  arts_send_db_snapshot_redirect(owner, cache->db_guid, requester, cover,
                                 &a->rdzv);
}

/* Fan-out callback for arts_rank_bitset_for_each during destroy.  ctx carries
 * the db_guid encoded as uintptr_t (the callback is synchronous). */
static void owner_destroy_fanout_cb(unsigned int rank, void *ctx) {
  if (rank != arts_global_rank_id) {
    arts_send_db_cache_destroy(rank, (arts_guid_t)(uintptr_t)ctx);
  }
}

/* Cat-B pure body (OoO g_ooo_table[OOO_DB_DESTROY]): the OoO engine has already
 * acquired the home db_s and pinned a ref across this call (cache is its FIRST
 * member).  Order: roster fan-out, then arts_ooo_retire_item LAST (parked
 * waiter at destroy = UB, cleaned up by the refcount-0 destructor).  WB roster
 * source = the cached-ranks bit-set: the creator joins it at create and every
 * requester when its request arrives, so it names rw_holder and, during a
 * transfer, pending_install_owner too.  The teardown is immediate: no release
 * on this arm sends the home anything, and a transfer's CONFIRM is answered
 * before the new owner's writers run, so under the destroy contract nothing
 * owed to the home can arrive after the destroy. */
void arts_handler_db_destroy(void *item_v, void *args_v) {
  (void)args_v;
  struct arts_db_s *db = (struct arts_db_s *)item_v;
  if (db == NULL) {
    return;
  }
  arts_db_cover_destroy(db);
}

/* The teardown, run by whichever of the destroy and the last request the home
 * was told is coming reaches the gate's edge: roster fan-out, then the slot
 * return.  The request FIFO is not read: its one consumer is the baton holder,
 * and every queued requester is already on the bit-set. */
void arts_db_home_destroy_now(struct arts_db_s *db) {
  arts_rank_bitset_for_each(&db->cached_ranks, owner_destroy_fanout_cb,
                            (void *)(uintptr_t)db->cache.db_guid);
  (void)arts_ooo_retire_item(db->cache.db_guid, db);
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


void arts_handler_db_snapshot_redirect(void *item_v, void *args_v) {
  struct arts_db_cache_s *cache = &((struct arts_db_s *)item_v)->cache;
  struct arts_db_snapshot_redirect_args_s *a =
      (struct arts_db_snapshot_redirect_args_s *)args_v;
  unsigned int requester = a->requester_rank;
  uint32_t cover = a->cover;

  /* A remote first use of a block whose create took no hold: this rank owes
   * the reply the block's first image, so it materializes one (on a progress
   * thread) when the block has no storage yet.  Gated on HOLDING the block,
   * not on being the rank the directory named: between a ship and its
   * CONFIRM the directory still names the previous holder, and that rank has
   * already given the block up — inventing a first image there would put a
   * second version 1 in the world, which the new holder's first release then
   * collides with.  A holder that no longer holds answers with the
   * holds-nothing reply below, and the requester adopts its own landing. */
  bool holds =
      atomic_load_explicit(&cache->writer_count, memory_order_acquire) != 0u;
  /* Read after the acquire on writer_count, which makes the size the holder
   * learned before its install visible. */
  uint64_t db_size =
      atomic_load_explicit(&cache->db_size, memory_order_relaxed);
  if (holds) {
    (void)arts_db_buf_ensure(cache, db_size);
  }

  arts_shared_ptr_t buf_h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *buf =
      (struct arts_db_buffer_s *)arts_shared_get(buf_h);
  if (buf == NULL) {
    /* Nothing to serve, for one of two reasons: the block is zero-sized
     * (NULL is its defined value), or the directory named this rank while its
     * ship to the next holder was still in flight, so it has already given
     * the block up.  Both answer the same way — version 0 is
     * ARTS_GRANT_VERSION_NONE, "this rank holds nothing" — and the requester
     * reads that as "nobody has written the block", adopting its own
     * advertised landing as the storage it is owed. */
    arts_send_db_snapshot_response(requester, arts_db_of_cache(cache),
                                   a->db_guid, /*version=*/0, cover,
                                   /*kind=*/0, db_size, &a->rdzv, NULL);
    return;
  }
  /* Acquire: the version names the bytes served below, so every write of
   * the release that set it (its acq_rel bump in arts_db_release_rw) must be
   * visible to the serve. */
  uint64_t cur_v = atomic_load_explicit(&buf->version, memory_order_acquire);
  uint64_t known_v =
      arts_rank_u64_map_get(cache->cached_version, requester);

  if (known_v >= cur_v) {
    /* Requester already holds this version — no payload moves; echo the
     * unused landing for recycling. */
    INCREMENT_NUM_SNAPSHOT_HEADER_ONLY_BY(1);
    arts_db_buf_release(&buf_h);
    arts_send_db_snapshot_response(requester, arts_db_of_cache(cache),
                                   a->db_guid, cur_v, cover, /*kind=*/0,
                                   db_size, &a->rdzv, NULL);
  } else if (a->rdzv.txid == 0) {
    /* Data must move but no landing was forwarded (a first-touch request that
     * slipped past home's CTS gate can only mean home learned the size after
     * forwarding — defensive).  Size-only CTS; the re-request (via home)
     * carries a landing.  The watermark does NOT advance. */
    arts_db_buf_release(&buf_h);
    INCREMENT_NUM_SNAPSHOT_SIZE_CTS_BY(1);
    arts_send_db_snapshot_response(requester, arts_db_of_cache(cache),
                                   a->db_guid, cur_v, cover, /*kind=*/2,
                                   db_size, &a->rdzv, NULL);
  } else {
    /* Advance dedup watermark (monotonic max), then PUT the payload into the
     * forwarded landing (buf_h transfers into the sender). */
    arts_rank_u64_map_advance(cache->cached_version, requester, cur_v);
    arts_send_db_snapshot_response(requester, arts_db_of_cache(cache),
                                   a->db_guid, cur_v, cover, /*kind=*/1,
                                   db_size, &a->rdzv, buf_h);
  }
}

void arts_send_db_snapshot_redirect(unsigned int owner_rank,
                                    arts_guid_t db_guid,
                                    unsigned int requester_rank, uint32_t cover,
                                    const struct arts_rdzv_landing_s *rdzv) {
  struct arts_rdzv_landing_s fwd =
      (rdzv != NULL) ? *rdzv : (struct arts_rdzv_landing_s){0, 0, 0, 0};
  struct arts_msg_snapshot_redirect_packet_s p;
  arts_fill_packet_header(&p.header, sizeof(p), MSG_DB_SNAPSHOT_REDIRECT);
  p.header.rank = arts_global_rank_id;
  p.db_guid = db_guid;
  p.requester_rank = requester_rank;
  p.cover = cover;
  p.rdzv.addr = fwd.addr;
  p.rdzv.key = fwd.key;
  p.rdzv.txid = fwd.txid;
  p.rdzv.cookie = fwd.cookie;
  if (owner_rank == arts_global_rank_id) {
    /* Self-send: mirror the wire RX dispatcher's lookup.  HIT serves
     * SNAPSHOT_RESPONSE from the ref-pinned owner-side db_s; MISS dismisses
     * the requester, as the wire path does. */
    struct arts_db_snapshot_redirect_args_s args = {
        .db_guid = db_guid,
        .requester_rank = requester_rank,
        .cover = cover,
        .rdzv = fwd,
    };
    arts_shared_ptr_t h = arts_route_table_lookup_db(db_guid);
    struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(h);
    if (db != NULL) {
      arts_handler_db_snapshot_redirect(db, &args);
    } else {
      /* Torn down after the home took the request in: dismiss it. */
      arts_send_db_snapshot_response(requester_rank, NULL, db_guid,
                                     /*version=*/0, cover, /*kind=*/2,
                                     /*db_size=*/0, &fwd, NULL);
    }
    arts_shared_release(&h);
    return;
  }
  if (!arts_net_send_held((int)owner_rank, (char *)&p, sizeof(p),
                          ARTS_HELD_HOP)) {
    arts_transport_send_async((int)owner_rank, (char *)&p, sizeof(p));
  }
}

/* Case-D leaf: the WB write policy defers the home-buffer install to the
 * creator's first release_rw (PUBLISH / GRANT path); nothing to do at
 * create. */
void arts_db_create_install_home_buffer(struct arts_db_cache_s *cache,
                                        uint64_t db_size) {
  (void)cache;
  (void)db_size;
}

/* Readers re-check a version at every acquire, so an ex-holder's retained
 * buffer can never be mistaken for current: nothing to register. */
void arts_db_grant_note_ex_holder(struct arts_db_s *db, unsigned int rank) {
  (void)db;
  (void)rank;
}
