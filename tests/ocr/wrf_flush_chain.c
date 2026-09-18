/* A long strictly ordered chain of remote write acquisitions on one block.
 *
 * Each link reads the counter and writes it back one higher, and the next link
 * starts only once the previous one has completed and released, so exactly one
 * write acquisition is live at a time and the final value is the link count.
 * A write-back that is dropped, applied twice, or applied out of turn shows up
 * as a wrong total; a release that never returns leaves the chain stalled.
 */
#include <stdint.h>

#include "arts.h"
#include "../test_failure_status.h"

#define N_LINKS 200

static void link_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  uint64_t *v = (uint64_t *)depv[0].ptr;
  if (v == NULL) {
    arts_test_fail();
    arts_printf("FAIL: DB_WRF chain link got NULL\n");
    return;
  }
  *v = *v + 1u;
}

static void reader_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  uint64_t *v = (uint64_t *)depv[0].ptr;
  uint64_t got = v ? *v : (uint64_t)-1;
  if (got == (uint64_t)N_LINKS) {
    arts_printf("DB_WRF: PASS value=%llu\n", (unsigned long long)got);
  } else {
    arts_test_fail();
    arts_printf("FAIL: DB_WRF chain got=%llu expected=%d\n",
                (unsigned long long)got, N_LINKS);
  }
}

static void shutdown_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                         arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc; (void)depv;
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc; (void)depv;
  unsigned int writer_rank = (arts_get_total_ranks() > 1) ? 1u : 0u;

  void *addr = NULL;
  arts_guid_t db = arts_db_create(&addr, sizeof(uint64_t), ARTS_DB,
                                  ARTS_DB_PROP_NONE,
                                  &(arts_db_hint_t){.rank = 0});
  *(uint64_t *)addr = 0;
  arts_db_release(db, DB_MODE_RW);

  arts_guid_t shut = arts_edt_create(shutdown_edt, 0, NULL, 1, NULL);
  arts_guid_t scope = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_add_dependence(scope, shut, 0, DB_MODE_NULL);

  arts_guid_t start = arts_event_create(&ARTS_EVENT_HINT_ONCE);
  arts_guid_t done[N_LINKS];
  for (int i = 0; i < N_LINKS; i++) {
    done[i] = arts_event_create(&ARTS_EVENT_HINT_ONCE);
  }

  arts_guid_t rdr = arts_edt_create(
      reader_edt, 0, NULL, 2,
      &(arts_edt_hint_t){.rank = 0, .finish_event = scope});
  arts_add_dependence(db, rdr, 0, DB_MODE_RO);
  arts_add_dependence(done[N_LINKS - 1], rdr, 1, DB_MODE_NULL);

  /* Back to front: the event a link waits on is created by the link made
   * after it, so every consumer exists before its producer can fire. */
  for (int i = N_LINKS - 1; i >= 0; i--) {
    arts_guid_t e = arts_edt_create(
        link_edt, 0, NULL, 2,
        &(arts_edt_hint_t){.rank = writer_rank, .finish_event = scope,
                           .output_event = done[i]});
    arts_add_dependence(db, e, 0, DB_MODE_RW);
    arts_add_dependence((i == 0) ? start : done[i - 1], e, 1, DB_MODE_NULL);
  }

  arts_event_satisfy(start, NULL_GUID);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
