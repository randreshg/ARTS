/* SPDX-License-Identifier: Apache-2.0
 *
 * cover_gate_step — the declared-arrival gate's one step
 * (arts/coherence/cover_gate.h): every step is one RMW of the gate word, and
 * its pre-image decides whether the step is the one that leaves a marked gate
 * with nothing owed (the teardown edge) and whether the gate was marked.
 *
 * Every step runs from boundary words (owed 0, 1, 2, the field's maximum and
 * one below it; each of td, rwo and roo set and clear) and is checked against
 * the transition the gate defines: the word it leaves, the edge and the mark.
 * A step outside its precondition is fatal and is not driven here.  A
 * duplicate MARK leaves the word as it found it and reports no edge, so a
 * second destroy can never tear the home down twice.
 *
 * Whitebox unit test: header-only, no runtime.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "arts/coherence/cover_gate.h"
#include "arts/runtime_state.h"
#include "arts/system/identity.h"

/* What the fatal path references; no fatal step is driven. */
unsigned int arts_global_rank_id = 0;
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;
void arts_abort(uint8_t code) { _exit(code ? code : 1); }

static int g_fails;

/* The gate's transition: false when the step's precondition fails. */
static bool expect(uint32_t cur, enum arts_cover_gate_op op, uint32_t *next,
                   bool *edge, bool *marked) {
  uint32_t owed = cur & ARTS_COVER_GATE_OWED;
  bool td = (cur & ARTS_COVER_GATE_TD) != 0u;
  *marked = td;
  *edge = false;
  switch (op) {
  case ARTS_COVER_GATE_OWE:
    if (owed == ARTS_COVER_GATE_OWED) {
      return false;
    }
    *next = cur + 1u;
    return true;
  case ARTS_COVER_GATE_MARK:
    *next = cur | ARTS_COVER_GATE_TD;
    *edge = !td && owed == 0u;
    return true;
  default: {
    uint32_t flag = op == ARTS_COVER_GATE_RWO_ARRIVE   ? ARTS_COVER_GATE_RWO
                    : op == ARTS_COVER_GATE_ROO_ARRIVE ? ARTS_COVER_GATE_ROO
                                                       : 0u;
    if (owed == 0u || (flag != 0u && (cur & flag) == 0u)) {
      return false;
    }
    *next = (cur - 1u) & ~flag;
    *edge = td && owed == 1u;
    return true;
  }
  }
}

int main(void) {
  struct arts_db_s *db = (struct arts_db_s *)calloc(1, sizeof(*db));
  if (db == NULL) {
    printf("FAIL cover_gate_step: no descriptor\n");
    return 1;
  }
  const uint32_t owed[] = {0u, 1u, 2u, ARTS_COVER_GATE_OWED - 1u,
                           ARTS_COVER_GATE_OWED};
  const enum arts_cover_gate_op ops[] = {
      ARTS_COVER_GATE_OWE, ARTS_COVER_GATE_ARRIVE, ARTS_COVER_GATE_RWO_ARRIVE,
      ARTS_COVER_GATE_ROO_ARRIVE, ARTS_COVER_GATE_MARK};
  unsigned steps = 0, dup_marks = 0;
  for (unsigned f = 0; f < 8; f++) {
    uint32_t flags = ((f & 1u) ? ARTS_COVER_GATE_TD : 0u) |
                     ((f & 2u) ? ARTS_COVER_GATE_RWO : 0u) |
                     ((f & 4u) ? ARTS_COVER_GATE_ROO : 0u);
    for (unsigned o = 0; o < sizeof owed / sizeof owed[0]; o++) {
      for (unsigned k = 0; k < sizeof ops / sizeof ops[0]; k++) {
        uint32_t cur = flags | owed[o], want = 0;
        bool want_edge = false, want_marked = false;
        if (!expect(cur, ops[k], &want, &want_edge, &want_marked)) {
          continue;
        }
        atomic_store_explicit(&db->cover_gate, cur, memory_order_relaxed);
        bool marked = !want_marked;
        bool edge = arts_cover_gate_step(db, ops[k], &marked);
        uint32_t got =
            atomic_load_explicit(&db->cover_gate, memory_order_relaxed);
        steps++;
        if (ops[k] == ARTS_COVER_GATE_MARK && (cur & ARTS_COVER_GATE_TD)) {
          dup_marks++;
          if (got != cur || edge) {
            printf("FAIL cover_gate_step: a duplicate MARK of 0x%x left 0x%x "
                   "edge=%d\n",
                   cur, got, (int)edge);
            g_fails++;
          }
        }
        if (got != want || edge != want_edge || marked != want_marked) {
          printf("FAIL cover_gate_step: op %d from 0x%x: word 0x%x (want "
                 "0x%x) edge %d (want %d) marked %d (want %d)\n",
                 (int)ops[k], cur, got, want, (int)edge, (int)want_edge,
                 (int)marked, (int)want_marked);
          g_fails++;
        }
      }
    }
  }
  free(db);
  if (dup_marks == 0u || steps < 100u) {
    printf("FAIL cover_gate_step: only %u steps (%u duplicate marks)\n", steps,
           dup_marks);
    g_fails++;
  }
  if (g_fails != 0) {
    printf("FAIL cover_gate_step: %d mismatch(es)\n", g_fails);
    return 1;
  }
  printf("PASS cover_gate_step: %u steps, %u duplicate marks\n", steps,
         dup_marks);
  return 0;
}
