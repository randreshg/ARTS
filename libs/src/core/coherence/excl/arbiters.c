/* SPDX-License-Identifier: Apache-2.0
 *
 * EXCL protocol pure state-transition arbiters, one pair per retention:
 * PURGE's excl_compute_next (home lock_state) and cache_compute_next (cache
 * word), RETAIN's lock_owner_compute_next and cache_owner_compute_next.  The
 * two retentions' words have different layouts, so each set is compiled only
 * under its own retention.
 *
 * purge.c and retain.c #include this file to get their private copy of their
 * retention's arbiters.  The whitebox unit test
 * (tests/unit/excl_compute_next.c) also #includes this file directly for
 * standalone testing of the pure functions.
 *
 * Every arbiter is ARTS_ALWAYS_INLINE; a case changes only the fields its
 * transition moves, in place in the word.
 *
 * NOT listed in CMakeLists.txt as a standalone TU.
 */
#ifdef ARTS_RETENTION_PURGE

#include "arts/coherence/excl/types.h"

#include <stdbool.h>
#include <stdint.h>

/* A marked home with nothing counted: torn down. */
static ARTS_ALWAYS_INLINE bool excl_torn_down(uint64_t s) {
  return (s & (EXCL_STATE_TEARDOWN_BIT | EXCL_STATE_W_FIELD |
               EXCL_STATE_R_FIELD)) == EXCL_STATE_TEARDOWN_BIT;
}

/* ===== excl_compute_next ================================================
 *
 * Home-side lock-state arbiter.  Pure function of the current packed
 * lock_state word and an op code; returns the next state word and sets
 * *out_grant to one of the LOCK_GRANT_* codes.
 *
 * op codes: EXCL_OP_RW_ACQ / EXCL_OP_RO_ACQ / EXCL_OP_RW_REL / EXCL_OP_RO_REL
 *           / EXCL_OP_TEARDOWN
 * grant codes: LOCK_GRANT_NONE / LOCK_GRANT_ONE_RW / LOCK_GRANT_ALL_RO /
 *              LOCK_GRANT_DESTROY
 *
 * state_bit is meaningful only with both counts above zero, so every
 * transition leaves it clear when one of them is zero.  w and r never pass
 * their fields, and a release finds its count above zero, the caller's
 * precondition, which no run inside the contract breaks; a count moves by
 * adding its unit to the word, so a release at zero borrows from the bits
 * above it.
 */
static ARTS_ALWAYS_INLINE uint64_t excl_compute_next(uint64_t cur, int op,
                                                     uint32_t *out_grant) {
  uint32_t w = EXCL_STATE_W(cur);
  uint32_t r = EXCL_STATE_R(cur);
  uint64_t next = cur;
  uint32_t grant = LOCK_GRANT_NONE;
  /* Torn down (marked, nothing counted): an acquire arriving
   * now was still pending at the destroy, which the contract leaves
   * unserved; a grant would hand out a block whose teardown has run.  It is
   * neither counted nor granted, and the caller answers nothing: a refused
   * write request's queued node goes with the descriptor, a refused read
   * request's is freed by its handler. */
  if ((op == EXCL_OP_RW_ACQ || op == EXCL_OP_RO_ACQ) && excl_torn_down(cur)) {
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
    next = cur + EXCL_STATE_W_ONE;
    if (w == 0 && r == 0) {
      grant = LOCK_GRANT_ONE_RW; /* none -> rw */
    } else if (w == 0) {
      next |= EXCL_STATE_PHASE_BIT; /* RO held (w was 0), RW waits */
    }
    /* else w>0 (rw->rw): bit unchanged, no grant — the held writer serves it.
     */
    if (r == 0) {
      next &= ~EXCL_STATE_PHASE_BIT;
    }
    break;
  case EXCL_OP_RO_ACQ:
    /* r+1.  none->ro / ro->ro drains all RO (D7: new RO grants immediately even
     * with RW waiting); if w>0 (RW held) the RO parks (state_bit=RW). */
    next = cur + EXCL_STATE_R_ONE;
    if (w == 0) {
      grant = LOCK_GRANT_ALL_RO; /* none->ro or ro->ro */
      next &= ~EXCL_STATE_PHASE_BIT;
    } else if ((cur & EXCL_STATE_PHASE_BIT) != 0u && r > 0) {
      grant = LOCK_GRANT_ALL_RO; /* RO phase extends (D7) */
    } else {
      next &= ~EXCL_STATE_PHASE_BIT; /* RW held, RO waits */
    }
    break;
  case EXCL_OP_RW_REL:
    /* w-1.  w-1>0 grants next writer (D6); w-1==0 && r>0 flips to RO + drains;
     * w-1==0 && r==0 -> none. */
    next = cur - EXCL_STATE_W_ONE;
    if (w - 1u > 0u) {
      grant = LOCK_GRANT_ONE_RW; /* rw->rw */
      if (r == 0) {
        next &= ~EXCL_STATE_PHASE_BIT;
      }
    } else {
      if (r > 0) {
        grant = LOCK_GRANT_ALL_RO; /* rw->ro */
      }
      next &= ~EXCL_STATE_PHASE_BIT;
    }
    break;
  case EXCL_OP_RO_REL:
    /* r-1.  r-1==0 && w>0 flips to RW + grants one; else nothing. */
    next = cur - EXCL_STATE_R_ONE;
    if (r - 1u == 0u) {
      if (w > 0) {
        grant = LOCK_GRANT_ONE_RW; /* ro->rw */
      }
      next &= ~EXCL_STATE_PHASE_BIT;
    } else if (w == 0) {
      next &= ~EXCL_STATE_PHASE_BIT;
    }
    break;
  case EXCL_OP_TEARDOWN:
    /* Mark in the same atom that reads the counts, so a destroy and a
     * concurrent last release cannot each conclude the other will tear down.
     * Only the op that FIRST sets the mark can claim the teardown here, so a
     * second destroy, a program error, claims none. */
    if ((cur & EXCL_STATE_TEARDOWN_BIT) == 0u) {
      next |= EXCL_STATE_TEARDOWN_BIT;
      if (excl_torn_down(next)) {
        grant = LOCK_GRANT_DESTROY; /* lock already free — tear down now */
      }
    }
    if (w == 0 || r == 0) {
      next &= ~EXCL_STATE_PHASE_BIT;
    }
    break;
  default:
    if (w == 0 || r == 0) {
      next &= ~EXCL_STATE_PHASE_BIT;
    }
    break;
  }
  /* A release that LANDS on the zero edge under an existing mark is the other
   * teardown point.  The two are mutually exclusive: the destroy claims it only
   * when both counts are already zero, and only one CAS can carry a counter to
   * zero.  No grant can be owed at that instant, so setting `grant` here
   * discards nothing. */
  if ((op == EXCL_OP_RW_REL || op == EXCL_OP_RO_REL) && excl_torn_down(next)) {
    grant = LOCK_GRANT_DESTROY;
  }
  *out_grant = grant;
  return next;
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
 *     request, which nothing but that request's grant answers.  So the
 *     writers a GRANT_RW transition counts are exactly the parked writers its
 *     committer must serve, and the readers a GRANT_RO transition counts,
 *     with the ones earlier write permission set aside (ro_aside), are
 *     exactly the parked readers: each committer is the sole drainer of its
 *     axis.
 * The read count means one thing at a time: the readers parked on the read
 * request while ro_state is REQ and no write grant is held, holders
 * otherwise.  Write permission that arrives while the read request is out
 * therefore takes the parked readers out of the count (the committer moves
 * them to ro_aside), so the write turn's zero edge reads holders alone and
 * ends without them; their request stays out and its grant returns them.
 * The REL_* ops carry the count-- inside the CAS, since the decrement and
 * the 0-edge state change must be atomic together; a release finds its count
 * above zero (its caller checks), so the count moves by subtracting its unit.
 * A refused transition (LATE) leaves the word as it is. */
static ARTS_ALWAYS_INLINE uint64_t cache_compute_next(uint64_t cur, int op,
                                                      uint32_t taken,
                                                      uint32_t *out_action) {
  uint32_t rws = CACHE_RW_ST(cur);
  uint32_t ros = CACHE_RO_ST(cur);
  uint32_t wc = CACHE_RW_CNT(cur);
  uint32_t rc = CACHE_RO_CNT(cur);
  uint64_t next = cur;
  uint32_t act = CACHE_ACT_NONE;
  switch (op) {
  case CACHE_OP_ACQ_RW:
    if (wc == (uint32_t)CACHE_RW_CNT_MASK) {
      act = CACHE_ACT_LATE; /* the count field is full: the caller raises */
      break;
    }
    next = cur + CACHE_RW_CNT_ONE;
    if (rws == CACHE_ST_GRANT) {
      act = CACHE_ACT_SELF_SERVE; /* join the held RW phase directly */
    } else if (rws == CACHE_ST_IDLE) {
      /* First writer of the round: request home at once, whatever the read
       * axis holds. */
      next |= CACHE_RW_ST_AT(CACHE_ST_REQ);
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
    next = cur + CACHE_RO_CNT_ONE;
    if (rws == CACHE_ST_GRANT || ros == CACHE_ST_GRANT) {
      act = CACHE_ACT_SELF_SERVE; /* RW⊇RO local join, or RO phase held */
    } else if (ros == CACHE_ST_IDLE) {
      /* First reader of the round: request home at once, whatever the write
       * axis holds. */
      next |= CACHE_RO_ST_AT(CACHE_ST_REQ);
      act = CACHE_ACT_SEND_RO;
    } else {
      act = CACHE_ACT_PARK; /* the read request is out: join it */
    }
    break;
  case CACHE_OP_GRANT_RW:
    /* One grant answers one request: the axis is at REQ with its opener
     * counted.  The home grants a write turn only once every read grant came
     * back, so no read grant is held, and readers are counted only on a read
     * request that is out.  Anything else is a grant for no request.  The
     * readers parked on the read request leave the count for the caller. */
    if (rws != CACHE_ST_REQ || wc == 0u || ros == CACHE_ST_GRANT ||
        (ros == CACHE_ST_IDLE && rc != 0u)) {
      act = CACHE_ACT_LATE;
      break;
    }
    next = (cur & ~(CACHE_RW_ST_FIELD | CACHE_RO_CNT_FIELD)) |
           CACHE_RW_ST_AT(CACHE_ST_GRANT);
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
    next = ((cur + taken) & ~CACHE_RO_ST_FIELD) |
           CACHE_RO_ST_AT(CACHE_ST_GRANT);
    act = CACHE_ACT_DRAIN_RO;
    break;
  }
  case CACHE_OP_REL_RW: /* precond: rws==GRANT */
    next = cur - CACHE_RW_CNT_ONE;
    if ((next & (CACHE_RW_CNT_FIELD | CACHE_RO_CNT_FIELD)) == 0u) {
      next &= ~CACHE_RW_ST_FIELD;
      act = CACHE_ACT_REL_RW; /* last holder of the RW grant → publish */
    }
    break;
  case CACHE_OP_REL_RO:
    next = cur - CACHE_RO_CNT_ONE;
    if (rws == CACHE_ST_GRANT &&
        (next & (CACHE_RW_CNT_FIELD | CACHE_RO_CNT_FIELD)) == 0u) {
      next &= ~CACHE_RW_ST_FIELD;
      act = CACHE_ACT_REL_RW; /* last RO joiner under an RW grant → publish */
    } else if (ros == CACHE_ST_GRANT && (next & CACHE_RO_CNT_FIELD) == 0u) {
      next &= ~CACHE_RO_ST_FIELD;
      act = CACHE_ACT_REL_RO; /* last holder of the RO grant → notify */
    }
    break;
  default:
    break;
  }
  *out_action = act;
  return next;
}

/* The readers a write-permission transition (CACHE_ACT_DRAIN_RW) took out of
 * the word's read count, read from its pre-image: its committer adds them to
 * the cache's ro_aside. */
static ARTS_ALWAYS_INLINE uint32_t cache_set_aside(uint64_t pre) {
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
static ARTS_ALWAYS_INLINE bool cache_owner_serves_ro(uint64_t s) {
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
 * out (a migration's DELIVER) takes the parked readers out of the count,
 * which ro_aside keeps until the request's answer.  So a count is either all
 * parked waiters or all live holders, and a transition that starts serving
 * owes the parked count its pre-image shows, counted waiters whose push has
 * not landed included. */
static ARTS_ALWAYS_INLINE uint32_t cache_owner_parked_rw(uint64_t s) {
  return (CACHE_RW_ST(s) == CACHE_ST_REQ) ? CACHE_RW_CNT(s) : 0u;
}

static ARTS_ALWAYS_INLINE uint32_t cache_owner_parked_ro(uint64_t s) {
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
 * request, which only that request's answer serves: the waiters a transition
 * owes are a function of its
 * pre-image (cache_owner_parked_rw/_ro above, with the readers the cache set
 * aside for a read answer).  The REL_* ops carry the count-- inside the CAS,
 * because the decrement and the 0-edge transition must be atomic together; a
 * release finds its count above zero (its caller checks), so the count moves
 * by subtracting its unit.  A refused transition (LATE) leaves the word as it
 * is.
 */
static ARTS_ALWAYS_INLINE uint64_t cache_owner_compute_next(
    uint64_t cur, int op, uint32_t taken, uint32_t *out_action) {
  uint32_t own = CACHE_OWNER(cur);
  uint32_t rws = CACHE_RW_ST(cur);
  uint32_t ros = CACHE_RO_ST(cur);
  uint32_t mt = CACHE_MIGRATE_TARGET(cur);
  uint32_t wc = CACHE_RW_CNT(cur);
  uint32_t rc = CACHE_RO_CNT(cur);
  uint64_t next = cur;
  uint32_t act = CACHE_ACT_NONE;
  /* ro_granted = "this rank owes the home exactly one RO_RETURN" (and, while
   * the read request is out, the early-recall token).  A read grant sets it;
   * the ro_st -> IDLE TRANSITION clears it (OWN_RO, and REL_RO on a grant
   * marked for return), not the rc zero edge: under RETAIN rc oscillates
   * 0<->1 while the grant is retained, so clearing on the edge would drop the
   * token on the first release and the eventual recall-driven return would
   * never be sent — the home's r would stick.  An owner serving its own
   * readers (RW⊇RO, the resident copy) borrowed nothing and never sets it. */
  switch (op) {
  case CACHE_OP_ACQ_RW:
    if (wc == (uint32_t)CACHE_OWNER_WC_MASK) {
      act = CACHE_ACT_LATE; /* the count field is full: the caller raises */
      break;
    }
    next = cur + CACHE_RW_CNT_ONE;
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
      next |= CACHE_RW_ST_AT(CACHE_ST_REQ);
      act = CACHE_ACT_SEND_RW; /* request migration from home */
    }
    break;
  case CACHE_OP_ACQ_RO:
    if (rc == (uint32_t)CACHE_OWNER_RC_MASK) {
      act = CACHE_ACT_LATE;
      break;
    }
    next = cur + CACHE_RO_CNT_ONE;
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
      next |= CACHE_RO_ST_AT(CACHE_ST_REQ);
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
    next = (cur & ~(CACHE_RW_ST_FIELD | CACHE_RO_CNT_FIELD)) | CACHE_OWNER_BIT |
           CACHE_RW_ST_AT(CACHE_ST_GRANT);
    act = CACHE_ACT_DRAIN_RW;
    break;
  case CACHE_OP_GRANT_RO:
  case CACHE_OP_GRANT_RO_MARKED: {
    /* A read grant answers the read request, which stays out until it: its
     * readers are the ones counted in the word and the ones a migration set
     * aside, which `taken` returns.  The home counts this
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
    next = ((cur + taken) & ~CACHE_RO_ST_FIELD) | CACHE_OWNER_LEASED_MASK |
           CACHE_RO_ST_AT((op == CACHE_OP_GRANT_RO_MARKED || CACHE_GRANTED(cur))
                              ? CACHE_ST_GRANT_PURGE
                              : CACHE_ST_GRANT);
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
    next = (cur + taken) & ~(CACHE_RO_ST_FIELD | CACHE_OWNER_LEASED_MASK);
    act = CACHE_ACT_DRAIN_RO;
    break;
  }
  case CACHE_OP_REL_RW: /* precond: rws==GRANT, own==1, wc>0 */
    next = cur - CACHE_RW_CNT_ONE;
    /* The grant ends only when BOTH counts reach zero: readers that joined
     * under it (RW⊇RO) may outlive the last writer, so wc==0 with rc>0 keeps the
     * grant and the migration ships at the rc 0-edge instead. */
    if ((next & (CACHE_RW_CNT_FIELD | CACHE_RO_CNT_FIELD)) == 0u &&
        mt != ARTS_EXCL_NO_TARGET) {
      /* 0-edge with a pending migration: the permission leaves with the
       * bytes, and the owner-bit and migrate_target clear in the SAME
       * next-state (single CAS); signal the ship.  This is the EXCL·RETAIN
       * analogue of VAL's 0-edge transfer, but a single CAS — no
       * split-decrement, no separate in-flight flag. */
      next = (next & ~(CACHE_RW_ST_FIELD | CACHE_OWNER_BIT)) |
             CACHE_MIGRATE_TARGET_FIELD;
      act = CACHE_ACT_MIGRATE;
    }
    /* Otherwise sticky: keep the data AND the write permission (rw_st stays
     * GRANT).  This is what RETAIN means on this axis — the permission is
     * given up only when another node asks, never merely because the local
     * writers finished.  Clearing rw_st here is PURGE's rule. */
    break;
  case CACHE_OP_REL_RO: /* precond: rc>0 */
    next = cur - CACHE_RO_CNT_ONE;
    if ((next & CACHE_RO_CNT_FIELD) == 0u) {
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
        next &= ~(CACHE_RO_ST_FIELD | CACHE_OWNER_LEASED_MASK);
      }
      act = CACHE_ACT_REL_RO; /* the edge; the caller derives any send */
    }
    break;
  default:
    break;
  }
  *out_action = act;
  return next;
}

/* The readers a transition that makes this rank serve reads with its read
 * request out (CACHE_ACT_DRAIN_RW: a migration's DELIVER) took out of the
 * word's read count, read from its pre-image: its committer
 * adds them to the cache's ro_aside. */
static ARTS_ALWAYS_INLINE uint32_t cache_set_aside(uint64_t pre) {
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
 * rw_waiters/ro_waiters.
 *
 * w and r never pass their fields, and a CONFIRM or RO_RET finds its count
 * above zero, the caller's precondition, which no run inside the contract
 * breaks; a count moves by adding its unit to the word, so one at zero
 * borrows from the fields above it. */
static ARTS_ALWAYS_INLINE uint64_t lock_owner_compute_next(
    uint64_t cur, int op, unsigned int new_owner, uint32_t *out_action) {
  uint32_t phase = LOCK_PHASE(cur);
  uint32_t w = LOCK_W(cur);
  uint32_t r = LOCK_R(cur);
  uint64_t next = cur;
  uint32_t action = EXCL_ACTION_NONE;
  switch (op) {
  case EXCL_OP_RW_ACQ:
    next = cur + EXCL_STATE_W_ONE;
    if (phase == EXCL_PHASE_IDLE) {
      next |= EXCL_STATE_PHASE_AT(EXCL_PHASE_RW);
      action = EXCL_ACTION_FORWARD_MIGRATE;
    }
    /* phase==RW: queue only (next migrate at CONFIRM).
     * phase==RO: hold (r unaffected here — the RW parks in rw_waiters). */
    break;
  case EXCL_OP_RO_ACQ:
    next = cur + EXCL_STATE_R_ONE;
    if (phase == EXCL_PHASE_RO) {
      action = EXCL_ACTION_FORWARD_SERVE_ONE;
    } else if (phase == EXCL_PHASE_IDLE) {
      next |= EXCL_STATE_PHASE_AT(EXCL_PHASE_RO);
      action = EXCL_ACTION_FORWARD_SERVE_ONE;
    }
    /* phase==RW: held in ro_waiters by the caller; no action. */
    break;
  case EXCL_OP_CONFIRM:
    next = ((cur - EXCL_STATE_W_ONE) & ~EXCL_STATE_OWNER_FIELD) |
           (((uint64_t)new_owner & EXCL_STATE_OWNER_MASK)
            << EXCL_STATE_OWNER_SHIFT);
    if (w - 1u > 0u) {
      action = EXCL_ACTION_FORWARD_MIGRATE; /* RW chain */
    } else if (r > 0u) {
      next =
          (next & ~EXCL_STATE_PHASE_FIELD) | EXCL_STATE_PHASE_AT(EXCL_PHASE_RO);
      action = EXCL_ACTION_FORWARD_SERVE_ALL; /* RW → RO flip */
    } else {
      next &= ~EXCL_STATE_PHASE_FIELD; /* phase = IDLE */
    }
    break;
  case EXCL_OP_RO_RET:
    next = cur - EXCL_STATE_R_ONE;
    if (r - 1u == 0u) {
      if (w > 0u) {
        next = (next & ~EXCL_STATE_PHASE_FIELD) |
               EXCL_STATE_PHASE_AT(EXCL_PHASE_RW);
        action = EXCL_ACTION_FORWARD_MIGRATE; /* RO → RW flip */
      } else {
        next &= ~EXCL_STATE_PHASE_FIELD; /* phase = IDLE */
      }
    }
    break;
  default:
    break;
  }
  *out_action = action;
  return next;
}
#endif /* ARTS_RETENTION_RETAIN */
