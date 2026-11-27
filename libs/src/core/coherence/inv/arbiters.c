/* SPDX-License-Identifier: Apache-2.0
 *
 * INV pure deciders: inv_cache_compute_next (reader plane) and the
 * invalidation round's own three, inv_round_submit_next (a release's decision
 * on round_queue), inv_round_keep_word and inv_round_targets_word (one roster
 * word of the round's snapshot).
 *
 * The two word deciders run inside a CAS-retry loop by their callers.  Purity
 * is the point: every decision a transition implies is derived from (current
 * word, op) alone, so the committing CAS and the decision it acts on can never
 * disagree — the classic split-decision race (decide on one population, act
 * on another) is unrepresentable.  The snapshot helpers decide a word's keep
 * before the exchange that writes it and its targets from the value that
 * exchange returned.
 *
 * All are identical under either read source.  None knows about ownership:
 * write ownership is the migrating sentinel grant (coherence/grant.c), which
 * lives in its own counter rather than in either word, so no decider carries
 * the write right or a writer count (the reader word's rw is the axis of
 * this rank's write request, not the right).
 *
 * engine.c #includes this file for the round's deciders, which are its own,
 * and the reader-plane decider, which the read-source units (home.c,
 * owner.c) #include it for too; a unit that compiles engine.c with its
 * read-source unit includes it twice, hence the guard.  The whitebox unit
 * tests also #include it directly.  NOT listed in CMakeLists.txt as a
 * standalone TU.
 *
 * Every decider is ARTS_ALWAYS_INLINE; a case of the reader-plane decider
 * changes only the fields its transition moves, in place in the word, and a
 * refused transition (LATE) leaves the word as it is.
 */
#if defined(ARTS_VALIDITY_INV) && !defined(ARTS_COHERENCE_INV_ARBITERS_C)
#define ARTS_COHERENCE_INV_ARBITERS_C

#include "arts/coherence/inv/types.h"

#include <stdbool.h>
#include <stdint.h>

/* ===== inv_cache_compute_next ===============================================
 *
 * The reader plane.  A copy is durable: once ro reaches VALID it survives
 * reader release, so a covering acquire is a pure load — no message, no CAS,
 * no version compare.  A copy dies exactly one way: an INVALIDATE arrives.
 *
 * At most one fetch is open per rank (the inflight flag), and the parked chain
 * belongs to it: parking IS the acquire's own decision CAS, so a parked node
 * can never miss the fetch it joined, and the publishing CAS grabs the whole
 * chain in the same atom, so the committer owns exactly its fetch's cohort and
 * serves it as the continuation of its own transition, however late that runs.
 * Nothing but that reply serves the chain: the write axis's transitions
 * leave the read plane as they find it.
 *
 * self_idx is the caller's pre-written pool node, consumed only by the actions
 * that park it (SEND_RO / PARK), or the OPEN's token (RW_COMMIT / RW_GIVE).
 */
static ARTS_ALWAYS_INLINE uint64_t inv_cache_compute_next(
    uint64_t cur, int op, uint32_t self_idx, uint32_t *out_action) {
  uint32_t ro = INV_CACHE_RO(cur);
  uint32_t inflight = INV_CACHE_INFLIGHT(cur);
  uint32_t rw = INV_CACHE_RW(cur);
  uint32_t stub = INV_CACHE_STUB(cur);
  uint64_t next = cur;
  uint32_t act = INV_CACHE_ACT_NONE;

  switch (op) {
  case INV_CACHE_OP_ACQ_RO:
    if (ro == INV_RO_VALID) {
      act = INV_CACHE_ACT_SELF_SERVE; /* the wait-free path */
      break;
    }
    /* No covering copy: join the open fetch if there is one, else open it,
     * whatever the write axis holds.  A kill-marked fetch is joinable too:
     * its doomed reply still serves the chain once before the reserved purge
     * retires the copy. */
    if (inflight != 0u) {
      next = (cur & ~INV_CACHE_HEAD_FIELD) |
             (((uint64_t)self_idx & INV_CACHE_HEAD_MASK)
              << INV_CACHE_HEAD_RO_SHIFT);
      act = INV_CACHE_ACT_PARK;
      break;
    }
    next =
        (cur & ~(INV_CACHE_RO_FIELD | INV_CACHE_HEAD_FIELD)) |
        INV_CACHE_RO_AT(INV_RO_REQ) | INV_CACHE_INFLIGHT_BIT |
        (((uint64_t)self_idx & INV_CACHE_HEAD_MASK) << INV_CACHE_HEAD_RO_SHIFT);
    act = INV_CACHE_ACT_SEND_RO;
    break;

  case INV_CACHE_OP_DELIVER:
    /* A reply landed.  Every fetch has exactly one answer, this reply, so a
     * word with no fetch open is a reply for no request: the caller raises
     * it. */
    if (inflight == 0u) {
      act = INV_CACHE_ACT_LATE;
      break;
    }
    /* The committer takes the whole chain, and the copy turns VALID. */
    next = (cur & ~(INV_CACHE_INFLIGHT_BIT | INV_CACHE_HEAD_FIELD)) |
           INV_CACHE_RO_AT(INV_RO_VALID);
    /* An answer proves the home installed, so this cache can no longer be
     * claimed; the stub bit clears unless a writer holds an OPEN (whose
     * token is the bit). */
    if (rw != INV_RW_OPEN) {
      next &= ~INV_CACHE_STUB_BIT;
    }
    if (ro == INV_RO_REQ_KILL) {
      /* An INVALIDATE marked this fetch mid-flight.  Serve the cohort that
       * joined it — they are entitled to these bytes — and only then run the
       * reserved purge and send the ack this rank owes the round. */
      act = INV_CACHE_ACT_PUBLISH_KILL;
      break;
    }
    act = INV_CACHE_ACT_PUBLISH;
    break;

  case INV_CACHE_OP_INVALIDATE:
    if (ro == INV_RO_VALID) {
      next = cur & ~INV_CACHE_RO_FIELD; /* ro = IDLE */
      act = INV_CACHE_ACT_PURGE_ACK;
      break;
    }
    if (ro == INV_RO_REQ) {
      /* Mark the in-flight fetch; its reply serves the cohort once, then
       * purges and fires the ack this transition owes the round. */
      next = (cur & ~INV_CACHE_RO_FIELD) | INV_CACHE_RO_AT(INV_RO_REQ_KILL);
      act = INV_CACHE_ACT_KILL_MARKED;
      break;
    }
    /* IDLE or already REQ_KILL: idempotent.  The roster over-approximates, and
     * a second invalidate on a kill-marked fetch is unreachable — the ack it
     * owes keeps that round open, and rounds serialize. */
    act = INV_CACHE_ACT_NOOP_ACK;
    break;

  case INV_CACHE_OP_KILL_PURGE:
    /* Precondition (caller-asserted): ro == VALID.  Between the kill-publish
     * and this purge nothing can retire the copy — the owed ack blocks the
     * round chain, and local activity never leaves VALID. */
    next = cur & ~INV_CACHE_RO_FIELD; /* ro = IDLE */
    act = INV_CACHE_ACT_NONE; /* the caller sends the owed ack LAST */
    break;

  case INV_CACHE_OP_RW_CLEAR:
  case INV_CACHE_OP_CLAIM:
    if (op == INV_CACHE_OP_RW_CLEAR && rw != INV_RW_SENT) {
      act = INV_CACHE_ACT_LATE;
      break;
    }
    /* The write axis only: a fetch still out keeps its chain for its own
     * reply.  A grant, like the claim, proves the cache will never be
     * claimed: the home answered, so it is installed, and had this rank been
     * the creator its claim would have come first. */
    next &= ~(INV_CACHE_RW_FIELD | INV_CACHE_STUB_BIT);
    break;

  case INV_CACHE_OP_RW_CLAIM:
    if (rw == INV_RW_IDLE) {
      next |= INV_CACHE_RW_AT(INV_RW_OPEN);
      act = INV_CACHE_ACT_WON;
    }
    break;

  case INV_CACHE_OP_RW_COMMIT:
    /* self_idx carries the OPEN's token: the stub bit its claim read. */
    if (rw == INV_RW_OPEN && stub == self_idx) {
      next = (cur & ~INV_CACHE_RW_FIELD) | INV_CACHE_RW_AT(INV_RW_SENT);
      act = INV_CACHE_ACT_WON;
    } else {
      act = INV_CACHE_ACT_LOST;
    }
    break;

  case INV_CACHE_OP_RW_GIVE:
    if (rw != INV_RW_OPEN || stub != self_idx) {
      break; /* a create's claim reset this OPEN */
    }
    next &= ~INV_CACHE_RW_FIELD;
    break;

  default:
    break;
  }
  *out_action = act;
  return next;
}

/* ===== inv_round_submit_next ===========================================
 *
 * A release's decision on round_queue, from the word it read (`h`) and its
 * entry's address (`e`, bit 0 clear): an idle word is claimed (OPEN: the
 * release now holds the round and runs its own entry), a held one gets the
 * entry pushed onto it (e|OPEN: the entry links behind the old head and the
 * holder takes it).  A push never lands on 0, so only a claim enters an idle
 * word.  The caller links the entry and CASes the result.
 */
static ARTS_ALWAYS_INLINE uintptr_t inv_round_submit_next(uintptr_t h,
                                                          uintptr_t e,
                                                          bool *claim) {
  if (h == 0u) {
    *claim = true;
    return INV_ROUND_OPEN;
  }
  *claim = false;
  return e | INV_ROUND_OPEN;
}

/* ===== inv_round_keep_word / inv_round_targets_word =======================
 *
 * One word of the round's roster snapshot (rank `base` at bit 0), decided
 * purely.  Every rank the snapshot found is a target of the round except two
 * kinds: `exempt` (the home under HOME, whose buffer is the canonical
 * copy) and the batch's releasers, whose bytes are what the round publishes.
 * A releaser is a sharer from its release on — its copy is durable and
 * nothing but a later round's INVALIDATE retires it — so the keep, which the
 * snapshot's exchange writes into the word, holds every releaser of this
 * word, whether or not the snapshot finds it: a creator's seeded copy was
 * never served, so nothing else ever registered it.  That is where the next
 * owner's first round finds the ex-holder, whether or not the home has
 * recorded that owner yet (a revoking home records it only at its CONFIRM).
 * The keep depends on the batch alone, never on the word, which is why it
 * can be the value the exchange writes.
 */
static ARTS_ALWAYS_INLINE uint64_t
inv_round_keep_word(unsigned int base, const unsigned int *writers,
                    unsigned int nwriters, unsigned int exempt) {
  uint64_t keep = 0u;
  for (unsigned int k = 0; k < nwriters; k++) {
    unsigned int w = writers[k];
    if (w != exempt && w >= base && w < base + 64u) {
      keep |= 1ull << (w - base);
    }
  }
  return keep;
}

static ARTS_ALWAYS_INLINE uint64_t inv_round_targets_word(uint64_t snap,
                                                          uint64_t keep,
                                                          unsigned int base,
                                                          unsigned int exempt) {
  uint64_t targets = snap & ~keep;
  if (exempt >= base && exempt < base + 64u) {
    targets &= ~(1ull << (exempt - base));
  }
  return targets;
}
#endif /* ARTS_VALIDITY_INV && !ARTS_COHERENCE_INV_ARBITERS_C */
