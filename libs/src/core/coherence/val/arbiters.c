/* SPDX-License-Identifier: Apache-2.0 */
#include "arts/coherence/val/types.h"

/* The VAL request word's arbiter: a pure function of the word (no calls, no
 * global state), included by coherence.c and by the unit test. */
uint64_t val_req_compute_next(uint64_t cur, int op, uint32_t idx,
                              uint32_t *out_act) {
  uint32_t rw = VREQ_RW(cur), ro = VREQ_RO(cur), rp = VREQ_RP(cur);
  uint32_t par = VREQ_PAR(cur), stub = VREQ_STUB(cur);
  uint32_t group = VREQ_GROUP(cur), next = VREQ_NEXT(cur);
  uint32_t act = VREQ_ACT_NONE;
  switch (op) {
  case VREQ_OP_ACQ_RO:
    if (rw != VREQ_RW_IDLE || ro) {
      next = idx; /* rides the write request, or waits for the next window */
      act = VREQ_ACT_PARK;
    } else {
      ro = 1u;
      par ^= 1u;
      group = idx;
      act = VREQ_ACT_SEND_RO;
    }
    break;
  case VREQ_OP_ANSWER:
    /* idx: the answered request's parity, which must be the open window's.
     * Any answer proves the home installed, so a cache that sent it can no
     * longer be claimed; the stub bit clears unless a writer holds an OPEN
     * (whose token is the bit). */
    if (!ro || rp || par != idx) {
      act = VREQ_ACT_LATE;
      break;
    }
    if (rw != VREQ_RW_OPEN) {
      stub = 0u;
    }
    group = 0u; /* the caller serves the pre-image's group */
    ro = 0u;
    act = VREQ_ACT_SERVE;
    if (next != 0u && rw == VREQ_RW_IDLE) {
      ro = 1u;
      par ^= 1u;
      group = next;
      next = 0u;
      act = VREQ_ACT_SERVE_SEND;
    }
    break;
  case VREQ_OP_CTS:
    /* A size round or a dismiss: an answer without data (idx: its parity).
     * Re-send only for a cohort that rides no write request; otherwise
     * close, the cohort riding the write request. */
    if (!ro || rp || par != idx) {
      act = VREQ_ACT_LATE;
      break;
    }
    if (rw != VREQ_RW_OPEN) {
      stub = 0u;
    }
    if (group == 0u && next == 0u) {
      ro = 0u; /* covered: the window closes */
    } else if (rw == VREQ_RW_IDLE) {
      /* The re-send is a request sent now, so a reader parked on next may
       * ride it; with the cohort empty (covered) next becomes the cohort.
       * Joining both chains would need a write to a node a concurrent
       * cover may be walking, so with both non-empty next waits one more
       * window. */
      if (group == 0u) {
        group = next;
        next = 0u;
      }
      par ^= 1u;
      act = VREQ_ACT_RESEND;
    } else {
      rp = 1u; /* the cohort rides the write request */
    }
    break;
  case VREQ_OP_COVER:
  case VREQ_OP_RW_CLEAR:
  case VREQ_OP_CLAIM:
    if (op == VREQ_OP_RW_CLEAR && rw != VREQ_RW_SENT) {
      act = VREQ_ACT_LATE;
      break;
    }
    group = 0u;
    next = 0u;
    if (rp || op == VREQ_OP_CLAIM) {
      /* A window that rode the write request closes with it; the claim also
       * closes a window whose request is out: that pre-claim request is the
       * home's to discard unanswered (the announce owes it). */
      ro = 0u;
      rp = 0u;
    }
    if (op != VREQ_OP_COVER) {
      rw = VREQ_RW_IDLE;
    }
    if (op != VREQ_OP_COVER) {
      /* A grant, like the claim, proves the cache will never be claimed:
       * the home answered, so it is installed, and had this rank been the
       * creator its claim would have come first. */
      stub = 0u;
    }
    act = VREQ_ACT_SERVE;
    break;
  case VREQ_OP_DECLAIM:
    /* While the write request is out no writer holds an OPEN, so no token
     * is outstanding when the stub bit clears. */
    if (rw != VREQ_RW_SENT) {
      act = VREQ_ACT_LATE;
      break;
    }
    stub = 0u;
    break;
  case VREQ_OP_RW_CLAIM:
    if (rw == VREQ_RW_IDLE) {
      rw = VREQ_RW_OPEN;
      act = VREQ_ACT_WON;
    }
    break;
  case VREQ_OP_RW_COMMIT:
    /* idx carries the OPEN's token: the stub bit its claim read. */
    if (rw == VREQ_RW_OPEN && stub == idx) {
      rw = VREQ_RW_SENT;
      act = VREQ_ACT_WON;
    } else {
      act = VREQ_ACT_LOST;
    }
    break;
  case VREQ_OP_RW_GIVE:
    if (rw != VREQ_RW_OPEN || stub != idx) {
      break; /* a create's claim reset this OPEN */
    }
    rw = VREQ_RW_IDLE;
    if (rp) {
      /* The parked window asks again; as at a size round, an empty cohort
       * takes the readers parked since, whose park precedes this re-send. */
      if (group == 0u) {
        group = next;
        next = 0u;
      }
      rp = 0u;
      par ^= 1u;
      act = VREQ_ACT_RESEND;
    } else if (!ro && next != 0u) {
      ro = 1u;
      par ^= 1u;
      group = next;
      next = 0u;
      act = VREQ_ACT_SEND_RO; /* riders with no write turn coming */
    }
    break;
  default:
    break;
  }
  *out_act = act;
  if (act == VREQ_ACT_LATE) {
    return cur;
  }
  return VREQ_MAKE(rw, ro, rp, par, stub, group, next);
}
