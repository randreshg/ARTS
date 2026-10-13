/* A data block's GUID passed through a NULL-mode dependence is an opaque
 * value: the slot keeps the GUID and mode and is never acquired, whether the
 * block is live or already destroyed when the EDT becomes ready, and on every
 * rank.  Beside a real dependence on the same block, releasing the block
 * releases the real slot and leaves the opaque one as it was. */

#include "arts.h"

static void check_control(uint32_t pc, const uint64_t *pv, uint32_t dc,
                          arts_edt_dep_t dv[]) {
  (void)pc;
  (void)dc;
  if (dv[0].guid != (arts_guid_t)pv[0] || dv[0].ptr != NULL ||
      dv[0].mode != DB_MODE_NULL) {
    arts_printf("FAIL: NULL dependence changed GUID, mode, or pointer\n");
    arts_abort(1);
  }
}

static void check_release(uint32_t pc, const uint64_t *pv, uint32_t dc,
                          arts_edt_dep_t dv[]) {
  (void)pc;
  (void)pv;
  (void)dc;
  arts_guid_t db = dv[1].guid;
  if (dv[0].guid != db || dv[0].ptr != NULL || dv[0].mode != DB_MODE_NULL ||
      dv[1].ptr == NULL) {
    arts_printf("FAIL: opaque GUID was acquired or hid the DB dependency\n");
    arts_abort(1);
  }
  *(uint64_t *)dv[1].ptr = 17;
  arts_db_release(db, DB_MODE_RW);
  if (dv[0].guid != db || dv[0].mode != DB_MODE_NULL || dv[1].ptr != NULL ||
      dv[1].guid != NULL_GUID) {
    arts_printf("FAIL: DB release selected the opaque-value slot\n");
    arts_abort(1);
  }
}

static arts_guid_t make_block(uint64_t value) {
  uint64_t *p;
  arts_guid_t db = arts_db_create((void **)&p, sizeof(*p), ARTS_DB_DEFAULT,
                                  ARTS_DB_PROP_NONE, NULL);
  *p = value;
  arts_db_release(db, DB_MODE_RW);
  return db;
}

void main_edt(uint32_t pc, const uint64_t *pv, uint32_t dc,
              arts_edt_dep_t dv[]) {
  (void)pc;
  (void)pv;
  (void)dc;
  (void)dv;
  for (unsigned int retired = 0; retired < 2; ++retired) {
    for (unsigned int rank = 0; rank < arts_get_total_ranks(); ++rank) {
      arts_guid_t data = make_block(42);
      arts_guid_t finish = arts_event_create(&ARTS_EVENT_HINT_FINISH);
      arts_guid_t source = arts_event_create(&ARTS_EVENT_HINT_ONCE);
      uint64_t args[] = {(uint64_t)data};
      arts_guid_t sink = arts_edt_create(
          check_control, 1, args, 2,
          &(arts_edt_hint_t){.rank = rank, .finish_event = finish});
      arts_add_dependence(source, sink, 0, DB_MODE_NULL);
      arts_event_satisfy(source, data);
      if (retired)
        arts_db_destroy(data);
      /* The sink becomes ready only after the optional retirement. */
      arts_add_dependence(NULL_GUID, sink, 1, DB_MODE_NULL);
      arts_event_wait(finish);
      arts_event_destroy(source);
      if (!retired)
        arts_db_destroy(data);
    }
  }
  for (unsigned int rank = 0; rank < arts_get_total_ranks(); ++rank) {
    arts_guid_t db = make_block(0);
    arts_guid_t finish = arts_event_create(&ARTS_EVENT_HINT_FINISH);
    arts_edt_hint_t hint = ARTS_EDT_HINT_DEFAULTS;
    hint.rank = rank;
    hint.finish_event = finish;
    arts_guid_t sink = arts_edt_create(check_release, 0, NULL, 2, &hint);
    arts_edt_satisfy_slot(sink, 0, db, DB_MODE_NULL);
    arts_add_dependence(db, sink, 1, DB_MODE_RW);
    arts_event_wait(finish);
    arts_db_destroy(db);
  }
  arts_printf("PASS: live and retired NULL control dependencies, and a "
              "release that leaves an equal opaque value alone\n");
  arts_shutdown();
}

int main(int argc, char **argv) { return arts_rt(argc, argv) != 0; }
