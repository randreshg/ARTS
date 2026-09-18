/* SPDX-License-Identifier: Apache-2.0
 *
 * FLUSH arts_db_acquire_is_serialized: false for every mode.
 *
 * Serialization exists so that an EDT whose acquire can WAIT on another EDT's
 * release takes its blocks in one global order and cannot deadlock against
 * another EDT.  This arm has no ownership round and no lock: an acquire waits
 * only on its own home's reply, which no EDT of this rank can withhold.  So
 * nothing here is serialized, in either acquiring mode.
 *
 * pure_unit: links the real symbol out of the per-config static libarts and
 * calls only the pure query; the runtime is never started.
 */

#include "arts.h" /* arts_db_access_mode_t, DB_MODE_* */

#include <stdbool.h>
#include <stdio.h>

/* Declared in coherence.h; defined per-protocol in the coherence TUs linked
 * from libarts.  Re-declared here to keep the test header-light. */
bool arts_db_acquire_is_serialized(arts_db_access_mode_t mode);

#if !defined(ARTS_PROTOCOL_FLUSH)
int main(void) {
  printf("SKIP wrf_flush_is_serialized: FLUSH-only\n");
  return 0;
}
#else
int main(void) {
  const arts_db_access_mode_t modes[] = {DB_MODE_NULL, DB_MODE_RO, DB_MODE_RW};
  const char *names[] = {"NULL", "RO", "RW"};
  const bool want[] = {false, false, false};
  int rc = 0;
  for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
    bool got = arts_db_acquire_is_serialized(modes[i]);
    if (got != want[i]) {
      (void)fprintf(stderr,
                    "FAIL wrf_flush_is_serialized: mode %s got %d want %d\n",
                    names[i], (int)got, (int)want[i]);
      rc = 1;
    }
  }
  if (rc != 0) {
    return 1;
  }
  printf("PASS wrf_flush_is_serialized: RO=0 RW=0 (NULL=false)\n");
  return 0;
}
#endif
