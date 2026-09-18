/* A block whose create takes no hold has no bytes anywhere until something
 * asks for it.  The first asker here is a remote write acquisition, so the
 * home must materialise the line when it serves that request and must take
 * the writer's bytes back at its release; a later read at the home then sees
 * the whole pattern.
 *
 * The pattern is byte-wide and covers the full extent, so a write-back that
 * moves only part of the block (or none of it) is visible as a byte index.
 */
#include <stdint.h>

#include "arts.h"
#include "../test_failure_status.h"

#define DB_BYTES 4096u

static uint8_t expected_byte(unsigned int i) { return (uint8_t)(i * 7u); }

static void writer_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  uint8_t *buf = (uint8_t *)depv[0].ptr;
  if (buf == NULL) {
    arts_test_fail();
    arts_printf("FAIL: DB_WRF writer got NULL storage\n");
    return;
  }
  for (unsigned int i = 0; i < DB_BYTES; i++) {
    buf[i] = expected_byte(i);
  }
}

static void reader_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  const uint8_t *buf = (const uint8_t *)depv[0].ptr;
  if (buf == NULL) {
    arts_test_fail();
    arts_printf("FAIL: DB_WRF reader got NULL storage\n");
    return;
  }
  for (unsigned int i = 0; i < DB_BYTES; i++) {
    if (buf[i] != expected_byte(i)) {
      arts_test_fail();
      arts_printf("FAIL: DB_WRF byte %u got=%u expected=%u\n", i,
                  (unsigned)buf[i], (unsigned)expected_byte(i));
      return;
    }
  }
  arts_printf("DB_WRF: PASS bytes=%u\n", DB_BYTES);
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
  arts_guid_t db = arts_db_create(&addr, DB_BYTES, ARTS_DB,
                                  ARTS_DB_PROP_NO_ACQUIRE,
                                  &(arts_db_hint_t){.rank = 0});
  /* No hold was taken, so there is nothing to release and no seed to write. */

  arts_guid_t shut = arts_edt_create(shutdown_edt, 0, NULL, 1, NULL);
  arts_guid_t scope = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_add_dependence(scope, shut, 0, DB_MODE_NULL);

  arts_guid_t written = arts_event_create(&ARTS_EVENT_HINT_ONCE);

  arts_guid_t rdr = arts_edt_create(
      reader_edt, 0, NULL, 2,
      &(arts_edt_hint_t){.rank = 0, .finish_event = scope});
  arts_add_dependence(db, rdr, 0, DB_MODE_RO);
  arts_add_dependence(written, rdr, 1, DB_MODE_NULL);

  arts_guid_t wtr = arts_edt_create(
      writer_edt, 0, NULL, 1,
      &(arts_edt_hint_t){.rank = writer_rank, .finish_event = scope,
                         .output_event = written});
  arts_add_dependence(db, wtr, 0, DB_MODE_RW);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
