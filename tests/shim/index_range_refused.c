/* SPDX-License-Identifier: Apache-2.0 */
/* ocrGuidFromIndex answers a GUID that names no range of EDTs or events (a data
 * block's GUID, NULL_GUID) with OCR's error return and the program goes on,
 * while an event range still answers its index. */
#include "ocr.h"

#include "extensions/ocr-labeling.h"

ocrGuid_t mainEdt(u32 pc, u64 *pv, u32 dc, ocrEdtDep_t dv[]) {
  (void)pc; (void)pv; (void)dc; (void)dv;
  int bad = 0;
  ocrGuid_t db, out;
  u64 *p = NULL;
  ocrDbCreate(&db, (void **)&p, 8, DB_PROP_NONE, NULL_HINT, NO_ALLOC);
  ocrDbRelease(db);
  if (ocrGuidFromIndex(&out, db, 0) == 0) {
    PRINTF("FAIL: a data block's GUID was taken as a range\n");
    bad = 1;
  }
  if (ocrGuidFromIndex(&out, NULL_GUID, 0) == 0) {
    PRINTF("FAIL: NULL_GUID was taken as a range\n");
    bad = 1;
  }
  ocrGuid_t range;
  if (ocrGuidRangeCreate(&range, 4, GUID_USER_EVENT_STICKY) != 0 ||
      ocrGuidFromIndex(&out, range, 0) != 0 || ocrGuidIsNull(out)) {
    PRINTF("FAIL: an event range did not answer its index 0\n");
    bad = 1;
  }
  ocrDbDestroy(db);
  if (!bad) {
    PRINTF("PASS: a GUID that names no range is answered with an error\n");
  }
  ocrShutdown();
  return NULL_GUID;
}
