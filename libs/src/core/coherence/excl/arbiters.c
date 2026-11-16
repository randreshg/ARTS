/* SPDX-License-Identifier: Apache-2.0
 *
 * EXCL protocol pure state-transition arbiters, one pair per retention:
 * PURGE's excl_compute_next (home lock_state) and cache_compute_next (cache
 * word), RETAIN's lock_owner_compute_next and cache_owner_compute_next.  The
 * two retentions' words have different layouts, so each pair is compiled
 * only under its own retention.
 *
 * purge.c and retain.c #include this file to get their private copy of their
 * retention's arbiters.  The whitebox unit test
 * (tests/unit/excl_compute_next.c) also #includes this file directly for
 * standalone testing of the pure functions.
 *
 * NOT listed in CMakeLists.txt as a standalone TU.
 */
#ifdef ARTS_RETENTION_PURGE

#include "arts/coherence/excl/types.h"

#include <stdint.h>

/* ===== excl_compute_next ================================================
 *
 * Home-side lock-state arbiter.  Pure function of the current packed
 * lock_state word and an op code; returns the next state word and sets
 * *out_grant to one of the LOCK_GRANT_* codes.
 *
 * op codes: EXCL_OP_RW_ACQ / EXCL_OP_RO_ACQ / EXCL_OP_RW_REL / EXCL_OP_RO_REL
 *           / EXCL_OP_TEARDOWN / EXCL_OP_RWO_ARRIVE
 * grant codes: LOCK_GRANT_NONE / LOCK_GRANT_ONE_RW / LOCK_GRANT_ALL_RO /
 *              LOCK_GRANT_DESTROY
 */
static uint64_t excl_compute_next(uint64_t cur, int op, uint32_t *out_grant) {
  uint32_t w = EXCL_STATE_W(cur);
  uint32_t r = EXCL_STATE_R(cur);
  uint32_t bit = EXCL_STATE_BIT(cur);
  uint32_t td = EXCL_STATE_TEARDOWN(cur);
  uint32_t rwo = EXCL_STATE_RWO(cur);
  uint32_t grant = LOCK_GRANT_NONE;
  /* Torn down (marked, nothing counted, nothing awaited): an acquire arriving
   * now was still pending at the destroy, which the contract leaves
   * unserved; a grant would hand out a block whose teardown has run.  It is
   * neither counted nor granted, and the caller answers nothing: a refused
   * write request's queued node goes with the descriptor, a refused read
   * request's is freed by its handler. */
  if (td && w == 0 && r == 0 && rwo == 0 &&
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
      if (w == 0 && r == 0 && rwo == 0) {
        grant = LOCK_GRANT_DESTROY; /* lock already free — tear down now */
      }
    }
    break;
  case EXCL_OP_RWO_ARRIVE:
    /* Precondition (caller-checked): rwo is set. */
    rwo = 0u;
    if (td && w == 0 && r == 0) {
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
      r == 0 && rwo == 0) {
    grant = LOCK_GRANT_DESTROY;
  }
  /* Normalize state_bit when one counter hit 0 (state_bit only meaningful with
   * both > 0). */
  if (w == 0 || r == 0) {
    bit = 0;
  }
  *out_grant = grant;
  return LOCK_MAKE_STATE_FULL(td, bit, rwo, w, r);
}

/* ===== cache_compute_next ==============================================
 * Cache-side analogue of excl_compute_next: pure function of the current
 * cache_state word, run inside a CAS-retry loop.  Returns the next word and
 * (via out_action) what the caller must do AFTER the CAS commits.  `taken`
 * is read by CACHE_OP_GRANT_RO alone: the count its committer took from the
 * cache's ro_aside before the CAS.
 *
 * The ACQ_* ops carry the count++ INSIDE the CAS, together with the
 * request/join decision.  That single-atom shape is load-bearing twice over:
 *   - No stale decision: the decision is computed in the same atom that
 *     counts the acquire, so it can never be derived from a word state that
 *     postdates the acquire's own service and release.  (A decision made
 *     after a separately-counted acquire was already served and released
 *     would read post-round IDLE and open a request for a dead acquire —
 *     whose grant no release edge would ever return.)
 *   - Exact grant populations: an arrival under a held grant joins WITHOUT
 *     parking (SELF_SERVE), and a waiter parks only on its own axis's
 *     request, which nothing but that request's grant (for a writer, also a
 *     create's hold) answers.  So the writers a GRANT_RW or CREATE_HOLD
 *     transition counts are exactly the parked writers its committer must
 *     serve, and the readers a GRANT_RO transition counts, with the ones
 *     earlier write permission set aside (ro_aside), are exactly the parked
 *     readers: each committer is the sole drainer of its axis.
 * The read count means one thing at a time: the readers parked on the read
 * request while ro_state is REQ and no write grant is held, holders
 * otherwise.  Write permission that arrives while the read request is out
 * therefore takes the parked readers out of the count (the committer moves
 * them to ro_aside), so the write turn's zero edge reads holders alone and
 * ends without them; their request stays out and its grant returns them.
 * The REL_* ops carry the count-- inside the CAS, since the decrement and
 * the 0-edge state change must be atomic together. */
static uint64_t cache_compute_next(uint64_t cur, int op, uint32_t taken,
                                   uint32_t *out_action) {
  uint32_t rws = CACHE_RW_ST(cur);
  uint32_t ros = CACHE_RO_ST(cur);
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
       * axis holds. */
      rws = CACHE_ST_REQ;
      act = CACHE_ACT_SEND_RW;
    } else {
      act = CACHE_ACT_PARK; /* the write request is out: join it */
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
    } else if (ros == CACHE_ST_IDLE) {
      /* First reader of the round: request home at once, whatever the write
       * axis holds. */
      ros = CACHE_ST_REQ;
      act = CACHE_ACT_SEND_RO;
    } else {
      act = CACHE_ACT_PARK; /* the read request is out: join it */
    }
    break;
  case CACHE_OP_GRANT_RW:
    /* One grant answers one request: the axis is at REQ with its opener
     * counted (a write request closed by the create's hold is absorbed at
     * the home, never answered).  The home grants a write turn only once
     * every read grant came back, so no read grant is held, and readers are
     * counted only on a read request that is out.  Anything else is a grant
     * for no request. */
    if (rws != CACHE_ST_REQ || wc == 0u || ros == CACHE_ST_GRANT ||
        (ros == CACHE_ST_IDLE && rc != 0u)) {
      act = CACHE_ACT_LATE;
      break;
    }
    rws = CACHE_ST_GRANT;
    stub = 0u; /* any reply: the home is installed and the claim elsewhere */
    rc = 0u;   /* the readers parked on the read request: the caller's */
    act = CACHE_ACT_DRAIN_RW;
    break;
  case CACHE_OP_GRANT_RO: {
    /* The read request stays out until this grant, so the axis is at REQ.
     * The home opens no read phase while a write turn is out, this rank's
     * included, so a write grant is not held here.  The request's readers
     * are all parked: the ones counted in the word and the ones write
     * permission set aside, which `taken` returns to the count.  A read
     * grant always finds some. */
    uint64_t sum = (uint64_t)rc + taken;
    if (ros != CACHE_ST_REQ || rws == CACHE_ST_GRANT || sum == 0u ||
        sum > CACHE_CNT_MASK) {
      act = CACHE_ACT_LATE;
      break;
    }
    ros = CACHE_ST_GRANT;
    stub = 0u;
    rc = (uint32_t)sum;
    act = CACHE_ACT_DRAIN_RO;
    break;
  }
  case CACHE_OP_CTS_RO:
    /* A size round answers the read request, which stays out: its readers
     * wait for its grant, whatever the write axis holds. */
    if (ros != CACHE_ST_REQ) {
      act = CACHE_ACT_LATE;
      break;
    }
    stub = 0u;
    act = CACHE_ACT_RESEND_RO;
    break;
  case CACHE_OP_CREATE_HOLD:
    /* A create records its own hold in a cache this rank made when it first
     * touched the block.  The hold IS a local RW grant, so it serves the
     * writers a granted RW phase serves.  The write request this rank had
     * sent is the claim's: the home discards it unanswered (the caller reads
     * it from the pre-image for the announce), so the write axis closes
     * here.  The read request is the home's to grant like any other: it
     * stays out, and its parked readers leave the count as under a write
     * grant.  A grant on either axis is the one refusal — the block exists
     * elsewhere, and nothing here may take a turn over it. */
    if (rws == CACHE_ST_GRANT || ros == CACHE_ST_GRANT) {
      break; /* act stays NONE: this create records no hold */
    }
    if (wc == (uint32_t)CACHE_RW_CNT_MASK ||
        (ros == CACHE_ST_IDLE && rc != 0u)) {
      /* The count field is full, or readers are counted on no request: the
       * caller raises. */
      act = CACHE_ACT_LATE;
      break;
    }
    rws = CACHE_ST_GRANT;
    stub = 0u;
    wc += 1;
    rc = 0u;
    act = CACHE_ACT_DRAIN_RW;
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
  return CACHE_MAKE_X(rws, ros, stub, wc, rc);
}

/* The readers a write-permission transition (CACHE_ACT_DRAIN_RW) took out of
 * the word's read count, read from its pre-image: its committer adds them to
 * the cache's ro_aside. */
static inline uint32_t cache_set_aside(uint64_t pre) {
  return CACHE_RO_ST(pre) == CACHE_ST_REQ ? CACHE_RO_CNT(pre) : 0u;
}
#endif /* ARTS_RETENTION_PURGE */

#ifdef ARTS_RETENTION_RETAIN

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
 * parks only while the write request is out, and a reader only while
 * nothing here serves it; the write axis leaves GRANT only at a zero count
 * (a migration and the serve's relinquish wait for it), and a read grant
 * returns only at one.  A transition that starts serving writes drains the
 * parked writers; one that starts serving reads while the read request is
 * out (a migration's DELIVER, a create's hold) takes the parked readers out
 * of the count, which ro_aside keeps until the request's answer.  So a count
 * is either all parked waiters or all live holders, and a transition that
 * starts serving owes the parked count its pre-image shows, counted waiters
 * whose push has not landed included. */
static inline uint32_t cache_owner_parked_rw(uint64_t s) {
  return (CACHE_RW_ST(s) == CACHE_ST_REQ) ? CACHE_RW_CNT(s) : 0u;
}

static inline uint32_t cache_owner_parked_ro(uint64_t s) {
  return cache_owner_serves_ro(s) ? 0u : CACHE_RO_CNT(s);
}

/* ===== cache_owner_compute_next =========================================
 * Cache arbiter — pure function of the single cache_state word (and, for a
 * read request's answer, of the count its committer took from the cache's
 * ro_aside before the CAS), run inside a CAS-retry loop.  The cache_state
 * packs owner-bit + rw_st/ro_st + migrate_target + wc + rc into one word, so
 * every transition (the acquire's count++ and the RW-release 0-edge migration
 * decision included) is a single CAS.
 *
 * The word keeps two facts apart:
 *   owner-bit  = the canonical bytes are RESIDENT here.  Gates serving and
 *                migration, and nothing else.
 *   rw_st/ro_st = the PERMISSION this rank holds.  A write is granted locally
 *                only on rw_st==GRANT; residency alone is not enough, because
 *                during the home's read phase the bytes are still here while
 *                the permission has been given up.
 *
 * That is also the whole difference between the two retentions.  The
 * state machine is the same as PURGE's; PURGE relinquishes VOLUNTARILY at the
 * count zero edge, RETAIN holds until another node asks — a recall, or a grant
 * the home issued pre-marked (ro_st==GRANT_PURGE) because a writer was already
 * queued.  A read acquire still keys off the owner-bit alone: reading one's own
 * resident bytes is safe, since rw_st==GRANT implies the owner-bit and
 * ownership is unique, so no other rank can be writing.
 *
 * The forwards that serve another rank or migrate the block do NOT pass
 * through this arbiter — their target rank is not derivable from the word, so
 * the FORWARD handler installs migrate_target / pushes ro_serve itself.
 *
 * The ACQ_* ops carry the count++ and the request/join decision in one CAS,
 * as under PURGE.  A decision made in a later atom than the count could read
 * a word from after the acquire had been drained by another thread's grant,
 * run, released and seen its grant migrate away, and open a request no EDT
 * waits for.  An arrival that a grant or the resident copy serves joins
 * without parking (SELF_SERVE), and a waiter parks only on its own axis's
 * request, which only that request's answer serves (for a writer, also a
 * create's hold): the waiters a transition owes are a function of its
 * pre-image (cache_owner_parked_rw/_ro above, with the readers the cache set
 * aside for a read answer).  The REL_* ops carry the count-- inside the CAS,
 * because the decrement and the 0-edge transition must be atomic together.
 */
static uint64_t cache_owner_compute_next(uint64_t cur, int op, uint32_t taken,
                                         uint32_t *out_action) {
  uint32_t own = CACHE_OWNER(cur);
  uint32_t rws = CACHE_RW_ST(cur);
  uint32_t ros = CACHE_RO_ST(cur);
  uint32_t mt = CACHE_MIGRATE_TARGET(cur);
  uint32_t wc = CACHE_RW_CNT(cur);
  uint32_t rc = CACHE_RO_CNT(cur);
  uint64_t stub_bit = cur & CACHE_OWNER_STUB_BIT;
  uint64_t granted = cur & CACHE_OWNER_LEASED_MASK;
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
      act = CACHE_ACT_PARK; /* the write request is out: join it */
    } else {
      /* WRITE PERMISSION lives in rw_st, not in the owner bit: own==1 means
       * only "the canonical bytes are resident here", which
       * gates serving and migration.  The two are different facts — during the
       * home's read phase the bytes are still here while the permission has
       * been relinquished — and conflating them lets this rank write while a
       * remote rank still holds a readable copy.  A rank that relinquished is
       * an ordinary requester and takes the same path as any node holding only
       * a read grant.  The request leaves at once, whatever the read axis
       * holds. */
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
    /* GRANT_PURGE is a grant — it merely owes a return at the zero edge.
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
    } else if (ros == CACHE_ST_IDLE) {
      /* The first reader nothing serves opens the read request at once,
       * whatever the write axis holds. */
      ros = CACHE_ST_REQ;
      act = CACHE_ACT_SEND_RO;
    } else {
      act = CACHE_ACT_PARK; /* the read request is out: join it */
    }
    break;
  case CACHE_OP_GRANT_RW:
    /* A migration answers this rank's write request, its writers counted; the
     * bytes come from the owner, which is another rank, and the home migrates
     * nothing while a read grant of this rank is out.  The readers parked on
     * a read request still out stay parked on it until its own answer, and
     * leave the count for ro_aside (the caller adds them), so that at a
     * resident rank the count is holders alone. */
    if (rws != CACHE_ST_REQ || wc == 0u || own != 0u ||
        ros == CACHE_ST_GRANT || ros == CACHE_ST_GRANT_PURGE ||
        (ros == CACHE_ST_IDLE && rc != 0u)) {
      act = CACHE_ACT_LATE;
      break;
    }
    own = 1u;
    rws = CACHE_ST_GRANT;
    stub_bit &= ~CACHE_OWNER_STUB_BIT; /* any reply: the home is installed */
    rc = 0u;
    act = CACHE_ACT_DRAIN_RW;
    break;
  case CACHE_OP_GRANT_RO:
  case CACHE_OP_GRANT_RO_MARKED: {
    /* A read grant answers the read request, which stays out until it: its
     * readers are the ones counted in the word and the ones a migration or a
     * create's hold set aside, which `taken` returns.  The home counts this
     * grant in its read phase until its RO_RETURN, so no migration to this
     * rank can start while it is out, and a rank that is the owner answers
     * its own read request itself: the grant never meets the write grant or
     * the resident bytes.  It is marked for return when the home marked it or
     * a recall overtook it (the early token in ro_granted), and it always
     * owes the home its return. */
    uint64_t sum = (uint64_t)rc + taken;
    if (ros != CACHE_ST_REQ || rws == CACHE_ST_GRANT || own != 0u ||
        sum == 0u || sum > CACHE_OWNER_RC_MASK) {
      act = CACHE_ACT_LATE;
      break;
    }
    ros = (op == CACHE_OP_GRANT_RO_MARKED || granted != 0u)
              ? CACHE_ST_GRANT_PURGE
              : CACHE_ST_GRANT;
    granted = CACHE_OWNER_LEASED_MASK;
    stub_bit &= ~CACHE_OWNER_STUB_BIT;
    rc = (uint32_t)sum;
    act = CACHE_ACT_DRAIN_RO;
    break;
  }
  case CACHE_OP_OWN_RO: {
    /* The home forwarded this rank's own read request to the owner it names,
     * this rank: the request is answered here.  Its readers are all set
     * aside (at a resident rank the count is holders alone), `taken` returns
     * them to the count as holders under the owner bit, and the read axis
     * closes; the caller returns the home's count of the request.  The write
     * permission is untouched. */
    uint64_t sum = (uint64_t)rc + taken;
    if (ros != CACHE_ST_REQ || own != 1u || taken == 0u ||
        sum > CACHE_OWNER_RC_MASK) {
      act = CACHE_ACT_LATE;
      break;
    }
    ros = CACHE_ST_IDLE;
    stub_bit &= ~CACHE_OWNER_STUB_BIT;
    rc = (uint32_t)sum;
    act = CACHE_ACT_DRAIN_RO;
    break;
  }
  case CACHE_OP_CREATE_HOLD:
    /* A create records its own hold in a cache this rank made when it first
     * touched the block.  The hold IS a local RW grant and makes the bytes
     * resident, so it serves the writers a migration serves; the write
     * request this rank had sent is the claim's, which the home discards
     * unanswered (the caller reads it from the pre-image for the announce),
     * so the write axis closes here.  The read request is the home's to
     * answer like any other: it stays out, and its parked readers leave the
     * count as at a migration.  Refused where the block is already here or on
     * its way: the bytes resident, a grant on either axis (a read grant
     * marked for return is still a grant), or a migration pending. */
    if (own != 0u || rws == CACHE_ST_GRANT || ros == CACHE_ST_GRANT ||
        ros == CACHE_ST_GRANT_PURGE || mt != ARTS_EXCL_NO_TARGET) {
      break; /* act stays NONE: this create records no hold */
    }
    if (wc == (uint32_t)CACHE_OWNER_WC_MASK ||
        (ros == CACHE_ST_IDLE && rc != 0u)) {
      /* The count field is full, or readers are counted on no request: the
       * caller raises. */
      act = CACHE_ACT_LATE;
      break;
    }
    own = 1u;
    rws = CACHE_ST_GRANT;
    stub_bit &= ~CACHE_OWNER_STUB_BIT;
    wc += 1;
    rc = 0u;
    act = CACHE_ACT_DRAIN_RW;
    break;
  case CACHE_OP_CTS_RO:
    /* A size round answers the read request, which stays out: it is re-sent
     * with a landing for its readers, whatever else this rank holds.  Any
     * reply clears the stub bit (the home is installed and the claim
     * elsewhere). */
    if (ros != CACHE_ST_REQ) {
      act = CACHE_ACT_LATE;
      break;
    }
    stub_bit &= ~CACHE_OWNER_STUB_BIT;
    act = CACHE_ACT_RESEND_RO;
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
         * EXCL·RETAIN analogue of VAL's 0-edge transfer, but a single CAS — no
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
  if (act == CACHE_ACT_LATE) {
    return cur;
  }
  /* ro_granted = "this rank owes the home exactly one RO_RETURN" (and, while
   * the read request is out, the early-recall token).  A read grant sets it;
   * the ro_st -> IDLE TRANSITION clears it, not the rc zero edge: under
   * RETAIN rc oscillates 0<->1 while the grant is retained, so clearing on
   * the edge would drop the token on the first release and the eventual
   * recall-driven return would never be sent — the home's r would stick.
   * An owner serving its own readers (RW⊇RO, the resident copy) borrowed
   * nothing and never sets it. */
  if (CACHE_RO_ST(cur) != CACHE_ST_IDLE && ros == CACHE_ST_IDLE) {
    granted = 0u;
  }
  return CACHE_MAKE_FULL(own, rws, ros, mt, wc, rc) | stub_bit | granted;
}

/* The readers a transition that makes this rank serve reads with its read
 * request out (CACHE_ACT_DRAIN_RW: a migration's DELIVER, a create's hold)
 * took out of the word's read count, read from its pre-image: its committer
 * adds them to the cache's ro_aside. */
static inline uint32_t cache_set_aside(uint64_t pre) {
  return CACHE_RO_ST(pre) == CACHE_ST_REQ ? CACHE_RO_CNT(pre) : 0u;
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
#endif /* ARTS_RETENTION_RETAIN */
