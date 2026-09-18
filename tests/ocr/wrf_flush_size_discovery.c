/* A block named by a reserved GUID carries no size in its name, so the first
 * rank to fetch it cannot size a landing: it asks with none, is answered with
 * the size alone, and asks again with a landing of that size.  Every later
 * fetch on that rank knows the size and takes the one-round path.
 *
 * Both access modes take their first fetch on a rank that has never seen the
 * block — the reader on one rank, the writer on another where there is one —
 * and the bytes they are handed, and the bytes the writer sends back, must be
 * the whole block: a size-discovery round that lost the payload or landed a
 * partial one shows as a byte index.  The size is not a power of two, so a
 * landing sized from anything but the answer is a wrong index too.
 */
#include <stdbool.h>
#include <stdint.h>

#include "arts.h"
#include "../test_failure_status.h"

#define DB_BYTES 6000u

static uint8_t first_byte(unsigned int i) { return (uint8_t)(i * 7u + 1u); }
static uint8_t second_byte(unsigned int i) { return (uint8_t)(i * 13u + 5u); }

static bool holds(const uint8_t *buf, uint8_t (*want)(unsigned int),
                  const char *who) {
  if (buf == NULL) {
    arts_test_fail();
    arts_printf("FAIL: DB_WRF %s got NULL storage\n", who);
    return false;
  }
  for (unsigned int i = 0; i < DB_BYTES; i++) {
    if (buf[i] != want(i)) {
      arts_test_fail();
      arts_printf("FAIL: DB_WRF %s byte %u got=%u expected=%u\n", who, i,
                  (unsigned)buf[i], (unsigned)want(i));
      return false;
    }
  }
  return true;
}

static void reader_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  (void)holds((const uint8_t *)depv[0].ptr, first_byte, "reader");
}

static void writer_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  uint8_t *buf = (uint8_t *)depv[0].ptr;
  if (!holds(buf, first_byte, "writer")) {
    return;
  }
  for (unsigned int i = 0; i < DB_BYTES; i++) {
    buf[i] = second_byte(i);
  }
}

static void final_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  if (holds((const uint8_t *)depv[0].ptr, second_byte, "final reader")) {
    arts_printf("DB_WRF: PASS bytes=%u\n", DB_BYTES);
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
  unsigned int ranks = arts_get_total_ranks();
  unsigned int reader_rank = (ranks > 1) ? 1u : 0u;
  unsigned int writer_rank = (ranks > 2) ? 2u : reader_rank;

  /* Reserved at this rank, so the create is local and the block's home is
   * the rank that fills it. */
  arts_guid_t label = arts_guid_reserve(ARTS_GUID_DB, arts_get_current_rank());
  arts_db_hint_t hint = ARTS_DB_HINT_DEFAULTS;
  hint.guid = label;
  void *addr = NULL;
  arts_db_create(&addr, DB_BYTES, ARTS_DB, ARTS_DB_PROP_NONE, &hint);
  if (addr == NULL) {
    arts_test_fail();
    arts_printf("FAIL: DB_WRF labeled create handed no storage\n");
    arts_shutdown();
    return;
  }
  for (unsigned int i = 0; i < DB_BYTES; i++) {
    ((uint8_t *)addr)[i] = first_byte(i);
  }
  /* The create's write acquisition ends before anything depends on the
   * label. */
  arts_db_release(label, DB_MODE_RW);

  arts_guid_t shut = arts_edt_create(shutdown_edt, 0, NULL, 1, NULL);
  arts_guid_t scope = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_add_dependence(scope, shut, 0, DB_MODE_NULL);

  arts_guid_t read_done = arts_event_create(&ARTS_EVENT_HINT_ONCE);
  arts_guid_t write_done = arts_event_create(&ARTS_EVENT_HINT_ONCE);

  arts_guid_t fin = arts_edt_create(
      final_edt, 0, NULL, 2,
      &(arts_edt_hint_t){.rank = 0, .finish_event = scope});
  arts_add_dependence(label, fin, 0, DB_MODE_RO);
  arts_add_dependence(write_done, fin, 1, DB_MODE_NULL);

  arts_guid_t wtr = arts_edt_create(
      writer_edt, 0, NULL, 2,
      &(arts_edt_hint_t){.rank = writer_rank, .finish_event = scope,
                         .output_event = write_done});
  arts_add_dependence(label, wtr, 0, DB_MODE_RW);
  arts_add_dependence(read_done, wtr, 1, DB_MODE_NULL);

  arts_guid_t rdr = arts_edt_create(
      reader_edt, 0, NULL, 1,
      &(arts_edt_hint_t){.rank = reader_rank, .finish_event = scope,
                         .output_event = read_done});
  arts_add_dependence(label, rdr, 0, DB_MODE_RO);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
