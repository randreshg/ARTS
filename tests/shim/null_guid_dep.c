/* SPDX-License-Identifier: Apache-2.0 */
/* Dependences that carry NULL_GUID in the model's default mode (read-write):
 * a create-time dependence on an event later satisfied with NULL_GUID, and an
 * EDT's output event, satisfied with no result, feeding a create-time slot.
 * Neither slot names a data block, so neither is acquired, and the consumer's
 * release must skip both. */
#include "ocr.h"

static ocrGuid_t producer(u32 pc, u64 *pv, u32 dc, ocrEdtDep_t dv[]) {
  (void)pc; (void)pv; (void)dc; (void)dv;
  return NULL_GUID;
}

static ocrGuid_t consumer(u32 pc, u64 *pv, u32 dc, ocrEdtDep_t dv[]) {
  (void)pc; (void)pv;
  int bad = (dc != 2);
  for (u32 i = 0; i < dc; ++i) {
    if (!ocrGuidIsNull(dv[i].guid) || dv[i].ptr != NULL) {
      PRINTF("FAIL slot %u: guid=%lx ptr=%p, want NULL_GUID and NULL\n", i,
             (unsigned long)dv[i].guid.guid, dv[i].ptr);
      bad = 1;
    }
  }
  if (bad) {
    PRINTF("FAIL: a NULL_GUID dependence\n");
    ocrAbort(1);
    return NULL_GUID;
  }
  PRINTF("PASS: NULL_GUID dependences in the default mode\n");
  ocrShutdown();
  return NULL_GUID;
}

ocrGuid_t mainEdt(u32 pc, u64 *pv, u32 dc, ocrEdtDep_t dv[]) {
  (void)pc; (void)pv; (void)dc; (void)dv;
  ocrGuid_t ev, gate, ptpl, ctpl, prod, out, cons;
  ocrEventCreate(&ev, OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG);
  ocrEventCreate(&gate, OCR_EVENT_ONCE_T, EVT_PROP_NONE);
  /* The producer waits on the gate, so its output event fires after the
   * consumer is registered on it. */
  ocrEdtTemplateCreate(&ptpl, producer, 0, 1);
  ocrEdtCreate(&prod, ptpl, 0, NULL, 1, NULL, EDT_PROP_NONE, NULL_HINT, &out);
  ocrEdtTemplateDestroy(ptpl);
  ocrAddDependence(gate, prod, 0, DB_MODE_NULL);
  ocrGuid_t deps[2] = {ev, out};
  ocrEdtTemplateCreate(&ctpl, consumer, 0, 2);
  ocrEdtCreate(&cons, ctpl, 0, NULL, 2, deps, EDT_PROP_NONE, NULL_HINT, NULL);
  ocrEdtTemplateDestroy(ctpl);
  ocrEventSatisfy(ev, NULL_GUID);
  ocrEventSatisfy(gate, NULL_GUID);
  return NULL_GUID;
}
