/* SPDX-License-Identifier: Apache-2.0 */
/* An outer finish EDT's output event waits for an inner finish EDT's subtree:
 * a finish EDT completes once everything created transitively inside it has
 * completed, nested finish EDTs' subtrees included.
 *
 * The outer finish EDT creates an inner finish EDT, whose child creates a
 * marker on rank 0.  The continuation of the outer output event, on rank 0,
 * must find the marker done.  A missing link between the two finish EDTs
 * shows only when the inner subtree is still running as the outer finish EDT
 * returns, so the child sleeps for about 100 ms before it creates the marker;
 * a run may still miss it, which repeated runs make unlikely. */
#include <errno.h>
#include <stdatomic.h>
#include <time.h>

#include "ocr.h"
#include "extensions/ocr-affinity.h"

static _Atomic(unsigned int) marked;

static void hint_at(ocrHint_t *hint, u64 rank) {
  ocrGuid_t affinity;
  ocrAffinityGetAt(AFFINITY_PD, rank, &affinity);
  ocrHintInit(hint, OCR_HINT_EDT_T);
  ocrSetHintValue(hint, OCR_HINT_EDT_AFFINITY,
                  ocrAffinityToHintValue(affinity));
}

static ocrGuid_t marker(u32 pc, u64 *pv, u32 dc, ocrEdtDep_t dv[]) {
  (void)pc; (void)pv; (void)dc; (void)dv;
  atomic_fetch_add_explicit(&marked, 1u, memory_order_release);
  return NULL_GUID;
}

static ocrGuid_t child(u32 pc, u64 *pv, u32 dc, ocrEdtDep_t dv[]) {
  (void)pc; (void)pv; (void)dc; (void)dv;
  struct timespec left = {.tv_sec = 0, .tv_nsec = 100000000L};
  while (clock_nanosleep(CLOCK_MONOTONIC, 0, &left, &left) == EINTR) {
  }
  ocrHint_t here;
  hint_at(&here, 0);
  ocrGuid_t tpl, task;
  ocrEdtTemplateCreate(&tpl, marker, 0, 0);
  ocrEdtCreate(&task, tpl, 0, NULL, 0, NULL, EDT_PROP_NONE, &here, NULL);
  ocrEdtTemplateDestroy(tpl);
  return NULL_GUID;
}

static ocrGuid_t inner(u32 pc, u64 *pv, u32 dc, ocrEdtDep_t dv[]) {
  (void)pc; (void)pv; (void)dc; (void)dv;
  ocrGuid_t tpl, task;
  ocrEdtTemplateCreate(&tpl, child, 0, 0);
  ocrEdtCreate(&task, tpl, 0, NULL, 0, NULL, EDT_PROP_NONE, NULL_HINT, NULL);
  ocrEdtTemplateDestroy(tpl);
  return NULL_GUID;
}

static ocrGuid_t outer(u32 pc, u64 *pv, u32 dc, ocrEdtDep_t dv[]) {
  (void)pc; (void)pv; (void)dc; (void)dv;
  ocrGuid_t tpl, task;
  ocrEdtTemplateCreate(&tpl, inner, 0, 0);
  ocrEdtCreate(&task, tpl, 0, NULL, 0, NULL, EDT_PROP_FINISH, NULL_HINT, NULL);
  ocrEdtTemplateDestroy(tpl);
  return NULL_GUID;
}

static ocrGuid_t continuation(u32 pc, u64 *pv, u32 dc, ocrEdtDep_t dv[]) {
  (void)pc; (void)pv; (void)dc; (void)dv;
  unsigned int n = atomic_load_explicit(&marked, memory_order_acquire);
  if (n != 1u) {
    PRINTF("FAIL: the outer finish EDT's output event fired with %u of 1 "
           "markers done\n", n);
    ocrAbort(1);
    return NULL_GUID;
  }
  PRINTF("PASS: the outer finish EDT waited for the inner subtree\n");
  ocrShutdown();
  return NULL_GUID;
}

ocrGuid_t mainEdt(u32 pc, u64 *pv, u32 dc, ocrEdtDep_t dv[]) {
  (void)pc; (void)pv; (void)dc; (void)dv;
  ocrHint_t here;
  hint_at(&here, 0);
  ocrGuid_t otpl, ctpl, fin, cont, out;
  ocrEdtTemplateCreate(&otpl, outer, 0, 0);
  ocrEdtCreate(&fin, otpl, 0, NULL, 0, NULL, EDT_PROP_FINISH, NULL_HINT, &out);
  ocrEdtTemplateDestroy(otpl);
  ocrEdtTemplateCreate(&ctpl, continuation, 0, 1);
  ocrEdtCreate(&cont, ctpl, 0, NULL, 1, NULL, EDT_PROP_NONE, &here, NULL);
  ocrEdtTemplateDestroy(ctpl);
  ocrAddDependence(out, cont, 0, DB_MODE_NULL);
  return NULL_GUID;
}
