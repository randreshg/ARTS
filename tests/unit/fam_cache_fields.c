/* excl/types.h maintains two hand-written views of arts_db_cache_s — one for
 * C11 atomics, one for C++/nvcc.  A field added to one and misplaced in the
 * other is a layout the two languages disagree about, which no runtime test
 * can see.  This probe prints its view's fingerprint; the C++ wrapper prints
 * the other; the comparison is a ctest of its own. */
#include "arts/coherence/excl/types.h"

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
#define FAM_LAYOUT_LANG "cpp"
#else
#define FAM_LAYOUT_LANG "c"
#endif

int main(void) {
#ifdef ARTS_FAM
  printf("FAM_LAYOUT %s cache_size=%zu fam_addr=%zu/%zu "
         "db_size=%zu payload_pending=%zu stub_end=%zu\n",
         FAM_LAYOUT_LANG, sizeof(struct arts_db_cache_s),
         offsetof(struct arts_db_cache_s, fam_addr),
         sizeof(((struct arts_db_cache_s *)0)->fam_addr),
         offsetof(struct arts_db_cache_s, db_size),
         offsetof(struct arts_db_cache_s, payload_pending),
         offsetof(struct arts_db_s, lock_state));
  printf("PASS fam_cache_fields [%s]\n", FAM_LAYOUT_LANG);
#else
  printf("SKIP fam_cache_fields [%s]: the arm is not compiled here\n",
         FAM_LAYOUT_LANG);
#endif
  return 0;
}
