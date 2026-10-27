/* A data block's GUID passed through a NULL-mode dependence is an opaque
 * value: the slot keeps the GUID and mode and is never acquired, whether the
 * block is live or already destroyed when the EDT becomes ready, and on every
 * rank.  Beside a real dependence on the same block, releasing the block
 * releases the real slot and leaves the opaque one as it was.  Each case runs
 * in a finish scope of its own, and the next case starts in a continuation
 * gated on it. */

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
  arts_guid_t db = arts_db_create((void **)&p, sizeof(*p), ARTS_DB,
                                  ARTS_DB_PROP_NONE, NULL);
  *p = value;
  arts_db_release(db, DB_MODE_RW);
  return db;
}

/* A case's number, and what the previous case leaves to retire once its scope
 * drained: its source event and its block (NULL_GUID for none). */
enum { P_CASE, P_SOURCE, P_DATA, P_COUNT };

static void step_edt(uint32_t pc, const uint64_t *pv, uint32_t dc,
                     arts_edt_dep_t dv[]);

/* Cases 0 .. 2n-1 are the control lane, live (retired 0) then retired, one
 * per rank; cases 2n .. 3n-1 the release lane, one per rank. */
static void step(const uint64_t *pv) {
  if ((arts_guid_t)pv[P_SOURCE] != NULL_GUID)
    arts_event_destroy((arts_guid_t)pv[P_SOURCE]);
  if ((arts_guid_t)pv[P_DATA] != NULL_GUID)
    arts_db_destroy((arts_guid_t)pv[P_DATA]);
  unsigned int ranks = arts_get_total_ranks();
  uint64_t c = pv[P_CASE];
  uint64_t next[P_COUNT] = {c + 1u, NULL_GUID, NULL_GUID};
  arts_guid_t finish;
  if (c < 2u * ranks) {
    unsigned int retired = (unsigned int)(c / ranks);
    unsigned int rank = (unsigned int)(c % ranks);
    arts_guid_t data = make_block(42);
    finish = arts_event_create(&ARTS_EVENT_HINT_FINISH);
    arts_guid_t source = arts_event_create(&ARTS_EVENT_HINT_DEFAULTS);
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
    next[P_SOURCE] = (uint64_t)source;
    next[P_DATA] = retired ? NULL_GUID : (uint64_t)data;
  } else if (c < 3u * ranks) {
    unsigned int rank = (unsigned int)(c - 2u * ranks);
    arts_guid_t db = make_block(0);
    finish = arts_event_create(&ARTS_EVENT_HINT_FINISH);
    arts_edt_hint_t hint = ARTS_EDT_HINT_DEFAULTS;
    hint.rank = rank;
    hint.finish_event = finish;
    arts_guid_t sink = arts_edt_create(check_release, 0, NULL, 2, &hint);
    arts_edt_satisfy_slot(sink, 0, db, DB_MODE_NULL);
    arts_add_dependence(db, sink, 1, DB_MODE_RW);
    next[P_DATA] = (uint64_t)db;
  } else {
    arts_printf("PASS: live and retired NULL control dependencies, and a "
                "release that leaves an equal opaque value alone\n");
    arts_shutdown();
    return;
  }
  arts_guid_t then = arts_edt_create(step_edt, P_COUNT, next, 1,
                                     &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(finish, then, 0, DB_MODE_NULL);
}

static void step_edt(uint32_t pc, const uint64_t *pv, uint32_t dc,
                     arts_edt_dep_t dv[]) {
  (void)pc;
  (void)dc;
  (void)dv;
  step(pv);
}

void main_edt(uint32_t pc, const uint64_t *pv, uint32_t dc,
              arts_edt_dep_t dv[]) {
  (void)pc;
  (void)pv;
  (void)dc;
  (void)dv;
  step((const uint64_t[P_COUNT]){0u, NULL_GUID, NULL_GUID});
}

int main(int argc, char **argv) { return arts_rt(argc, argv) != 0; }
