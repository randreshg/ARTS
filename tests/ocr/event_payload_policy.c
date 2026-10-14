/* An event delivers its payload as an opaque value through a NULL-mode
 * dependence: never acquired, whatever kind of GUID the value happens to be.
 *
 * Policy lane: a latch event created on each rank (with and without
 * discard_data), its sink on the next rank, bound before or after the fire;
 * the sink sees the payload, or NULL_GUID where the event discards it.
 * Value lane: a single-fire event on each rank carries values of every shape
 * — zero, small, the GUID of a data block and of an event (neither of which
 * the sink may acquire), all ones, a value with high bits set — and the sink
 * on the same rank sees each one unchanged. */

#include "arts.h"

static void check(uint32_t pc, const uint64_t *pv, uint32_t dc,
                  arts_edt_dep_t dv[]) {
  (void)pc;
  (void)dc;
  if ((uint64_t)dv[0].guid != pv[0] || dv[0].ptr != NULL ||
      dv[0].mode != DB_MODE_NULL) {
    arts_printf("FAIL: event payload was not delivered as an opaque value\n");
    arts_abort(1);
  }
}

static void policy_lane(void) {
  for (unsigned int rank = 0; rank < arts_get_total_ranks(); ++rank) {
    for (unsigned int discard = 0; discard < 2; ++discard) {
      for (unsigned int late = 0; late < 2; ++late) {
        arts_guid_t finish = arts_event_create(&ARTS_EVENT_HINT_FINISH);
        arts_event_hint_t event_hint = ARTS_EVENT_HINT_LATCH(2);
        event_hint.rank = rank;
        event_hint.discard_data = discard != 0;
        arts_guid_t event = arts_event_create(&event_hint);
        uint64_t expected = discard ? NULL_GUID : UINT64_MAX;
        arts_edt_hint_t edt_hint = ARTS_EDT_HINT_DEFAULTS;
        edt_hint.rank = (rank + 1) % arts_get_total_ranks();
        edt_hint.finish_event = finish;
        arts_guid_t sink = arts_edt_create(check, 1, &expected, 1, &edt_hint);
        if (!late)
          arts_add_dependence(event, sink, 0, DB_MODE_NULL);
        arts_event_satisfy(event, UINT64_MAX);
        arts_event_satisfy(event, UINT64_MAX);
        if (late)
          arts_add_dependence(event, sink, 0, DB_MODE_NULL);
        arts_event_wait(finish);
        arts_event_destroy(event);
      }
    }
  }
}

static void value_lane(void) {
  for (unsigned int rank = 0; rank < arts_get_total_ranks(); ++rank) {
    arts_guid_t block = arts_guid_reserve(ARTS_GUID_DB, rank);
    arts_guid_t opaque_event = arts_event_create(&ARTS_EVENT_HINT_DEFAULTS);
    uint64_t payloads[] = {0,
                           42,
                           (uint64_t)block,
                           (uint64_t)opaque_event,
                           UINT64_MAX,
                           UINT64_C(0x4000000000000001)};
    for (unsigned int i = 0; i < sizeof(payloads) / sizeof(payloads[0]); ++i) {
      arts_guid_t finish = arts_event_create(&ARTS_EVENT_HINT_FINISH);
      arts_event_hint_t hint = ARTS_EVENT_HINT_DEFAULTS;
      hint.rank = rank;
      arts_guid_t source = arts_event_create(&hint);
      arts_guid_t sink = arts_edt_create(
          check, 1, &payloads[i], 2,
          &(arts_edt_hint_t){.rank = rank, .finish_event = finish});
      arts_add_dependence(source, sink, 0, DB_MODE_NULL);
      arts_event_satisfy(source, (arts_guid_t)payloads[i]);
      arts_edt_satisfy_slot(sink, 1, NULL_GUID, DB_MODE_NULL);
      arts_event_wait(finish);
      arts_event_destroy(source);
    }
    arts_event_destroy(opaque_event);
  }
}

void main_edt(uint32_t pc, const uint64_t *pv, uint32_t dc,
              arts_edt_dep_t dv[]) {
  (void)pc;
  (void)pv;
  (void)dc;
  (void)dv;
  policy_lane();
  value_lane();
  arts_printf("PASS: event payloads survive remote creation and binding, and "
              "arrive as opaque values without an acquire\n");
  arts_shutdown();
}

int main(int argc, char **argv) { return arts_rt(argc, argv) != 0; }
