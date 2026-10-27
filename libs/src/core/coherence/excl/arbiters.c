/* SPDX-License-Identifier: Apache-2.0
 *
 * EXCL protocol pure state-transition arbiters, one pair per release policy:
 * PURGE's excl_compute_next (home lock_state) and cache_compute_next (cache
 * word), RETAIN's lock_owner_compute_next and cache_owner_compute_next.  The
 * two policies' words have different layouts, so each pair is compiled only
 * under its own policy.
 *
 * purge.c and retain.c #include this file to get their private copy of their
 * policy's arbiters.  The whitebox unit test (tests/unit/excl_compute_next.c)
 * also #includes this file directly for standalone testing of the pure
 * functions.
 *
 * NOT listed in CMakeLists.txt as a standalone TU.
 */
#ifdef ARTS_RELEASE_PURGE

#include "arts/coherence/excl/types.h"

#include <stdint.h>

/* ===== excl_compute_next ================================================
 *
 * Home-side lock-state arbiter.  Pure function of the current packed
 * lock_state word and an op code; returns the next state word and sets
 * *out_grant to one of the LOCK_GRANT_* codes.
 *
 * op codes: EXCL_OP_RW_ACQ / EXCL_OP_RO_ACQ / EXCL_OP_RW_REL / EXCL_OP_RO_REL
 * grant codes: LOCK_GRANT_NONE / LOCK_GRANT_ONE_RW / LOCK_GRANT_ALL_RO
 */
static uint64_t excl_compute_next(uint64_t cur, int op, uint32_t *out_grant) {
  uint32_t w = EXCL_STATE_W(cur);
  uint32_t r = EXCL_STATE_R(cur);
  uint32_t bit = EXCL_STATE_BIT(cur);
  uint32_t td = EXCL_STATE_TEARDOWN(cur);
  uint32_t rwo = EXCL_STATE_RWO(cur);
  uint32_t roo = EXCL_STATE_ROO(cur);
  uint32_t owed = EXCL_STATE_OWED(cur);
  uint32_t grant = LOCK_GRANT_NONE;
  /* Torn down (marked, nothing counted, nothing owed): an acquire arriving now
   * was either still pending at the destroy, which the contract leaves
   * unserved, or a read request its rank's write turn covered that was still
   * inside its handler when that turn ended and the destroy tore the block
   * down; neither is owed a grant, and a grant would hand out a block whose
   * teardown has run.  It is neither counted nor granted; a refused request's
   * queued node, RW or RO, goes with the descriptor. */
  if (td && w == 0 && r == 0 && owed == 0 &&
      (op == EXCL_OP_RW_ACQ || op == EXCL_OP_RO_ACQ)) {
    *out_grant = LOCK_GRANT_NONE;
    return cur;
  }
  switch (op) {
  case EXCL_OP_RW_ACQ:
    /* w+1.  none->rw (w==0 && r==0) grants one RW.  A NEW RW participant
     * (w:0->1) arriving while readers hold the lock (w==0 && r>0) means the RO
     * phase is held and this RW parks → state_bit=RO.  If this rank is ALREADY
     * an RW participant (w>0, the RW phase is in progress, rw->rw) the held
     * writer serves it: state_bit MUST be left unchanged — flipping it to RO
     * here would corrupt the live RW phase into an RO phase.  Hence the guard
     * is `w == 0 && r > 0`, NOT `r > 0`. */
    if (w == 0 && r == 0) {
      grant = LOCK_GRANT_ONE_RW; /* none -> rw */
    } else if (w == 0 && r > 0) {
      bit = EXCL_PHASE_BIT_RO; /* RO held (w was 0), RW waits */
    }
    /* else w>0 (rw->rw): bit unchanged, no grant — the held writer serves it.
     */
    w += 1;
    break;
  case EXCL_OP_RO_ACQ:
    /* r+1.  none->ro / ro->ro drains all RO (D7: new RO grants immediately even
     * with RW waiting); if w>0 (RW held) the RO parks (state_bit=RW). */
    if (w == 0) {
      grant = LOCK_GRANT_ALL_RO; /* none->ro or ro->ro */
      bit = EXCL_PHASE_BIT_RO;
    } else if (bit == EXCL_PHASE_BIT_RO && r > 0) {
      grant = LOCK_GRANT_ALL_RO; /* RO phase extends (D7) */
    } else {
      bit = EXCL_PHASE_BIT_RW; /* RW held, RO waits */
    }
    r += 1;
    break;
  case EXCL_OP_RW_REL:
    /* w-1.  w-1>0 grants next writer (D6); w-1==0 && r>0 flips to RO + drains;
     * w-1==0 && r==0 -> none. */
    w -= 1;
    if (w > 0) {
      grant = LOCK_GRANT_ONE_RW; /* rw->rw */
    } else if (r > 0) {
      grant = LOCK_GRANT_ALL_RO; /* rw->ro */
      bit = EXCL_PHASE_BIT_RO;
    }
    break;
  case EXCL_OP_RO_REL:
    /* r-1.  r-1==0 && w>0 flips to RW + grants one; else nothing. */
    r -= 1;
    if (r == 0 && w > 0) {
      grant = LOCK_GRANT_ONE_RW; /* ro->rw */
      bit = EXCL_PHASE_BIT_RW;
    }
    break;
  case EXCL_OP_TEARDOWN:
    /* Mark in the same atom that reads the counts, so a destroy and a
     * concurrent last release cannot each conclude the other will tear down.
     * Only the op that FIRST sets the mark can claim the teardown here:
     * duplicate destroys are designed into the protocol. */
    if (td == 0u) {
      td = 1u;
      if (w == 0 && r == 0 && owed == 0) {
        grant = LOCK_GRANT_DESTROY; /* lock already free — tear down now */
      }
    }
    break;
  case EXCL_OP_OWE:
    /* A declaration: one more request is coming.  Precedes the parity step of
     * the declaration, so an arrival that reads that step finds this count. */
    if (owed == (uint32_t)EXCL_STATE_OWED_MASK) {
      *out_grant = LOCK_GRANT_FULL;
      return cur;
    }
    owed += 1;
    break;
  case EXCL_OP_ARRIVE:
  case EXCL_OP_RWO_ARRIVE:
  case EXCL_OP_ROO_ARRIVE:
    /* Precondition (caller-checked): owed > 0, and rwo / roo for the
     * pre-claim arrivals. */
    owed -= 1;
    if (op == EXCL_OP_RWO_ARRIVE) {
      rwo = 0u;
    } else if (op == EXCL_OP_ROO_ARRIVE) {
      roo = 0u;
    }
    if (td && w == 0 && r == 0 && owed == 0) {
      grant = LOCK_GRANT_DESTROY;
    }
    break;
  default:
    break;
  }
  /* A release that LANDS on the zero edge under an existing mark is the other
   * teardown point.  The two are mutually exclusive: the destroy claims it only
   * when both counts are already zero, and only one CAS can carry a counter to
   * zero.  No grant can be owed at that instant, so setting `grant` here
   * discards nothing. */
  if (td && (op == EXCL_OP_RW_REL || op == EXCL_OP_RO_REL) && w == 0 &&
      r == 0 && owed == 0) {
    grant = LOCK_GRANT_DESTROY;
  }
  /* Normalize state_bit when one counter hit 0 (state_bit only meaningful with
   * both > 0). */
  if (w == 0 || r == 0) {
    bit = 0;
  }
  *out_grant = grant;
  return LOCK_MAKE_STATE_FULL(td, bit, rwo, roo, owed, w, r);
}

/* ===== cache_compute_next ==============================================
 * Cache-side analogue of excl_compute_next: pure function of the current
 * cache_state word, run inside a CAS-retry loop.  Returns the next word and
 * (via out_action) what the caller must do AFTER the CAS commits.
 *
 * The ACQ_* ops carry the count++ INSIDE the CAS, together with the
 * request/join decision.  That single-atom shape is load-bearing twice over:
 *   - No stale decision: the decision is computed in the same atom that
 *     counts the acquire, so it can never be derived from a word state that
 *     postdates the acquire's own service and release.  (A decision made
 *     after a separately-counted acquire was already served and released
 *     would read post-round IDLE and open a request for a dead acquire —
 *     whose grant no release edge would ever return.)
 *   - Exact grant populations: a phase's counts return to zero at every
 *     0-edge, and an arrival under a held grant joins WITHOUT parking
 *     (SELF_SERVE), so the counts a GRANT_* transition observes are exactly
 *     the parked-waiter population its committer must serve — making the
 *     grant committer the sole drainer, with nothing to serve stale.
 * The REL_* ops carry the count-- inside the CAS, since the decrement and
 * the 0-edge state change must be atomic together. */
static uint64_t cache_compute_next(uint64_t cur, int op, uint32_t *out_action) {
  uint32_t rws = CACHE_RW_ST(cur);
  uint32_t ros = CACHE_RO_ST(cur);
  uint32_t par = CACHE_RO_PAR(cur);
  uint32_t stub = CACHE_STUB(cur);
  uint32_t wc = CACHE_RW_CNT(cur);
  uint32_t rc = CACHE_RO_CNT(cur);
  uint32_t act = CACHE_ACT_NONE;
  switch (op) {
  case CACHE_OP_ACQ_RW:
    if (wc == (uint32_t)CACHE_RW_CNT_MASK) {
      act = CACHE_ACT_LATE; /* the count field is full: the caller raises */
      break;
    }
    wc += 1;
    if (rws == CACHE_ST_GRANT) {
      act = CACHE_ACT_SELF_SERVE; /* join the held RW phase directly */
    } else if (rws == CACHE_ST_IDLE) {
      /* First writer of the round: request home at once, whatever the read
       * axis holds (a read request still out is declared by this one: the
       * caller reads it from this transition's pre-image). */
      rws = CACHE_ST_REQ;
      act = CACHE_ACT_SEND_RW;
    } else {
      act = CACHE_ACT_PARK; /* covered by the in-flight request */
    }
    break;
  case CACHE_OP_ACQ_RO:
    if (rc == (uint32_t)CACHE_CNT_MASK) {
      act = CACHE_ACT_LATE;
      break;
    }
    rc += 1;
    if (rws == CACHE_ST_GRANT || ros == CACHE_ST_GRANT) {
      act = CACHE_ACT_SELF_SERVE; /* RW⊇RO local join, or RO phase held */
    } else if (rws == CACHE_ST_IDLE && ros == CACHE_ST_IDLE) {
      ros = CACHE_ST_REQ;
      par ^= 1u; /* the request this transition opens */
      act = CACHE_ACT_SEND_RO;
    } else {
      /* Covered by the in-flight request: the write request's grant serves
       * readers too (RW ⊇ RO, so no read request is opened under one), and a
       * read request's answer serves every reader parked on the block. */
      act = CACHE_ACT_PARK;
    }
    break;
  case CACHE_OP_GRANT_RW:
    /* One grant answers one request: the axis is at REQ with its opener
     * counted (a write request covered by the create's hold is absorbed at
     * the home, never answered).  Anything else is a grant for no request. */
    if (rws != CACHE_ST_REQ || wc == 0u) {
      act = CACHE_ACT_LATE;
      break;
    }
    rws = CACHE_ST_GRANT;
    stub = 0u; /* any reply: the home is installed and the claim elsewhere */
    act = CACHE_ACT_DRAIN_BOTH; /* RW grant serves this rank's RW + RO cohort */
    break;
  case CACHE_OP_GRANT_RO:
    /* The read request stays out until its own answer, so the axis is at
     * REQ.  The home opens no read phase for this rank while its write turn
     * is out, so a write grant is not held here.  The readers it counted are
     * still parked: a write turn that covers them is this rank's own write
     * request, which the home answers in place of this grant (it dismisses
     * the queued read request), so a read grant always finds them. */
    if (ros != CACHE_ST_REQ || rws == CACHE_ST_GRANT || rc == 0) {
      act = CACHE_ACT_LATE;
      break;
    }
    ros = CACHE_ST_GRANT;
    stub = 0u;
    act = CACHE_ACT_DRAIN_RO;
    break;
  case CACHE_OP_CTS_RO:
    /* A size round or a dismiss answers the read request.  Re-sent only for
     * readers that still wait on it and ride no write request; otherwise the
     * axis closes: a covered request needs no second leg, and readers under a
     * write request are that request's to serve. */
    if (ros != CACHE_ST_REQ) {
      act = CACHE_ACT_LATE;
      break;
    }
    stub = 0u;
    if (rc > 0 && rws == CACHE_ST_IDLE) {
      par ^= 1u;
      act = CACHE_ACT_RESEND_RO;
    } else {
      ros = CACHE_ST_IDLE;
    }
    break;
  case CACHE_OP_CREATE_HOLD:
    /* A create records its own hold in a cache this rank made when it first
     * touched the block.  The hold IS a local RW grant, so it serves the same
     * cohorts a granted RW phase serves, readers included (RW ⊇ RO).  The
     * requests this rank had sent are the claim's: the home discards them
     * unanswered (the caller reads them from the pre-image for the announce),
     * so both axes close here.  A grant on either axis is the one
     * refusal — the block exists elsewhere, and nothing here may take a turn
     * over it. */
    if (rws == CACHE_ST_GRANT || ros == CACHE_ST_GRANT) {
      break; /* act stays NONE: this create records no hold */
    }
    if (wc == (uint32_t)CACHE_RW_CNT_MASK) {
      act = CACHE_ACT_LATE; /* the count field is full: the caller raises */
      break;
    }
    rws = CACHE_ST_GRANT;
    /* A read request still out is the claim's too: the home discards it
     * unanswered (the announce owes it), and its readers are the hold's. */
    ros = CACHE_ST_IDLE;
    stub = 0u;
    wc += 1;
    act = CACHE_ACT_DRAIN_BOTH;
    break;
  case CACHE_OP_DECLAIM:
    if (rws != CACHE_ST_REQ) {
      act = CACHE_ACT_LATE;
      break;
    }
    stub = 0u;
    break;
  case CACHE_OP_REL_RW: /* precond: rws==GRANT */
    wc -= 1;
    if (wc == 0 && rc == 0) {
      rws = CACHE_ST_IDLE;
      act = CACHE_ACT_REL_RW; /* last holder of the RW grant → publish */
    }
    break;
  case CACHE_OP_REL_RO:
    rc -= 1;
    if (rws == CACHE_ST_GRANT && wc == 0 && rc == 0) {
      rws = CACHE_ST_IDLE;
      act = CACHE_ACT_REL_RW; /* last RO joiner under an RW grant → publish */
    } else if (ros == CACHE_ST_GRANT && rc == 0) {
      ros = CACHE_ST_IDLE;
      act = CACHE_ACT_REL_RO; /* last holder of the RO grant → notify */
    }
    break;
  default:
    break;
  }
  *out_action = act;
  if (act == CACHE_ACT_LATE) {
    return cur;
  }
  return CACHE_MAKE_X(rws, ros, par, stub, wc, rc);
}
#endif /* ARTS_RELEASE_PURGE */

#ifdef ARTS_RELEASE_RETAIN

#include "arts/coherence/excl/types.h"

#include <stdbool.h>
#include <stdint.h>

/* Whether a read acquire on this word is served here at once: a write grant
 * covers it (RW⊇RO), a read grant (marked for return or not) serves it, or
 * the canonical bytes are resident. */
static inline bool cache_owner_serves_ro(uint64_t s) {
  uint32_t ros = CACHE_RO_ST(s);
  return CACHE_RW_ST(s) == CACHE_ST_GRANT || ros == CACHE_ST_GRANT ||
         ros == CACHE_ST_GRANT_PURGE || CACHE_OWNER(s) == 1u;
}

/* The waiters parked on rw_pending / ro_pending, read from a word.  A writer
 * parks only while the write request is out, and a reader only while nothing
 * here serves it; each axis leaves its serving states only at a zero count
 * (a migration, the serve's relinquish and a read grant's return all wait for
 * it).  So a count is either all parked waiters or all live holders, and a
 * transition that starts serving owes exactly the parked count its pre-image
 * shows, counted waiters whose push has not landed included. */
static inline uint32_t cache_owner_parked_rw(uint64_t s) {
  return (CACHE_RW_ST(s) == CACHE_ST_REQ) ? CACHE_RW_CNT(s) : 0u;
}

static inline uint32_t cache_owner_parked_ro(uint64_t s) {
  return cache_owner_serves_ro(s) ? 0u : CACHE_RO_CNT(s);
}

/* ===== cache_owner_compute_next =========================================
 * Cache arbiter — pure function of the single cache_state word, run inside a
 * CAS-retry loop.  The cache_state packs owner-bit + rw_st/ro_st +
 * migrate_target + wc + rc into one word, so every transition (the acquire's
 * count++ and the RW-release 0-edge migration decision included) is a single
 * CAS.
 *
 * The word keeps two facts apart:
 *   owner-bit  = the canonical bytes are RESIDENT here.  Gates serving and
 *                migration, and nothing else.
 *   rw_st/ro_st = the PERMISSION this rank holds.  A write is granted locally
 *                only on rw_st==GRANT; residency alone is not enough, because
 *                during the home's read phase the bytes are still here while
 *                the permission has been given up.
 *
 * That is also the whole difference between the two release policies.  The
 * state machine is the same as PURGE's; PURGE relinquishes VOLUNTARILY at the
 * count zero edge, RETAIN holds until another node asks — a recall, or a grant
 * the home issued pre-marked (ro_st==GRANT_PURGE) because a writer was already
 * queued.  A read acquire still keys off the owner-bit alone: reading one's own
 * resident bytes is safe, since rw_st==GRANT implies the owner-bit and
 * ownership is unique, so no other rank can be writing.
 *
 * FORWARD-migrate and FORWARD-serve do NOT pass through this arbiter — their
 * target rank is not derivable from the word, so the FORWARD handler installs
 * migrate_target / pushes ro_serve itself.
 *
 * The ACQ_* ops carry the count++ and the request/join decision in one CAS,
 * as under PURGE.  A decision made in a later atom than the count could read
 * a word from after the acquire had been drained by another thread's grant,
 * run, released and seen its grant migrate away, and open a request no EDT
 * waits for.  An arrival that a grant or the resident copy serves joins
 * without parking (SELF_SERVE), and every transition that starts serving an
 * axis drains its waiters, so a writer parks only while the write request is
 * out and a reader only while nothing here serves it: the waiters a
 * transition owes are a function of its pre-image (cache_owner_parked_rw/_ro
 * above).  The REL_* ops carry the count-- inside the CAS, because the
 * decrement and the 0-edge transition must be atomic together.
 */
static uint64_t cache_owner_compute_next(uint64_t cur, int op,
                                         uint32_t *out_action) {
  uint32_t own = CACHE_OWNER(cur);
  uint32_t rws = CACHE_RW_ST(cur);
  uint32_t ros = CACHE_RO_ST(cur);
  uint32_t mt = CACHE_MIGRATE_TARGET(cur);
  uint32_t wc = CACHE_RW_CNT(cur);
  uint32_t rc = CACHE_RO_CNT(cur);
  uint64_t sticky = cur & CACHE_OWNER_STICKY_MASK;
  uint32_t act = CACHE_ACT_NONE;
  switch (op) {
  case CACHE_OP_ACQ_RW:
    if (wc == (uint32_t)CACHE_OWNER_WC_MASK) {
      act = CACHE_ACT_LATE; /* the count field is full: the caller raises */
      break;
    }
    wc += 1;
    if (rws == CACHE_ST_GRANT) {
      act = CACHE_ACT_SELF_SERVE; /* join the held RW grant */
    } else if (rws == CACHE_ST_REQ) {
      act = CACHE_ACT_PARK; /* covered by the in-flight RW request */
    } else {
      /* WRITE PERMISSION lives in rw_st, not in the owner bit: own==1 means
       * only "the canonical bytes are resident here", which
       * gates serving and migration.  The two are different facts — during the
       * home's read phase the bytes are still here while the permission has
       * been relinquished — and conflating them lets this rank write while a
       * remote rank still holds a readable copy.  A rank that relinquished is
       * an ordinary requester and takes the same path as any node holding only
       * a read grant.  A read request still out is declared by this one: the
       * caller reads it from this transition's pre-image. */
      rws = CACHE_ST_REQ;
      act = CACHE_ACT_SEND_RW; /* request migration from home */
    }
    break;
  case CACHE_OP_ACQ_RO:
    if (rc == (uint32_t)CACHE_OWNER_RC_MASK) {
      act = CACHE_ACT_LATE;
      break;
    }
    rc += 1;
    /* A write grant covers reads (RW⊇RO), so an in-flight RW request already
     * covers this reader and it must NOT send an RO request of its own: the home
     * would count an r that is later served through the RW grant and therefore
     * never RO_RETURNed, stranding r above zero and deadlocking the RO phase.
     *
     * GRANT_PURGE is a grant — it merely owes a return at the zero edge.
     * Leaving it out of the serving test drops the acquire into the park arm
     * with no request in flight to ever wake it, and its count then never
     * falls, so the home's r sticks and the waiting writer hangs too.
     *
     * A local hit on the resident copy keys off the owner-bit ALONE and must
     * not touch ro_st.  ro_st tracks this rank's home-visible RO request;
     * promoting it to GRANT here would erase an outstanding REQ, so a later
     * REL_RO would clear the word and a second RO request would go out —
     * double-counting the home's r against a single RO_RETURN.  It would also
     * leave ro_st==GRANT behind after this rank migrates away, and the serving
     * test would then read stale local bytes. */
    if (cache_owner_serves_ro(cur)) {
      act = CACHE_ACT_SELF_SERVE; /* RW⊇RO join, a read grant, or resident */
    } else if (rws == CACHE_ST_IDLE && ros == CACHE_ST_IDLE) {
      ros = CACHE_ST_REQ;
      sticky ^= CACHE_OWNER_RO_PAR_BIT; /* the request this transition opens */
      act = CACHE_ACT_SEND_RO; /* fresh RO REQUEST to home */
    } else {
      /* rw_st==REQ (covered by the in-flight RW request, served via RW⊇RO) or
       * ro_st==REQ (coalesce onto the in-flight RO request) → park. */
      act = CACHE_ACT_PARK;
    }
    break;
  case CACHE_OP_CREATE_HOLD:
    /* A create records its own hold in a cache this rank made when it first
     * touched the block.  The hold IS a local RW grant, so it serves the same
     * cohorts a granted RW phase serves, readers included (RW ⊇ RO); the
     * requests this rank had already sent are the home's to discard (both
     * axes close).  Refused where the block is already here or on its way: the
     * bytes resident, a grant on either axis (a read grant marked for return
     * is still a grant), or a migration pending. */
    if (own != 0u || rws == CACHE_ST_GRANT || ros == CACHE_ST_GRANT ||
        ros == CACHE_ST_GRANT_PURGE || mt != ARTS_EXCL_NO_TARGET) {
      break; /* act stays NONE: this create records no hold */
    }
    if (wc == (uint32_t)CACHE_OWNER_WC_MASK) {
      act = CACHE_ACT_LATE; /* the count field is full: the caller raises */
      break;
    }
    own = 1u;
    rws = CACHE_ST_GRANT;
    /* A read request still out is the claim's too: the home discards it
     * unanswered (the announce owes it), and its readers are the hold's. */
    ros = CACHE_ST_IDLE;
    sticky &= ~CACHE_OWNER_STUB_BIT;
    wc += 1;
    act = CACHE_ACT_DRAIN_BOTH;
    break;
  case CACHE_OP_CTS_RO:
    /* A size round or a dismiss answers the read request: re-sent only for
     * readers that still wait on it and ride no write request, else the axis
     * closes.  Any reply clears the stub bit (the home is installed and the
     * claim elsewhere). */
    if (ros != CACHE_ST_REQ) {
      act = CACHE_ACT_LATE;
      break;
    }
    sticky &= ~CACHE_OWNER_STUB_BIT;
    if (rc > 0u && rws == CACHE_ST_IDLE && own == 0u) {
      sticky ^= CACHE_OWNER_RO_PAR_BIT;
      act = CACHE_ACT_RESEND_RO;
    } else {
      ros = CACHE_ST_IDLE;
    }
    break;
  case CACHE_OP_REL_RW: /* precond: rws==GRANT, own==1 */
    wc -= 1;
    /* The grant ends only when BOTH counts reach zero: readers that joined
     * under it (RW⊇RO) may outlive the last writer, so wc==0 with rc>0 keeps the
     * grant and the migration ships at the rc 0-edge instead. */
    if (wc == 0u && rc == 0u) {
      if (mt != ARTS_EXCL_NO_TARGET) {
        rws = CACHE_ST_IDLE; /* the permission leaves with the bytes */
        /* 0-edge with a pending migration: clear owner-bit + migrate_target in
         * the SAME next-state (single CAS) and signal the ship.  This is the
         * EXCL RETAIN analogue of VAL's 0-edge transfer, but a single CAS — no
         * split-decrement, no separate in-flight flag. */
        own = 0u;
        mt = ARTS_EXCL_NO_TARGET;
        act = CACHE_ACT_MIGRATE;
      } else {
        /* Sticky: keep the data AND the write permission (rw_st stays GRANT).
         * This is what RETAIN means on this axis — the permission is given up
         * only when another node asks, never merely because the local writers
         * finished.  Clearing rw_st here is PURGE's rule. */
        act = CACHE_ACT_NONE;
      }
    }
    break;
  case CACHE_OP_REL_RO:
    rc -= 1;
    if (rc == 0u) {
      /* The two axes are INDEPENDENT here.  Under RETAIN a rank can hold a
       * write permission (rw_st==GRANT, taken locally) and a read grant from
       * the home at the same time, so an if/else-if between them lets the RO
       * side be swallowed: ro_st would stay GRANT_PURGE, no RO_RETURN would go
       * out, the home's r would stick and the waiting writer would hang.
       *
       * Neither axis relinquishes merely because the count hit zero — that is
       * PURGE's rule.  Only a grant already marked to return voluntarily
       * (GRANT_PURGE, set when the home issued it pre-marked or when a recall
       * arrived while readers were live) gives itself back here. */
      if (ros == CACHE_ST_GRANT_PURGE) {
        ros = CACHE_ST_IDLE;
      }
      act = CACHE_ACT_REL_RO; /* the edge; the caller derives any send */
    }
    break;
  default:
    break;
  }
  *out_action = act;
  /* Preserve the ro_granted flag (spare bit 63 — see CACHE_GRANTED) across every
   * transition EXCEPT the RO release 0-edge, which fully relinquishes the RO
   * grant and clears it.  The flag is SET by the DELIVER(RO) handler when a
   * lent RO copy is actually received (a borrower owes exactly one RO_RETURN) —
   * NOT at request time, and NEVER for an owner serving its own readers locally
   * (RW⊇RO / fast-path: the owner holds the data, it borrowed nothing).
   * CACHE_MAKE_FULL produces bit63==0, so clearing is the default; we OR the
   * bit back only while the grant is still (partially) held. */
  if (act == CACHE_ACT_LATE) {
    *out_action = act;
    return cur;
  }
  uint64_t next = CACHE_MAKE_FULL(own, rws, ros, mt, wc, rc) | sticky;
  /* ro_granted = "this rank owes the home exactly one RO_RETURN".  It is
   * cleared by the ro_st -> IDLE TRANSITION, not by the rc zero edge: under
   * RETAIN rc oscillates 0<->1 while the grant is retained, so clearing on the
   * edge would drop the token on the first release and the eventual
   * recall-driven return would never be sent — the home's r would stick. */
  if (!(CACHE_RO_ST(cur) != CACHE_ST_IDLE && ros == CACHE_ST_IDLE)) {
    next |= (cur & CACHE_OWNER_LEASED_MASK);
  }
  return next;
}

/* ===== lock_owner_compute_next ==========================================
 * Home arbiter — pure function of the single lock_state word
 * [phase:2 | owner:14 | w:24 | r:24], run inside a CAS-retry loop.  This IS the
 * home phase-transition table.
 *
 * The transitions:
 *   RW_REQ  : w++; phase==IDLE → phase=RW, action=MIGRATE.
 *             (phase==RW: just queue, the next migrate fires at CONFIRM;
 *             phase==RO: hold, r untouched)
 *   RO_REQ  : r++; phase==RO → SERVE_ONE; phase==IDLE → phase=RO + SERVE_ONE.
 *             (phase==RW: held in ro_waiters by the caller, no action)
 *   CONFIRM : w--, owner=new_owner (ONE CAS); w>0 → MIGRATE (RW chain);
 *             elif r>0 → phase=RO + SERVE_ALL (RW→RO flip); else phase=IDLE.
 *   RO_RET  : r--; on r==0: w>0 → phase=RW + MIGRATE (RO→RW flip);
 *             else phase=IDLE.
 *
 * phase==RW means a migration is in flight and phase==RO that read grants are
 * being served; no separate flag carries either — everything lives in the
 * word.
 *
 * The FORWARD recipient is LOCK_OWNER() of the returned word.  The migrate
 * target / served reader(s) are resolved by the caller from
 * rw_waiters/ro_waiters. */
static uint64_t lock_owner_compute_next(uint64_t cur, int op,
                                        unsigned int new_owner,
                                        uint32_t *out_action) {
  uint32_t phase = LOCK_PHASE(cur);
  uint32_t owner = LOCK_OWNER(cur);
  uint32_t w = LOCK_W(cur);
  uint32_t r = LOCK_R(cur);
  uint32_t action = EXCL_ACTION_NONE;
  switch (op) {
  case EXCL_OP_RW_ACQ:
    w += 1;
    if (phase == EXCL_PHASE_IDLE) {
      phase = EXCL_PHASE_RW;
      action = EXCL_ACTION_FORWARD_MIGRATE;
    }
    /* phase==RW: queue only (next migrate at CONFIRM).
     * phase==RO: hold (r unaffected here — the RW parks in rw_waiters). */
    break;
  case EXCL_OP_RO_ACQ:
    r += 1;
    if (phase == EXCL_PHASE_RO) {
      action = EXCL_ACTION_FORWARD_SERVE_ONE;
    } else if (phase == EXCL_PHASE_IDLE) {
      phase = EXCL_PHASE_RO;
      action = EXCL_ACTION_FORWARD_SERVE_ONE;
    }
    /* phase==RW: held in ro_waiters by the caller; no action. */
    break;
  case EXCL_OP_CONFIRM:
    w -= 1;
    owner = new_owner;
    if (w > 0u) {
      action = EXCL_ACTION_FORWARD_MIGRATE; /* RW chain */
    } else if (r > 0u) {
      phase = EXCL_PHASE_RO;
      action = EXCL_ACTION_FORWARD_SERVE_ALL; /* RW → RO flip */
    } else {
      phase = EXCL_PHASE_IDLE;
    }
    break;
  case EXCL_OP_RO_RET:
    r -= 1;
    if (r == 0u) {
      if (w > 0u) {
        phase = EXCL_PHASE_RW;
        action = EXCL_ACTION_FORWARD_MIGRATE; /* RO → RW flip */
      } else {
        phase = EXCL_PHASE_IDLE;
      }
    }
    break;
  default:
    break;
  }
  *out_action = action;
  return LOCK_MAKE(phase, owner, w, r);
}
#endif /* ARTS_RELEASE_RETAIN */
