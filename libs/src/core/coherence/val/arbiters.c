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
  uint32_t rw = VREQ_RW(cur), ro = VREQ_RO(cur);
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
    if (!ro) {
      act = VREQ_ACT_LATE;
      break;
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
  case VREQ_OP_RW_CLEAR:
    if (rw != VREQ_RW_SENT) {
      act = VREQ_ACT_LATE;
      break;
    }
    next &= ~VREQ_RW_FIELD;
    break;
  case VREQ_OP_RW_CLAIM:
    if (rw == VREQ_RW_IDLE) {
      next |= VREQ_RW_AT(VREQ_RW_OPEN);
      act = VREQ_ACT_WON;
    }
    break;
  case VREQ_OP_RW_COMMIT:
  case VREQ_OP_RW_GIVE:
    /* Only the claim's winner commits or gives back its OPEN, and nothing
     * else moves an OPEN axis, so any other state is a caller that does not
     * hold one. */
    if (rw != VREQ_RW_OPEN) {
      act = VREQ_ACT_LATE;
      break;
    }
    next = (cur & ~VREQ_RW_FIELD) |
           (op == VREQ_OP_RW_COMMIT ? VREQ_RW_AT(VREQ_RW_SENT) : 0u);
    break;
  default:
    break;
  }
  *out_act = act;
  return next;
}
