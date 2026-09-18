/* SPDX-License-Identifier: Apache-2.0
 *
 * A remote write-back whose landing credit is gone must ask the home for one
 * and still deliver the bytes.
 *
 * A fetch hands the requester a credit for its next write-back, so the ordinary
 * release writes straight into the home's line.  The credit-less release is the
 * fallback leg (announce, then the home's clear-to-send) and nothing in an
 * ordinary run reaches it, because the fetch that preceded the release always
 * brought one.  This test spends the credit by hand from inside the write
 * acquisition — the block's own state, so no wire behaviour is faked — and the
 * value oracle behind the release then judges the fallback leg.
 *
 * Whitebox: it reads the block's descriptor through the internal route table,
 * so it is compiled per protocol and self-skips where the field does not
 * exist.
 */
#include <stdint.h>

#include "arts.h"
#include "../test_failure_status.h"

#if !defined(ARTS_PROTOCOL_FLUSH)
#include <stdio.h>
int main(void) {
  printf("SKIP wrf_flush_announce: FLUSH-only\n");
  return 0;
}
#else

#include "arts/coherence/types.h"
#include "arts/gas/route_table.h"
#include "arts/utils/shared.h"

#define SEED 0x1234u
#define WRITTEN 0x5A5Au

/* paramv = { db guid } — the body runs on a rank whose process image never saw
 * the creating EDT's locals. */
static void writer_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc; (void)depc;
  arts_guid_t db = (arts_guid_t)paramv[0];
  uint64_t *v = (uint64_t *)depv[0].ptr;
  if (v == NULL) {
    arts_test_fail();
    arts_printf("FAIL: DB_WRF announce writer got NULL\n");
    return;
  }
  *v = WRITTEN;

  arts_shared_ptr_t h = arts_route_table_lookup_db(db);
  struct arts_db_s *d = (struct arts_db_s *)arts_shared_get(h);
  if (d == NULL) {
    arts_test_fail();
    arts_printf("FAIL: DB_WRF announce writer found no descriptor\n");
  } else {
    __atomic_store_n(&d->cache.flush_txid, (uint64_t)0, __ATOMIC_RELEASE);
  }
  arts_shared_release(&h);
}

static void verify_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  uint64_t *v = (uint64_t *)depv[0].ptr;
  uint64_t got = v ? *v : (uint64_t)-1;
  if (got == (uint64_t)WRITTEN) {
    arts_printf("DB_WRF: PASS value=0x%llx\n", (unsigned long long)got);
  } else {
    arts_test_fail();
    arts_printf("FAIL: DB_WRF got=0x%llx expected=0x%x\n",
                (unsigned long long)got, WRITTEN);
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
  unsigned int away = (arts_get_total_ranks() > 1) ? 1u : 0u;

  void *addr = NULL;
  arts_guid_t db = arts_db_create(&addr, sizeof(uint64_t), ARTS_DB,
                                  ARTS_DB_PROP_NONE,
                                  &(arts_db_hint_t){.rank = 0});
  *(uint64_t *)addr = SEED;
  arts_db_release(db, DB_MODE_RW);

  arts_guid_t shut = arts_edt_create(shutdown_edt, 0, NULL, 1, NULL);
  arts_guid_t scope = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_add_dependence(scope, shut, 0, DB_MODE_NULL);

  arts_guid_t written = arts_event_create(&ARTS_EVENT_HINT_ONCE);

  arts_guid_t ver = arts_edt_create(
      verify_edt, 0, NULL, 2,
      &(arts_edt_hint_t){.rank = 0, .finish_event = scope});
  arts_add_dependence(db, ver, 0, DB_MODE_RO);
  arts_add_dependence(written, ver, 1, DB_MODE_NULL);

  uint64_t pv[1] = {(uint64_t)db};
  arts_guid_t wtr = arts_edt_create(
      writer_edt, 1, pv, 1,
      &(arts_edt_hint_t){.rank = away, .finish_event = scope,
                         .output_event = written});
  arts_add_dependence(db, wtr, 0, DB_MODE_RW);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
#endif /* ARTS_PROTOCOL_FLUSH */
