/* A datablock created without the creator's auto-acquire gets its payload
 * from its FIRST USER, not from its creation — and that must be invisible.
 *
 * ARTS_DB_PROP_NO_ACQUIRE says the creator neither takes the block nor
 * writes it, so there is no first user at creation time and the block's
 * storage is allocated by whichever thread first acquires it.  Two
 * properties have to survive that deferral, and this test pins both:
 *
 *   - the very first acquire of the block is served with storage, although
 *     nobody has written it and its contents are therefore unspecified;
 *   - a write to it is then seen by an event-ordered later reader, so the
 *     deferred allocation produced the block's ONE storage rather than a
 *     private copy per acquirer.
 *
 * The reader that checks the write waits on the writer's output event, which
 * the runtime satisfies only after that EDT's datablocks are released — so
 * "later" is an ordering the programming model guarantees, not a race the
 * scheduler usually wins.  The block's GUID travels in paramv: an EDT body
 * runs on whichever rank the placement hint named, and a file-scope variable
 * assigned by the creating EDT exists only in that rank's process image.
 */

#include "arts.h"
#include "../test_failure_status.h"

#include <stdint.h>

#define DB_ELEMS 512u
#define DB_BYTES (DB_ELEMS * sizeof(uint64_t))
#define STAMP(i) ((uint64_t)(i) * 7u + 3u)

static void fail(const char *what) {
  arts_printf("FAIL: %s (rank %u)\n", what, arts_get_current_rank());
  arts_test_fail();
  arts_shutdown();
}

/* depv[0] = the writer's output event, depv[1] = the block, RO. */
void verify_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  if (depv[1].ptr == NULL) {
    fail("RO acquire after the write returned no storage");
    return;
  }
  const uint64_t *p = (const uint64_t *)depv[1].ptr;
  for (unsigned i = 0; i < DB_ELEMS; i++) {
    if (p[i] != STAMP(i)) {
      arts_printf("FAIL: element %u reads %llu, expected %llu\n", i,
                  (unsigned long long)p[i], (unsigned long long)STAMP(i));
      arts_test_fail();
      arts_shutdown();
      return;
    }
  }
  arts_printf("PASS db_no_acquire_lazy\n");
  arts_shutdown();
}

/* depv[0] = the block, RW. */
void write_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  if (depv[0].ptr == NULL) {
    fail("RW acquire of a datablock created without an acquire returned no "
         "storage");
    return;
  }
  uint64_t *p = (uint64_t *)depv[0].ptr;
  for (unsigned i = 0; i < DB_ELEMS; i++) {
    p[i] = STAMP(i);
  }
}

/* depv[0] = the block, RO.  The block's first user: it gets storage, whose
 * contents nothing has defined yet. */
void first_read_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                    arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  arts_guid_t db = (arts_guid_t)paramv[0];
  unsigned int home = (unsigned int)paramv[1];

  if (depv[0].ptr == NULL) {
    fail("RO acquire of a datablock created without an acquire returned no "
         "storage");
    return;
  }

  /* A writer somewhere else again where the run is wide enough, and the
   * read-back at the block's home, so the write turn has to migrate and the
   * final read is served where the directory lives. */
  unsigned int n = arts_get_total_ranks();
  arts_guid_t written = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));

  arts_edt_hint_t wh = ARTS_EDT_HINT_DEFAULTS;
  wh.rank = (home + ((n > 2u) ? 2u : (n - 1u))) % n;
  wh.output_event = written;
  arts_guid_t w = arts_edt_create(write_edt, 0, NULL, 1, &wh);
  arts_add_dependence(db, w, 0, DB_MODE_RW);

  arts_edt_hint_t vh = ARTS_EDT_HINT_DEFAULTS;
  vh.rank = home;
  arts_guid_t v = arts_edt_create(verify_edt, 0, NULL, 2, &vh);
  arts_add_dependence(written, v, 0, DB_MODE_NULL);
  arts_add_dependence(db, v, 1, DB_MODE_RO);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  unsigned int home = arts_get_current_rank();
  unsigned int n = arts_get_total_ranks();
  arts_db_hint_t dh = ARTS_DB_HINT_DEFAULTS;
  dh.rank = home;
  void *addr = NULL;
  arts_guid_t db = arts_db_create(&addr, DB_BYTES, ARTS_DB_DEFAULT,
                                  ARTS_DB_PROP_NO_ACQUIRE, &dh);
  if (db == NULL_GUID) {
    fail("could not create the datablock");
    return;
  }
  if (addr != NULL) {
    fail("a create that acquires nothing must hand back no pointer");
    return;
  }

  /* The first user off the home rank where the run is wide enough, so the
   * block's storage is materialized by the rank that serves the request. */
  arts_edt_hint_t hint = ARTS_EDT_HINT_DEFAULTS;
  hint.rank = (home + 1u) % n;
  uint64_t pv[2] = {(uint64_t)db, (uint64_t)home};
  arts_guid_t r = arts_edt_create(first_read_edt, 2, pv, 1, &hint);
  arts_add_dependence(db, r, 0, DB_MODE_RO);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
