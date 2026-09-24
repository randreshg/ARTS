/* SPDX-License-Identifier: Apache-2.0
 *
 * T292 — ARTS_EVENT_LATCH_INCR_SLOT + explicit latch=N accounting.
 *
 * Target: the latch-counter arithmetic behind arts_event_satisfy_slot's
 * ARTS_EVENT_LATCH_INCR_SLOT (increment) vs ARTS_EVENT_LATCH_DECR_SLOT
 * (decrement), with a non-default initial latch.  The event fires when
 * curr_latch reaches <= 0.
 *
 * Every DECR carries its own ordinal as data, and a dependent bound before the
 * first satisfy receives the data of the DECR that fired the event, so the
 * dependent names the exact DECR that fired it:
 *   - latch = 3, one INCR (3 -> 4), four DECR: it must be fired by the FOURTH
 *     DECR.  Fired by the third means the INCR was lost.
 *   - control, latch = 3 with no INCR, three DECR: it must be fired by the
 *     THIRD DECR — without the INCR the latch reaches zero one DECR earlier.
 *   - fire-and-linger: a dependent bound to the first event AFTER it fired is
 *     satisfied at once from the stored fire state, with the same data.
 * Each dependent runs exactly once; a finalizer gated on a finish scope reads
 * the recorded data back.
 *
 * Config-agnostic single-node public-API check.
 */
#include "arts.h"
#include <stdatomic.h>
#include <stdint.h>

static int g_failed = 0;

enum { SEEN_INCR, SEEN_LINGER, SEEN_NO_INCR, SEEN_COUNT };

typedef struct {
  _Atomic unsigned int fires;
  _Atomic uint64_t seen[SEEN_COUNT];
} record_t;

#define DECR_DATA(i) ((arts_guid_t)(0x100 + (i)))

/* depv[0] = record RW, depv[1] = the latch event; paramv[0] = record index. */
void fire_counter_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  record_t *rec = (record_t *)depv[0].ptr;
  if (rec) {
    atomic_fetch_add_explicit(&rec->fires, 1u, memory_order_relaxed);
    atomic_store_explicit(&rec->seen[paramv[0]], (uint64_t)depv[1].guid,
                          memory_order_relaxed);
  }
}

static void expect_seen(const record_t *rec, unsigned int idx, int decr,
                        const char *what) {
  uint64_t got = atomic_load_explicit(&rec->seen[idx], memory_order_relaxed);
  if (got != (uint64_t)DECR_DATA(decr)) {
    arts_printf("FAIL api_latch_incr_slot: %s fired by data 0x%lx, want the "
                "DECR #%d (0x%lx)\n",
                what, (unsigned long)got, decr,
                (unsigned long)DECR_DATA(decr));
    g_failed = 1;
  }
}

/* Finalizer gated on the finish scope: every dependent has run by now. */
void check_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  const record_t *rec = (const record_t *)depv[0].ptr;
  unsigned int n = atomic_load_explicit(&rec->fires, memory_order_relaxed);
  if (n != SEEN_COUNT) {
    arts_printf("FAIL api_latch_incr_slot: dependents fired %u times, "
                "expected %d\n",
                n, SEEN_COUNT);
    g_failed = 1;
  }
  expect_seen(rec, SEEN_INCR, 4, "latch=3 + INCR");
  expect_seen(rec, SEEN_LINGER, 4, "the late binder of latch=3 + INCR");
  expect_seen(rec, SEEN_NO_INCR, 3, "latch=3 without INCR");
  if (!g_failed) {
    arts_printf("PASS api_latch_incr_slot: INCR held the fire to the 4th "
                "DECR, no INCR fired at the 3rd, linger re-bind fired once\n");
  }
  arts_shutdown();
}

static arts_guid_t bind_dependent(arts_guid_t rec_db, arts_guid_t ev,
                                  arts_guid_t fe, uint64_t idx) {
  arts_edt_hint_t dh = ARTS_EDT_HINT_DEFAULTS;
  dh.finish_event = fe;
  arts_guid_t d = arts_edt_create(fire_counter_edt, 1, &idx, 2, &dh);
  arts_add_dependence(rec_db, d, 0, DB_MODE_RW);
  arts_add_dependence(ev, d, 1, DB_MODE_NULL);
  return d;
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== api_latch_incr_slot ===\n");

  void *rp = NULL;
  arts_guid_t rec_db = arts_db_create(&rp, sizeof(record_t), ARTS_DB,
                                      ARTS_DB_PROP_NONE, NULL);
  record_t *rec = (record_t *)rp;
  atomic_init(&rec->fires, 0u);
  for (int i = 0; i < SEEN_COUNT; i++) {
    atomic_init(&rec->seen[i], 0u);
  }
  arts_db_release(rec_db, DB_MODE_RW);

  arts_guid_t fe = arts_event_create(&ARTS_EVENT_HINT_FINISH);

  arts_event_hint_t eh = ARTS_EVENT_HINT_DEFAULTS;
  eh.latch = 3;

  /* latch = 3, one INCR, four DECR. */
  arts_guid_t ev = arts_event_create(&eh);
  (void)bind_dependent(rec_db, ev, fe, SEEN_INCR);
  arts_event_satisfy_slot(ev, NULL_GUID, ARTS_EVENT_LATCH_INCR_SLOT);
  for (int i = 1; i <= 4; i++) {
    arts_event_satisfy_slot(ev, DECR_DATA(i), ARTS_EVENT_LATCH_DECR_SLOT);
  }
  /* Bound after the fire: fire-and-linger satisfies it from the stored
   * state. */
  (void)bind_dependent(rec_db, ev, fe, SEEN_LINGER);
  arts_event_destroy(ev);

  /* Control: latch = 3, no INCR, three DECR. */
  arts_guid_t ctl = arts_event_create(&eh);
  (void)bind_dependent(rec_db, ctl, fe, SEEN_NO_INCR);
  for (int i = 1; i <= 3; i++) {
    arts_event_satisfy_slot(ctl, DECR_DATA(i), ARTS_EVENT_LATCH_DECR_SLOT);
  }
  arts_event_destroy(ctl);

  arts_edt_hint_t ch = ARTS_EDT_HINT_DEFAULTS;
  arts_guid_t chk = arts_edt_create(check_edt, 0, NULL, 2, &ch);
  arts_add_dependence(rec_db, chk, 0, DB_MODE_RO);
  arts_add_dependence(fe, chk, 1, DB_MODE_NULL);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : g_failed;
}
