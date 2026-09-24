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
#include "arts/gas/guid.h"
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

/* arts_handler_db_destroy is protocol-specific — the roster fan-out source
 * differs (WT walks home->cached_version; WB walks rw_holder +
 * cached_ranks + pending_rw) — so its whole body lives in
 * each arm's own write-policy TU.  All three skeletons run the roster fan-out,
 * then arts_route_table_set_destroyed; any waiter left parked at destroy time
 * (UB per OCR) is cleaned up by the refcount-0 cache destructor. */

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
   * unasked, a returning holder's claim both arbitrate against; on the
   * coalesce arms the descriptor is already visible when this runs. */
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

#ifdef ARTS_FAM
/* Offer a slot this rank minted to a cache that may already have one, and say
 * whether it was taken.  The two kinds of address are not alike: one a
 * CREATOR announced is authoritative, so a second address for the block is
 * the protocol error arts_db_fam_slot_record refuses; one minted HERE for a
 * create that named none is PROVISIONAL until an install wins, because
 * several ranks may create one label with no acquisition and each will mint
 * for it.  Exactly one of those survives and the losers free their own. */
static bool fam_slot_offer(struct arts_db_cache_s *cache, uint64_t addr) {
  uint64_t expect = 0;
  return __atomic_compare_exchange_n(&cache->fam_addr, &expect, addr, false,
                                     __ATOMIC_RELEASE, __ATOMIC_ACQUIRE);
}

/* A creator that acquires caches the block from create without ever
 * requesting a turn, so the destroy roster is the only thing that can tell it
 * the block is gone and its store may be handed out again.  The roster is a
 * home field, absent from a cache-only stub. */
static void fam_roster_creator(struct arts_db_s *db,
                               unsigned int creator_rank) {
  if (db->home_initialized) {
    (void)arts_rank_bitset_set(&db->cached_ranks, creator_rank);
  }
}

/* The same, for a block whose route slot is ALREADY published: a bit set on a
 * pinned object is not ordered against a destroy's scan of the roster, so it
 * is re-checked against the route slot and the creator is told directly if the
 * block is already gone.  The destroy fences between detaching the object and
 * scanning; this fences between setting and looking up — so the two cannot
 * both miss, and both telling the creator is harmless because the message is
 * idempotent.  Reports what the re-check saw: true once the block is gone, so
 * a caller still holding something of the block's can stop handing it over. */
static bool fam_roster_creator_published(struct arts_db_s *db,
                                         arts_guid_t db_guid,
                                         unsigned int creator_rank) {
  fam_roster_creator(db, creator_rank);
  atomic_thread_fence(memory_order_seq_cst);
  arts_shared_ptr_t h = arts_route_table_lookup_db(db_guid);
  bool gone = (arts_shared_get(h) == NULL);
  arts_shared_release(&h);
  if (gone && creator_rank != arts_global_rank_id) {
    arts_send_db_cache_destroy(creator_rank, db_guid);
  }
  return gone;
}
#endif

void arts_handler_db_create(struct arts_msg_db_create_coherent_packet_s *p) {
  /* Home-side init for non-home creator.  Per coherence design plan
   * §968-988: install zero-init buffer, home struct with rw_holder =
   * creator_rank, writer_count = 0 (home is non-owner). */
  unsigned int creator_rank = p->header.rank;
  arts_guid_t db_guid = p->db_guid;
  uint64_t db_size = p->db_size;
  /* NO_ACQUIRE: the creator neither acquires nor publishes, so the home is
   * the sole idle owner (not a non-owner awaiting a creator publish). */
  bool no_acquire = (p->flags & ARTS_DB_PROP_NO_ACQUIRE) != 0;

  /* This handler installs/initializes the home directory, so it is only ever
   * dispatched to the GUID's home rank — where the descriptor is always a
   * full allocation.  A cache-only stub (which omits the home-arm fields)
   * is only ever installed on non-home ranks, so the home-field writes below
   * (arts_db_home_init / rw_holder) stay in bounds. */
  assert(arts_db_home_rank(db_guid) == arts_global_rank_id &&
         "home-directory init must run on the block's home rank");

#ifdef ARTS_CXL_COHERENT
  if ((arts_db_types_t)p->db_type == ARTS_DB_CXL) {
    /* A CXL block's create message carries no payload obligation: the bytes
     * were allocated by the creator directly in the shared window, and this
     * rank already addresses them.  What the creator is telling us is the two
     * things only it knows — that the block exists, and that it is holding it
     * RW right now — so that a writer arriving from a third rank queues behind
     * that hold instead of being granted alongside it.
     *
     * Nothing installs a buffer here, and no create-return credit goes back:
     * a credit exists to let a write-through creator publish without an
     * announce round, and a CXL release publishes nothing. */
    arts_shared_ptr_t cxl_h = arts_route_table_lookup_db(db_guid);
    struct arts_db_s *cxl_live = (struct arts_db_s *)arts_shared_get(cxl_h);
    if (cxl_live != NULL) {
      /* Already present — the only way that happens is a duplicate create for
       * a labeled GUID, or a destroy/recreate.  Learn the size and leave the
       * live directory alone. */
      if (cxl_live->cache.db_size == 0) {
        cxl_live->cache.db_size = db_size;
      }
      arts_shared_release(&cxl_h);
      return;
    }
    arts_shared_release(&cxl_h);

    struct arts_db_s *cxl_home = (struct arts_db_s *)arts_malloc_aligned(
        sizeof(struct arts_db_s), ARTS_CACHE_LINE_SIZE);
    memset(cxl_home, 0, sizeof(struct arts_db_s));
    cxl_home->db_type = ARTS_DB_CXL;
    /* HOME_RECV seeds the directory with the creator's RW hold (w = 1), which
     * its release drives back to zero — the same accounting an ordinary
     * remote-home create gets.  NO_ACQUIRE then idles both words, because no
     * EDT will ever run that release. */
    arts_db_cache_init(&cxl_home->cache, db_guid, db_size,
                       ARTS_DB_INIT_HOME_RECV, creator_rank);
    db_create_no_acquire_idle(cxl_home, no_acquire);
    if (arts_route_table_install_if_absent(cxl_home, db_guid,
                                           arts_global_rank_id,
                                           /*used=*/true)) {
      arts_ooo_drain_guid(db_guid);
      return;
    }
    arts_db_free(cxl_home); /* lost the race — the winner is equivalent */
    return;
  }
#endif /* ARTS_CXL_COHERENT */

  /* Race against stub_install or another path that already set up an
   * empty cache_s on this rank — coalesce by promoting the existing
   * OWNER entry rather than allocating a duplicate. */
  arts_shared_ptr_t existing_h = arts_route_table_lookup_db(db_guid);
  struct arts_db_s *existing = (struct arts_db_s *)arts_shared_get(existing_h);
  if (existing != NULL && existing->db_type == ARTS_DB) {
    struct arts_db_cache_s *cache = &existing->cache;
    struct arts_db_s *db = existing;
    arts_shared_ptr_t buf_h = arts_db_buf_acquire(cache);
    bool buf_absent = (arts_shared_get(buf_h) == NULL);
    arts_db_buf_release(&buf_h);
    if (buf_absent && db_size > 0 && !no_acquire) {
      /* Same seam as the fresh-stub path below: the coalesce winner must end
       * in the identical buffer state, or the arm's publication predicate
       * (buffer presence / version zero) reads differently depending on who
       * won an internal race. */
      arts_db_create_install_home_buffer(cache, db_size);
    }
    if (cache->db_size == 0) {
      cache->db_size = db_size;
    }
#ifdef ARTS_FAM
    /* One block, one store.  An ANNOUNCED address is authoritative: a create
     * whose announce reaches an object the home already has made no block, so
     * the address it names must be the one the home already holds, and a
     * different one means two creators minted a store for one label -- which
     * the record refuses.  A create that named no address says nothing here,
     * and the store the home minted for the block stands. */
    if (p->fam_addr != 0) {
      (void)arts_db_fam_slot_record(cache, p->fam_addr);
    }
#endif
    /* The home-directory fields below are out of bounds on a cache-only
     * stub, and a stub is never installed on a block's own home rank: the
     * acquire path installs one only when the owner is not this rank, and a
     * transfer response cannot land here in a supported program.  So a
     * coalesce target on the home always carries its directory already —
     * asserted, and then GUARDED, because an assert says nothing about the
     * build where the write would actually land past the allocation. */
    assert(db->home_initialized &&
           "a home rank's descriptor carries its home directory");
    /* The directory is NOT re-seeded here.  It is live protocol state from
     * the moment this block's create installed it, and a create reaching
     * this arm made no block: naming a holder now would either restate what
     * the directory already says or hand the right to a rank that does not
     * hold it — and the next hand-back would then be discharged from a word
     * the home had already taken possession of.  A block's holder is named
     * once, by the create that made it. */
#ifdef ARTS_FAM
    if (!no_acquire) {
      (void)fam_roster_creator_published(db, db_guid, creator_rank);
    }
#endif
#if (!defined(ARTS_PROTOCOL_EXCL) && defined(ARTS_WRITE_POLICY_WT)) ||       \
    defined(ARTS_PROTOCOL_FLUSH)
    if (!no_acquire) {
      arts_send_db_create_return(creator_rank, cache);
    }
#endif
    arts_shared_release(&existing_h);
    return;
  }
  if (existing != NULL) {
    /* existing non-ARTS_DB entry (no coherence cache) — drop the ref and
     * proceed to the install/coalesce branch below. */
    arts_shared_release(&existing_h);
  }

  /* No existing entry -- allocate the db_s stub (cache embedded), install in
   * route_table.
   *
   * Whether the home installs a buffer here is the arm's decision
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
     * CREATOR_HOME sets writer_count = sentinel(1) + creator_hold(1) = 2, but
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
#ifdef ARTS_FAM
  /* The creator either names the block's store or leaves it to the home: the
   * home is the only other rank that can make one before a request can be
   * granted.  Before the home's buffer install, which adopts the slot on the
   * residency that keeps no copy, and before the route install, whose drain
   * can serve a request out of it.  The stub's init has recorded the size the
   * create declares, so the allocation has one. */
  bool stub_slot_minted = false;
  if (p->fam_addr != 0) {
    (void)arts_db_fam_slot_record(&stub->cache, p->fam_addr);
  } else {
    stub_slot_minted = arts_db_fam_slot_create(&stub->cache);
  }
  /* An announce carrying no address is a creator with no write turn of its
   * own coming (NO_ACQUIRE) — the mint above just gave the block its store,
   * and nothing else will ever write it before a first acquirer reads it, so
   * the declared-zero contract has to be established right here. */
  if (stub_slot_minted && no_acquire) {
    arts_db_fam_slot_zero(&stub->cache);
  }
  /* No re-check here: the route install below publishes the object, so a
   * destroy that can scan the roster at all runs after this set. */
  if (!no_acquire) {
    fam_roster_creator(stub, creator_rank);
  }
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

  if (arts_route_table_install_if_absent(stub, db_guid, arts_global_rank_id,
                                         /*used=*/true)) {
    arts_ooo_drain_guid(db_guid);
#if (!defined(ARTS_PROTOCOL_EXCL) && defined(ARTS_WRITE_POLICY_WT)) ||       \
    defined(ARTS_PROTOCOL_FLUSH)
    /* Off the critical path: the credit flies while the creator EDT is still
     * writing, so a create -> write -> release sequence publishes with no
     * announce round.  A creator that took no right never publishes — no
     * credit.
     * Re-acquire through the route table rather than using the raw stub
     * pointer: the drain above can run a deferred DESTROY that frees the
     * stub, and a dead slot must not advertise a recycled buffer. */
    if (!no_acquire) {
      arts_shared_ptr_t live_h = arts_route_table_lookup_db(db_guid);
      struct arts_db_s *live = (struct arts_db_s *)arts_shared_get(live_h);
      if (live != NULL) {
        arts_send_db_create_return(creator_rank, &live->cache);
      }
      arts_shared_release(&live_h);
    }
#endif
    return;
  }

  /* Lost race — free our stub and coalesce into the existing entry. */
#ifdef ARTS_FAM
  /* The stub dies before the winner is resolved, so its store travels in a
   * local. */
  uint64_t lost_addr = arts_db_fam_slot_addr(&stub->cache);
  __atomic_store_n(&stub->cache.fam_addr, (uint64_t)0, __ATOMIC_RELEASE);
  bool winner_gone = false;
#endif
  arts_db_free(stub);
  arts_shared_ptr_t winner_h = arts_route_table_lookup_db(db_guid);
  struct arts_db_s *winner = (struct arts_db_s *)arts_shared_get(winner_h);
  if (winner != NULL && winner->db_type == ARTS_DB) {
    struct arts_db_cache_s *cache = &winner->cache;
    struct arts_db_s *db = winner;
    arts_shared_ptr_t buf_h = arts_db_buf_acquire(cache);
    bool buf_absent = (arts_shared_get(buf_h) == NULL);
    arts_db_buf_release(&buf_h);
    if (buf_absent && db_size > 0 && !no_acquire) {
      /* Same seam as the fresh-stub path — see the coalesce branch above. */
      arts_db_create_install_home_buffer(cache, db_size);
    }
    if (cache->db_size == 0) {
      cache->db_size = db_size;
    }
    /* The home-directory fields below are out of bounds on a cache-only
     * stub, and a stub is never installed on a block's own home rank: the
     * acquire path installs one only when the owner is not this rank, and a
     * transfer response cannot land here in a supported program.  So a
     * coalesce target on the home always carries its directory already —
     * asserted, and then GUARDED, because an assert says nothing about the
     * build where the write would actually land past the allocation. */
    assert(db->home_initialized &&
           "a home rank's descriptor carries its home directory");
    (void)db;
    /* The directory is NOT re-seeded here — see the coalesce branch above. */
#ifdef ARTS_FAM
    if (!no_acquire) {
      winner_gone = fam_roster_creator_published(db, db_guid, creator_rank);
    }
#endif
#if (!defined(ARTS_PROTOCOL_EXCL) && defined(ARTS_WRITE_POLICY_WT)) ||       \
    defined(ARTS_PROTOCOL_FLUSH)
    if (!no_acquire) {
      arts_send_db_create_return(creator_rank, cache);
    }
#endif
  }
#ifdef ARTS_FAM
  /* The store the dead stub carried is the winner's, because one block has
   * one store — but only an announced address may insist on it.  A store this
   * rank minted was provisional: it is offered to the winner and, if the
   * winner already has one, goes back to this rank's slice.  A store the
   * creator announced is never freed here — the free demands the caller's own
   * slice, and the creator's block still rests in it.  A winner the re-check
   * above found already destroyed takes neither: a provisional store goes
   * straight back to this rank's slice instead of onto a cache whose teardown
   * has passed, and an announced one is left to the creator that re-check just
   * notified — a foreign slice cannot be freed here, and a second free of a
   * store the winner did record would hand one granule out twice. */
  if (lost_addr != 0) {
    if (stub_slot_minted) {
      if (winner_gone || winner == NULL || winner->db_type != ARTS_DB ||
          !fam_slot_offer(&winner->cache, lost_addr)) {
        arts_fam_free((void *)(uintptr_t)lost_addr);
      }
    } else if (!winner_gone && winner != NULL && winner->db_type == ARTS_DB) {
      (void)arts_db_fam_slot_record(&winner->cache, lost_addr);
    }
  }
#endif
  if (winner != NULL) {
    arts_shared_release(&winner_h);
  }
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
   *     nobody has written the block, its value IS zero, and this rank is
   *     entitled to storage of the declared size whatever its contents —
   *     adopt the landing it already allocated, stamped 1 rather than the 0
   *     that means "nothing";
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
 * Destroy is just the route-slot detach (CAS value→NULL + drop the install
 * ref); the cb deleter (arts_db_cache_destructor) runs at refcount 0 and does
 * the cleanup (free the parked-waiter nodes).  Destroying a DB that an EDT
 * still has a pending dependence on is undefined per OCR (ocrDbDestroy: the
 * user ensures the DB is not in use), so no parked-EDT wake is attempted. */
void arts_handler_db_cache_destroy(void *item_v, void *args_v) {
  struct arts_db_cache_destroy_args_s *a =
      (struct arts_db_cache_destroy_args_s *)args_v;
  (void)arts_route_table_set_destroyed(a->db_guid);
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
 * the teardown that claimed arts_route_table_set_destroyed, and only the
 * home ever frees a slot it handed out). */
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
