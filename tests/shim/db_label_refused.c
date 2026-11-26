/* SPDX-License-Identifier: Apache-2.0 */
/* A labeled data-block create is refused: a data block is named by its
 * create, so the shim ends the program at a create that names its block by a
 * label, whatever the label.  Reaching the FAIL line means the labeled create
 * was accepted. */
#include "ocr.h"

#include "extensions/ocr-labeling.h"

ocrGuid_t mainEdt(u32 pc, u64 *pv, u32 dc, ocrEdtDep_t dv[]) {
  (void)pc; (void)pv; (void)dc; (void)dv;
  ocrGuid_t range, label;
  if (ocrGuidRangeCreate(&range, 4, GUID_USER_EVENT_STICKY) != 0 ||
      ocrGuidFromIndex(&label, range, 0) != 0) {
    PRINTF("FAIL: no label from an event range\n");
    ocrShutdown();
    return NULL_GUID;
  }
  u64 *p = NULL;
  u8 rc = ocrDbCreate(&label, (void **)&p, 8, GUID_PROP_IS_LABELED, NULL_HINT,
                      NO_ALLOC);
  PRINTF("FAIL: a labeled data-block create was accepted (returned %u)\n",
         (unsigned int)rc);
  ocrShutdown();
  return NULL_GUID;
}
