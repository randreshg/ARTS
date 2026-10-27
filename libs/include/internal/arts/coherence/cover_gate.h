/* SPDX-License-Identifier: Apache-2.0
 *
 * The declared-arrival gate of a data block's home, on the arms whose home
 * counts covered requests apart from its release accounting (VAL, INV and
 * EXCL × RETAIN):
 *
 *   cover_gate = [ td:1 (31) | rwo:1 (30) | roo:1 (29) | owed:29 ]
 *
 * owed counts the covered requests still coming, rwo and roo mark the
 * claiming creator's pre-claim write and read requests among them, and td
 * says the destroy marked the gate.  The teardown runs at the one step that
 * leaves a marked gate with nothing owed.  After its init every write is an
 * acq_rel RMW, so the mark heads a release sequence that an acquire load of
 * the gate synchronizes with through any later step.
 */
#ifndef ARTS_COHERENCE_COVER_GATE_H
#define ARTS_COHERENCE_COVER_GATE_H

#include <stdbool.h>
#include <stdint.h>

#include "arts/coherence/types.h"
#include "arts/system/print.h"
#include "arts/utils/stdatomic_compat.h"

#define ARTS_COVER_GATE_TD 0x80000000u
#define ARTS_COVER_GATE_RWO 0x40000000u
#define ARTS_COVER_GATE_ROO 0x20000000u
#define ARTS_COVER_GATE_OWED 0x1fffffffu

enum arts_cover_gate_op {
  ARTS_COVER_GATE_OWE,        /* a write request declared a read request */
  ARTS_COVER_GATE_ARRIVE,     /* an owed request settled */
  ARTS_COVER_GATE_RWO_ARRIVE, /* the creator's pre-claim write request */
  ARTS_COVER_GATE_ROO_ARRIVE, /* the creator's pre-claim read request */
  ARTS_COVER_GATE_MARK,       /* the destroy */
};

#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV) ||                \
    (defined(ARTS_PROTOCOL_EXCL) && defined(ARTS_RELEASE_RETAIN))
/* One RMW of the gate; its pre-image decides the edge (true: this step left a
 * marked gate with nothing owed, and the caller tears down) and *marked
 * (whether the gate was marked when the step committed).  A step outside its
 * precondition is fatal after its RMW has committed, so while the abort
 * prints another thread's step may read a corrupted gate. */
static inline bool arts_cover_gate_step(struct arts_db_s *db,
                                        enum arts_cover_gate_op op,
                                        bool *marked) {
  uint32_t pre;
  bool edge = false;
  if (op == ARTS_COVER_GATE_OWE) {
    pre = atomic_fetch_add_explicit(&db->cover_gate, 1u, memory_order_acq_rel);
    if ((pre & ARTS_COVER_GATE_OWED) == ARTS_COVER_GATE_OWED) {
      ARTS_ERROR("coherence: guid %lu owes more requests than its gate holds "
                 "(gate=%x)",
                 (unsigned long)db->cache.db_guid, pre);
    }
  } else if (op == ARTS_COVER_GATE_MARK) {
    /* A duplicate destroy writes the word it found and reports no edge. */
    pre = atomic_fetch_or_explicit(&db->cover_gate, ARTS_COVER_GATE_TD,
                                   memory_order_acq_rel);
    edge =
        (pre & ARTS_COVER_GATE_TD) == 0u && (pre & ARTS_COVER_GATE_OWED) == 0u;
  } else {
    uint32_t flag = op == ARTS_COVER_GATE_RWO_ARRIVE   ? ARTS_COVER_GATE_RWO
                    : op == ARTS_COVER_GATE_ROO_ARRIVE ? ARTS_COVER_GATE_ROO
                                                       : 0u;
    pre = atomic_fetch_sub_explicit(&db->cover_gate, 1u + flag,
                                    memory_order_acq_rel);
    if ((pre & ARTS_COVER_GATE_OWED) == 0u ||
        (flag != 0u && (pre & flag) == 0u)) {
      ARTS_ERROR("coherence: guid %lu settles a request it was never owed "
                 "(gate=%x)",
                 (unsigned long)db->cache.db_guid, pre);
    }
    edge =
        (pre & ARTS_COVER_GATE_TD) != 0u && (pre & ARTS_COVER_GATE_OWED) == 1u;
  }
  *marked = (pre & ARTS_COVER_GATE_TD) != 0u;
  return edge;
}
#endif

#endif /* ARTS_COHERENCE_COVER_GATE_H */
