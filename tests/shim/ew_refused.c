/* SPDX-License-Identifier: Apache-2.0 */
/* An EW dependence is refused: no ARTS mode excludes the writers of one node
 * from each other, so the shim ends the program at the dependence instead of
 * weakening it to RW.  Reaching either FAIL line means the dependence was
 * accepted. */
#include "ocr.h"

static ocrGuid_t writer(u32 pc, u64 *pv, u32 dc, ocrEdtDep_t dv[]) {
  (void)pc; (void)pv; (void)dc; (void)dv;
  PRINTF("FAIL: an EDT ran on an EW dependence\n");
  ocrShutdown();
  return NULL_GUID;
}

ocrGuid_t mainEdt(u32 pc, u64 *pv, u32 dc, ocrEdtDep_t dv[]) {
  (void)pc; (void)pv; (void)dc; (void)dv;
  ocrGuid_t db, tpl, edt;
  u64 *p;
  ocrDbCreate(&db, (void **)&p, sizeof(u64), DB_PROP_NONE, NULL_HINT, NO_ALLOC);
  *p = 1;
  ocrDbRelease(db);
  ocrEdtTemplateCreate(&tpl, writer, 0, 1);
  ocrEdtCreate(&edt, tpl, 0, NULL, 1, NULL, EDT_PROP_NONE, NULL_HINT, NULL);
  ocrEdtTemplateDestroy(tpl);
  ocrAddDependence(db, edt, 0, DB_MODE_EW);
  PRINTF("FAIL: an EW dependence was accepted\n");
  return NULL_GUID;
}
