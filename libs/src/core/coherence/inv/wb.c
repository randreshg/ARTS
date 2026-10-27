/* SPDX-License-Identifier: Apache-2.0
 *
 * INV protocol, WB write policy.
 *
 * WB means the canonical payload stays with whoever wrote it last and moves
 * only on demand, so the home holds no bytes at all.  That single fact is this
 * file: an RO acquire never reads a local home buffer, a release publishes
 * control only (the round still runs — the invalidation IS what makes this
 * INV), and a read request at the home is redirected to the current grant
 * holder, which serves the requester directly.
 *
 * Everything else — the reader plane, the invalidation round, the cache
 * lifecycle, the senders — is in engine.c; the home request FIFO and
 * directory lifecycle are in directory.c.  Write ownership is the shared
 * migrating grant (coherence/grant.c).
 *
 * The redirect is a retry, never a park: a rank that no longer holds the bytes
 * bounces the request back through the home, and each pass re-resolves against
 * a later ownership generation.  Nothing queues and nothing sleeps, which is
 * what keeps a reader from ever waiting behind a writer.
 */
#include "arts/coherence/inv/types.h"

#include "arts/utils/stdatomic_compat.h"
#include <stdbool.h>
#include <stddef.h>

#include "arts/coherence/buffer.h"
#include "arts/coherence/census.h"
#include "arts/coherence/coherence.h"
#include "arts/coherence/directory.h"
#include "arts/coherence/handlers.h"
#include "arts/db.h"
#include "arts/edt.h"
#include "arts/ooo.h"
#include "arts/runtime_state.h"
#include "arts/system/identity.h"
#include "arts/transport/net.h"
#include "arts/transport/protocol.h"
#include "arts/utils/malloc.h"
#include "arts/counter/Preamble.h"

/* ===== acquire ==========================================================
 * The two planes are answered independently, because they are independent:
 * a read never waits on a writer and a write never waits on a reader.
 *
 *   RO — no rank can serve a read from a home buffer here (there is none), so
 *        a rank without a covering copy always fetches: it joins or opens the
 *        single in-flight fetch, and the home redirects that fetch to the
 *        current holder.
 *   RW — the migrating sentinel grant, verbatim from the shared plane.
 */

bool arts_db_acquire_is_serialized(arts_db_access_mode_t mode) {
  /* A write acquire can wait on another rank's release (cross-DB cyclic
   * hazard: acquire-all must issue it in the one global GUID order).  A read
   * acquire waits only on the serve — never on another EDT's release — so it
   * carries no cycle risk and may issue in parallel. */
  return mode == DB_MODE_RW;
}

void arts_handler_db_acquire(void *item, void *args) {
  struct arts_db_s *db = (struct arts_db_s *)item;
  struct arts_ooo_args_db_acquire_s *a =
      (struct arts_ooo_args_db_acquire_s *)args;
  struct arts_edt_s *edt = a->edt;
  unsigned int slot = a->slot;
  struct arts_db_cache_s *cache = &db->cache;
  arts_edt_dep_t *dep = &((arts_edt_dep_t *)arts_get_depv(edt))[slot];
  arts_db_access_mode_t mode = dep->mode;

  if (mode != DB_MODE_RO) {
    /* RW: the shared grant.  A positive writer_count IS "this rank holds it",
     * and an install whose directory flip the home has not yet published may
     * NOT run: its stores would be observable while the directory still names
     * the previous owner, and a reader registered in that window is redirected
     * to that rank's retained (now stale) copy.
     *
     * Both facts live in the one word — ARTS_GRANT_UNCONFIRMED makes such an
     * install read back negative — so the fast path's CAS is the entire test
     * and needs no pre-read here.  That is what closes the window: two
     * separate loads cannot be taken atomically with each other nor with the
     * CAS, so a grant lost and re-granted between them reads exactly like one
     * never lost.  A rank that may not run parks instead, and the CONFIRM_ACK
     * drains whatever accumulated behind it. */
    if (arts_db_acquire_rw_local_fast(cache, edt, slot)) {
      arts_db_acquire_resolved(edt, slot);
      return;
    }
    arts_db_acquire_remote_rw(cache, edt, slot); /* parks */
    return;
  }

  /* Covering-copy fast path: pure loads, no CAS, no node.  A rank holding a
   * CONFIRMED grant covers reads too — it has the newest bytes by definition.
   * An unconfirmed install does not: until the home publishes the flip, the
   * bytes here are not yet the ones the directory points readers at.  The
   * SIGNED test is what draws that line: ARTS_GRANT_UNCONFIRMED puts such an
   * install below zero, so "> 0" already means "held and confirmed". */
  uint64_t peek =
      atomic_load_explicit(&cache->cache_state, memory_order_acquire);
  /* One observation governs both the decision to answer locally and the
   * materialization below: a second load of the same word could see the
   * grant withdrawn in between and skip the storage the first one entitled,
   * handing the EDT no pointer for a sized block. */
  unsigned int wc =
      atomic_load_explicit(&cache->writer_count, memory_order_acquire);
  bool own = (int)wc > 0;
  if (INV_CACHE_RO(peek) == INV_RO_VALID || own) {
    /* First use of a block whose create took no hold: its storage was left
     * for whoever uses it first, and holding the block's OWNERSHIP is what
     * entitles this rank to be it — so allocate here, on the acquiring
     * thread's node.  Not on the valid-copy half of the test above: a valid
     * copy already implies storage (the delivery that made it valid brought
     * the bytes), so a valid copy without a buffer would mean the claim
     * itself is false, and inventing a first image under it would answer
     * reads from a block this rank never held. */
    if (own) {
      (void)arts_db_buf_ensure(
          cache, atomic_load_explicit(&cache->db_size, memory_order_relaxed));
    }
    /* A durable copy answers with no message and no CAS. */
    uint8_t census = arts_db_census_serving(false);
    arts_db_resolve_slot_cached(edt, slot);
    arts_db_census_served(census);
    return;
  }

  /* Park-or-open: the decision CAS links our node as the chain head in the
   * same atom (node fields written before every attempt). */
  uint32_t idx = arts_db_reader_alloc(&cache->reader_pool);
  struct arts_db_reader_node_s *node =
      arts_db_reader_node(&cache->reader_pool, idx);
  node->edt = edt;
  ARTS_DEP_WAITER_SET(node, edt);
  node->slot = slot;
  /* Nonzero here is an install whose confirmation is still out: the right
   * that covers this read is already on this rank. */
  ARTS_DB_CENSUS_SET_HELD(node, wc != 0u);

  uint32_t act;
  uint64_t cur, next;
  do {
    cur = atomic_load_explicit(&cache->cache_state, memory_order_acquire);
    atomic_store_explicit(&node->next, INV_CACHE_HEAD_RO(cur),
                          memory_order_relaxed);
    next = arts_inv_cache_compute_next(cur, INV_CACHE_OP_ACQ_RO, idx, &act);
    if (next == cur) {
      break; /* copy turned valid mid-retry: no word write */
    }
  } while (!atomic_compare_exchange_weak_explicit(&cache->cache_state, &cur,
                                                  next, memory_order_acq_rel,
                                                  memory_order_acquire));

  switch (act) {
  case INV_CACHE_ACT_SELF_SERVE: {
    /* The copy turned valid while we were deciding — served from here. */
    arts_db_reader_free(&cache->reader_pool, idx);
    uint8_t census = arts_db_census_serving(false);
    arts_db_resolve_slot_cached(edt, slot);
    arts_db_census_served(census);
    break;
  }
  case INV_CACHE_ACT_SEND_RO:
    arts_send_db_inv_request(cache, DB_MODE_RO, arts_inv_read_cover(next));
    break;
  default: /* PARK: the fetch's committer will serve us */
    break;
  }
}

/* ===== release_rw (WB write policy) =====================================
 * The WT arm with one argument changed: the payload does not travel to the
 * home, because the home never serves it.  The round still runs — that is what
 * makes this INV and not VAL — so the release still returns only once every
 * stale copy is dead, and the count is still dropped only after the round
 * acks, which is what orders any ownership transfer behind it.
 */
void arts_db_release_rw(struct arts_db_cache_s *cache, void *payload) {
  (void)payload;
  /* Defensive: no local hold to drop means our acquire never bumped the grant;
   * decrementing would underflow. */
  if (arts_db_grant_release_skip(cache)) {
    return;
  }
  arts_shared_ptr_t buf_h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *buf =
      (struct arts_db_buffer_s *)arts_shared_get(buf_h);
  uint64_t new_version = 0;
  if (buf != NULL) {
    new_version =
        atomic_fetch_add_explicit(&buf->version, 1u, memory_order_acq_rel) + 1u;
    arts_db_buf_release(&buf_h);
  }
  /* Control-only publish: ask the home for this release's invalidation round
   * and block until every ack is in.  No bytes move. */
  TIME_INVALIDATE_ROUND_START();
  arts_db_publish_sync(cache, new_version);
  TIME_INVALIDATE_ROUND_STOP();

  /* The count-dropping edge, sequenced after the round: the last releaser
   * ships the grant onward when an INVALIDATE has already named a target. */
  arts_db_grant_release_commit(cache);
}

/* ===== read request at the home =========================================
 * The home holds no bytes, so it cannot answer.  It registers the requester in
 * the roster — BEFORE the redirect departs, so a copy that is about to exist is
 * already a target of the next round — and points the requester at the rank the
 * directory currently names.
 */
void arts_handler_db_inv_request(void *item_v, void *args_v) {
  struct arts_db_s *db = (struct arts_db_s *)item_v;
  struct arts_ooo_args_db_inv_request_s *a =
      (struct arts_ooo_args_db_inv_request_s *)args_v;
  struct arts_db_cache_s *cache = &db->cache;
  if (!a->bounce) {
    bool td = false;
    int r = arts_db_cover_request(db, a->requester, false, a->cover, &td);
    if (r == ARTS_COVER_DISMISS) {
      /* Declared: a write turn of the requester's rank covers this request.
       * Dismiss it -- no data, no read turn -- returning the landing. */
      arts_send_db_inv_cts(
          a->requester, db, db->cache.db_guid,
          atomic_load_explicit(&db->cache.db_size, memory_order_relaxed),
          a->cover, a->rdzv.cookie);
    }
    if (td) {
      arts_db_home_destroy_now(db);
    }
    if (r != ARTS_COVER_PROCESS) {
      return;
    }
  }
  arts_rank_bitset_note(&db->cached_ranks, a->requester);
  if (a->rdzv.txid == 0 &&
      atomic_load_explicit(&cache->db_size, memory_order_relaxed) != 0 &&
      a->requester != arts_global_rank_id) {
    /* First touch: the request carried no landing (db_size unknown at the
     * requester).  Answer the size and let it re-issue with one. */
    INCREMENT_NUM_INV_SIZE_CTS_BY(1);
    arts_send_db_inv_cts(
        a->requester, db, cache->db_guid,
        atomic_load_explicit(&cache->db_size, memory_order_relaxed), a->cover,
        a->rdzv.cookie);
    return;
  }
  if (a->mode != DB_MODE_RO) {
    /* There is no write branch: RW acquires go through the shared grant's
     * GRANT_REQUEST.  This handler serves readers only. */
    return;
  }
  /* Roster bit BEFORE the serve/redirect departs.  Same-word pairing with
   * the round's snapshot exchange (inv_round_run): either that round names
   * this reader, or this serve is ordered after the snapshot.  A reader
   * still sent to the previous holder is one unordered with that round's
   * write, and may read the older version. */
  (void)arts_rank_bitset_set(&db->roster, a->requester);
  unsigned int holder =
      atomic_load_explicit(&db->rw_holder, memory_order_acquire);
  if (holder == arts_global_rank_id) {
    /* The home happens to hold the grant: serve from here, no redirect. */
    arts_send_db_inv_deliver(a->requester, db, &a->rdzv);
    return;
  }
  arts_send_db_inv_redirect(holder, cache->db_guid, a->requester, a->cover,
                            &a->rdzv);
}

/* ===== redirect (home -> current holder) ================================
 * The holder serves the named requester from its own buffer.  A MISS — the
 * bytes left this rank between the home's directory read and this arrival —
 * bounces the request back to the home rather than queueing: nothing here may
 * block a reader, and the next pass resolves against a later generation.
 */
void arts_handler_db_inv_redirect(void *item_v, void *args_v) {
  struct arts_db_s *db = (struct arts_db_s *)item_v;
  struct arts_ooo_args_db_inv_redirect_s *a =
      (struct arts_ooo_args_db_inv_redirect_s *)args_v;
  struct arts_db_cache_s *cache = &db->cache;
  /* "Do I still hold the bytes?" is a question only a DB that HAS bytes can be
   * asked.  A sentinel DB (db_size == 0) never installs a buffer — NULL is its
   * defined value — so testing for one would bounce every read back to the
   * home forever.  Serve it unconditionally; the reply is header-only. */
  if (atomic_load_explicit(&cache->db_size, memory_order_relaxed) != 0) {
    arts_shared_ptr_t buf_h = arts_db_buf_acquire(cache);
    bool have = (arts_shared_get(buf_h) != NULL);
    arts_db_buf_release(&buf_h);
    if (!have) {
      /* The bytes moved on between the home's directory read and this
       * arrival.  Bounce rather than queue — a reader must never wait here —
       * and let the next pass resolve against a later generation. */
      arts_send_db_inv_request_for(cache->db_guid, a->requester, a->cover,
                                   &a->rdzv);
      return;
    }
  }
  arts_send_db_inv_deliver(a->requester, db, &a->rdzv);
}


/* ===== WB-only senders ================================================== */

/* home → the current grant holder: serve this reader.  The subject rides in
 * the packet because the holder answers it directly, so the wire sender and
 * the requester are different ranks. */
void arts_send_db_inv_redirect(unsigned int holder_rank, arts_guid_t db_guid,
                               unsigned int requester, uint32_t cover,
                               const struct arts_rdzv_landing_s *rdzv) {
  struct arts_msg_inv_redirect_packet_s p;
  arts_fill_packet_header(&p.header, sizeof(p), MSG_DB_INV_REDIRECT);
  p.header.rank = arts_global_rank_id;
  p.db_guid = db_guid;
  p.requester = requester;
  p.cover = cover;
  p.rdzv.addr = rdzv->addr;
  p.rdzv.key = rdzv->key;
  p.rdzv.txid = rdzv->txid;
  p.rdzv.cookie = rdzv->cookie;
  if (holder_rank == arts_global_rank_id) {
    struct arts_ooo_args_db_inv_redirect_s args = {.requester = requester,
                                                   .cover = cover,
                                                   .db_guid = db_guid,
                                                   .rdzv = *rdzv};
    arts_shared_ptr_t h = arts_route_table_lookup_db(db_guid);
    struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(h);
    if (db != NULL) {
      arts_handler_db_inv_redirect(db, &args);
    } else {
      /* Torn down after the home took the request in: dismiss it. */
      arts_send_db_inv_cts(requester, NULL, db_guid, 0u, cover, rdzv->cookie);
    }
    arts_shared_release(&h);
    return;
  }
  if (!arts_net_send_held((int)holder_rank, (char *)&p, sizeof(p),
                          ARTS_HELD_HOP, db_guid)) {
    arts_transport_send_async((int)holder_rank, (char *)&p, sizeof(p));
  }
}

/* The bounce: a holder that no longer has the bytes re-sends the read request
 * to the home on the reader's behalf.  A retry, not a park — the next pass
 * resolves against a later ownership generation. */
void arts_send_db_inv_request_for(arts_guid_t db_guid, unsigned int requester,
                                  uint32_t cover,
                                  const struct arts_rdzv_landing_s *rdzv) {
  unsigned int home_rank = arts_guid_get_rank(db_guid);
  struct arts_msg_inv_request_packet_s p;
  arts_fill_packet_header(&p.header, sizeof(p), MSG_DB_INV_REQUEST);
  p.header.rank = arts_global_rank_id;
  p.db_guid = db_guid;
  p.mode = (uint16_t)DB_MODE_RO;
  p.cover = (uint8_t)cover;
  p.flags = ARTS_INV_REQ_BOUNCE;
  p.requester = requester;
  p.rdzv.addr = rdzv->addr;
  p.rdzv.key = rdzv->key;
  p.rdzv.txid = rdzv->txid;
  p.rdzv.cookie = rdzv->cookie;
  if (home_rank == arts_global_rank_id) {
    struct arts_ooo_args_db_inv_request_s args = {.requester = requester,
                                                  .db_guid = db_guid,
                                                  .mode = DB_MODE_RO,
                                                  .cover = cover,
                                                  .bounce = true,
                                                  .rdzv = *rdzv};
    arts_shared_ptr_t h = arts_route_table_lookup_db(db_guid);
    struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(h);
    if (db != NULL) {
      arts_handler_db_inv_request(db, &args);
    } else {
      /* Torn down after the home took the request in: dismiss it. */
      arts_send_db_inv_cts(requester, NULL, db_guid, 0u, cover, rdzv->cookie);
    }
    arts_shared_release(&h);
    return;
  }
  if (!arts_net_send_held((int)home_rank, (char *)&p, sizeof(p),
                          ARTS_HELD_HOP, db_guid)) {
    arts_transport_send_async((int)home_rank, (char *)&p, sizeof(p));
  }
}
