/* SPDX-License-Identifier: Apache-2.0
 *
 * Home-side metadata helpers for the coherence protocol.
 *
 * Concurrency model:
 *   - pending_rw queue: Vyukov MPSC.  Multi-producer (any handler thread
 * enqueues on GRANT_REQUEST); single-consumer in time (the unique actor
 * holding the invalidate_in_flight = 1 baton).  Lock-free queue ops; the
 * orders are stated at the queue's declaration below.
 *   - cached_version map: per-slot atomic.  Each rank slot is an independent
 *     _Atomic(uint64_t) accessed via atomic load/store and CAS-loop
 * monotonic-max for advance.  No cross-slot invariant.
 *   - rw_holder (where the home revokes), invalidate_in_flight: _Atomic.
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

/* Declared, not defined, here: only the protocol arms that run a home request
 * queue define the struct (arts_db_s embeds it) before including this header,
 * and only they call these entry points.  Without the declaration the tag
 * would be scoped to each parameter list below, never matching the real
 * definition.
 *
 * Orders (every arm's definition has these members and refers here):
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
struct arts_home_grantreq_queue_s;

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
 * (+ *out_rdzv / *out_have when non-NULL) on success; false when empty. */
bool arts_home_grantreq_queue_pop(struct arts_home_grantreq_queue_s *q,
                                 unsigned int *out_rank,
                                 struct arts_rdzv_landing_s *out_rdzv,
                                 uint64_t *out_have);
/* Peek the front (oldest) requester without popping. Single consumer (the
 * baton holder). Returns true + sets *out_rank (+ *out_rdzv / *out_have when
 * non-NULL) when non-empty. */
bool arts_home_grantreq_queue_peek(const struct arts_home_grantreq_queue_s *q,
                                  unsigned int *out_rank,
                                  struct arts_rdzv_landing_s *out_rdzv,
                                  uint64_t *out_have);
bool arts_home_grantreq_queue_empty(const struct arts_home_grantreq_queue_s *q);
/* Conservative "is anything queued" probe, safe to call WITHOUT the consumer
 * baton — two field loads, no node dereference.  A push linearizes on the tail
 * exchange, so head != tail already implies a queued requester even while its
 * forward link is still in flight; head == tail is exact emptiness.  The
 * dereferencing tests above read head->next, which a concurrent pop may have
 * freed, so they belong to the baton holder alone; this one is what a baton
 * RELEASE re-checks with. */
bool arts_home_grantreq_queue_pending(
    const struct arts_home_grantreq_queue_s *q);

/*--- cached_version dense map ----------------------------------------*/

struct arts_rank_to_u64_map_s {
  /* Sized to nranks; each slot is an independent atomic: the newest version
   * its rank was sent.  Only its own value: a dedup decision reads it, and
   * no data is read on the strength of a slot. */
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
 *   invalidate_in_flight  the home's serving baton.  Claimed 0 -> 1 by an
 *     acq_rel CAS.  Released by a release store, then a seq_cst fence and a
 *     re-probe of what a claimant may have left: the request FIFO, and
 *     where grants come back unasked also the home's possession and the
 *     return latch.  The fence pairs with the one each claimant passes
 *     between its publication (a FIFO push, a latched return, a home
 *     writer's idle edge) and its baton CAS.  The release carries the
 *     holder's directory writes (pending_install_owner, rw_holder) to the
 *     next claimant.  Loads outside the baton (the destroy, the quiescence
 *     walk) carry only its value.
 *   rw_holder  the write holder a revocation targets.  Stored with release
 *     under the baton, by the confirm that flips it; loaded with acquire
 *     under the baton.  Loads outside it (a read redirect, the destroy, the
 *     quiescence walk) carry only its value. */

/* Initialize the home-directory fields inlined in struct arts_db_s (no separate
 * allocation).  rw_holder is the block's initial holder, chosen by the caller
 * (typically creator_rank under PROP_NONE, or self_rank under NO_ACQUIRE); a
 * revoking home names it in its rw_holder field. */
void arts_db_home_init(struct arts_db_s *db, unsigned int rw_holder,
                       unsigned int nranks);
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
