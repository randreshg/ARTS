/* SPDX-License-Identifier: Apache-2.0 */
/* A range of data-block GUIDs is refused: a data block is named by its create,
 * so the shim ends the program at the range's reservation.  Reaching the FAIL
 * line means the range was reserved. */
#include "ocr.h"

#include "extensions/ocr-labeling.h"

ocrGuid_t mainEdt(u32 pc, u64 *pv, u32 dc, ocrEdtDep_t dv[]) {
  (void)pc; (void)pv; (void)dc; (void)dv;
  ocrGuid_t range;
  u8 rc = ocrGuidRangeCreate(&range, 4, GUID_USER_DB);
  PRINTF("FAIL: a data-block GUID range was reserved (returned %u)\n",
         (unsigned int)rc);
  ocrShutdown();
  return NULL_GUID;
}
