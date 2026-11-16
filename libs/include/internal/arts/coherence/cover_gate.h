/* SPDX-License-Identifier: Apache-2.0
 *
 * The destroy's gate of a data block's home, on the arms whose home does not
 * count releases in its lock word (VAL, INV and EXCL·RETAIN):
 *
 *   cover_gate = [ td:1 (1) | rwo:1 (0) ]
 *
 * rwo is the one request that can still arrive after a legal destroy: the
 * claiming creator's pre-claim write request, which the create's announce
 * said is coming.  td says the destroy marked the gate.  The teardown runs at
 * the one step that leaves the gate marked with rwo clear.  After its init
 * every write is an acq_rel RMW, so the step that reaches the edge follows
 * the other.
 */
#ifndef ARTS_COHERENCE_COVER_GATE_H
#define ARTS_COHERENCE_COVER_GATE_H

#include <stdbool.h>
#include <stdint.h>

#include "arts/coherence/types.h"
#include "arts/system/print.h"
#include "arts/utils/stdatomic_compat.h"

#define ARTS_COVER_GATE_TD 0x2u
#define ARTS_COVER_GATE_RWO 0x1u

enum arts_cover_gate_op {
  ARTS_COVER_GATE_RWO_ARRIVE, /* the creator's pre-claim write request */
  ARTS_COVER_GATE_MARK,       /* the destroy */
};

#if defined(ARTS_VALIDITY_VAL) || defined(ARTS_VALIDITY_INV) ||                \
    (defined(ARTS_VALIDITY_EXCL) && defined(ARTS_RETENTION_RETAIN))
/* One RMW of the gate; its pre-image decides the edge (true: this step left
 * the gate marked with rwo clear, and the caller tears down).  An arrival the
 * gate does not await is fatal after its RMW has committed, so while the
 * abort prints another thread's step may read a corrupted gate. */
static inline bool arts_cover_gate_step(struct arts_db_s *db,
                                        enum arts_cover_gate_op op) {
  uint32_t pre;
  if (op == ARTS_COVER_GATE_MARK) {
    /* A duplicate destroy writes the word it found and reports no edge. */
    pre = atomic_fetch_or_explicit(&db->cover_gate, ARTS_COVER_GATE_TD,
                                   memory_order_acq_rel);
    return pre == 0u;
  }
  pre = atomic_fetch_sub_explicit(&db->cover_gate, ARTS_COVER_GATE_RWO,
                                  memory_order_acq_rel);
  if ((pre & ARTS_COVER_GATE_RWO) == 0u) {
    ARTS_ERROR("coherence: guid %lu settles a request it was never owed "
               "(gate=%x)",
               (unsigned long)db->cache.db_guid, pre);
  }
  return (pre & ARTS_COVER_GATE_TD) != 0u;
}
#endif

#endif /* ARTS_COHERENCE_COVER_GATE_H */
