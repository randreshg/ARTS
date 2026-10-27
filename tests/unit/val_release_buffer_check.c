/* SPDX-License-Identifier: Apache-2.0
 *
 * val_release_buffer_check — a VAL write release bumps the version of the
 * buffer its write turn's payload names, not of a fresh load of the cache's
 * slot: while a rank holds a write turn the installed buffer is that one.  A
 * Debug build checks it at every release that names a payload, and a release
 * that names another buffer ends the process with the named fatal error
 * (ARTS_ERROR: exit status 1).
 *
 * One EDT holds the write turns of two blocks and releases the first with the
 * second's payload; the run must end in the error, never reach the PASS-less
 * shutdown after it.
 *
 * Whitebox: it calls the arm's release directly on the block's descriptor.
 */
#include "arts.h"

#include <stdint.h>

#include "arts/coherence/coherence.h"
#include "arts/db.h"
#include "arts/gas/route_table.h"
#include "arts/utils/shared.h"

static void writer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  arts_shared_ptr_t h = arts_route_table_lookup_db(depv[0].guid);
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(h);
  if (db == NULL || depv[1].ptr == NULL) {
    arts_printf("FAIL: val_release_buffer_check found no block to release\n");
    arts_abort(1);
  }
  arts_db_release_rw(&db->cache, depv[1].ptr);
  arts_shared_release(&h);
  arts_printf("FAIL: val_release_buffer_check: a release naming another "
              "buffer was let through\n");
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  void *pa = NULL, *pb = NULL;
  arts_guid_t a = arts_db_create(&pa, 4096, ARTS_DB, ARTS_DB_PROP_NONE,
                                 &(arts_db_hint_t){.rank = 0});
  arts_guid_t b = arts_db_create(&pb, 4096, ARTS_DB, ARTS_DB_PROP_NONE,
                                 &(arts_db_hint_t){.rank = 0});
  arts_db_release(a, DB_MODE_RW);
  arts_db_release(b, DB_MODE_RW);
  arts_guid_t w =
      arts_edt_create(writer, 0, NULL, 2, &(arts_edt_hint_t){.rank = 0});
  arts_add_dependence(a, w, 0, DB_MODE_RW);
  arts_add_dependence(b, w, 1, DB_MODE_RW);
}

int main(int argc, char **argv) {
  /* Non-zero when a rank this process spawned ended badly. */
  return arts_rt(argc, argv) != 0 ? 1 : 0;
}
