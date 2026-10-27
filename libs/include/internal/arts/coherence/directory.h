/* SPDX-License-Identifier: Apache-2.0
 *
 * Home-side metadata helpers for the coherence protocol.
 *
 * Concurrency model:
 *   - pending_rw queue: Vyukov MPSC.  Multi-producer (any handler thread
 * enqueues on GRANT_REQUEST); single-consumer in time (the unique holder of
 * the arm's serving turn).  Progress and orders are stated at the queue's
 * declaration below.
 *   - cached_version map: per-slot atomic.  Each rank slot is an independent
 *     _Atomic(uint64_t) accessed via atomic load/store and CAS-loop
 * monotonic-max for advance.  No cross-slot invariant.
 *   - rw_holder and pending_install_owner (where the home revokes),
 *     grant_baton: _Atomic.
 *     Their protocols are stated below, with the grant plane's home words.
 */

#ifndef ARTS_COHERENCE_DIRECTORY_H
#define ARTS_COHERENCE_DIRECTORY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arts/coherence/coherence.h"
/* The rank bit-set and its functions, which the home-side callers of this
 * header use. */
#include "arts/rank_bitset.h"

/* The home's ownership-request FIFO (struct arts_home_grantreq_queue_s, in
 * coherence/types_common.h; the operations are linked by every arm but FLUSH,
 * whose home keeps none).  A Vyukov MPSC queue over an embedded permanent stub.
 *
 * Progress.  A producer is wait-free: the node is allocated and filled before
 * its one exchange, so the window between the exchange and the link store
 * holds no allocation or syscall.  pop and peek (EXCL's consumer) are not
 * lock-free: one that finds head->next NULL with head != tail waits out
 * exactly one producer's window between its exchange and its link store, and
 * makes no progress until that producer is scheduled.  pop_linked and
 * peek_linked (the grant plane's consumer) never wait: they report no front,
 * and since the grant plane's producers claim the baton only after their
 * link store, the holder's put-down either meets that claim and looks again,
 * or the producer's own claim finds the baton free; under PURGE a producer
 * that read possession clear claims nothing, and the accept that sets
 * possession again looks behind it.
 *
 * Orders:
 *   tail  producers exchange it acq_rel: the release publishes the node's
 *         own NULL link, and the acquire orders the previous node's NULL
 *         link before this producer's store into it.  The consumer and the
 *         pending probe load it with acquire: head != tail names a queued
 *         requester even before its link store.
 *   next  a producer's release store of prev->next publishes its node and
 *         the rank, landing and version written into it; the consumer's
 *         acquire load of head->next takes them.
 *   head  written only by the one consumer, the holder of the arm's serving
 *         turn, with a release store after it has copied the popped node
 *         out; every load is acquire.  The turn's hand-off already orders
 *         successive consumers; the pair repeats that order, so a consumer
 *         frees a node only after the previous one's reads of it.
 * Init and teardown run with no other user and are relaxed. */
void arts_home_grantreq_queue_init(struct arts_home_grantreq_queue_s *q);
void arts_home_grantreq_queue_destroy(struct arts_home_grantreq_queue_s *q);
/* Push a requester with its transfer landing (NULL rdzv = zero landing —
 * a data-less / sentinel round) and the version of the copy it already holds
 * (ARTS_GRANT_VERSION_NONE when it holds none), which travels with it so a
 * server can decide at serve time whether any bytes need to move. */
void arts_home_grantreq_queue_push(struct arts_home_grantreq_queue_s *q,
                                  unsigned int rank,
                                  const struct arts_rdzv_landing_s *rdzv,
                                  uint64_t have_version);
/* Pop the front requester (single consumer).  Returns true and sets *out_rank
 * (+ *out_rdzv / *out_have when non-NULL) on success; false when empty.  Waits
 * out a producer caught between its exchange and its link store. */
bool arts_home_grantreq_queue_pop(struct arts_home_grantreq_queue_s *q,
                                 unsigned int *out_rank,
                                 struct arts_rdzv_landing_s *out_rdzv,
                                 uint64_t *out_have);
/* Peek the front (oldest) requester without popping. Single consumer (the
 * turn holder). Returns true + sets *out_rank (+ *out_rdzv / *out_have when
 * non-NULL) when non-empty; waits as pop does. */
bool arts_home_grantreq_queue_peek(const struct arts_home_grantreq_queue_s *q,
                                  unsigned int *out_rank,
                                  struct arts_rdzv_landing_s *out_rdzv,
                                  uint64_t *out_have);
/* pop and peek of a front whose producer has linked it; false, at once, when
 * there is none, including a front caught between its producer's exchange and
 * link store (and every node behind it).  Single consumer, as above. */
bool arts_home_grantreq_queue_pop_linked(struct arts_home_grantreq_queue_s *q,
                                         unsigned int *out_rank,
                                         struct arts_rdzv_landing_s *out_rdzv,
                                         uint64_t *out_have);
bool arts_home_grantreq_queue_peek_linked(
    const struct arts_home_grantreq_queue_s *q, unsigned int *out_rank,
    struct arts_rdzv_landing_s *out_rdzv, uint64_t *out_have);
bool arts_home_grantreq_queue_empty(const struct arts_home_grantreq_queue_s *q);
/* "Is anything queued", safe without the consumer turn: two field loads, no
 * node dereference.  head != tail names a queued requester even before its
 * link store.  head == tail can also be read across a pop and a reuse of the
 * popped node's address; its one caller, the PURGE home's idle edge, is
 * unaffected: a pop that frees the node the head load named serves the node
 * after it, and a producer that reuses the address either claims the baton or
 * is covered by an accept (coherence/grant_purge.c). */
bool arts_home_grantreq_queue_pending(
    const struct arts_home_grantreq_queue_s *q);

/*--- cached_version dense map ----------------------------------------*/

struct arts_rank_to_u64_map_s {
  /* Sized to nranks; each slot is an independent atomic: the newest version
   * its rank is known to hold.  Only its own value: a dedup decision reads
   * it, and no data is read on the strength of a slot. */
  _Atomic(uint64_t) *slots;
  unsigned int nranks;
};

struct arts_rank_to_u64_map_s *arts_rank_u64_map_create(unsigned int nranks);
void arts_rank_u64_map_destroy(struct arts_rank_to_u64_map_s *m);
uint64_t arts_rank_u64_map_get(const struct arts_rank_to_u64_map_s *m,
                               unsigned int rank);
void arts_rank_u64_map_set(struct arts_rank_to_u64_map_s *m, unsigned int rank,
                           uint64_t value);
/* Monotonic max update — sets m[rank] = max(m[rank], value) via CAS-loop;
 * returns true if the slot advanced (i.e. value was strictly greater than the
 * old slot).  Safe for concurrent callers on the same slot: loses are
 * harmless because a higher value will have won the CAS. */
bool arts_rank_u64_map_advance(struct arts_rank_to_u64_map_s *m,
                               unsigned int rank, uint64_t value);

/*--- home-directory lifecycle (inlined in arts_db_s) --------------------*/

/* The grant plane's home words, declared by each arm that has them:
 *   grant_baton  the home's serving baton, a claim counter: 0 is free, and
 *     k >= 1 is held with k - 1 claims made since the hold began.  Every
 *     write after init is an acq_rel RMW.  A claimant publishes first (its
 *     FIFO push, its latched return, the home's idle edge) and then claims
 *     with fetch_add; the one that reads 0 holds the baton.  The holder puts
 *     it down only by a CAS from the count it last looked behind: 1 for the
 *     claim that won, the value an acquire load returned before the look for
 *     a holder that inherits the baton through a round's messages, the value
 *     a failed put-down returned otherwise.  A CAS that fails hands it the
 *     newer count, with the publication of every claim in it (acquire failure
 *     order), and it looks again still holding.  So a claim and a put-down
 *     are same-word paired: the later of the two in the baton's modification
 *     order reads the earlier, and the baton is never put down over a claim
 *     nobody looked behind.  The release half carries the holder's writes
 *     (the FIFO's head, pending_install_owner, rw_holder) to the next holder,
 *     and the hold passes through a round's messages where the home revokes.
 *     64 bits: a hold lasts as long as claims keep arriving.  Loads outside
 *     the baton (the quiescence walk) carry only its value.
 *   rw_holder  the write holder a revocation targets.  Stored with release
 *     under the baton, by the confirm that flips it; loaded with acquire
 *     under the baton.  Loads outside it (a read redirect, the destroy, the
 *     quiescence walk) carry only its value.
 *   pending_install_owner  the requester the round in flight transfers the
 *     grant to.  Stored by the baton holder when it pops that requester,
 *     loaded by the confirm that ends the round.  Both relaxed, ordered by
 *     the baton: within a round its messages carry the store to the
 *     confirm, and the baton's put-down and the next winning claim order one
 *     hold's accesses before the next hold's.  The destroy's load outside the
 *     baton carries only its value: it names one more rank to tell. */

/* Initialize the home-directory fields inlined in struct arts_db_s (no separate
 * allocation).  rw_holder is the block's initial holder, chosen by the caller
 * (typically creator_rank under PROP_NONE, or self_rank under NO_ACQUIRE); a
 * revoking home names it in its rw_holder field. */
void arts_db_home_init(struct arts_db_s *db, unsigned int rw_holder,
                       unsigned int nranks);
/* Record what a create's announce said the claimed first-touch cache of
 * `creator` had already asked for (ARTS_CREATE_OWED_*): those requests are
 * still coming, and the block is not torn down before they arrive.  Runs on
 * the descriptor before it is published. */
void arts_db_home_seed_owed(struct arts_db_s *db, unsigned int creator,
                            uint8_t owed);
/* Release the home-directory owned sub-resources (queues / maps) in place.
 * Does NOT free the descriptor (the fields live inside the arts_db_s). */
void arts_db_home_teardown(struct arts_db_s *db);

/*--- cached_version map serialization (defined in coherence/rank_u64_map.c) -
 *
 * Used to piggyback the owner-side dedup map onto GRANT_RESPONSE
 * messages so the new owner can continue skipping redundant SNAPSHOT_RESPONSE
 * sends without re-learning which ranks already hold a fresh copy.
 * Independent of which ownership protocol keeps the ledger, so the
 * declarations are unconditional and this header carries no coherence-model
 * preprocessor logic; an arm that keeps no versions links no ledger.
 *
 * Wire layout (in out buffer, starting at byte 0):
 *   uint32_t count;        number of non-zero (rank, version) pairs
 *   uint32_t pad;          alignment pad
 *   arts_msg_rank_version_pair_s pairs[count];
 *
 * Caller must allocate at least:
 *   sizeof(uint32_t) * 2 + nranks * sizeof(arts_msg_rank_version_pair_s)
 * bytes for the output buffer.
 *
 * Each slot is read with its own atomic load, so an advance concurrent with
 * the serialize (a redirect served while the ledger migrates) is either in
 * the image or not.  An advance the image misses only under-credits its
 * rank: the next owner then re-sends bytes that rank already holds, which is
 * always legal; no slot is ever credited above what was sent. */
size_t arts_rank_u64_map_serialize(const struct arts_rank_to_u64_map_s *m,
                                   void *out);

/* Overwrite `m`'s slots from the wire-format buffer produced by
 * arts_rank_u64_map_serialize: slots the image names take its values, the
 * rest are cleared, and the map object itself is never replaced (see the
 * definition for why the ledger is loaded in place).  `size` is the number of
 * bytes readable at `in`; nothing past them is read.  False, with `m`
 * untouched, when the image does not fit in `size`. */
bool arts_rank_u64_map_load(struct arts_rank_to_u64_map_s *m, const void *in,
                            size_t size);

#ifdef __cplusplus
}
#endif

#endif /* ARTS_COHERENCE_DIRECTORY_H */
