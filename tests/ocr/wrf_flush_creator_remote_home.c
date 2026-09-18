/* A block created away from its home.  The creating EDT's hold is a write
 * acquisition like any other: its release must reach the home before the next
 * write acquisition begins, and that next one must take its value from the
 * home rather than from anything the creating rank still holds.
 *
 * The chain is creator hold -> release -> home writer -> reader -> a second
 * writer back on the creating rank -> reader.  Every write acquisition is
 * separated from the next by a release and an event, so the program is
 * exclusive-write by construction and the value at each step is defined.
 */
#include <stdint.h>

#include "arts.h"
#include "../test_failure_status.h"

#define SEED 7u
#define AFTER_HOME_WRITE 8u
#define AFTER_REWRITE 9u

static void fail_value(const char *what, uint64_t got, uint64_t want) {
  arts_test_fail();
  arts_printf("FAIL: DB_WRF %s got=%llu expected=%llu\n", what,
              (unsigned long long)got, (unsigned long long)want);
}

/* Runs at the home: the creator's release must already be in the line. */
static void home_writer_edt(uint32_t paramc, const uint64_t *paramv,
                            uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  uint64_t *v = (uint64_t *)depv[0].ptr;
  if (v == NULL) {
    arts_test_fail();
    arts_printf("FAIL: DB_WRF home writer got NULL\n");
    return;
  }
  if (*v != SEED) {
    fail_value("home writer", *v, SEED);
    return;
  }
  *v = AFTER_HOME_WRITE;
}

static void reader_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  uint64_t *v = (uint64_t *)depv[0].ptr;
  uint64_t got = v ? *v : (uint64_t)-1;
  if (got != AFTER_HOME_WRITE) {
    fail_value("reader", got, AFTER_HOME_WRITE);
  }
}

/* A second write acquisition on the creating rank, after that rank's own
 * creator hold was released.  It must see the home's current value: a copy
 * kept from the create would read the seed. */
static void rewriter_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                         arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  uint64_t *v = (uint64_t *)depv[0].ptr;
  if (v == NULL) {
    arts_test_fail();
    arts_printf("FAIL: DB_WRF rewriter got NULL\n");
    return;
  }
  if (*v != AFTER_HOME_WRITE) {
    fail_value("rewriter (stale creator copy?)", *v, AFTER_HOME_WRITE);
    return;
  }
  *v = AFTER_REWRITE;
}

static void verify_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  uint64_t *v = (uint64_t *)depv[0].ptr;
  uint64_t got = v ? *v : (uint64_t)-1;
  if (got == (uint64_t)AFTER_REWRITE) {
    arts_printf("DB_WRF: PASS value=%llu\n", (unsigned long long)got);
  } else {
    fail_value("verify", got, AFTER_REWRITE);
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
  unsigned int home = (arts_get_total_ranks() > 1) ? 1u : 0u;

  void *addr = NULL;
  arts_guid_t db = arts_db_create(&addr, sizeof(uint64_t), ARTS_DB,
                                  ARTS_DB_PROP_NONE,
                                  &(arts_db_hint_t){.rank = home});
  *(uint64_t *)addr = SEED;
  /* The write acquisition the create took ends here, before any dependence on
   * the block is asked for. */
  arts_db_release(db, DB_MODE_RW);

  arts_guid_t shut = arts_edt_create(shutdown_edt, 0, NULL, 1, NULL);
  arts_guid_t scope = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_add_dependence(scope, shut, 0, DB_MODE_NULL);

  arts_guid_t after_home = arts_event_create(&ARTS_EVENT_HINT_ONCE);
  arts_guid_t after_read = arts_event_create(&ARTS_EVENT_HINT_ONCE);
  arts_guid_t after_rewrite = arts_event_create(&ARTS_EVENT_HINT_ONCE);

  /* Created back to front so every consumer of an event exists before the EDT
   * that satisfies it. */
  arts_guid_t ver = arts_edt_create(
      verify_edt, 0, NULL, 2,
      &(arts_edt_hint_t){.rank = 0, .finish_event = scope});
  arts_add_dependence(db, ver, 0, DB_MODE_RO);
  arts_add_dependence(after_rewrite, ver, 1, DB_MODE_NULL);

  arts_guid_t rew = arts_edt_create(
      rewriter_edt, 0, NULL, 2,
      &(arts_edt_hint_t){.rank = 0, .finish_event = scope,
                         .output_event = after_rewrite});
  arts_add_dependence(db, rew, 0, DB_MODE_RW);
  arts_add_dependence(after_read, rew, 1, DB_MODE_NULL);

  arts_guid_t rdr = arts_edt_create(
      reader_edt, 0, NULL, 2,
      &(arts_edt_hint_t){.rank = 0, .finish_event = scope,
                         .output_event = after_read});
  arts_add_dependence(db, rdr, 0, DB_MODE_RO);
  arts_add_dependence(after_home, rdr, 1, DB_MODE_NULL);

  arts_guid_t hw = arts_edt_create(
      home_writer_edt, 0, NULL, 1,
      &(arts_edt_hint_t){.rank = home, .finish_event = scope,
                         .output_event = after_home});
  arts_add_dependence(db, hw, 0, DB_MODE_RW);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
