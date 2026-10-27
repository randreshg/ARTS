/******************************************************************************
** This material was prepared as an account of work sponsored by an agency   **
** of the United States Government.  Neither the United States Government    **
** nor the United States Department of Energy, nor Battelle, nor any of      **
** their employees, nor any jurisdiction or organization that has cooperated **
** in the development of these materials, makes any warranty, express or     **
** implied, or assumes any legal liability or responsibility for the accuracy,*
** completeness, or usefulness or any information, apparatus, product,       **
** software, or process disclosed, or represents that its use would not      **
** infringe privately owned rights.                                          **
**                                                                           **
**                      PACIFIC NORTHWEST NATIONAL LABORATORY                **
**                                  operated by                              **
**                                    BATTELLE                               **
**                                     for the                               **
**                      UNITED STATES DEPARTMENT OF ENERGY                   **
**                         under Contract DE-AC05-76RL01830                  **
**                                                                           **
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License");           **
** you may not use this file except in compliance with the License.          **
******************************************************************************/
#ifndef ARTS_COHERENCE_VAL_TYPES_H
#define ARTS_COHERENCE_VAL_TYPES_H
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file val/types.h
 * @brief VAL (Multi-Reader, Node-Exclusive-Writer) cache/db layout.
 *
 * Selected by arts/coherence/types.h when ARTS_PROTOCOL_VAL is defined.
 * The WT vs WB write-policy variant is chosen here by ARTS_WRITE_POLICY_WB.
 * The protocol-agnostic pieces (buffer, snapshot waiter, defines,
 * container_of) come from types_common.h; the of_cache/total_size/stub_size
 * helpers live in the dispatcher (types.h), after this header defines cache +
 * db_s.
 *
 * @note Internal header.  User code should include @c arts.h.
 */

#include "arts/coherence/types_common.h"
#ifndef ARTS_PROTOCOL_VAL
#error "val/types.h is the VAL layout; this translation unit's cell is another"
#endif

#include <assert.h>

/* ========================================================================= */

/*--- Per-DB home metadata ------------------------------------------------
 * Lives only on the rank that hosts a given DB (GUID home decides).  Holds
 * the directory state needed for ownership transfer and dedup.  The MPSC
 * queues are defined inline below (needed for struct embedding in arts_db_s).
 *
 * Vyukov MPSC queue node carrying a requester rank (home GRANT_REQUEST
 * queue). The embedded `next` pointer is owned by the queue (push/pop manage
 * it). Producers are foreign-rank GRANT_REQUEST handlers; the single
 * consumer is the home-side dispatcher holding the invalidate_in_flight baton.
 * Orders: the queue's declaration in coherence/directory.h.
 */
struct arts_home_grantreq_node_s {
  _Atomic(struct arts_home_grantreq_node_s *) next;
  unsigned int rank;
  uint64_t have_version; /* version the requester already holds */
  struct arts_rdzv_landing_s rdzv; /* requester's transfer landing */
};

struct arts_home_grantreq_queue_s {
  _Atomic(struct arts_home_grantreq_node_s *)
      tail; /* producer end (push exchanges here) */
  _Atomic(struct arts_home_grantreq_node_s *)
      head;                             /* consumer end (pop advances here) */
  struct arts_home_grantreq_node_s stub; /* permanent sentinel */
};

/*--- The request word ---------------------------------------------------
 * One 64-bit word per cache holds both axes' request state and the chains of
 * readers parked on them, so every decision that couples the axes is one CAS:
 *
 *   [ rw:2 (63..62) | ro:1 (61) | rp:1 (60) | par:1 (59) | stub:1 (58) |
 *     group:29 (57..29) | next:29 (28..0) ]
 *
 *   rw    the write request: IDLE, OPEN (claimed, not yet committed) or SENT.
 *   ro    a read request (the window) is out.
 *   rp    the window's request was answered by a size round or a dismiss
 *         while a write request was out (or OPEN): its cohort rides that write
 *         request instead.
 *   par   the parity of the last read request this cache sent.
 *   stub  a dependence's first touch made the cache and nothing has shown the
 *         home installed yet: the create's claim clears it, and so does every
 *         reply unless rw is OPEN (the OPEN's holder acts only while the bit
 *         it read stands).  A request sent while it is set carries PRECLAIM.
 *   group the window's cohort, fixed when its request left (reader pool
 *         indices).
 *   next  readers waiting for the next window, or riding the write request.
 */
#define VREQ_HEAD_BITS ARTS_DB_READER_IDX_BITS
#define VREQ_HEAD_MASK ((uint64_t)((1ULL << VREQ_HEAD_BITS) - 1))
#define VREQ_NEXT(s) ((uint32_t)((s) & VREQ_HEAD_MASK))
#define VREQ_GROUP(s) ((uint32_t)(((s) >> VREQ_HEAD_BITS) & VREQ_HEAD_MASK))
#define VREQ_STUB(s) ((uint32_t)(((s) >> 58) & 1ULL))
#define VREQ_PAR(s) ((uint32_t)(((s) >> 59) & 1ULL))
#define VREQ_RP(s) ((uint32_t)(((s) >> 60) & 1ULL))
#define VREQ_RO(s) ((uint32_t)(((s) >> 61) & 1ULL))
#define VREQ_RW(s) ((uint32_t)(((s) >> 62) & 3ULL))
#define VREQ_MAKE(rw, ro, rp, par, stub, group, next)                          \
  ((((uint64_t)(rw) & 3ULL) << 62) | (((uint64_t)(ro) & 1ULL) << 61) |        \
   (((uint64_t)(rp) & 1ULL) << 60) | (((uint64_t)(par) & 1ULL) << 59) |       \
   (((uint64_t)(stub) & 1ULL) << 58) |                                        \
   (((uint64_t)(group) & VREQ_HEAD_MASK) << VREQ_HEAD_BITS) |                 \
   ((uint64_t)(next) & VREQ_HEAD_MASK))
static_assert(2 * VREQ_HEAD_BITS == 58,
              "the two chain heads must fill the bits below stub");
#define VREQ_RW_IDLE 0u
#define VREQ_RW_OPEN 1u
#define VREQ_RW_SENT 2u

/* The request word's operations and the actions its arbiter returns. */
enum {
  VREQ_OP_ACQ_RO,    /* park a reader (idx), or open a request for it */
  VREQ_OP_ANSWER,    /* the window's request was answered */
  VREQ_OP_CTS,       /* ... by a size round or a dismiss (no data) */
  VREQ_OP_COVER,     /* a write permission landed: serve every chain */
  VREQ_OP_RW_CLAIM,  /* IDLE -> OPEN */
  VREQ_OP_RW_COMMIT, /* OPEN -> SENT */
  VREQ_OP_RW_GIVE,   /* OPEN -> IDLE */
  VREQ_OP_RW_CLEAR,  /* SENT -> IDLE where the grant lands, + COVER */
  VREQ_OP_CLAIM,     /* a create claims the cache: COVER, the write axis is
                        the claim's, and the cache is no longer a stub */
  VREQ_OP_DECLAIM,   /* a size round answered the write request: the home is
                        installed and was created elsewhere, so the cache can
                        never be claimed (precondition rw == SENT) */
};
enum {
  VREQ_ACT_NONE,
  VREQ_ACT_PARK,     /* linked onto next */
  VREQ_ACT_SEND_RO,  /* opened a window with this reader as its cohort */
  VREQ_ACT_SERVE,    /* serve the chains the transition took */
  VREQ_ACT_SERVE_SEND, /* + the window re-armed: send its request */
  VREQ_ACT_RESEND,   /* a size round, dismiss or give-back re-sends the
                        window's request */
  VREQ_ACT_WON,
  VREQ_ACT_LOST,
  VREQ_ACT_LATE,     /* an answer for no request out: the caller raises it */
};

uint64_t val_req_compute_next(uint64_t cur, int op, uint32_t idx,
                              uint32_t *out_act);

/*--- Per-rank DB cache ---------------------------------------------------
 * Every rank that has acquired or hosts a given DB has one of these.  The
 * cache is the runtime's coherence-protocol state: ownership, the live
 * buffer, parked waiters, and destroy lifecycle.
 *
 *   writer_count   ownership counter AND confirmation state in one word.
 *                  > 0 ⇒ this rank holds RW ownership; 0 ⇒ it holds none;
 *                  < 0 (ARTS_GRANT_UNCONFIRMED set, write-back only) ⇒ held
 *                  but the directory flip is not yet published, so the data is
 *                  here and readable while no RW EDT may run.  Reading the
 *                  sign is what makes the turn-taking CAS a complete test.
 *   buffer         currently-installed buffer, an atomic shared_ptr slot;
 *                  readers acquire via acquire_buf's acquire-and-validate load.
 *   publication_waiters
 *                  (WT) Treiber stack of reads the home parked before the
 *                  block's first publication; drained whole once published.
 *
 * The home-directory fields (pending_rw, invalidate_in_flight, rw_holder,
 * cached_ranks, cached_version, ...) are NOT here — they are inlined
 * directly in the wrapping struct arts_db_s, after this cache + db_type, and
 * reached via arts_db_of_cache(cache).  Non-home ranks allocate a cache-only
 * footprint that stops before them. */
struct arts_db_cache_s {
  /* Every write after the cache is published is a read-modify-write, so a
   * release RMW heads a release sequence the next acquire RMW or load reads:
   * an install's release carries the buffer, the learned size and (PURGE) the
   * grant generation to the admission that sees the right held; a writer's
   * acq_rel decrement carries its bytes and version bump to whichever
   * decrement reaches the transfer edge, which acquires them before it ships.
   * Every load outside the post-join quiescence walk is acquire, a load
   * whose value a following CAS re-validates included. */
  _Atomic(unsigned int) writer_count;
  /* Buffers drawn from this cache and not parked back, plus the flag the
   * storage release sets once the destructor has run (encoding in buffer.c,
   * lifetime rule in types_common.h).  A draw adds 1 relaxed, under the
   * drawer's hold on the descriptor; a park's decrement and the flag's add
   * are acq_rel, so every park is ordered before the storage free. */
  _Atomic(unsigned int) buf_holds;
  _Atomic(arts_shared_slot_t) buffer;
  arts_lockfree_pool_t buf_freelist; /* parked buffers, drawn first */
  /* Parked buffers a draw found too small: a popper must not free a node
   * another popper may still be reading, so they wait here for the storage
   * release, which frees them with the list. */
  arts_lf_stack_t buf_retired;
  arts_guid_t db_guid;
  /* The block's exact size once this rank has learned it, 0 until then.
   * Every writer stores the one size the block was created with, so a
   * reader sees 0 or that size and nothing else: relaxed throughout
   * (arts_db_cache_learn_size).  A use that needs the exact size for an
   * allocation or a transfer length is ordered after the learn by its own
   * edge (the grant install, the route install, program order). */
  _Atomic(uint64_t) db_size;
  /* Does this cache's payload slot still need materializing?  1 until a
   * buffer has been installed, 0 after — the first entitled use of a block
   * whose create left it no storage is what installs one, and every later
   * use must find that out without touching the slot's refcount.  The 0 is
   * stored with release after the slot's install, and the fast exit that
   * reads it acquires the installed buffer with it.  A stale 1 costs one
   * materialize attempt that then clears it.  The slot is monotone for a
   * live cache — only the destructor puts NULL back — so a 0 can never go
   * stale. */
  _Atomic(uint8_t) payload_pending;
  /* What made this cache (arts_db_init_kind_t): a create's own descriptor,
   * the home's directory, or a dependence's first touch.  A create that
   * claims a first touch's cache turns it into its own
   * (arts_db_create_claims_stub); nothing else writes it after init.  Only
   * its own value: the claim is one CAS, so one create claims a cache. */
  _Atomic(uint8_t) init_kind;
#ifndef ARTS_RELEASE_PURGE
  /* New owner rank for the next ownership transfer, published by this round's
   * GRANT_INVALIDATE handler BEFORE it withdraws the sentinel.  Sentinel
   * ARTS_NO_PENDING_OWNER == no transfer pending.  The publish-before-
   * sentinel-withdraw ordering makes a separate transfer_pending flag
   * redundant: whichever actor drives writer_count to 0 (the INVALIDATE, or the
   * last release_rw) reads this field — a non-sentinel value names the target,
   * so that actor ships the grant to it (GRANT_RESPONSE under both write
   * policies, dispatched inline for a self-transfer).  Single writer per round
   * (home baton gate), and the decrement that reaches 0 reads the withdrawal's
   * release sequence, so the field is plain.  Shared by both write policies:
   * the INVALIDATE carries new_owner under each. */
  unsigned int incoming_new_owner;
  /* The pending new owner's transfer landing, published together with (and
   * under the same single-writer / publish-before-withdraw discipline as)
   * incoming_new_owner: the 0-edge actor PUTs the transfer payload here. */
  struct arts_rdzv_landing_s incoming_new_owner_rdzv;
  /* The version the pending new owner reported holding (its request's
   * have_version), published with the two fields above; the 0-edge actor
   * ships no payload when the canonical bytes are at that very version. */
  uint64_t incoming_new_owner_have;
#endif
  /* The request word (VREQ_* above).  Every write after init is an acq_rel
   * CAS: a reader's park carries its node's fields to the CAS that takes the
   * chain, and the write axis's claim and clear each pair with the pending_rw
   * stack through a seq_cst fence (arts_db_acquire_remote_rw,
   * arts_db_grant_take_waiters). */
  _Atomic(uint64_t) req_state;
  struct arts_db_reader_pool_s reader_pool; /* nodes of the word's chains */
#ifdef ARTS_WRITE_POLICY_WB
  /* WB: owner-side dedup map.  Allocated with the cache and freed with it; its
   * content migrates with ownership (GRANT_RESPONSE serializes it). */
  struct arts_rank_to_u64_map_s *cached_version;
  /* Treiber stack of RW waiters parked on this rank (order-free drain-all). */
  arts_lf_stack_t pending_rw;
#else
  /* Owner-side dedup map.  WT never creates it (always NULL): home serves RO
   * via SNAPSHOT_REQUEST with the home->cached_version watermark, so the WT
   * owner→owner transfer ships an EMPTY map.  The field exists so the shared
   * arts_db_send_grant_response (coherence/grant.c) compiles for
   * both write policies (it gates the map build on this being non-NULL). */
  struct arts_rank_to_u64_map_s *cached_version;
  /* Treiber stack of RW waiters parked on this rank (order-free drain-all). */
  arts_lf_stack_t pending_rw;
  arts_lf_stack_t publication_waiters;

  /* WT publish write-combining — the write-side twin of the RO combining
   * window below.  pub_flight is the one-word flight state ({FLYING,DIRTY}):
   * a claim (CAS 0->FLYING) or a join (CAS ->|DIRTY) follows the releaser's
   * push onto pub_waiters, and every reset of the word by the flight owner
   * is an exchange, so a join is either read by that reset (and its node
   * re-drained) or lands after it (and fails the owner's landing CAS).
   * pub_waiters holds heap {sem, version} nodes for every releaser blocked
   * until a covering publish is ACKed (heap for the same reason the publish
   * rendezvous is — a shutdown-escaped waiter leaks its node and a late
   * drain may still post into it). */
  _Atomic(unsigned int) pub_flight;
  arts_lf_stack_t pub_waiters;
  /* Waiters a completing flight drained but its version did not cover, held
   * for the trailing flight's ACK.  Written by the flight owner (one
   * completion at a time) with release, and taken by an exchange, either by
   * that next completion or by an abandon (a destroy, a shutdown), which may
   * run on another thread at any time.  Non-NULL only while the flight word
   * is FLYING. */
  _Atomic(arts_lf_link_t *) pub_parked;
  /* The durable publish credit: the home's stable buffer plus a
   * receiver-minted txid, refilled by a payload flight's ACK and by the
   * create's return.  txid is the presence flag (0 = none): a teacher stores
   * addr and rkey relaxed and then txid with release, and the flight's
   * acquire exchange of txid sees the whole triple. */
  _Atomic(uint64_t) home_pub_addr;
  _Atomic(uint64_t) home_pub_rkey;
  _Atomic(uint64_t) home_pub_txid;
#endif
#ifdef ARTS_RELEASE_PURGE
  /* The hand-back obligation: (grant_generation << 1) | armed, or zero.
   * Armed by a release that gave the write right up while its payload was
   * still in flight; discharged by whichever of the publish sender and the
   * release itself CASes it back to zero, so exactly one return goes out.
   * The generation is what makes the release's discharge its own: a settle
   * that wakes after the right has come back and been re-granted here finds
   * a token of a later generation and must not touch it.  Every write is an
   * acq_rel RMW; the token carries only its own value. */
  _Atomic(uint64_t) pending_grant_return;
  /* Count of write rights installed on this rank; the token above names the
   * one it belongs to.  Bumped by the install before its possession CAS
   * (release) publishes the right, and read by the releaser after its own
   * claim CAS acquired that possession. */
  _Atomic(uint64_t) grant_generation;
#endif
};

/** Internal DataBlock descriptor.
 *
 *  The per-rank coherence cache (struct arts_db_cache_s) is embedded by value
 *  as the FIRST member: the cb object the route_table wraps is the db_s, and
 *  cache-to-db_s recovery is a zero-cost cast (cache == &db->cache, and since
 *  cache is first, (struct arts_db_s *)cache aliases the wrapping db_s).  Use
 *  arts_db_of_cache() for that recovery.  The GPU kinds
 *  (ARTS_DB_GPU_PIN/ARTS_DB_GPU) leave the cache zeroed (no DB-level
 *  coherence) and store their payload at (db + 1). */
struct arts_db_s {
  struct arts_db_cache_s cache; /**< FIRST — coherence state (embedded by
                                     value).  Zeroed for the GPU kinds. */
  arts_db_types_t db_type;      /**< Storage subtype (DB/GPU/GPU_PIN).  Placed
                                     right after the cache so a cache-only stub
                                     (arts_db_cache_stub_size()) still covers it
                                     — every coherence lookup/free reads it. */
  /* Home-directory metadata — present only on the rank that is the GUID home
   * for this DB.  Non-home / WB / creator-remote ranks allocate a cache-only
   * footprint of arts_db_cache_stub_size() bytes: it ends at the first home-arm
   * field below (rw_holder, or pending_rw where there is none), so it
   * INCLUDES home_initialized but omits every
   * home-arm field.  home_initialized MUST stay in bounds: the cache destructor
   * reads it on every free to decide whether to tear the home directory down —
   * on a stub it reads zeroed false and skips teardown.  The home-arm fields
   * are touched only on the home rank, never through a stub. */
  bool home_initialized; /**< one-shot init sentinel (set by arts_db_home_init).
                          */
#ifdef ARTS_WRITE_POLICY_WB
  /* The write holder a revocation targets and a read is redirected to:
   * stored release and loaded acquire under the baton; loads outside it (the
   * redirect, the destroy, the walk) carry only its value (directory.h). */
  _Atomic(unsigned int) rw_holder;
  struct arts_home_grantreq_queue_s pending_rw; /* embedded Vyukov MPSC */
  /* The serving baton: claimed by an acq_rel CAS, given up by a release
   * store, a seq_cst fence and a re-probe (directory.h). */
  _Atomic(unsigned int) invalidate_in_flight;
  /* destroy fan-out roster: creator, readers, writers */
  struct arts_rank_bitset_s cached_ranks;
  /* The declared-arrival state: per requester rank the parity of its last
   * read request this home accounted for (by its arrival or a declaration of
   * it, whichever came first); the gate [td:1 | rwo:1 | roo:1 | owed:29],
   * the destroy's mark and what the home was told is still coming, every
   * write after init an acq_rel RMW; the rank whose create claimed a
   * first-touch cache, written before the descriptor is published. */
  struct arts_rank_bitset_s ro_parity;
  _Atomic(uint32_t) cover_gate;
  unsigned int claim_rank;
  /* The requester the round in flight transfers the grant to: relaxed,
   * ordered by the baton (directory.h). */
  _Atomic(unsigned int) pending_install_owner;
#else /* WT */
#ifndef ARTS_RELEASE_PURGE
  /* The holder a revocation targets.  A home whose grants come back unasked
   * revokes no one, and its own possession is its whole directory.  Stored
   * release and loaded acquire under the baton (directory.h). */
  _Atomic(unsigned int) rw_holder;
#endif
  struct arts_home_grantreq_queue_s pending_rw; /* embedded Vyukov MPSC */
  /* The serving baton: claimed by an acq_rel CAS, given up by a release
   * store, a seq_cst fence and a re-probe (directory.h). */
  _Atomic(unsigned int) invalidate_in_flight;
  /* The RO dedup watermark (SNAPSHOT_REQUEST reply).  Not the destroy
   * roster: a rank can hold a cache the ledger never credits (a version-0
   * reply, a holder with no buffer). */
  struct arts_rank_to_u64_map_s *cached_version;
  struct arts_rank_bitset_s cached_ranks; /* destroy fan-out roster */
  /* The declared-arrival state: per requester rank the parity of its last
   * read request this home accounted for (by its arrival or a declaration of
   * it, whichever came first); the gate [td:1 | rwo:1 | roo:1 | owed:29],
   * the destroy's mark and what the home was told is still coming, every
   * write after init an acq_rel RMW; the rank whose create claimed a
   * first-touch cache, written before the descriptor is published. */
  struct arts_rank_bitset_s ro_parity;
  _Atomic(uint32_t) cover_gate;
  unsigned int claim_rank;
#ifndef ARTS_RELEASE_PURGE
  /* The requester the round in flight transfers the grant to: relaxed,
   * ordered by the baton (directory.h). */
  _Atomic(unsigned int) pending_install_owner;
#endif
#endif
#ifdef ARTS_RELEASE_PURGE
  /* Single-slot return latch.  A voluntary return is never rejected: it is
   * either accepted at once or parked here for the thread already inside the
   * home's serving tail.  Both sides publish then check and consume by CAS, so
   * exactly one of them accepts any given return.  Grants are serialized, so
   * at most one return can be outstanding and one slot suffices. */
  _Atomic(unsigned int) pending_return_from;
  /* The reply the returner waits on, owned by the latched return: written
   * before the latch CAS publishes the slot, read by the latch's one
   * consumer after its consuming CAS. */
  uint64_t pending_return_cv;
  uint64_t pending_return_version;
  uint8_t pending_return_ack;
  uint8_t pending_return_credit;
#endif
#ifdef ARTS_GPU_LAYOUT
  /* GPU staging state.  The lock exists only for an ARTS_DB_GPU block (NULL
   * otherwise) and is set before the create publishes the descriptor.  A CPU
   * EDT running with the block holds its read side for the whole body;
   * staging and the LC merges hold its write side.  The lock orders every
   * access to version and time_stamp; version is atomic only because
   * concurrent read-side holders bump it. */
  pthread_rwlock_t *gpu_lock;
  _Atomic(unsigned int) version; /* LC version */
  unsigned int time_stamp;       /* LC time stamp */
#endif
} ARTS_ALIGNED_MAX;

#ifdef __cplusplus
}
#endif

#endif /* ARTS_COHERENCE_VAL_TYPES_H */
