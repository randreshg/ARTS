/* A read acquisition that lands while a write acquisition of the same block is
 * running on the same rank must not disturb the writer's bytes.
 *
 * The order is what makes this an oracle.  The writer WRITES FIRST, then hands
 * a read-side event off, then keeps the write acquisition open long enough for
 * the reader's payload to arrive.  So the reader's bytes — the block's earlier
 * value, still what the home holds — land after the write and before the
 * release.  Storage shared between the two acquirers would therefore be
 * clobbered back to that earlier value and the release would write THAT home,
 * which the verifier ordered after the release sees.  Storage private to each
 * acquirer cannot be: the landing has nowhere to touch the writer's bytes.
 *
 * Reads are unconstrained by the model, so the overlap is a legal program, and
 * the writer's is the block's only write acquisition.
 */
#include <stdint.h>
#include <time.h>

#include "arts.h"
#include "../test_failure_status.h"

#define SEED 0x1111u
#define WRITTEN 0xABCDu
#define OVERLAP_NS 50000000L /* 50 ms */

static void spin_ns(long ns) {
  struct timespec t0, now;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  do {
    clock_gettime(CLOCK_MONOTONIC, &now);
  } while ((now.tv_sec - t0.tv_sec) * 1000000000L + (now.tv_nsec - t0.tv_nsec) <
           ns);
}

/* paramv = { go event }. */
static void writer_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc; (void)depc;
  arts_guid_t go = (arts_guid_t)paramv[0];
  uint64_t *v = (uint64_t *)depv[0].ptr;
  if (v == NULL) {
    arts_test_fail();
    arts_printf("FAIL: DB_WRF writer got NULL\n");
    arts_event_satisfy(go, NULL_GUID);
    return;
  }
  /* Write first, so a landing that arrives during the spin below can only
   * destroy this value, never race ahead of it. */
  *v = WRITTEN;
  arts_event_satisfy(go, NULL_GUID);
  /* The acquisition stays open across the spin: the reader's request is
   * issued now and its payload lands well inside this window. */
  spin_ns(OVERLAP_NS);
}

/* Only its timing matters: the request leaves while the writer holds the
 * block, so a shared line would be written under the writer's feet. */
static void overlap_reader_edt(uint32_t paramc, const uint64_t *paramv,
                               uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  const volatile uint64_t *v = (const volatile uint64_t *)depv[0].ptr;
  if (v != NULL) {
    (void)*v;
  }
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

  arts_guid_t go = arts_event_create(&ARTS_EVENT_HINT_ONCE);
  arts_guid_t written = arts_event_create(&ARTS_EVENT_HINT_ONCE);

  arts_guid_t ver = arts_edt_create(
      verify_edt, 0, NULL, 2,
      &(arts_edt_hint_t){.rank = 0, .finish_event = scope});
  arts_add_dependence(db, ver, 0, DB_MODE_RO);
  arts_add_dependence(written, ver, 1, DB_MODE_NULL);

  arts_guid_t rdr = arts_edt_create(
      overlap_reader_edt, 0, NULL, 2,
      &(arts_edt_hint_t){.rank = away, .finish_event = scope});
  arts_add_dependence(db, rdr, 0, DB_MODE_RO);
  arts_add_dependence(go, rdr, 1, DB_MODE_NULL);

  uint64_t pv[1] = {(uint64_t)go};
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
