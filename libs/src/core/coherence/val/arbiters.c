/* SPDX-License-Identifier: Apache-2.0 */
#include "arts/coherence/val/types.h"

/* The VAL request word's arbiter: a pure function of the word (no calls, no
 * global state), included by coherence.c and by the unit tests.  It is
 * ARTS_ALWAYS_INLINE; a case changes only the fields its transition moves, in
 * place in the word, and a refused transition (LATE) leaves the word as it
 * is. */
static ARTS_ALWAYS_INLINE uint64_t val_req_compute_next(uint64_t cur, int op,
                                                        uint32_t idx,
                                                        uint32_t *out_act) {
  uint32_t rw = VREQ_RW(cur), ro = VREQ_RO(cur), stub = VREQ_STUB(cur);
  uint32_t waiting = VREQ_NEXT(cur);
  uint64_t next = cur;
  uint32_t act = VREQ_ACT_NONE;
  switch (op) {
  case VREQ_OP_ACQ_RO:
    if (ro) {
      /* waits for the next window */
      next = (cur & ~VREQ_NEXT_FIELD) | ((uint64_t)idx & VREQ_HEAD_MASK);
      act = VREQ_ACT_PARK;
    } else {
      next = (cur & ~VREQ_GROUP_FIELD) | VREQ_RO_BIT |
             (((uint64_t)idx & VREQ_HEAD_MASK) << VREQ_HEAD_BITS);
      act = VREQ_ACT_SEND_RO;
    }
    break;
  case VREQ_OP_ANSWER:
    /* Any answer proves the home installed, so a cache that sent it can no
     * longer be claimed; the stub bit clears unless a writer holds an OPEN
     * (whose token is the bit). */
    if (!ro) {
      act = VREQ_ACT_LATE;
      break;
    }
    if (rw != VREQ_RW_OPEN) {
      next &= ~VREQ_STUB_BIT;
    }
    /* The caller serves the pre-image's group.  The readers parked since
     * its request left go out on a request sent now. */
    if (waiting != 0u) {
      next = (next & ~(VREQ_GROUP_FIELD | VREQ_NEXT_FIELD)) |
             ((uint64_t)waiting << VREQ_HEAD_BITS);
      act = VREQ_ACT_SERVE_SEND;
    } else {
      next &= ~(VREQ_GROUP_FIELD | VREQ_RO_BIT);
      act = VREQ_ACT_SERVE;
    }
    break;
  case VREQ_OP_CTS:
    if (!ro) {
      act = VREQ_ACT_LATE;
      break;
    }
    if (rw != VREQ_RW_OPEN) {
      next &= ~VREQ_STUB_BIT;
    }
    act = VREQ_ACT_RESEND;
    break;
  case VREQ_OP_RW_CLEAR:
    if (rw != VREQ_RW_SENT) {
      act = VREQ_ACT_LATE;
      break;
    }
    /* A grant, like the claim, proves the cache will never be claimed: the
     * home answered, so it is installed, and had this rank been the creator
     * its claim would have come first. */
    next &= ~(VREQ_RW_FIELD | VREQ_STUB_BIT);
    break;
  case VREQ_OP_CLAIM:
    next &= ~(VREQ_RW_FIELD | VREQ_STUB_BIT);
    break;
  case VREQ_OP_DECLAIM:
    /* While the write request is out no writer holds an OPEN, so no token
     * is outstanding when the stub bit clears. */
    if (rw != VREQ_RW_SENT) {
      act = VREQ_ACT_LATE;
      break;
    }
    next &= ~VREQ_STUB_BIT;
    break;
  case VREQ_OP_RW_CLAIM:
    if (rw == VREQ_RW_IDLE) {
      next |= VREQ_RW_AT(VREQ_RW_OPEN);
      act = VREQ_ACT_WON;
    }
    break;
  case VREQ_OP_RW_COMMIT:
    /* idx carries the OPEN's token: the stub bit its claim read. */
    if (rw == VREQ_RW_OPEN && stub == idx) {
      next = (cur & ~VREQ_RW_FIELD) | VREQ_RW_AT(VREQ_RW_SENT);
      act = VREQ_ACT_WON;
    } else {
      act = VREQ_ACT_LOST;
    }
    break;
  case VREQ_OP_RW_GIVE:
    if (rw == VREQ_RW_OPEN && stub == idx) {
      next &= ~VREQ_RW_FIELD;
    } /* else a create's claim reset this OPEN */
    break;
  default:
    break;
  }
  *out_act = act;
  return next;
}
