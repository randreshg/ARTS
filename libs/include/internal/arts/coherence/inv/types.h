/* SPDX-License-Identifier: Apache-2.0
 *
 * INV protocol (write-invalidate) type layouts.  Both write policies share
 * them; the build selects exactly one.
 *
 * A rank's cache carries two planes that share no word.  The reader plane is
 * one 64-bit word (cache_state, INV_CACHE_*): durable read copies, at most
 * one fetch in flight, and the chain of acquires parked on it.  The writer
 * plane is the migrating write grant every grant-bearing arm shares
 * (writer_count and the grant fields below; coherence/grant.c).
 *
 * The home adds the invalidation round, which is what makes this arm INV:
 * one directory word (dir_state, INV_DIR_*) that admits one round at a time
 * and counts its outstanding acks, the queue of releases awaiting a round,
 * and the copy roster a round snapshots.  A release returns only after the
 * round that covers it has an ack from every copy it retired.
 *
 * The write policy decides only where the canonical bytes live.  Under WT a
 * release's publish carries its payload into the home's stable buffer and
 * the home serves reads from it; under WB the bytes stay with the grant's
 * holder, the publish carries control only, and the home redirects reads to
 * the holder.  The round is the same under both.
 *
 * Invariants of the reader word (the arbiters keep them):
 *   - a fetch is open (inflight) exactly while ro is REQ or REQ_KILL, so at
 *     most one read reply is in flight per rank;
 *   - a chain is parked only on an open fetch, so a VALID copy has an empty
 *     chain and a covering acquire is a pure load;
 *   - a second INVALIDATE on a kill-marked fetch is unreachable: the ack it
 *     owes keeps the round that marked it open, and rounds serialize.
 */
#ifndef ARTS_COHERENCE_INV_TYPES_H
#define ARTS_COHERENCE_INV_TYPES_H

#include "arts/coherence/types_common.h"
#ifndef ARTS_PROTOCOL_INV
#error "inv/types.h is the INV layout; this translation unit's cell is another"
#endif
#include "arts/rank_bitset.h"
#include "arts/utils/stdatomic_compat.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── cache word: the READER plane, and only the reader plane ─────────────
 * Identical under both write policies — what differs between WT and WB is
 * where a fetch is SERVED FROM, never what the requester's word does.
 *
 *   [ ro:2 (63..62) | inflight:1 (61) | spare:43 | head_ro:18 (17..0) ]
 *
 *   ro       ∈ {IDLE, REQ, REQ_KILL, VALID}.  VALID survives reader release:
 *              a reader holding it is a pure load — no message, no CAS, no
 *              version compare.  A copy dies exactly one way: an INVALIDATE
 *              arrived.
 *   inflight = a read reply is on the wire to this rank.  Set by the CAS that
 *              issues a fetch, cleared by the CAS the reply lands in; no new
 *              fetch may open while it is set, so at most one reply is ever in
 *              flight per rank and a landing reply always belongs to the unique
 *              open fetch.
 *   head_ro  = head of the parked-reader chain, an index into the per-DB waiter
 *              pool (0 = empty; pool indices start at 1).  The head lives IN
 *              the word so parking IS the acquire's own decision CAS (a parked
 *              node can never miss the fetch it joined) and the publishing CAS
 *              grabs the whole chain in the same atom (the committer owns
 *              exactly its fetch's cohort and serves it as the continuation of
 *              its own transition, however late that runs).
 *
 * The WRITER plane is not here.  Write ownership is the migrating grant
 * (coherence/grant.c): this rank's writers and its possession of the grant
 * live in writer_count, RW waiters park on the cache's pending_rw Treiber
 * stack, and the home's rw_holder and grant queue say where the grant is and
 * who asked for it next.  The write plane lives in writer_count and the grant
 * plane, never in this word: it carries no rw state, no writer count and no
 * ownership bit.
 *
 * The install-lane arbitration stamp is not in this word either: it is the
 * buffer's own version field.  Two asynchronous installs can target one rank's
 * buffer slot (a read reply and an ownership transfer), neither can put the
 * pointer swap inside its own decision CAS, so the swap is stamp-conditional
 * (larger wins, a stale one retreats and recycles) and always ordered BEFORE
 * the publishing CAS.  Nothing reads it to decide whether a copy is still
 * valid — only the arrival of an INVALIDATE decides that.
 */
#define INV_CACHE_HEAD_BITS 18
#define INV_CACHE_ST_BITS 2
#define INV_CACHE_FLAG_BITS 1

#define INV_CACHE_HEAD_RO_SHIFT 0
#define INV_CACHE_INFLIGHT_SHIFT 61
#define INV_CACHE_RO_SHIFT (INV_CACHE_INFLIGHT_SHIFT + INV_CACHE_FLAG_BITS)

#define INV_CACHE_HEAD_MASK ((uint64_t)((1ULL << INV_CACHE_HEAD_BITS) - 1))
#define INV_CACHE_ST_MASK ((uint64_t)0x3ULL)
#define INV_CACHE_FLAG_MASK ((uint64_t)0x1ULL)

/* ro values */
#define INV_RO_IDLE 0u
#define INV_RO_REQ 1u
#define INV_RO_REQ_KILL 2u /* in-flight fetch marked by an INVALIDATE */
#define INV_RO_VALID 3u

#define INV_CACHE_RO(s)                                                        \
  ((uint32_t)(((s) >> INV_CACHE_RO_SHIFT) & INV_CACHE_ST_MASK))
#define INV_CACHE_INFLIGHT(s)                                                  \
  ((uint32_t)(((s) >> INV_CACHE_INFLIGHT_SHIFT) & INV_CACHE_FLAG_MASK))
#define INV_CACHE_HEAD_RO(s)                                                   \
  ((uint32_t)(((s) >> INV_CACHE_HEAD_RO_SHIFT) & INV_CACHE_HEAD_MASK))
#define INV_CACHE_MAKE(ro, inflight, hro)                                      \
  ((((uint64_t)(ro) & INV_CACHE_ST_MASK) << INV_CACHE_RO_SHIFT) |              \
   (((uint64_t)(inflight) & INV_CACHE_FLAG_MASK) << INV_CACHE_INFLIGHT_SHIFT) |\
   (((uint64_t)(hro) & INV_CACHE_HEAD_MASK) << INV_CACHE_HEAD_RO_SHIFT))

/* ── directory word: the invalidation round, and only the round ──────────
 * Identical under both write policies.
 *
 *   [ round_open:1 (63) | acks:14 (62..49) | spare:49 ]
 *
 *   round_open = at most one invalidation round is open at a time.
 *   acks       = outstanding INVALIDATE_ACKs of the open round.
 *
 * Who holds the write grant, who is queued for it, and whether a transfer is
 * in flight are NOT here: they are the grant plane's pending_rw /
 * invalidate_in_flight (and rw_holder where the home revokes), shared with
 * every other migrating-grant arm.
 */
#define INV_DIR_ACKS_BITS 14
#define INV_DIR_FLAG_BITS 1

#define INV_DIR_ACKS_SHIFT 49
#define INV_DIR_ROUND_SHIFT (INV_DIR_ACKS_SHIFT + INV_DIR_ACKS_BITS)

#define INV_DIR_ACKS_MASK ((uint64_t)((1ULL << INV_DIR_ACKS_BITS) - 1))
#define INV_DIR_FLAG_MASK ((uint64_t)0x1ULL)

#define INV_DIR_ROUND_OPEN(s)                                                  \
  ((uint32_t)(((s) >> INV_DIR_ROUND_SHIFT) & INV_DIR_FLAG_MASK))
#define INV_DIR_ACKS(s)                                                        \
  ((uint32_t)(((s) >> INV_DIR_ACKS_SHIFT) & INV_DIR_ACKS_MASK))
#define INV_DIR_MAKE(open, acks)                                               \
  ((((uint64_t)(open) & INV_DIR_FLAG_MASK) << INV_DIR_ROUND_SHIFT) |           \
   (((uint64_t)(acks) & INV_DIR_ACKS_MASK) << INV_DIR_ACKS_SHIFT))

static_assert(INV_CACHE_RO_SHIFT + INV_CACHE_ST_BITS == 64,
              "cache word must pack to exactly 64 bits");
static_assert(INV_DIR_ROUND_SHIFT + INV_DIR_FLAG_BITS == 64,
              "directory word must pack to exactly 64 bits");
static_assert(INV_RO_VALID <= INV_CACHE_ST_MASK,
              "reader state encodings must fit the 2-bit field");
#define INV_CACHE_PIN_ INV_CACHE_MAKE(INV_RO_REQ_KILL, 1u, 0x1ABCDu)
static_assert(INV_CACHE_RO(INV_CACHE_PIN_) == INV_RO_REQ_KILL &&
                  INV_CACHE_INFLIGHT(INV_CACHE_PIN_) == 1u &&
                  INV_CACHE_HEAD_RO(INV_CACHE_PIN_) == 0x1ABCDu,
              "cache word field round-trip");
#undef INV_CACHE_PIN_
#define INV_DIR_PIN_ INV_DIR_MAKE(1u, 0x2AAAu)
static_assert(INV_DIR_ROUND_OPEN(INV_DIR_PIN_) == 1u &&
                  INV_DIR_ACKS(INV_DIR_PIN_) == 0x2AAAu,
              "directory word field round-trip");
#undef INV_DIR_PIN_

/* ── parked waiter (chain node, index-addressed) ────────────────────────── */
/* Nodes live in a per-DB grow-only pool; the chain link and the free list
 * both use pool indices (0 = none), so a head fits the word's head field.
 * A node's fields are written by its owner before the CAS that links it and
 * read by whoever a later CAS hands it to: the committer that grabbed the
 * chain, which serves it and frees it back to the pool, or the allocator
 * that pops it. */
struct arts_db_inv_waiter_s {
  /* Pool index of the next node, 0 = end.  Written by the node's owner
   * before the CAS that links it (onto a chain or the free list), and a
   * reader that acts on it is ordered after that CAS by its own acquire
   * (the chain's grab, the pop's head load).  A pop can also read it while
   * the node's next owner rewrites it; its tagged head CAS then fails
   * (barring 2^14 head updates inside that window) and the value is
   * discarded: relaxed. */
  _Atomic(uint32_t) next;
  unsigned int slot;     /* dep slot of the parked acquire */
  arts_guid_t edt_guid;  /* parked EDT (guid-addressed idempotent serve) */
};

#define INV_WAITER_CHUNK_CAP 256u /* nodes per pool chunk (chunked growth) */
#define INV_WAITER_IDX_MAX INV_CACHE_HEAD_MASK

struct arts_db_inv_waiter_pool_s {
  /* Chunk directory (grow-only), installed once by a CAS whose release
   * carries the zeroed directory to every acquire load of it. */
  _Atomic(uintptr_t) chunks;
  /* Bump allocator over chunk capacity: only its own value. */
  _Atomic(uint32_t) next_fresh;
  /* Free list by index (0 = empty), tagged against ABA.  A free's CAS
   * releases the node's link to the pop that acquires the head naming it. */
  _Atomic(uint32_t) free_head;
};

/* ── publish-queue entry (Treiber, whole-chain drain) ───────────────────── */
/* One entry per RW release awaiting its invalidation round at the home.  Both
 * write policies use it; only `rdzv` differs — under WT a nonzero address
 * marks an entry whose payload has already landed in place in the stable
 * buffer (the commit leg's pairing gates the queue), WB leaves it zero
 * because the canonical copy never travels to the home.  A batch is drained
 * whole and is order-insensitive: at most one version stamp (the entry that
 * carries data) and a set of commuting ack wakes. */
struct arts_db_inv_pub_s {
  arts_lf_link_t link; /* FIRST — required by arts_lf_stack_t */
  uint64_t vnew;
  unsigned int releaser_rank;
  /* The releaser handed the write right back with this release.  The round
   * close is where it is accepted: not before, because the home may not hand
   * the block on while a stale copy of it still stands. */
  unsigned int returns_grant;
  uint64_t cv; /* the publish's cv: 0, since an announce is answered by the
                * PUBLISH_CTS and never queued; the ACK completes the flight */
  struct arts_rdzv_landing_s rdzv; /* addr != 0: bytes landed (WT only) */
};

/* Home request queue: same Vyukov MPSC shape as the other arms so the shared
 * home.h declarations apply (producers = request handlers, single consumer =
 * the grant-claim holder). */
struct arts_home_grantreq_node_s {
  _Atomic(struct arts_home_grantreq_node_s *) next;
  unsigned int rank;
  uint64_t have_version; /* version the requester already holds */
  struct arts_rdzv_landing_s rdzv;
};
struct arts_home_grantreq_queue_s {
  _Atomic(struct arts_home_grantreq_node_s *) tail;
  _Atomic(struct arts_home_grantreq_node_s *) head;
  struct arts_home_grantreq_node_s stub;
};

/* ── per-rank DB cache ──────────────────────────────────────────────────── */
/* The reader plane lives in cache_state (durable copies, one in-flight fetch,
 * one parked chain).  The writer plane is the migrating sentinel grant, held in
 * the fields shared with every other grant-bearing arm: writer_count carries
 * local writers plus a +1 sentinel while this rank holds the grant, so
 * writer_count > 0 IS "this rank may write".  Nothing withdraws the sentinel
 * except an INVALIDATE, which is what makes the grant sticky.  Under WB the
 * same word also carries ARTS_GRANT_UNCONFIRMED, so "may write" additionally
 * means the home has published the directory flip; the negative value it
 * produces is why the signed test is the predicate and not a convenience.
 *
 * The version axis is the buffer's own version field, bumped by each release.
 * A second, publication-numbering counter would be redundant: no home-issued
 * grant sequence exists for a local install count to race against. */
struct arts_db_cache_s {
  /* Reader plane (INV_CACHE_*).  Every modification is an acq_rel CAS from
   * an acquire load: a parker's CAS carries its node's fields to the
   * DELIVER that grabs the chain, and DELIVER's carries the install it
   * follows to every acquire that then finds the copy VALID. */
  _Atomic(uint64_t) cache_state;
  /* Local writers + the ownership sentinel.  A CAS-loop "increment if > 0"
   * is the whole RW fast path: > 0 means this rank holds the grant.  Every
   * write after the cache is published is a read-modify-write: an install's
   * release carries the buffer and the learned size to the admission that
   * sees the right held, and a writer's acq_rel decrement carries its bytes
   * and version bump to whichever decrement reaches the transfer edge. */
  _Atomic(unsigned int) writer_count;
  /* Buffers drawn from this cache and not parked back, plus the flag the
   * storage release sets once the destructor has run (encoding in buffer.c,
   * lifetime rule in types_common.h). */
  _Atomic(unsigned int) buf_holds;
  _Atomic(arts_shared_slot_t) buffer;
  arts_lockfree_pool_t buf_freelist; /* parked buffers, drawn first */
  /* Parked buffers a draw found too small: a popper must not free a node
   * another popper may still be reading, so they wait here for the storage
   * release, which frees them with the list. */
  arts_lf_stack_t buf_retired;
#ifdef ARTS_WRITE_POLICY_WT
  arts_lf_stack_t publication_waiters; /* pre-publication read holds */
#endif
  /* RW waiters parked on this rank (Treiber, order-free drain-all): every
   * waiter is woken regardless of order, so a LIFO stack suffices. */
  arts_lf_stack_t pending_rw;
  struct arts_db_inv_waiter_pool_s waiters; /* reader chain-node pool */
  /* Ownership-request coalescing: only the actor that CASes 0->1 sends
   * GRANT_REQUEST; same-node RW acquires piggyback and are picked up by
   * the transfer's drain.  A grant's take clears it with release after
   * taking the waiters, and a claim (acq_rel) sees that take.  The claim and
   * the clear each pair with the pending_rw stack through a seq_cst fence
   * (arts_db_acquire_remote_rw, arts_db_grant_take_waiters). */
  _Atomic(unsigned int) grant_req_in_flight;
#ifndef ARTS_RELEASE_PURGE
  /* Next transfer target, published by the INVALIDATE handler BEFORE it
   * withdraws the sentinel.  ARTS_NO_PENDING_OWNER = none pending.  Publishing
   * before the withdrawal is what makes a separate flag redundant: whichever
   * actor drives writer_count to exactly 0 reads this field and ships, and
   * that decrement reads the withdrawal's release sequence, so the field is
   * plain. */
  unsigned int incoming_new_owner;
  struct arts_rdzv_landing_s incoming_new_owner_rdzv;
  /* The version the pending new owner reported holding (its request's
   * have_version), published with the two fields above; the 0-edge actor
   * ships no payload when the canonical bytes are at that very version. */
  uint64_t incoming_new_owner_have;
#endif
  /* Always NULL in this arm.  INV's sharer plane deliberately carries no
   * version ledger — a dedup watermark on it would stop the plane being
   * write-driven — but the shared transfer helper reads this field to decide
   * whether a map travels with ownership, and NULL selects its empty branch. */
  struct arts_rank_to_u64_map_s *cached_version;
  arts_guid_t db_guid;
  /* The block's exact size once learned, 0 until then; relaxed throughout
   * (see the VAL layout and arts_db_cache_learn_size). */
  _Atomic(uint64_t) db_size;
  /* 1 until a buffer has been installed, 0 after: stored 0 with release
   * after the slot's install and read with acquire at the fast exit (see the
   * VAL layout). */
  _Atomic(uint8_t) payload_pending;
  /* What made this cache (arts_db_init_kind_t): a create's own descriptor,
   * the home's directory, or a dependence's first touch.  A create that
   * claims a first touch's cache turns it into its own
   * (arts_db_create_claims_stub); nothing else writes it after init.  Only
   * its own value: one CAS decides between a claim and an answer. */
  _Atomic(uint8_t) init_kind;

  /* Publish write-combining — the write-side twin of VAL's RO combining
   * window.  The flight word, its waiter stack, the parked uncovered
   * waiters and the publish credit follow the VAL write-through layout's
   * protocol exactly (see coherence/val/types.h). */
  _Atomic(unsigned int) pub_flight;
  arts_lf_stack_t pub_waiters;
  _Atomic(arts_lf_link_t *) pub_parked;
  _Atomic(uint64_t) home_pub_addr;
  _Atomic(uint64_t) home_pub_rkey;
  _Atomic(uint64_t) home_pub_txid;
#ifdef ARTS_WRITE_POLICY_WT
  /* A remote creator's name for this descriptor, carried by its announce and
   * echoed by the home's CREATE_RETURN: the create's credit is applied only
   * where the two match, never to a later descriptor of the same GUID.  0 on
   * every other cache.  Write-through only: a write-back create carries no
   * credit, so nothing names it there.
   * Minted by the creator with a release store before its announce
   * leaves (the home's stub takes the announced value at init); the
   * credit handler's acquire compare is the only reader that decides
   * anything.  It carries nothing beyond its own value. */
  _Atomic(uint64_t) create_token;
#endif
#ifdef ARTS_RELEASE_PURGE
  /* The hand-back obligation and the install count it names, as in the VAL
   * write-through layout (see coherence/val/types.h). */
  _Atomic(uint64_t) pending_grant_return;
  _Atomic(uint64_t) grant_generation;
#endif
};

/** Internal DataBlock descriptor (INV protocol).
 *
 *  The per-rank coherence cache is embedded by value as the FIRST member.
 *  Non-home ranks allocate a cache-only footprint
 *  (arts_db_cache_stub_size() = offsetof(dir_state)) that includes
 *  home_initialized and the grant plane's home fields but omits the
 *  invalidation round's.
 *
 *  The home directory is two independent planes that share no word:
 *    - the migrating write grant (pending_rw / invalidate_in_flight, plus
 *      rw_holder / pending_install_owner where the home revokes and the return
 *      latch where it does not), identical to every other grant-bearing arm
 *      and driven by coherence/grant.c;
 *    - the invalidation round (dir_state / roster / pub_queue), which is what
 *      makes this arm INV.
 *  Both write policies carry exactly these fields; the write policy decides only
 *  whether a release's publish entry carries payload. */
struct arts_db_s {
  struct arts_db_cache_s cache; /**< FIRST — coherence state. */
  arts_db_types_t db_type;
  bool home_initialized;

  /* ---- migrating write grant (home side; see coherence/grant.c) ---- */
#ifndef ARTS_RELEASE_PURGE
  /* The holder a revocation targets; a home whose grants come back unasked
   * revokes no one, and its own possession is its whole directory. */
  _Atomic(unsigned int) rw_holder;
#endif
  struct arts_home_grantreq_queue_s pending_rw; /* Vyukov MPSC, pop-one */
  _Atomic(unsigned int) invalidate_in_flight;   /* transfer-round baton */
#ifndef ARTS_RELEASE_PURGE
  unsigned int pending_install_owner; /* baton-holder-written transfer target */
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

  /* ---- invalidation round (home side; INV's own) ---- */
  /* round_open + acks (INV_DIR_*).  Every modification is an acq_rel CAS
   * from an acquire load, so each one reads the whole chain before it: the
   * ARM carries round_entries and the claim holder's round to the ack that
   * closes it, and the CLOSE carries the closer's clearing of round_entries
   * to the next CLAIM.  A producer's queue push and the closer's CLOSE are
   * each followed by a seq_cst fence before the other's word is read
   * (inv_home_round_try_open, inv_home_round_close). */
  _Atomic(uint64_t) dir_state;
  /* The home's canonical publication axis, advanced to each round's highest
   * published version.  It is NOT the installed buffer's version, and must not
   * be: a home-resident owner writes THROUGH the home buffer in place, so its
   * releases change the bytes without ever swapping an install — the buffer's
   * own lane stands still while the data moves on.  Serving readers off that
   * lane hands two different rounds the same stamp, and the receiver's
   * version-conditional install then rejects the newer copy while its state
   * machine still marks it valid: a reader left on bytes from a write window
   * that has already closed.  Advanced only under the round claim, with a
   * release store made after the batch's bytes are in place; a serve loads
   * it with acquire at any time, so the bytes it then sends are at least the
   * ones the stamp names.  (Under WB the home holds no bytes and the field
   * idles — there the owner's own buffer version IS the axis, because
   * ownership migration carries it.) */
  _Atomic(uint64_t) hver;
  arts_lf_stack_t pub_queue; /* releases awaiting a round */
  /* The open round's drained batch.  Written by the claim holder before its
   * ARM and by whichever actor closes the round (the ack whose ACK_DEC
   * reached zero, or the claim holder itself when the round has no target),
   * so every access is ordered by the dir_state chain or by program order:
   * plain. */
  struct arts_db_inv_pub_s *round_entries;
  /* Copy roster (the round's targets).  A read serve's fetch_or and the
   * round's snapshot exchange pair on the word itself: the later of the two
   * reads the other, so either the round names the reader or the serve is
   * ordered after the snapshot and the stamp made before it. */
  struct arts_rank_bitset_s roster;
  struct arts_rank_bitset_s cached_ranks; /* destroy fan-out roster */

#ifdef ARTS_USE_GPU
  /* GPU staging fields (full arts_db_s alloc; unused on the CPU INV path). */
  volatile unsigned int reader;
  volatile unsigned int writer;
  volatile unsigned int version;
  unsigned int time_stamp;
#endif
} ARTS_ALIGNED_MAX;

/* ── pure arbiters (CAS-retry loop bodies; see arbiters.c) ──────────────── */
/* Both arbiters are pure functions of one word, run inside a CAS-retry loop,
 * and are identical under both write policies: the reader word decides fetch /
 * park / publish / invalidate, and the directory word decides round mutual
 * exclusion and ack accounting.  Neither knows anything about ownership —
 * that is the grant plane's, and it does not live in a word. */

/* arg: ACQ_RO takes the parking node's pool index; the rest ignore it. */
#define INV_CACHE_OP_ACQ_RO 0
#define INV_CACHE_OP_DELIVER 1    /* a read reply landed (publish attempt) */
#define INV_CACHE_OP_INVALIDATE 2
#define INV_CACHE_OP_KILL_PURGE 3 /* reserved invalidate after the kill serve */

#define INV_CACHE_ACT_NONE 0
#define INV_CACHE_ACT_SELF_SERVE 1   /* covering VALID copy: pure load, run */
#define INV_CACHE_ACT_SEND_RO 2      /* opened the fetch: park + send */
#define INV_CACHE_ACT_PARK 3         /* chained onto the in-flight fetch */
#define INV_CACHE_ACT_PUBLISH 4      /* installed: serve the grabbed chain */
#define INV_CACHE_ACT_PUBLISH_KILL 5 /* + reserved purge + owed ack after */
#define INV_CACHE_ACT_DROP 6         /* superseded reply: discard */
#define INV_CACHE_ACT_PURGE_ACK 7    /* INVALIDATE on VALID: purged, ack */
#define INV_CACHE_ACT_KILL_MARKED 8  /* INVALIDATE on REQ: ack owed */
#define INV_CACHE_ACT_NOOP_ACK 9     /* INVALIDATE idempotent: ack */

uint64_t inv_cache_compute_next(uint64_t cur, int op, uint32_t self_idx,
                                uint32_t *out_action);

#define INV_DIR_OP_ROUND_CLAIM 0 /* open an invalidation round */
#define INV_DIR_OP_ACKS_ARM 1    /* under the claim: arm the ack count */
#define INV_DIR_OP_ACK_DEC 2
#define INV_DIR_OP_ROUND_CLOSE 3

#define INV_DIR_ACT_NONE 0
#define INV_DIR_ACT_CLAIMED 1
#define INV_DIR_ACT_CLOSE 2 /* acks reached 0 with the round open */

uint64_t inv_dir_compute_next(uint64_t cur, int op, unsigned int arg,
                              uint32_t *out_action);

/* The round's roster snapshot, one word at a time: who this round retires and
 * which bits stay (the batch's releasers remain sharers).  Pure; see
 * arbiters.c. */
unsigned int inv_round_snapshot_word(uint64_t snap, unsigned int base,
                                     const unsigned int *writers,
                                     unsigned int nwriters,
                                     unsigned int exempt,
                                     unsigned int *targets, unsigned int cap,
                                     unsigned int ntargets, uint64_t *keep);

/* ── shared machinery the write-policy TUs call (inv/engine.c) ───────────── */
/* Reader chain-node pool (see struct arts_db_inv_waiter_s). */
uint32_t inv_waiter_alloc(struct arts_db_cache_s *c);
void inv_waiter_free(struct arts_db_cache_s *c, uint32_t idx);
struct arts_db_inv_waiter_s *inv_waiter_ptr(struct arts_db_cache_s *c,
                                            uint32_t idx);
/* Wake a grabbed chain: the committer's continuation, however late it runs. */
void inv_serve_chain(struct arts_db_cache_s *cache, uint32_t head);
/* Drive the invalidation round: called by every producer (a publish push, a
 * close re-arm).  A claim that raced past the work it saw closes empty. */
void inv_home_round_try_open(struct arts_db_s *db);

#ifdef __cplusplus
}
#endif

#endif /* ARTS_COHERENCE_INV_TYPES_H */
