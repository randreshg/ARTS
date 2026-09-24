/* SPDX-License-Identifier: Apache-2.0 */
/* A finish EDT's output event fires when the finish EDT and its descendants
 * complete, not when the task that created it returns: the creator spins after
 * ocrEdtCreate, and a continuation bound to the output event must run while it
 * spins.  The finish EDT runs on the last rank and spawns a child there. */
#include <stdatomic.h>
#include <time.h>

#include "ocr.h"
#include "extensions/ocr-affinity.h"

#define WAIT_NS (30ull * 1000000000ull)

static atomic_uint fired;

static u64 now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (u64)t.tv_sec * 1000000000ull + (u64)t.tv_nsec;
}

static ocrGuid_t child(u32 pc, u64 *pv, u32 dc, ocrEdtDep_t dv[]) {
  (void)pc; (void)pv; (void)dc; (void)dv;
  return NULL_GUID;
}

static ocrGuid_t finish_body(u32 pc, u64 *pv, u32 dc, ocrEdtDep_t dv[]) {
  (void)pc; (void)pv; (void)dc; (void)dv;
  ocrGuid_t tpl, task;
  ocrEdtTemplateCreate(&tpl, child, 0, 0);
  ocrEdtCreate(&task, tpl, 0, NULL, 0, NULL, EDT_PROP_NONE, NULL_HINT, NULL);
  ocrEdtTemplateDestroy(tpl);
  return NULL_GUID;
}

static ocrGuid_t continuation(u32 pc, u64 *pv, u32 dc, ocrEdtDep_t dv[]) {
  (void)pc; (void)pv; (void)dc; (void)dv;
  atomic_store_explicit(&fired, 1u, memory_order_release);
  return NULL_GUID;
}

static void hint_at(ocrHint_t *hint, u64 rank) {
  ocrGuid_t affinity;
  ocrAffinityGetAt(AFFINITY_PD, rank, &affinity);
  ocrHintInit(hint, OCR_HINT_EDT_T);
  ocrSetHintValue(hint, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(affinity));
}

ocrGuid_t mainEdt(u32 pc, u64 *pv, u32 dc, ocrEdtDep_t dv[]) {
  (void)pc; (void)pv; (void)dc; (void)dv;
  u64 ranks = 0;
  ocrAffinityCount(AFFINITY_PD, &ranks);
  ocrHint_t last, here;
  hint_at(&last, ranks - 1);
  hint_at(&here, 0);

  ocrGuid_t ftpl, ctpl, fin, cont, out;
  ocrEdtTemplateCreate(&ftpl, finish_body, 0, 0);
  ocrEdtCreate(&fin, ftpl, 0, NULL, 0, NULL, EDT_PROP_FINISH, &last, &out);
  ocrEdtTemplateDestroy(ftpl);
  ocrEdtTemplateCreate(&ctpl, continuation, 0, 1);
  ocrEdtCreate(&cont, ctpl, 0, NULL, 1, NULL, EDT_PROP_NONE, &here, NULL);
  ocrEdtTemplateDestroy(ctpl);
  ocrAddDependence(out, cont, 0, DB_MODE_NULL);

  u64 start = now_ns();
  while (atomic_load_explicit(&fired, memory_order_acquire) == 0u &&
         now_ns() - start < WAIT_NS) {
  }
  if (atomic_load_explicit(&fired, memory_order_acquire) == 0u) {
    PRINTF("FAIL: the finish EDT's output event did not fire while its "
           "creator ran\n");
    ocrAbort(1);
    return NULL_GUID;
  }
  PRINTF("PASS: finish EDT output fired before its creator returned\n");
  ocrShutdown();
  return NULL_GUID;
}
