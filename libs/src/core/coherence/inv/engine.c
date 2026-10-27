/* SPDX-License-Identifier: Apache-2.0
 *
 * The INV engine — everything both write policies run identically.
 *
 * The reader plane (fetch install and the transitions that take its chain)
 * and the invalidation round (the hold and its queue, roster snapshot,
 * multicast, ack accounting, finish) live here, along with the cache
 * lifecycle and the wire senders.  What is left to the write-policy TUs is
 * exactly three decisions, because those are the only three that "where do
 * the canonical bytes live" actually changes:
 *
 *   - an RO acquire: served locally (WT, at the home rank) or redirected to
 *     the current grant holder (WB);
 *   - a release: publishes the payload to the home (WT) or publishes control
 *     only (WB) — the round itself is identical either way;
 *   - a read request at the home: answered from the home's buffer (WT) or
 *     forwarded to the holder (WB).
 *
 * Write ownership is not here at all: it is the migrating sentinel grant
 * (coherence/grant.c plus its write-policy half), shared with VAL.
 *
 * Compiled only for ARTS_COHERENCE_PROTOCOL=INV.
 */
#include "arts/coherence/inv/types.h"

#include "arts/utils/stdatomic_compat.h"
#include <assert.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arts/coherence/buffer.h"
#include "arts/coherence/census.h"
#include "arts/coherence/coherence.h"
#include "arts/coherence/directory.h"
#include "arts/coherence/handlers.h"
#include "arts/db.h"
#include "arts/edt.h"
#include "arts/gas/guid.h"
#include "arts/gas/route_table.h"
#include "arts/memory/regpool.h"
#include "arts/ooo.h"
#include "arts/runtime_state.h"
#include "arts/system/identity.h"
#include "arts/system/print.h" /* ARTS_ERROR */
#include "arts/system/schedfuzz.h"
#include "arts/transport/net.h"
#include "arts/transport/protocol.h"
#include "arts/utils/malloc.h"
#include "arts/counter/Preamble.h"

/* ===== pure arbiters (private copy — see arbiters.c) ==================== */
#include "arbiters.c"

/* ===== the request axes (shared grant plane hooks) ======================= */
/* Commit `op` on the reader word; returns the pre-image, *post the image. */
static uint64_t inv_req_step_tok(struct arts_db_cache_s *cache, int op,
                                 uint32_t tok, uint64_t *post, uint32_t *act) {
  uint64_t cur = atomic_load_explicit(&cache->cache_state, memory_order_acquire);
  uint64_t next;
  for (;;) {
    next = arts_inv_cache_compute_next(cur, op, tok, act);
    if (next == cur ||
        atomic_compare_exchange_weak_explicit(&cache->cache_state, &cur, next,
                                              memory_order_acq_rel,
                                              memory_order_acquire)) {
      break;
    }
  }
  *post = next;
  return cur;
}

static uint64_t inv_req_step(struct arts_db_cache_s *cache, int op,
                             uint64_t *post, uint32_t *act) {
  return inv_req_step_tok(cache, op, 0u, post, act);
}

/* A read request's cover word: its parity, and PRECLAIM while the cache is a
 * claimable first touch (such a request leaves without a landing). */
uint32_t arts_inv_read_cover(uint64_t s) {
  return (INV_CACHE_PAR(s) ? ARTS_COVER_PAR : 0u) |
         (INV_CACHE_STUB(s) ? ARTS_COVER_PRECLAIM : 0u);
}

static uint32_t inv_rw_cover(uint64_t s) {
  uint32_t c = 0u;
  if (INV_CACHE_INFLIGHT(s) != 0u) {
    c |= ARTS_COVER_RO_OPEN;
    if (INV_CACHE_PAR(s)) {
      c |= ARTS_COVER_RO_PAR;
    }
  }
  if (INV_CACHE_STUB(s)) {
    c |= ARTS_COVER_PRECLAIM;
  }
  return c;
}

bool arts_db_rw_req_claim(struct arts_db_cache_s *cache, uint32_t *tok) {
  uint64_t post;
  uint32_t act;
  (void)inv_req_step(cache, INV_CACHE_OP_RW_CLAIM, &post, &act);
  *tok = INV_CACHE_STUB(post);
  return act == INV_CACHE_ACT_WON;
}

bool arts_db_rw_req_commit(struct arts_db_cache_s *cache, uint32_t tok,
                           uint32_t *cover) {
  uint64_t post;
  uint32_t act;
  uint64_t pre =
      inv_req_step_tok(cache, INV_CACHE_OP_RW_COMMIT, tok, &post, &act);
  *cover = inv_rw_cover(pre);
  return act == INV_CACHE_ACT_WON;
}

void arts_db_rw_req_giveback(struct arts_db_cache_s *cache, uint32_t tok) {
  uint64_t post;
  uint32_t act;
  uint64_t pre = inv_req_step_tok(cache, INV_CACHE_OP_RW_GIVE, tok, &post, &act);
  if (act == INV_CACHE_ACT_LATE) {
    ARTS_ERROR("inv: guid %lu gave back a write request with readers parked "
               "on a valid copy (word=%llx)",
               (unsigned long)cache->db_guid, (unsigned long long)pre);
  }
  if (act == INV_CACHE_ACT_SEND_RO) {
    arts_send_db_inv_request(cache, DB_MODE_RO, arts_inv_read_cover(post));
  }
}

/* The grant lands (WT: its install; WB: its confirmation): every reader
 * parked on this rank is served under it, a fetch still out included -- that
 * fetch stays open until its own reply. */
void arts_db_rw_req_clear(struct arts_db_cache_s *cache) {
  uint64_t post;
  uint32_t act;
  uint64_t pre = inv_req_step(cache, INV_CACHE_OP_RW_CLEAR, &post, &act);
  if (act == INV_CACHE_ACT_LATE) {
    ARTS_ERROR("inv: guid %lu got a write grant it has no request out for "
               "(word=%llx)",
               (unsigned long)cache->db_guid, (unsigned long long)pre);
  }
  arts_db_reader_serve_chain(&cache->reader_pool, INV_CACHE_HEAD_RO(pre));
}

uint32_t arts_db_rw_req_declaim(struct arts_db_cache_s *cache) {
  uint64_t post;
  uint32_t act;
  uint64_t pre = inv_req_step(cache, INV_CACHE_OP_DECLAIM, &post, &act);
  if (act == INV_CACHE_ACT_LATE) {
    ARTS_ERROR("inv: guid %lu got a write size round it has no request out "
               "for (word=%llx)",
               (unsigned long)cache->db_guid, (unsigned long long)pre);
  }
  return inv_rw_cover(post);
}

/* ===== cache lifecycle ================================================== */

void arts_db_cache_init(struct arts_db_cache_s *c, arts_guid_t db_guid,
                        uint64_t db_size, arts_db_init_kind_t kind,
                        unsigned int creator_rank) {
  /* Reader word: IDLE, no fetch open, chain empty.  A creator's copy is a
   * valid read copy — it holds the bytes it is about to write. */
  uint64_t seed = 0ULL;
  bool creator = (kind == ARTS_DB_INIT_CREATOR_HOME ||
                  kind == ARTS_DB_INIT_CREATOR_REMOTE);
  if (creator) {
    seed = INV_CACHE_MAKE(INV_RO_VALID, 0u, 0u);
  } else if (kind == ARTS_DB_INIT_STUB && ARTS_DB_STUB_CLAIMABLE(db_guid)) {
    seed = INV_CACHE_WITH_REQ(0ULL, INV_RW_IDLE, 0u, 1u);
  }
  atomic_store_explicit(&c->cache_state, seed, memory_order_relaxed);
  /* Grant plane.  A creator boots holding the write right with the
   * create-default RW acquire's own hold under it; everyone else starts at
   * the all-zero word, which IS "not the owner". */
  atomic_init(&c->writer_count, creator ? ARTS_GRANT_SEED_HOLDING : 0u);
#ifdef ARTS_RELEASE_PURGE
  atomic_init(&c->pending_grant_return, 0u);
  atomic_init(&c->grant_generation, 0u);
#endif
  arts_pending_rw_queue_init(&c->pending_rw);

#ifndef ARTS_RELEASE_PURGE
  c->incoming_new_owner = ARTS_NO_PENDING_OWNER;
  c->incoming_new_owner_rdzv = (struct arts_rdzv_landing_s){0, 0, 0, 0};
  c->incoming_new_owner_have = ARTS_GRANT_VERSION_NONE;
#endif
  /* INV's sharer plane carries no version ledger by construction; NULL selects
   * the shared transfer helper's empty-map branch. */
  c->cached_version = NULL;
  arts_db_cache_common_init(c, db_guid, db_size, kind, creator_rank);
}

void arts_db_cache_destructor(struct arts_db_cache_s *cache) {
  if (cache == NULL) {
    return;
  }
  arts_db_cache_common_destroy_pre(cache); /* buffer-NULL FIRST */
  arts_pending_rw_queue_destroy(&cache->pending_rw);
  arts_db_cache_common_destroy_post(cache);
}

/* The reader plane's VALID is a claim to durable bytes, and it dies exactly
 * one way — an INVALIDATE arrives.  A create that takes no hold is given no
 * bytes, so seeding VALID there would leave a claim nothing can retire until
 * some later round happens to name this rank: its own reads would be answered
 * from a copy it never held, and the ensure that materializes a first image
 * would do so on a rank with no right to one.  Retract the claim instead, and
 * only the claim: an in-flight fetch and a parked reader chain live in the
 * same word and must survive (this runs on a create that claims a cache
 * readers already reached). */
/* The hold is possession plus this create's own writer, from a word that
 * holds nothing.  It is a write permission landing on this rank: every EDT
 * parked on the first-touch cache is served under it -- readers whose fetch
 * is still out included (the fetch stays open until its reply) -- and the
 * requests that cache sent are the home's to absorb. */
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
  uint64_t post;
  uint32_t act;
  uint64_t pre = inv_req_step(cache, INV_CACHE_OP_CLAIM, &post, &act);
  if (INV_CACHE_INFLIGHT(pre) != 0u) {
    *owed |= ARTS_CREATE_OWED_RO;
    if (INV_CACHE_PAR(pre)) {
      *owed |= ARTS_CREATE_OWED_RO_PAR;
    }
  }
  if (INV_CACHE_RW(pre) == INV_RW_SENT) {
    *owed |= ARTS_CREATE_OWED_RW;
  }
  arts_db_reader_serve_chain(&cache->reader_pool, INV_CACHE_HEAD_RO(pre));
  arts_db_grant_rearm(cache);
  return true;
}

void arts_db_create_retract_creator_copy(struct arts_db_s *db) {
  struct arts_db_cache_s *cache = &db->cache;
  uint64_t cur, next;
  do {
    cur = atomic_load_explicit(&cache->cache_state, memory_order_acquire);
    if (INV_CACHE_RO(cur) != INV_RO_VALID) {
      return;
    }
    next = INV_CACHE_WITH_REQ(
        INV_CACHE_MAKE(INV_RO_IDLE, INV_CACHE_INFLIGHT(cur),
                       INV_CACHE_HEAD_RO(cur)),
        INV_CACHE_RW(cur), INV_CACHE_PAR(cur), INV_CACHE_STUB(cur));
  } while (!atomic_compare_exchange_weak_explicit(&cache->cache_state, &cur,
                                                  next, memory_order_acq_rel,
                                                  memory_order_acquire));
}


/* One transition, from the one state a create may claim a copy in: the reader
 * word exactly idle.  A fetch in flight is a reader that has already asked,
 * and its delivery brings the copy with it — stamping VALID over it would
 * leave that answer with nowhere to land; a parked chain is the same reader,
 * waiting.  So a non-idle word is left alone and the create simply
 * does not claim. */
void arts_db_create_claim_creator_copy(struct arts_db_s *db) {
  struct arts_db_cache_s *cache = &db->cache;
  uint64_t cur = atomic_load_explicit(&cache->cache_state, memory_order_acquire);
  for (;;) {
    if (INV_CACHE_RO(cur) != INV_RO_IDLE || INV_CACHE_INFLIGHT(cur) != 0u ||
        INV_CACHE_HEAD_RO(cur) != 0u) {
      return;
    }
    uint64_t want = INV_CACHE_WITH_REQ(INV_CACHE_MAKE(INV_RO_VALID, 0u, 0u),
                                       INV_CACHE_RW(cur), INV_CACHE_PAR(cur),
                                       INV_CACHE_STUB(cur));
    if (atomic_compare_exchange_weak_explicit(&cache->cache_state, &cur, want,
                                              memory_order_acq_rel,
                                              memory_order_acquire)) {
      return;
    }
  }
}

void arts_db_create_install_home_buffer(struct arts_db_cache_s *cache,
                                        uint64_t db_size) {
#ifdef ARTS_WRITE_POLICY_WT
  /* The write-through home owns ONE stable buffer for the DB's whole
   * lifetime, installed here at version 0 — "allocated, never published".
   * Publishes land in it one-sided and in place; the round's version bump
   * from zero is the publication edge that drains pre-publication holds.
   * A read acquire event-ordered after the creator's writes must observe
   * the release-published bytes, so version 0 must serve exactly like the
   * old buffer-absent state — every serve/park predicate keys on it. */
  if (db_size > 0) {
    arts_db_buf_install(cache, /*new_version=*/0, /*data_payload=*/NULL,
                        db_size);
  }
#else
  /* The write-back home holds routing and permission, never payload: a read
   * is redirected to the grant's holder (served here when the home holds the
   * grant), which serves it from its own copy; a create that acquires nothing
   * leaves its storage to its first use. */
  (void)cache;
  (void)db_size;
#endif
}

void arts_handler_db_destroy(void *item_v, void *args_v) {
  (void)args_v;
  struct arts_db_s *db = (struct arts_db_s *)item_v;
  if (db == NULL) {
    return;
  }
  arts_db_cover_destroy(db);
}

/* The teardown, run by whichever of the destroy and the last request the home
 * was told is coming reaches the gate's edge.  The roster is complete: every
 * rank holding a cache of the block (its creator, every read requester and
 * every write requester) has a bit, so each is told.  No pending_rw drain:
 * the grant request queue is single-consumer (the transfer baton's holder),
 * and a legitimately destroyed DB has no outstanding acquires.  No release is
 * waited for: every release that sends the home anything (a round, a
 * hand-back) blocks until the home ACKs it, so under the destroy contract none
 * arrives after the destroy; a transfer's CONFIRM that does finds no home and
 * is dropped. */
void arts_db_home_destroy_now(struct arts_db_s *db) {
  struct arts_db_cache_s *cache = &db->cache;
  arts_guid_t guid = cache->db_guid;
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

/* ===== the invalidation round ===========================================
 * INV's whole identity, and the only thing this arm adds to the shared grant.
 *
 * A release does not return until every copy of the data block it just wrote
 * is dead.  The home runs one round at a time, held through round_queue (its
 * declaration states the protocol): a release that finds the word idle claims
 * it and runs its own entry, one that finds it held pushes its entry and
 * leaves it to the holder.  A round stamps the version of the payload that
 * landed in place (WT), snapshots the roster and, only when the snapshot
 * names a copy to retire, arms the ack count and multicasts INVALIDATE.  It is
 * finished by the thread that ran it when it retired nothing (the release
 * that claimed the round, or a finisher that took the batch), or by the ack
 * whose decrement reaches zero.  The finisher wakes the batch's releasers,
 * then ends the hold or takes what arrived meanwhile and runs it, in a loop.
 *
 * A batch is order-insensitive: at most one version stamp (the entry that
 * carries data) and a set of commuting wakes.  The roster snapshot is a
 * per-word exchange, so a rank registered during the round lands in the NEXT
 * one — which is correct, because it will be served the bytes this round
 * published — and the exchange writes the batch's releasers back in, because
 * a releaser's copy is a sharer's copy from then on and only a later round
 * may retire it.
 */

/* Under the hold.  Returns the batch when no copy needs retiring (the caller
 * finishes it), NULL when the round is armed (its last ack finishes it). */
static struct arts_db_inv_pub_s *
inv_round_run(struct arts_db_s *db, struct arts_db_inv_pub_s *batch) {
  struct arts_db_cache_s *cache = &db->cache;
  INCREMENT_NUM_INVALIDATE_ROUND_BY(1);
#ifdef ARTS_WRITE_POLICY_WT
  /* Single publication point: a landed entry's bytes are already IN PLACE in
   * the stable buffer (the commit-leg pairing gates the queue), so publishing
   * is the version stamp alone.  At most one entry of a batch carries data —
   * payload publishes are globally serialized (one flight per rank, the
   * releaser blocks on this round's ACK, and the write right migrates only
   * after) — which the stamp's monotonicity check enforces loudly. */
  uint64_t maxv = 0;
  for (struct arts_db_inv_pub_s *e = batch; e != NULL; e = e->next) {
    if (e->vnew > maxv) {
      maxv = e->vnew;
    }
    if (e->landed) {
      arts_db_buf_bump_inplace(cache, e->vnew);
    }
  }
  /* Advance the publication axis over the WHOLE batch, data-less entries
   * included: a same-rank release moves no bytes (the home buffer IS the
   * owner's working buffer) but its round still retires every remote copy,
   * so it is a publication and readers served after it must be given a
   * stamp that says so. */
  if (maxv > atomic_load_explicit(&db->hver, memory_order_relaxed)) {
    atomic_store_explicit(&db->hver, maxv, memory_order_release);
  }
  /* First publication wakes the pre-publication read holds (readers
   * event-ordered after the creator's writes must see released bytes,
   * never the never-published version-0 state).  Published = an installed
   * buffer whose version has been bumped past the create-time zero.  The
   * drain pairs with each parker's push on the stack itself (see
   * arts_handler_db_acquire): stamp, then drain. */
  {
    arts_shared_ptr_t pub_h = arts_db_buf_acquire(cache);
    struct arts_db_buffer_s *pb =
        (struct arts_db_buffer_s *)arts_shared_get(pub_h);
    bool pub = (pb != NULL &&
                atomic_load_explicit(&pb->version, memory_order_acquire) > 0);
    arts_db_buf_release(&pub_h);
    if (pub) {
      arts_db_drain_publication_waiters(cache);
    }
  }
  /* The home rank is exempt outright ONLY where the home's own buffer is the
   * canonical copy: under WT it is, and this round just stamped it; under WB
   * the home holds no canonical bytes, so a reader copy that happens to live
   * on the home rank is as stale as any other and is retired like one. */
  const unsigned int exempt = arts_global_rank_id;
#else
  const unsigned int exempt = (unsigned int)-1; /* no rank is exempt */
#endif
  /* Self-exclusion names the ranks that actually WROTE this round — the
   * batch's releasers — not whatever the directory currently points at.
   * The justification for skipping a rank is "its copy is the newest by
   * definition", and that is true of a releaser.  Where the home
   * revokes, `rw_holder` may still name the previous generation, because
   * the home flips it only at the CONFIRM, well after the new owner has
   * installed and (under WT, which has no confirm gate) already run and
   * released, so excluding by `rw_holder` would skip exactly the
   * ex-holder that this round exists to retire; where grants come back
   * unasked, the home names no holder at all. */
  unsigned int writers[8];
  unsigned int nwriters = 0;
  for (struct arts_db_inv_pub_s *e = batch; e != NULL; e = e->next) {
    bool seen = false;
    for (unsigned int k = 0; k < nwriters; k++) {
      if (writers[k] == e->releaser_rank) {
        seen = true;
        break;
      }
    }
    if (seen) {
      continue;
    }
    /* A batch holds releases of the one rank that holds the write
     * right, which leaves it only at a commit after that rank's round
     * finished, so it names one releaser; a releaser dropped here would be
     * a target of its own round and lose its sharer bit. */
    if (nwriters == (unsigned int)(sizeof(writers) / sizeof(writers[0]))) {
      ARTS_ERROR("inv: guid %lu round batch names more than %u releasers",
                 (unsigned long)cache->db_guid, nwriters);
    }
    writers[nwriters++] = e->releaser_rank;
  }
  /* Roster snapshot: per word, the targets are the copies the exchange took
   * out, and the batch's releasers are what it writes in (inv_round_keep_word;
   * a releaser's copy is a sharer's copy from its release on, durable until a
   * later round retires it).  That is what lets the next owner's first
   * release find the ex-holder however that owner's CONFIRM is timed where
   * the home revokes; a snapshot that dropped the writer left its copy live
   * and uninvalidated whenever that round beat the CONFIRM. */
  struct arts_rank_bitset_s *bs = &db->roster;
  const unsigned int nwords = bs->nwords; /* <= INV_ROUND_MAX_WORDS (init) */
  uint64_t tmask[INV_ROUND_MAX_WORDS];
  uint32_t ntargets = 0;
  for (unsigned int wi = 0; wi < nwords; wi++) {
    uint64_t keep = inv_round_keep_word(wi * 64u, writers, nwriters, exempt);
    /* Same-word pairing with a read serve's fetch_or of this word
     * (arts_handler_db_inv_request): the later of the two reads the other,
     * so the serve is a target here or follows the stamp above. */
    uint64_t snap =
        atomic_exchange_explicit(&bs->words[wi], keep, memory_order_acq_rel);
    tmask[wi] = inv_round_targets_word(snap, keep, wi * 64u, exempt);
    ntargets += (uint32_t)__builtin_popcountll(tmask[wi]);
  }
  if (ntargets == 0u) {
    return batch;
  }
  const arts_guid_t guid = cache->db_guid;
  db->round_entries = batch;
  /* ARM, before the first INVALIDATE leaves.  From here on only locals: the
   * last ack can finish this round, and start the next, before this loop
   * returns (under WB the home's own ack runs inline inside it). */
  atomic_store_explicit(&db->round_acks, ntargets, memory_order_release);
  for (unsigned int wi = 0; wi < nwords; wi++) {
    for (uint64_t m = tmask[wi]; m != 0u; m &= m - 1u) {
      arts_send_db_inv_invalidate(wi * 64u + (unsigned int)__builtin_ctzll(m),
                                  guid);
    }
  }
  return NULL;
}

/* The batch's round is over.  Wake its releasers, accept a hand-back, then
 * end the hold — or keep it for what arrived meanwhile and run that. */
static void inv_round_finish(struct arts_db_s *db,
                             struct arts_db_inv_pub_s *batch) {
  for (;;) {
    /* At most one entry in a batch can hand the write right back: only its
     * holder can, and there is exactly one. */
    struct arts_db_inv_pub_s *returning = NULL;
    while (batch != NULL) {
      struct arts_db_inv_pub_s *e = batch;
      batch = e->next;
      if (e->returns_grant != 0u) {
        assert(returning == NULL &&
               "one batch cannot carry two hand-backs of one write right");
        returning = e;
        continue;
      }
#ifdef ARTS_WRITE_POLICY_WT
      /* Write-through commits land in the stable buffer, so the ACK refills
       * the releaser's in-place credit. */
      arts_send_db_publish_ack(e->releaser_rank, db->cache.db_guid, 0u, e->vnew,
                               &db->cache);
#else
      /* Write-back releases carry no payload — nothing to credit. */
      arts_send_db_publish_ack(e->releaser_rank, db->cache.db_guid, 0u, e->vnew,
                               /*home_cache=*/NULL);
#endif
      arts_free(e);
    }
    if (returning != NULL) {
      /* Accepted only now, after the acks of the round it rode: that round
       * has retired every stale copy (and stamped the publication axis), so a
       * block handed straight on from here carries no copy this release has
       * already invalidated.  Accepted under the hold, which is safe only
       * because the acceptance never waits on anything the round gates; its
       * acknowledgement rides the hand-back and leaves once the home has
       * applied it. */
      struct arts_db_grant_return_reply_s reply = {
          .cv = 0u,
          .version = returning->vnew,
          .ack = true,
#ifdef ARTS_WRITE_POLICY_WT
          .credit = true,
#else
          .credit = false,
#endif
      };
      unsigned int returner = returning->releaser_rank;
      arts_free(returning);
      arts_db_grant_return_arrived(db, returner, &reply);
    }
    arts_sched_fuzz_point(); /* widen the window for a release to reach the
                                held round */
    /* Same-word pairing with inv_round_submit's push: of the push and this
     * end, the later reads the other -- the end fails and the take below
     * gets the entry, or the push reads 0 and claims the next round. */
    uintptr_t want = INV_ROUND_OPEN;
    if (atomic_compare_exchange_strong_explicit(
            &db->round_queue, &want, (uintptr_t)0, memory_order_release,
            memory_order_relaxed)) {
      return;
    }
    /* The end failed, so entries arrived: only the holder ends or empties
     * the word, and a push only adds.  Take them all and run them. */
    batch = (struct arts_db_inv_pub_s *)(atomic_exchange_explicit(
                                             &db->round_queue, INV_ROUND_OPEN,
                                             memory_order_acquire) &
                                         ~INV_ROUND_OPEN);
    assert(batch != NULL && "a failed end leaves entries to take");
    batch = inv_round_run(db, batch);
    if (batch == NULL) {
      return; /* armed: its last ack finishes it */
    }
  }
}

/* Every release's entry point (the publish handler, the commit leg's pairing
 * continuation): claim an idle round and run it, or queue behind the held
 * one for its holder to take. */
static void inv_round_submit(struct arts_db_s *db,
                             struct arts_db_inv_pub_s *e) {
  assert(((uintptr_t)e & INV_ROUND_OPEN) == 0u &&
         "an entry's address leaves bit 0 for the hold");
  uintptr_t h = atomic_load_explicit(&db->round_queue, memory_order_relaxed);
  bool claim;
  for (;;) {
    uintptr_t next = inv_round_submit_next(h, (uintptr_t)e, &claim);
    e->next = claim ? NULL : (struct arts_db_inv_pub_s *)(h & ~INV_ROUND_OPEN);
    arts_sched_fuzz_point(); /* widen the load<->CAS window */
    /* Same-word pairing with inv_round_finish's end (see there).  A claim
     * acquires the previous holder's end; a push releases its entry. */
    if (claim
            ? atomic_compare_exchange_weak_explicit(&db->round_queue, &h, next,
                                                    memory_order_acquire,
                                                    memory_order_relaxed)
            : atomic_compare_exchange_weak_explicit(&db->round_queue, &h, next,
                                                    memory_order_release,
                                                    memory_order_relaxed)) {
      break;
    }
  }
  if (!claim) {
    return; /* the holder takes it */
  }
  struct arts_db_inv_pub_s *b = inv_round_run(db, e);
  if (b != NULL) {
    inv_round_finish(db, b); /* nothing to retire: finish it here */
  }
}

/* ===== wire handler bodies (OoO Cat-B) ================================== */

#ifdef ARTS_WRITE_POLICY_WT
/* Commit-leg pairing continuation: the dirty bytes are in the home landing;
 * submit the entry to the round. */
struct inv_pub_landed_ctx_s {
  arts_shared_ptr_t db_h;
  arts_guid_t db_guid;
  struct arts_db_inv_pub_s *entry;
};

static void inv_pub_landed_cb(void *arg) {
  struct inv_pub_landed_ctx_s *ctx = (struct inv_pub_landed_ctx_s *)arg;
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(ctx->db_h);
  if (db == NULL) {
    /* Destroyed mid-pairing: nothing to reclaim (the bytes landed in the
     * stable buffer, which died with the cache) — never strand the blocked
     * releaser. */
    struct arts_db_inv_pub_s *e = ctx->entry;
    arts_send_db_publish_ack(e->releaser_rank, ctx->db_guid, 0u, e->vnew,
                             /*home_cache=*/NULL);
    arts_free(e);
    arts_shared_release(&ctx->db_h);
    arts_free(ctx);
    return;
  }
  inv_round_submit(db, ctx->entry);
  arts_shared_release(&ctx->db_h);
  arts_free(ctx);
}
#endif

void arts_handler_db_publish(void *item_v, void *args_v) {
  struct arts_db_s *db = (struct arts_db_s *)item_v;
  struct arts_ooo_args_db_publish_s *a =
      (struct arts_ooo_args_db_publish_s *)args_v;
  assert((a->returns_grant == 0u || a->rdzv_txid != 0u) &&
         "a hand-back rides a payload commit leg only");
#ifdef ARTS_WRITE_POLICY_WT
  struct arts_db_cache_s *cache = &db->cache;
  if (a->data_size != 0 && a->rdzv_txid == 0) {
    /* Announce leg: advertise the STABLE buffer as the landing (the PUT
     * lands in place; bytes ahead of their round can only mix NEWER data
     * into unordered readers' copies, which the memory model permits — a
     * durable reader copy stays valid until its INVALIDATE regardless) and
     * mint the pairing txid.  Nothing is allocated; the round's install
     * point is a version stamp. */
    if (a->data_size >
        atomic_load_explicit(&cache->db_size, memory_order_relaxed)) {
      ARTS_ERROR("coherence: publish announce of %llu bytes exceeds the "
                 "DB's %llu-byte stable buffer",
                 (unsigned long long)a->data_size,
                 (unsigned long long)atomic_load_explicit(
                     &cache->db_size, memory_order_relaxed));
    }
    struct arts_rdzv_landing_s landing = {0, 0, 0, 0};
    arts_shared_ptr_t mh = arts_db_buf_acquire(cache);
    struct arts_db_buffer_s *master =
        (struct arts_db_buffer_s *)arts_shared_get(mh);
    if (master == NULL ||
        !arts_net_rdzv_local(
            master->data,
            atomic_load_explicit(&cache->db_size, memory_order_relaxed),
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
#else
  if (a->data_size != 0 || a->rdzv_txid != 0) {
    ARTS_ERROR("inv: DB[Guid:%lu] got a payload publish from rank %u — a "
               "write-back release publishes control only",
               (unsigned long)a->db_guid, a->releaser);
  }
#endif
  struct arts_db_inv_pub_s *e =
      (struct arts_db_inv_pub_s *)arts_malloc(sizeof(*e));
  e->vnew = a->version;
  e->releaser_rank = a->releaser;
  e->returns_grant = a->returns_grant;
#ifdef ARTS_WRITE_POLICY_WT
  e->landed = false;
  if (a->rdzv_txid != 0) {
    /* Commit leg: the entry may only queue once the one-sided payload has
     * fully landed in place — pair {commit, PUT completion} (either arrival
     * order) and submit from the pairing continuation.  Submitting on the
     * control packet alone would let the round stamp a torn landing as
     * published. */
    e->landed = true;
    struct inv_pub_landed_ctx_s *ctx =
        (struct inv_pub_landed_ctx_s *)arts_malloc(sizeof(*ctx));
    ctx->db_h = arts_route_table_lookup_db(a->db_guid);
    ctx->db_guid = a->db_guid;
    ctx->entry = e;
    arts_net_rdzv_expect(a->rdzv_txid, a->data_size, inv_pub_landed_cb, ctx);
    return;
  }
#endif
  inv_round_submit(db, e);
}

/* ===== Cat-C handler bodies ============================================= */

void arts_handler_db_inv_cts(struct arts_db_s *db,
                             struct arts_msg_inv_cts_packet_s *p) {
  struct arts_db_cache_s *cache = &db->cache;
  uint32_t cover = (uint32_t)(p->db_size >> ARTS_INV_CTS_SIZE_BITS);
  arts_db_cache_learn_size(cache, p->db_size & ARTS_INV_CTS_SIZE_MASK);
  if (p->rdzv_cookie != 0) {
    arts_db_buf_landing_return(
        (struct arts_db_buffer_s *)(uintptr_t)p->rdzv_cookie);
  }
  uint64_t post;
  uint32_t act;
  uint64_t pre = inv_req_step_tok(cache, INV_CACHE_OP_CTS,
                                  cover & ARTS_COVER_PAR, &post, &act);
  if (act == INV_CACHE_ACT_LATE) {
    ARTS_ERROR("inv: guid %lu got a size round or dismiss (parity %u) it has "
               "no fetch out for (word=%llx)",
               (unsigned long)cache->db_guid, cover & ARTS_COVER_PAR,
               (unsigned long long)pre);
  }
  if (act == INV_CACHE_ACT_RESEND) {
    arts_send_db_inv_request(cache, DB_MODE_RO, arts_inv_read_cover(post));
  } else if (act == INV_CACHE_ACT_CLOSE_ACK) {
    arts_send_db_inv_invalidate_ack(arts_guid_get_rank(cache->db_guid),
                                    cache->db_guid);
  }
}

/* Publish + whole-chain grab: install the landed payload (home-issued
 * install-lane version; a stale install retreats and recycles), then ONE
 * word CAS takes {REQ->VALID, chain head -> 0} together.  The kill-marked
 * variant serves its cohort once, then executes the reserved purge and the
 * owed ack — the ack LAST, so the round that killed this fetch cannot finish
 * (and nothing newer can complete) until the doomed serve is done. */
static void inv_deliver_commit(arts_shared_ptr_t db_h, uint64_t version,
                               struct arts_db_buffer_s *landing,
                               uint64_t data_size) {

  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(db_h);
  struct arts_db_cache_s *cache = &db->cache;
  if (landing != NULL) {
    (void)arts_db_buf_install_landed(cache, version, landing, data_size);
  }
  uint32_t act;
  uint64_t cur, next;
  do {
    cur = atomic_load_explicit(&cache->cache_state, memory_order_acquire);
    next = arts_inv_cache_compute_next(cur, INV_CACHE_OP_DELIVER, 0u, &act);
    if (act == INV_CACHE_ACT_LATE) {
      ARTS_ERROR("inv: guid %lu got a read reply it has no fetch out for "
                 "(word=%llx)",
                 (unsigned long)cache->db_guid, (unsigned long long)cur);
    }
  } while (!atomic_compare_exchange_weak_explicit(&cache->cache_state, &cur,
                                                  next, memory_order_acq_rel,
                                                  memory_order_acquire));
  if (act == INV_CACHE_ACT_PUBLISH || act == INV_CACHE_ACT_PUBLISH_KILL) {
    arts_db_reader_serve_chain(&cache->reader_pool, INV_CACHE_HEAD_RO(cur));
  }
  if (act == INV_CACHE_ACT_PUBLISH_KILL) {
    uint32_t pact;
    do {
      cur = atomic_load_explicit(&cache->cache_state, memory_order_acquire);
      /* The owed ack keeps the round armed, so nothing can retire the
       * transient valid copy before this purge. */
      if (INV_CACHE_RO(cur) != INV_RO_VALID) {
        ARTS_ERROR("inv: reserved purge found ro=%u (drifted)",
                   INV_CACHE_RO(cur));
      }
      next =
          arts_inv_cache_compute_next(cur, INV_CACHE_OP_KILL_PURGE, 0u, &pact);
    } while (!atomic_compare_exchange_weak_explicit(
        &cache->cache_state, &cur, next, memory_order_acq_rel,
        memory_order_acquire));
    arts_send_db_inv_invalidate_ack(arts_guid_get_rank(cache->db_guid),
                                    cache->db_guid); /* owed ack, LAST */
  }
  arts_shared_release(&db_h);
}

struct inv_landed_ctx_s {
  arts_shared_ptr_t db_h;
  struct arts_db_buffer_s *landing;
  uint64_t version;
  uint64_t data_size;
};


static void inv_landed_cb(void *arg) {
  struct inv_landed_ctx_s *ctx = (struct inv_landed_ctx_s *)arg;
  if (arts_shared_get(ctx->db_h) == NULL) {
    /* Destroyed while the pairing was outstanding (destroy during a pending
     * acquire is app UB): end the landing and drop. */
    arts_db_buf_landing_return(ctx->landing);
    arts_shared_release(&ctx->db_h);
    arts_free(ctx);
    return;
  }
  /* A one-sided payload comes from another rank. */
  uint8_t census = arts_db_census_serving(true);
  inv_deliver_commit(ctx->db_h, ctx->version, ctx->landing, ctx->data_size);
  arts_db_census_served(census);
  arts_free(ctx);
}

void arts_handler_db_inv_deliver(void *payload, size_t size) {
  (void)size;
  struct arts_msg_inv_deliver_packet_s *p =
      (struct arts_msg_inv_deliver_packet_s *)payload;
  arts_shared_ptr_t db_h = arts_route_table_lookup_db(p->db_guid);
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(db_h);
  if (db == NULL) {
    arts_db_rdzv_discard_landing(p->rdzv_txid, p->data_size, p->rdzv_cookie);
    arts_shared_release(&db_h);
    return;
  }
  /* Hinted first touch: the size may still be unlearned here. */
  arts_db_cache_learn_size(&db->cache, p->data_size);
  if (p->rdzv_txid != 0) {
    struct inv_landed_ctx_s *ctx =
        (struct inv_landed_ctx_s *)arts_malloc(sizeof(*ctx));
    ctx->db_h = db_h;
    ctx->landing = (struct arts_db_buffer_s *)(uintptr_t)p->rdzv_cookie;
    ctx->version = p->version;
    ctx->data_size = p->data_size;
    arts_net_rdzv_expect(p->rdzv_txid, p->data_size, inv_landed_cb, ctx);
    return;
  }
  /* Data-less reply.  The commit below turns this rank's copy VALID and
   * durable, so every later read is answered from it with no message at all —
   * which makes storage here mandatory, not optional: a sized block whose
   * cache holds no buffer would answer those reads with a NULL pointer
   * forever.  The advertised landing is that storage; adopt it when the cache
   * has none and return it to the pool when the cache already has one. */
  if (p->rdzv_cookie != 0) {
    /* Stamped at version 1 when the server held nothing: a live image may
     * never carry the version that means "holds nothing", or the first
     * release of real bytes would mint the same stamp and its install would
     * retreat as stale against the invented first image. */
    (void)arts_db_buf_adopt_landing(
        &db->cache, p->version ? p->version : 1u,
        (struct arts_db_buffer_s *)(uintptr_t)p->rdzv_cookie,
        atomic_load_explicit(&db->cache.db_size, memory_order_relaxed));
  } else if (atomic_load_explicit(&db->cache.db_size, memory_order_relaxed) !=
             0) {
    arts_shared_ptr_t have_h = arts_db_buf_acquire(&db->cache);
    bool have = (arts_shared_get(have_h) != NULL);
    if (have) {
      arts_db_buf_release(&have_h);
    } else {
      ARTS_ERROR("inv: data-less deliver for a sized block with neither a "
                 "landing nor a buffer to make valid");
    }
  }
  uint8_t census = arts_db_census_serving_from(p->header.rank);
  inv_deliver_commit(db_h, p->version, NULL, 0u);
  arts_db_census_served(census);
}

void arts_handler_db_inv_invalidate(struct arts_db_s *db) {
  struct arts_db_cache_s *cache = &db->cache;
  unsigned int home_rank = arts_guid_get_rank(cache->db_guid);
  uint32_t act;
  uint64_t cur, next;
  do {
    cur = atomic_load_explicit(&cache->cache_state, memory_order_acquire);
    /* An invalidate can never hit the grant holder: the round snapshot
     * self-excludes it and rounds serialize.  Tested against the whole word,
     * so a hold still carrying ARTS_GRANT_UNCONFIRMED (negative, not zero)
     * counts as a holder here too. */
    if (atomic_load_explicit(&cache->writer_count, memory_order_acquire) !=
        0u) {
      ARTS_ERROR("inv: invalidate hit the grant holder");
    }
    next = arts_inv_cache_compute_next(cur, INV_CACHE_OP_INVALIDATE, 0u, &act);
  } while (!atomic_compare_exchange_weak_explicit(&cache->cache_state, &cur,
                                                  next, memory_order_acq_rel,
                                                  memory_order_acquire));
  if (act == INV_CACHE_ACT_PURGE_ACK || act == INV_CACHE_ACT_NOOP_ACK) {
    arts_send_db_inv_invalidate_ack(home_rank, cache->db_guid);
  }
  /* KILL_MARKED: the ack is owed — the doomed DELIVER fires it after its
   * serve-once, keeping every pre-ack serve legally racy. */
}

void arts_handler_db_inv_invalidate_ack(struct arts_db_s *db,
                                        unsigned int sharer_rank) {
  (void)sharer_rank;
  INCREMENT_NUM_INVALIDATE_ACK_BY(1);
  uint32_t prev =
      atomic_fetch_sub_explicit(&db->round_acks, 1u, memory_order_acq_rel);
  if (prev == 0u) {
    /* Every ack answers an INVALIDATE of the armed round, which stays armed
     * until its last ack: an ack that finds none outstanding breaks the
     * protocol. */
    ARTS_ERROR("inv: invalidate ack for DB[Guid:%lu] with no outstanding ack",
               (unsigned long)db->cache.db_guid);
  }
  if (prev != 1u) {
    return;
  }
  struct arts_db_inv_pub_s *batch = db->round_entries;
  db->round_entries = NULL;
  inv_round_finish(db, batch);
}

/* ===== senders ========================================================== */
/* Control-only senders are plain packet fills; the payload-carrying trio
 * (deliver/publish) lands with the data-plane implementation. */

void arts_send_db_inv_cts(unsigned int requester_rank, struct arts_db_s *db,
                          arts_guid_t db_guid, uint64_t db_size,
                          uint32_t cover, uint64_t cookie) {
  struct arts_msg_inv_cts_packet_s p;
  arts_fill_packet_header(&p.header, sizeof(p), MSG_DB_INV_CTS);
  p.header.rank = arts_global_rank_id;
  p.db_guid = db_guid;
  if (db_size > ARTS_INV_CTS_SIZE_MASK) {
    ARTS_ERROR("inv: guid %lu has size %llu, past the size round's 60-bit "
               "size field",
               (unsigned long)db_guid, (unsigned long long)db_size);
  }
  p.db_size = (db_size & ARTS_INV_CTS_SIZE_MASK) |
              ((uint64_t)cover << ARTS_INV_CTS_SIZE_BITS);
  p.rdzv_cookie = cookie;
  if (requester_rank == arts_global_rank_id) {
    if (db != NULL) {
      arts_handler_db_inv_cts(db, &p);
    } else if (cookie != 0) {
      arts_db_buf_landing_return((struct arts_db_buffer_s *)(uintptr_t)cookie);
    }
    return;
  }
  arts_transport_send_async((int)requester_rank, (char *)&p, sizeof(p));
}


/* One message per sharer in the roster snapshot: the multicast whose width is
 * what the round costs. */
void arts_send_db_inv_invalidate(unsigned int sharer_rank,
                                 arts_guid_t db_guid) {
  if (sharer_rank == arts_global_rank_id) {
    /* Local hit: run the retirement inline.  Reachable under WB, where the
     * home holds no canonical copy and is therefore an ordinary sharer — its
     * own copy has to be retired like everyone else's.  A MISS still owes the
     * round its ack, which the handler body sends. */
    arts_shared_ptr_t h = arts_route_table_lookup_db(db_guid);
    struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(h);
    if (db != NULL) {
      arts_handler_db_inv_invalidate(db);
    } else {
      arts_send_db_inv_invalidate_ack(arts_guid_get_rank(db_guid), db_guid);
    }
    arts_shared_release(&h);
    return;
  }
  struct arts_msg_inv_invalidate_packet_s p;
  arts_fill_packet_header(&p.header, sizeof(p), MSG_DB_INV_INVALIDATE);
  p.header.rank = arts_global_rank_id;
  p.db_guid = db_guid;
  arts_transport_send_async((int)sharer_rank, (char *)&p, sizeof(p));
}

void arts_send_db_inv_invalidate_ack(unsigned int home_rank,
                                     arts_guid_t db_guid) {
  if (home_rank == arts_global_rank_id) {
    /* Local hit: account the ack inline.  A MISS drops — the round's other
     * acks still finish it, and a destroyed DB has no round to finish. */
    arts_shared_ptr_t h = arts_route_table_lookup_db(db_guid);
    struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(h);
    if (db != NULL) {
      arts_handler_db_inv_invalidate_ack(db, arts_global_rank_id);
    }
    arts_shared_release(&h);
    return;
  }
  struct arts_msg_inv_invalidate_ack_packet_s p;
  arts_fill_packet_header(&p.header, sizeof(p), MSG_DB_INV_INVALIDATE_ACK);
  p.header.rank = arts_global_rank_id;
  p.db_guid = db_guid;
  arts_transport_send_async((int)home_rank, (char *)&p, sizeof(p));
}

void arts_send_db_inv_request(struct arts_db_cache_s *cache,
                              arts_db_access_mode_t mode, uint32_t cover) {
  arts_guid_t db_guid = cache->db_guid;
  unsigned int home_rank = arts_guid_get_rank(db_guid);
  struct arts_rdzv_landing_s rdzv = {0, 0, 0, 0};
  /* A FRESH landing per fetch: reception isolation when a read reply and an
   * ownership transfer are in flight together (each PUT lands in its own
   * buffer, installed or recycled at commit).  Sized by the exact size when
   * known, else the GUID's szhint bound; txid==0 survives only as the
   * sentinel fallback (the home answers INV_CTS and the fetch re-issues).
   *
   * Being the home rank does NOT excuse a requester from advertising one.
   * Only a write-back home asks (a write-through home serves its own readers
   * from its canonical buffer), and it holds no bytes at all, so its own reads
   * are served by the holder, which is another rank except in an
   * ownership-transfer window, and need somewhere to land.  Skipping the
   * allocation there silently produced a data-less reply, leaving the reader
   * on whatever stale copy it already had. */
  uint64_t fetch_size = arts_db_first_fetch_size(cache);
  /* A pre-claim request leaves without a landing: the home discards it
   * unanswered if the create that claims this cache is this rank's. */
  if (fetch_size != 0 && arts_global_rank_count > 1 &&
      (cover & ARTS_COVER_PRECLAIM) == 0u) {
    (void)arts_db_buf_landing_alloc(cache, fetch_size, &rdzv);
  }
  if (home_rank == arts_global_rank_id) {
    /* Self-send: route through the OoO engine so before-create reorders
     * defer exactly like the wire path. */
    struct arts_ooo_args_db_inv_request_s args = {
        .requester = arts_global_rank_id,
        .db_guid = db_guid,
        .mode = mode,
        .cover = cover,
        .rdzv = rdzv,
    };
    arts_ooo_dispatch_or_defer_guid(db_guid, OOO_DB_INV_REQUEST, &args,
                                    sizeof(args));
    return;
  }
  struct arts_msg_inv_request_packet_s p;
  arts_fill_packet_header(&p.header, sizeof(p), MSG_DB_INV_REQUEST);
  p.header.rank = arts_global_rank_id;
  p.db_guid = db_guid;
  p.mode = (uint16_t)mode;
  p.cover = (uint8_t)cover;
  p.flags = 0u;
  p.requester = arts_global_rank_id;
  p.rdzv.addr = rdzv.addr;
  p.rdzv.key = rdzv.key;
  p.rdzv.txid = rdzv.txid;
  p.rdzv.cookie = rdzv.cookie;
  if (!arts_net_send_held((int)home_rank, (char *)&p, sizeof(p),
                          mode == DB_MODE_RO ? ARTS_HELD_READ : ARTS_HELD_WRITE,
                          db_guid)) {
    arts_transport_send_async((int)home_rank, (char *)&p, sizeof(p));
  }
}


void arts_send_db_inv_deliver(unsigned int requester_rank,
                              struct arts_db_s *db,
                              const struct arts_rdzv_landing_s *rdzv) {
  struct arts_db_cache_s *cache = &db->cache;
  struct arts_msg_inv_deliver_packet_s p;
  arts_fill_packet_header(&p.header, sizeof(p), MSG_DB_INV_DELIVER);
  p.header.rank = arts_global_rank_id;
  p.db_guid = cache->db_guid;
  /* Always the DB's size (descriptor state): a hinted first touch skipped
   * the size CTS, so even a data-less reply must teach the exact size.
   * Payload presence is signaled by rdzv_txid alone. */
  p.data_size = atomic_load_explicit(&cache->db_size, memory_order_relaxed);
  p.rdzv_txid = 0;
  p.rdzv_cookie = (rdzv != NULL) ? rdzv->cookie : 0;
  /* Serve from the CURRENT install; the ref pins the bytes across the PUT
   * (released by the fabric's local completion).  The stamp is the served
   * buffer's own version — it exists only to arbitrate the two asynchronous
   * install lanes at the requester, never to decide validity. */
  arts_shared_ptr_t src_h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *src =
      (struct arts_db_buffer_s *)arts_shared_get(src_h);
#ifdef ARTS_WRITE_POLICY_WT
  /* Stamp on the home's publication axis, never the buffer's own lane — see
   * hver.  The stamp exists only to arbitrate the receiver's two asynchronous
   * install lanes; nothing compares it to decide whether a copy is valid. */
  p.version = atomic_load_explicit(&arts_db_of_cache(cache)->hver,
                                   memory_order_acquire);
#else
  /* Owner-canonical: the served buffer's own version IS the axis, because
   * ownership migration carries it from owner to owner as one chain. */
  p.version = (src != NULL)
                  ? atomic_load_explicit(&src->version, memory_order_acquire)
                  : 0u;
#endif
  if (requester_rank == arts_global_rank_id) {
    /* Self-serve: the requester's cache IS this cache, so no payload moves and
     * the reply is dispatched inline.  Reachable whenever the server and the
     * reader are the same rank (a single-rank run, or a home that also holds
     * the grant).  With a buffer present the bytes are already where they need
     * to be and the landing goes back to the pool; with none, the reply is
     * about to mark this rank's copy VALID for a sized block, so the landing
     * becomes that copy's storage instead. */
    bool have_bytes = (src != NULL);
    if (have_bytes) {
      arts_db_buf_release(&src_h);
    }
    if (rdzv != NULL && rdzv->cookie != 0) {
      struct arts_db_buffer_s *landing =
          (struct arts_db_buffer_s *)(uintptr_t)rdzv->cookie;
      if (have_bytes) {
        arts_db_buf_landing_return(landing);
      } else {
        /* Version 1, never 0 — see the wire-side adopt above. */
        (void)arts_db_buf_adopt_landing(
            cache, p.version ? p.version : 1u, landing,
            atomic_load_explicit(&cache->db_size, memory_order_relaxed));
      }
      p.rdzv_cookie = 0;
    }
    arts_handler_db_inv_deliver(&p, sizeof(p));
    return;
  }
  if (src == NULL || rdzv == NULL || rdzv->txid == 0) {
    if (src != NULL) {
      arts_db_buf_release(&src_h);
    }
    arts_transport_send_async((int)requester_rank, (char *)&p, sizeof(p));
    return;
  }
  p.data_size = atomic_load_explicit(&cache->db_size, memory_order_relaxed);
  p.rdzv_txid = rdzv->txid;
  arts_net_put_payload(
      (int)requester_rank, rdzv->addr, rdzv->key, rdzv->txid, src->data,
      atomic_load_explicit(&cache->db_size, memory_order_relaxed),
      arts_db_buf_ref_release_cb, (void *)src_h);
  arts_transport_send_async((int)requester_rank, (char *)&p, sizeof(p));
}



/* The ex-holder keeps the bytes it wrote, so from the flip onward it is a
 * sharer: the new owner's first release must invalidate it like any other.
 *
 * Under WT the home rank is exempt — its buffer IS the canonical copy, kept
 * current by every release's publish, so it is never stale and never a target.
 * Under WB nothing is exempt: the home holds no canonical bytes, so a copy
 * that happens to sit on the home rank goes stale exactly like any other and
 * must be registered. */
void arts_db_grant_note_ex_holder(struct arts_db_s *db, unsigned int rank) {
#ifdef ARTS_WRITE_POLICY_WT
  if (rank == arts_global_rank_id) {
    return;
  }
#endif
  (void)arts_rank_bitset_set(&db->roster, rank);
}

/* A create's hold is this arm's ordinary write hold, taken when the block was
 * made; the bytes under it are the cache's own, so the release needs no
 * pointer to them.  Policy-independent, so it sits here rather than in the
 * write-policy TU that defines arts_db_release_rw itself. */
void arts_db_release_created(struct arts_db_cache_s *cache) {
  arts_db_release_rw(cache, NULL);
}

/* arts_handler_db_publish_ack — shared flight-completion body, defined once in
 * coherence/coherence.c for every publishing arm. */
