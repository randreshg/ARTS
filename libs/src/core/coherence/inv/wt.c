/* SPDX-License-Identifier: Apache-2.0
 *
 * INV protocol, WT write policy.
 *
 * WT means the canonical payload comes back to the data block's home rank at
 * every release, so the home can serve readers from it.  That single fact is
 * this file: an RO acquire on the home rank reads the home buffer directly, a
 * release publishes its payload along with the round request, and a read
 * request at the home is answered from the home's own bytes.
 *
 * Everything else — the reader plane, the invalidation round, the cache
 * lifecycle, the senders — is in engine.c; the home request FIFO and
 * directory lifecycle are in directory.c.  Write ownership is the shared
 * migrating grant.
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
#include "arts/utils/malloc.h"
#include "arts/counter/Preamble.h"

/* ===== acquire ==========================================================
 * The two planes are answered independently, because they are independent:
 * a read never waits on a writer and a write never waits on a reader.
 *
 *   RO — served from the home (WT write policy puts current bytes there).  A
 *        rank holding a VALID copy short-circuits to a pure load; otherwise it
 *        joins or opens the single in-flight fetch.
 *   RW — the migrating sentinel grant, verbatim from the shared plane: bump
 *        the count if this rank already holds the grant, else park on
 *        pending_rw and let the coalescing flag decide who requests.
 */

bool arts_db_acquire_is_serialized(arts_db_access_mode_t mode) {
  /* A write acquire can wait on another rank's release (cross-DB cyclic
   * hazard: acquire-all must issue it in the one global GUID order).  A read
   * acquire waits only on the home's reply — never on another EDT's release —
   * so it carries no cycle risk and may issue in parallel. */
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
  bool is_home = (arts_guid_get_rank(cache->db_guid) == arts_global_rank_id);

  if (mode != DB_MODE_RO) {
    /* RW: the shared grant.  writer_count > 0 IS "this rank holds it". */
    if (arts_db_acquire_rw_local_fast(cache, edt, slot)) {
      arts_db_acquire_resolved(edt, slot);
      return;
    }
    arts_db_acquire_remote_rw(cache, edt, slot); /* parks */
    return;
  }

  if (is_home) {
    /* First use of a block whose create took no hold: its storage was left
     * for whoever uses it first, and on this arm only the home may hold the
     * canonical copy — so if the block has none, this acquiring thread
     * allocates it, on its own node.  A no-op for every other block (the
     * home's buffer exists from create, at version 0 until published). */
    (void)arts_db_buf_ensure(
        cache, atomic_load_explicit(&cache->db_size, memory_order_relaxed));
    /* The home's installed buffer is the canonical copy — serve it.  Before
     * the creator's first publish there is nothing to serve, so hold on the
     * publication waiters and let the install's drain resume us. */
    if (atomic_load_explicit(&cache->db_size, memory_order_relaxed) != 0) {
      arts_shared_ptr_t buf_h = arts_db_buf_acquire(cache);
      struct arts_db_buffer_s *pb =
          (struct arts_db_buffer_s *)arts_shared_get(buf_h);
      /* Published = version bumped past the create-time zero; the stable
       * buffer itself exists from create. */
      bool published =
          (pb != NULL &&
           atomic_load_explicit(&pb->version, memory_order_acquire) > 0);
      arts_db_buf_release(&buf_h);
      if (!published) {
        struct arts_db_read_waiter_s *w =
            (struct arts_db_read_waiter_s *)arts_malloc(sizeof(*w));
        w->edt = edt;
        ARTS_DEP_WAITER_SET(w, edt);
        w->slot = slot;
        w->serve = NULL; /* local waiter: the drain resumes the EDT */
        w->requester = arts_global_rank_id;
        w->rdzv = (struct arts_rdzv_landing_s){0, 0, 0, 0};
        arts_lf_stack_push(&cache->publication_waiters, &w->link);
        /* Race recovery: a publication may have landed between the read and
         * the push — drain (our own node included) so nobody parks forever.
         * Same-word pairing on publication_waiters closes the store->load
         * shape with every publisher, which stamps the version and then
         * drains (arts_db_drain_publication_waiters): of this acq_rel push
         * and that acq_rel drain, the later one in the stack's modification
         * order reads the other, so either the drain takes this node or this
         * re-read sees the stamp. */
        buf_h = arts_db_buf_acquire(cache);
        pb = (struct arts_db_buffer_s *)arts_shared_get(buf_h);
        published = (pb != NULL && atomic_load_explicit(
                                       &pb->version, memory_order_acquire) > 0);
        arts_db_buf_release(&buf_h);
        if (published) {
          arts_db_drain_publication_waiters(cache);
        }
        return;
      }
    }
    uint8_t census = arts_db_census_serving(false);
    arts_db_resolve_slot_cached(edt, slot);
    arts_db_census_served(census);
    return;
  }

  /* Covering-copy fast path: pure loads, no CAS, no node.  This is the whole
   * point of a write-invalidate sharer plane — a reader that holds a valid
   * copy costs nothing until a writer kills it.  A rank holding the grant
   * covers reads too: it has the newest bytes by definition. */
  uint64_t peek =
      atomic_load_explicit(&cache->cache_state, memory_order_acquire);
  if (INV_CACHE_RO(peek) == INV_RO_VALID ||
      (int)atomic_load_explicit(&cache->writer_count, memory_order_acquire) >
          0) {
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
  ARTS_DB_CENSUS_SET_HELD(node, false);

  uint32_t act;
  uint64_t cur, next;
  do {
    cur = atomic_load_explicit(&cache->cache_state, memory_order_acquire);
    atomic_store_explicit(&node->next, INV_CACHE_HEAD_RO(cur),
                          memory_order_relaxed);
    next = inv_cache_compute_next(cur, INV_CACHE_OP_ACQ_RO, idx, &act);
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
    arts_send_db_inv_request(cache, DB_MODE_RO, inv_read_cover(next));
    break;
  default: /* PARK: the fetch's committer will serve us */
    break;
  }
}

/* ===== release_rw (WT write policy) ======================================
 * Identical in shape to every other grant-bearing arm: bump the version,
 * publish, drop the count, and on the 0-edge ship the grant onward.  Two
 * things distinguish this arm, and only these two:
 *
 *   - WT write policy means the publish carries the payload, so the home holds
 *     current bytes and can serve readers from them;
 *   - INV means the home does not acknowledge that publish until it has run an
 *     invalidation round over the sharer roster and collected every ack.  The
 *     release therefore returns only when no stale copy of this data block
 *     exists anywhere.
 *
 * Both live behind the ONE blocking call below.  Everything after it is the
 * shared grant discipline: the count is dropped AFTER the round acks, so the
 * 0-edge — and therefore any ownership transfer — is structurally ordered
 * behind this release's round.  An INVALIDATE arriving mid-round is harmless:
 * it withdraws the sentinel and names the next owner, but this releaser still
 * holds its own count, so the ship happens here, at its decrement.
 */
void arts_db_release_rw(struct arts_db_cache_s *cache, void *payload) {
  (void)payload;
  /* Defensive: no local hold to drop means our acquire never bumped the grant;
   * decrementing would underflow.  Which word states carry no hold is the
   * release policy's — possession alone is one of them under a
   * voluntary-return policy and not under a revoked one. */
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
  }
  /* A home owner's buffer IS the home buffer, so no bytes move — but the round
   * still runs: it is what retires the remote copies.  Derived ONCE: the
   * hand-back rides this decision, and deriving it twice is how the two drift
   * apart. */
  bool will_publish = (buf != NULL);
  /* Where the write right goes back unasked it can travel with this round
   * instead of behind it — and the round is also what orders it, since the
   * home may not hand the block on until every stale copy is retired.  A won
   * claim replaces the count-dropping edge. */
  uint64_t hand_back = arts_db_grant_release_claim(cache, will_publish);
  if (will_publish) {
    TIME_INVALIDATE_ROUND_START();
    arts_db_publish_sync(cache, new_version);
    TIME_INVALIDATE_ROUND_STOP();
  }
  if (buf != NULL) {
    arts_db_buf_release(&buf_h);
  }

  /* The count-dropping edge, and what this rank owes at it, belong to the
   * release policy — a revoked grant ships onward to the target the round
   * named, a voluntarily returned one goes back to the home.  Sequenced after
   * the round above either way, so no stale copy outlives the hand-over.  A
   * claim taken above already dropped the count, and only has to settle which
   * vehicle carried it. */
  if (hand_back != 0u) {
    arts_db_grant_release_settle(cache, hand_back);
  } else {
    arts_db_grant_release_commit(cache);
  }
}

/* The home's publication axis, advanced by each round close.  Deliberately
 * NOT the installed buffer's version: a home-resident owner writes through
 * that buffer in place, so its lane stands still while round after round
 * moves the data on. */
uint64_t arts_db_grant_serve_version(struct arts_db_s *db) {
  return atomic_load_explicit(&db->hver, memory_order_acquire);
}

/* arts_db_release_ro: the shared no-op body in coherence.c applies — an INV
 * read release touches no protocol state (a valid copy persists; the EDT's
 * buffer ref is dropped by the dep-release path). */

/* Deferred pre-publication read serve: re-issued by the publication waiters'
 * drain once the first install lands.  Same 3-step as the direct serve
 * (roster bit before the send). */
static void home_serve_deferred_read(struct arts_db_cache_s *cache,
                                     struct arts_db_read_waiter_s *w) {
  struct arts_db_s *db = (struct arts_db_s *)cache; /* cache is FIRST member */
  /* Same-word pairing with the round's roster snapshot, as at the direct
   * serve below. */
  (void)arts_rank_bitset_set(&db->roster, w->requester);
  arts_send_db_inv_deliver(w->requester, db, &w->rdzv);
}

void arts_handler_db_inv_request(void *item_v, void *args_v) {
  struct arts_db_s *db = (struct arts_db_s *)item_v;
  struct arts_ooo_args_db_inv_request_s *a =
      (struct arts_ooo_args_db_inv_request_s *)args_v;
  struct arts_db_cache_s *cache = &db->cache;
  {
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
  (void)arts_rank_bitset_set(&db->cached_ranks, a->requester);
  if (a->rdzv.txid == 0 &&
      atomic_load_explicit(&cache->db_size, memory_order_relaxed) != 0 &&
      a->requester != arts_global_rank_id) {
    /* First touch: the request carried no landing (db_size unknown at the
     * requester).  It was NOT queued/served — answer the size and let the
     * requester re-issue with a landing. */
    INCREMENT_NUM_INV_SIZE_CTS_BY(1);
    arts_send_db_inv_cts(
        a->requester, db, cache->db_guid,
        atomic_load_explicit(&cache->db_size, memory_order_relaxed), a->cover,
        a->rdzv.cookie);
    return;
  }
  if (a->mode == DB_MODE_RO) {
    /* A remote first use of a block whose create took no hold: the home is
     * the canonical holder on this arm and the serve must carry the block's
     * first image, so it is materialized here — on a progress thread, hence
     * on whatever node that thread sits on rather than the requester's. */
    (void)arts_db_buf_ensure(
        cache, atomic_load_explicit(&cache->db_size, memory_order_relaxed));
    if (atomic_load_explicit(&cache->db_size, memory_order_relaxed) != 0) {
      arts_shared_ptr_t buf_h = arts_db_buf_acquire(cache);
      struct arts_db_buffer_s *pb =
          (struct arts_db_buffer_s *)arts_shared_get(buf_h);
      bool published =
          (pb != NULL &&
           atomic_load_explicit(&pb->version, memory_order_acquire) > 0);
      arts_db_buf_release(&buf_h);
      if (!published) {
        /* Pre-publication hold: no release has published this DB yet, and a
         * read that is event-ordered after the creator's writes must observe
         * the release-published bytes — serving now would hand out
         * never-published state.  Push-then-recheck: a concurrent first
         * install either sees our node in its drain or we drain ourselves,
         * by the same-word pairing on publication_waiters stated at the
         * acquire's parker above. */
        struct arts_db_read_waiter_s *w =
            (struct arts_db_read_waiter_s *)arts_malloc(sizeof(*w));
        w->edt = NULL;
        w->slot = 0;
        w->serve = home_serve_deferred_read;
        w->requester = a->requester;
        w->cover = a->cover;
        w->rdzv = a->rdzv;
        arts_lf_stack_push(&cache->publication_waiters, &w->link);
        buf_h = arts_db_buf_acquire(cache);
        pb = (struct arts_db_buffer_s *)arts_shared_get(buf_h);
        published = (pb != NULL && atomic_load_explicit(
                                       &pb->version, memory_order_acquire) > 0);
        arts_db_buf_release(&buf_h);
        if (published) {
          arts_db_drain_publication_waiters(cache);
        }
        return;
      }
    }
    /* Read serve: roster bit BEFORE the send — a serve whose copy departs
     * must already be a target of the next round.  The fetch_or and the
     * round's snapshot exchange (inv_home_round_try_open) are acq_rel RMWs
     * of one word, so the later reads the other: either that round names
     * this reader, or this serve's stamp load sees the round's hver store,
     * which precedes the exchange. */
    (void)arts_rank_bitset_set(&db->roster, a->requester);
    arts_send_db_inv_deliver(a->requester, db, &a->rdzv);
    return;
  }
  /* There is no write branch: RW acquires go through the shared grant's
   * GRANT_REQUEST, which the home answers by naming the next holder and
   * revoking the current one.  This handler serves readers only. */
}

