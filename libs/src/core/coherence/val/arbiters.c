/* SPDX-License-Identifier: Apache-2.0 */
#include "arts/coherence/val/types.h"

/* The VAL request word's arbiter: a pure function of the word (no calls, no
 * global state), included by coherence.c and by the unit test. */
uint64_t arts_val_req_compute_next(uint64_t cur, int op, uint32_t idx,
                                   uint32_t *out_act) {
  uint32_t rw = VREQ_RW(cur), ro = VREQ_RO(cur), stub = VREQ_STUB(cur);
  uint32_t group = VREQ_GROUP(cur), next = VREQ_NEXT(cur);
  uint32_t act = VREQ_ACT_NONE;
  switch (op) {
  case VREQ_OP_ACQ_RO:
    if (ro) {
      next = idx; /* waits for the next window */
      act = VREQ_ACT_PARK;
    } else {
      ro = 1u;
      group = idx;
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
      stub = 0u;
    }
    /* The caller serves the pre-image's group.  The readers parked since
     * its request left go out on a request sent now. */
    if (next != 0u) {
      group = next;
      next = 0u;
      act = VREQ_ACT_SERVE_SEND;
    } else {
      group = 0u;
      ro = 0u;
      act = VREQ_ACT_SERVE;
    }
    break;
  case VREQ_OP_CTS:
    if (!ro) {
      act = VREQ_ACT_LATE;
      break;
    }
    if (rw != VREQ_RW_OPEN) {
      stub = 0u;
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
    rw = VREQ_RW_IDLE;
    stub = 0u;
    break;
  case VREQ_OP_CLAIM:
    rw = VREQ_RW_IDLE;
    stub = 0u;
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
    if (rw == VREQ_RW_OPEN && stub == idx) {
      rw = VREQ_RW_IDLE;
    } /* else a create's claim reset this OPEN */
    break;
  default:
    break;
  }
  *out_act = act;
  if (act == VREQ_ACT_LATE) {
    return cur;
  }
  return VREQ_MAKE(rw, ro, stub, group, next);
}
