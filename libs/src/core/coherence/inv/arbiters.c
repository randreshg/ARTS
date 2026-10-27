/* SPDX-License-Identifier: Apache-2.0
 *
 * INV pure state-transition arbiters: arts_inv_cache_compute_next (reader
 * plane) and inv_dir_compute_next (invalidation round).
 *
 * Two pure functions of one packed 64-bit word each, both run inside a
 * CAS-retry loop by their callers.  Purity is the point: every decision a
 * transition implies is derived from (current word, op) alone, so the
 * committing CAS and the decision it acts on can never disagree — the classic
 * split-decision race (decide on one population, act on another) is
 * unrepresentable.
 *
 * Both are identical under either write policy.  Neither knows about ownership:
 * write ownership is the migrating sentinel grant (coherence/grant.c), which
 * lives in its own counter rather than in either word, so neither arbiter
 * carries rw state or a writer count.
 *
 * engine.c #includes this file: the round's deciders are its own, and the
 * reader-plane decider is the one the write-policy units call too.  The
 * whitebox unit tests also #include it directly.  NOT listed in
 * CMakeLists.txt as a standalone TU.
 */
#ifdef ARTS_PROTOCOL_INV

#include "arts/coherence/inv/types.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

/* ===== arts_inv_cache_compute_next ==========================================
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
 *
 * self_idx is the caller's pre-written pool node, consumed only by the actions
 * that park it (SEND_RO / PARK).
 */
uint64_t arts_inv_cache_compute_next(uint64_t cur, int op, uint32_t self_idx,
                                     uint32_t *out_action) {
  uint32_t ro = INV_CACHE_RO(cur);
  uint32_t inflight = INV_CACHE_INFLIGHT(cur);
  uint32_t hro = INV_CACHE_HEAD_RO(cur);
  uint32_t rw = INV_CACHE_RW(cur);
  uint32_t par = INV_CACHE_PAR(cur);
  uint32_t stub = INV_CACHE_STUB(cur);
  uint32_t act = INV_CACHE_ACT_NONE;

  switch (op) {
  case INV_CACHE_OP_ACQ_RO:
    if (ro == INV_RO_VALID) {
      act = INV_CACHE_ACT_SELF_SERVE; /* the wait-free path */
      break;
    }
    /* No covering copy.  A write request out covers this reader (RW ⊇ RO):
     * it opens no read request of its own and the write turn serves it.
     * Otherwise join the open fetch if there is one, else open it.  A
     * kill-marked fetch is joinable too: its doomed reply still serves the
     * chain once before the reserved purge retires the copy. */
    if (rw != INV_RW_IDLE || inflight != 0u) {
      hro = self_idx;
      act = INV_CACHE_ACT_PARK;
      break;
    }
    ro = INV_RO_REQ;
    inflight = 1u;
    par ^= 1u;
    hro = self_idx;
    act = INV_CACHE_ACT_SEND_RO;
    break;

  case INV_CACHE_OP_DELIVER:
    /* A reply landed.  Every fetch has exactly one answer (this reply or a
     * size round), so a word with no fetch open is a reply for no request:
     * the caller raises it.  A fetch a write turn covered stays open until
     * this reply: the chain it takes is whoever parked after that turn. */
    if (inflight == 0u) {
      act = INV_CACHE_ACT_LATE;
      break;
    }
    inflight = 0u;
    hro = 0u; /* the committer takes the whole chain */
    /* An answer proves the home installed, so this cache can no longer be
     * claimed; the stub bit clears unless a writer holds an OPEN (whose
     * token is the bit). */
    if (rw != INV_RW_OPEN) {
      stub = 0u;
    }
    if (ro == INV_RO_REQ_KILL) {
      /* An INVALIDATE marked this fetch mid-flight.  Serve the cohort that
       * joined it — they are entitled to these bytes — and only then run the
       * reserved purge and send the ack this rank owes the round. */
      ro = INV_RO_VALID;
      act = INV_CACHE_ACT_PUBLISH_KILL;
      break;
    }
    ro = INV_RO_VALID;
    act = INV_CACHE_ACT_PUBLISH;
    break;

  case INV_CACHE_OP_INVALIDATE:
    if (ro == INV_RO_VALID) {
      ro = INV_RO_IDLE;
      act = INV_CACHE_ACT_PURGE_ACK;
      break;
    }
    if (ro == INV_RO_REQ) {
      /* Mark the in-flight fetch; its reply serves the cohort once, then
       * purges and fires the ack this transition owes the round (a size
       * round or dismiss that closes the fetch sends the ack instead). */
      ro = INV_RO_REQ_KILL;
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
    ro = INV_RO_IDLE;
    act = INV_CACHE_ACT_NONE; /* the caller sends the owed ack LAST */
    break;

  case INV_CACHE_OP_CTS:
    /* A size round or a dismiss answers the fetch (self_idx: its parity,
     * which must be the open fetch's).  Re-sent only for readers that still
     * wait on it and ride no write request; otherwise the fetch closes (a
     * covered one needs no second leg; riders are the write turn's).  A fetch
     * can be kill-marked without the home ever serving it: a rank that held
     * the write permission is rostered as an ex-holder when the permission
     * moves on, so the next round invalidates it while its covered read
     * request is still out.  Closing such a fetch sends the ack it owes; a
     * re-send keeps the mark, so the re-sent fetch's reply serves once,
     * purges, and only then acks. */
    if (inflight == 0u || par != self_idx) {
      act = INV_CACHE_ACT_LATE;
      break;
    }
    if (rw != INV_RW_OPEN) {
      stub = 0u;
    }
    if (hro != 0u && rw == INV_RW_IDLE) {
      par ^= 1u;
      act = INV_CACHE_ACT_RESEND;
      break;
    }
    act = (ro == INV_RO_REQ_KILL) ? INV_CACHE_ACT_CLOSE_ACK : INV_CACHE_ACT_NONE;
    inflight = 0u;
    ro = INV_RO_IDLE;
    break;

  case INV_CACHE_OP_RW_CLEAR:
  case INV_CACHE_OP_CLAIM:
    if (op == INV_CACHE_OP_RW_CLEAR && rw != INV_RW_SENT) {
      act = INV_CACHE_ACT_LATE;
      break;
    }
    hro = 0u; /* every parked reader is served under the permission */
    rw = INV_RW_IDLE;
    if (op == INV_CACHE_OP_CLAIM && inflight != 0u) {
      /* The claim closes a fetch still out: that pre-claim request is the
       * home's to discard unanswered (the announce owes it).  No round can
       * have kill-marked it: the home is not installed before the claim. */
      inflight = 0u;
      ro = INV_RO_IDLE;
    }
    /* A grant, like the claim, proves the cache will never be claimed: the
     * home answered, so it is installed, and had this rank been the creator
     * its claim would have come first. */
    stub = 0u;
    act = INV_CACHE_ACT_SERVE;
    break;

  case INV_CACHE_OP_DECLAIM:
    /* While the write request is out no writer holds an OPEN, so no token
     * is outstanding when the stub bit clears. */
    if (rw != INV_RW_SENT) {
      act = INV_CACHE_ACT_LATE;
      break;
    }
    stub = 0u;
    break;

  case INV_CACHE_OP_RW_CLAIM:
    if (rw == INV_RW_IDLE) {
      rw = INV_RW_OPEN;
      act = INV_CACHE_ACT_WON;
    }
    break;

  case INV_CACHE_OP_RW_COMMIT:
    /* self_idx carries the OPEN's token: the stub bit its claim read. */
    if (rw == INV_RW_OPEN && stub == self_idx) {
      rw = INV_RW_SENT;
      act = INV_CACHE_ACT_WON;
    } else {
      act = INV_CACHE_ACT_LOST;
    }
    break;

  case INV_CACHE_OP_RW_GIVE:
    if (rw != INV_RW_OPEN || stub != self_idx) {
      break; /* a create's claim reset this OPEN */
    }
    rw = INV_RW_IDLE;
    if (hro != 0u && inflight == 0u && ro != INV_RO_VALID) {
      /* Riders with no write turn coming: open a fetch for them. */
      ro = INV_RO_REQ;
      inflight = 1u;
      par ^= 1u;
      act = INV_CACHE_ACT_SEND_RO;
    } else if (hro != 0u && ro == INV_RO_VALID) {
      /* Unreachable: a reader never parks on a valid copy, and every way
       * into one takes the chain.  The caller raises it. */
      act = INV_CACHE_ACT_LATE;
    }
    break;

  default:
    break;
  }
  *out_action = act;
  if (act == INV_CACHE_ACT_LATE) {
    return cur;
  }
  return INV_CACHE_WITH_REQ(INV_CACHE_MAKE(ro, inflight, hro), rw, par, stub);
}

/* ===== inv_dir_compute_next ============================================
 *
 * The invalidation round: one round at a time (the round_open claim) and one
 * outstanding-ack count for it.  A round is opened by a release, armed with the
 * size of its roster snapshot, and closed by the ack that drives the count to
 * zero.  That closer is unique, which is what makes it safe for it to wake the
 * releaser and re-open for whatever queued up meanwhile.
 *
 * arg: only ACKS_ARM reads it (the target count).
 */
static uint64_t inv_dir_compute_next(uint64_t cur, int op, unsigned int arg,
                                     uint32_t *out_action) {
  uint32_t open = INV_DIR_ROUND_OPEN(cur);
  uint32_t acks = INV_DIR_ACKS(cur);
  uint32_t act = INV_DIR_ACT_NONE;

  switch (op) {
  case INV_DIR_OP_ROUND_CLAIM:
    if (open == 0u) {
      open = 1u;
      act = INV_DIR_ACT_CLAIMED;
    } /* else: a round is already in flight; its closer re-arms (act NONE) */
    break;

  case INV_DIR_OP_ACKS_ARM:
    /* Under the claim, before the multicast: arm the outstanding-ack count for
     * this round's snapshot.  A CAS rather than a store because the word is
     * live — acks from this very round can already be arriving. */
    acks = (uint32_t)arg;
    break;

  case INV_DIR_OP_ACK_DEC:
    /* Precondition, checked by the caller, which fails loudly otherwise: the
     * round is open and acks > 0. */
    acks -= 1u;
    if (acks == 0u) {
      act = INV_DIR_ACT_CLOSE; /* this committer closes the round */
    }
    break;

  case INV_DIR_OP_ROUND_CLOSE:
    open = 0u;
    break;

  default:
    break;
  }
  *out_action = act;
  return INV_DIR_MAKE(open, acks);
}

/* ===== inv_round_snapshot_word ==========================================
 *
 * One word of the round's roster snapshot, decided purely.  `snap` is the
 * word the round took (rank `base` at bit 0).  Every rank in it is a target
 * of this round except two kinds: `exempt` (the write-through home, whose
 * buffer is the canonical copy) and the batch's releasers, whose bytes are
 * what this round publishes.  A releaser is a sharer from its release on —
 * its copy is durable and nothing but a later round's INVALIDATE retires it
 * — so every releaser of this word goes into the roster through *keep,
 * whether or not the snapshot found it: a creator's seeded copy was never
 * served, so nothing else ever registered it.  That is where the next
 * owner's first round finds the ex-holder, whether or not the home has
 * recorded that owner yet (a revoking home records it only at its CONFIRM).
 * Targets are appended after `ntargets`; the return is the new
 * count, or UINT_MAX when a target did not fit below `cap` (the caller
 * fails loudly).
 */
static unsigned int inv_round_snapshot_word(
    uint64_t snap, unsigned int base, const unsigned int *writers,
    unsigned int nwriters, unsigned int exempt, unsigned int *targets,
    unsigned int cap, unsigned int ntargets, uint64_t *keep) {
  uint64_t kept = 0u;
  for (unsigned int k = 0; k < nwriters; k++) {
    unsigned int w = writers[k];
    if (w != exempt && w >= base && w < base + 64u) {
      kept |= 1ull << (w - base);
    }
  }
  *keep = kept;
  snap &= ~kept;
  while (snap != 0u) {
    unsigned int b = (unsigned int)__builtin_ctzll(snap);
    unsigned int rank = base + b;
    snap &= snap - 1u;
    if (rank == exempt) {
      continue;
    }
    if (ntargets >= cap) {
      return UINT_MAX;
    }
    targets[ntargets++] = rank;
  }
  return ntargets;
}
#endif /* ARTS_PROTOCOL_INV */
