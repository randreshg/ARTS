/* A block with no bytes still has to complete an acquire.  Nothing can be
 * fetched and nothing can be written back, so both access modes must resolve
 * the dependence to a NULL pointer and let the EDT run; a path that waits for
 * a payload that can never arrive leaves the acquirer parked and the run
 * never ends.
 *
 * The two acquisitions are chained by an output event so the write one is a
 * turn of its own.
 */
#include <stdint.h>

#include "arts.h"
#include "../test_failure_status.h"

static void ro_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  if (depv[0].ptr != NULL) {
    arts_test_fail();
    arts_printf("FAIL: DB_WRF zero-size RO acquire returned storage\n");
  }
}

static void rw_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  if (depv[0].ptr != NULL) {
    arts_test_fail();
    arts_printf("FAIL: DB_WRF zero-size RW acquire returned storage\n");
    return;
  }
  arts_printf("DB_WRF: PASS zero-size RO+RW resolved\n");
}

static void shutdown_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                         arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc; (void)depv;
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc; (void)depv;
  unsigned int acquirer = (arts_get_total_ranks() > 1) ? 1u : 0u;

  void *addr = NULL;
  arts_guid_t db = arts_db_create(&addr, 0, ARTS_DB, ARTS_DB_PROP_NONE,
                                  &(arts_db_hint_t){.rank = 0});
  arts_db_release(db, DB_MODE_RW);

  arts_guid_t shut = arts_edt_create(shutdown_edt, 0, NULL, 1, NULL);
  arts_guid_t scope = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_add_dependence(scope, shut, 0, DB_MODE_NULL);

  arts_guid_t after_ro = arts_event_create(&ARTS_EVENT_HINT_ONCE);

  arts_guid_t w = arts_edt_create(
      rw_edt, 0, NULL, 2,
      &(arts_edt_hint_t){.rank = acquirer, .finish_event = scope});
  arts_add_dependence(db, w, 0, DB_MODE_RW);
  arts_add_dependence(after_ro, w, 1, DB_MODE_NULL);

  arts_guid_t r = arts_edt_create(
      ro_edt, 0, NULL, 1,
      &(arts_edt_hint_t){.rank = acquirer, .finish_event = scope,
                         .output_event = after_ro});
  arts_add_dependence(db, r, 0, DB_MODE_RO);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
