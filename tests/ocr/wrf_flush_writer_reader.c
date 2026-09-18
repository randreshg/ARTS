/* A remote writer's release must reach a later reader on the home.  The
 * writer runs inside a finish scope whose finish EDT is the reader, so the
 * two write-side and read-side acquisitions are ordered by the scope. */
#include <stdint.h>

#include "arts.h"
#include "../test_failure_status.h"

#define N_ITERS 16

static void writer_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  uint64_t *v = (uint64_t *)depv[0].ptr;
  if (v == NULL) { arts_test_fail(); arts_printf("FAIL: DB_WRF writer got NULL\n"); return; }
  for (int i = 0; i < N_ITERS; i++) { (*v)++; }
}

static void reader_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  uint64_t *v = (uint64_t *)depv[0].ptr;
  uint64_t got = v ? *v : (uint64_t)-1;
  if (got == (uint64_t)N_ITERS) {
    arts_printf("DB_WRF: PASS value=%llu\n", (unsigned long long)got);
  } else {
    arts_test_fail();
    arts_printf("FAIL: DB_WRF got=%llu expected=%d\n", (unsigned long long)got, N_ITERS);
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
  unsigned int writer_rank = (arts_get_total_ranks() > 1) ? 1 : 0;
  void *addr = NULL;
  arts_guid_t db = arts_db_create(&addr, sizeof(uint64_t), ARTS_DB, ARTS_DB_PROP_NONE,
                                  &(arts_db_hint_t){.rank = 0});
  *(uint64_t *)addr = 0;
  arts_db_release(db, DB_MODE_RW);

  arts_guid_t shut = arts_edt_create(shutdown_edt, 0, NULL, 1, NULL);
  arts_guid_t outer = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_add_dependence(outer, shut, 0, DB_MODE_NULL);

  arts_guid_t rdr = arts_edt_create(reader_edt, 0, NULL, 2,
                                    &(arts_edt_hint_t){.rank = 0, .finish_event = outer});
  arts_add_dependence(db, rdr, 0, DB_MODE_RO);
  arts_guid_t inner = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_add_dependence(inner, rdr, 1, DB_MODE_NULL);

  arts_guid_t wtr = arts_edt_create(writer_edt, 0, NULL, 1,
                                    &(arts_edt_hint_t){.rank = writer_rank, .finish_event = inner});
  arts_add_dependence(db, wtr, 0, DB_MODE_RW);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
