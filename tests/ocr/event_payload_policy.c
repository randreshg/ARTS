/* An event delivers its payload as an opaque value through a NULL-mode
 * dependence: never acquired, whatever kind of GUID the value happens to be.
 *
 * Policy lane: a latch event created on each rank (with and without
 * discard_data), its sink on the next rank, bound before or after the fire;
 * the sink sees the payload, or NULL_GUID where the event discards it.
 * Value lane: a single-fire event on each rank carries values of every shape
 * — zero, small, the GUID of a data block and of an event (neither of which
 * the sink may acquire), all ones, a value with high bits set — and the sink
 * on the same rank sees each one unchanged.  Each case runs in a finish scope
 * of its own, and the next case starts in a continuation gated on it. */

#include "arts.h"

#define POLICY_CASES 4u /* discard x late */
#define VALUE_CASES 6u  /* payload shapes */

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

/* A case's number, the event the previous case leaves to destroy once its
 * scope drained, and the value lane's per-rank block GUID and opaque event. */
enum { P_CASE, P_EVENT, P_BLOCK, P_OPAQUE, P_COUNT };

static void step_edt(uint32_t pc, const uint64_t *pv, uint32_t dc,
                     arts_edt_dep_t dv[]);

static arts_guid_t policy_case(unsigned int rank, unsigned int discard,
                               unsigned int late, arts_guid_t *event_out) {
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
  *event_out = event;
  return finish;
}

static arts_guid_t value_case(unsigned int rank, uint64_t payload,
                              arts_guid_t *event_out) {
  arts_guid_t finish = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_event_hint_t hint = ARTS_EVENT_HINT_DEFAULTS;
  hint.rank = rank;
  arts_guid_t source = arts_event_create(&hint);
  arts_guid_t sink =
      arts_edt_create(check, 1, &payload, 2,
                      &(arts_edt_hint_t){.rank = rank, .finish_event = finish});
  arts_add_dependence(source, sink, 0, DB_MODE_NULL);
  arts_event_satisfy(source, (arts_guid_t)payload);
  arts_edt_satisfy_slot(sink, 1, NULL_GUID, DB_MODE_NULL);
  *event_out = source;
  return finish;
}

/* Cases 0 .. 4n-1 are the policy lane (rank, then discard, then late);
 * cases 4n .. 10n-1 the value lane (rank, then payload). */
static void step(const uint64_t *pv) {
  unsigned int ranks = arts_get_total_ranks();
  uint64_t c = pv[P_CASE];
  if ((arts_guid_t)pv[P_EVENT] != NULL_GUID)
    arts_event_destroy((arts_guid_t)pv[P_EVENT]);
  uint64_t next[P_COUNT] = {c + 1u, NULL_GUID, pv[P_BLOCK], pv[P_OPAQUE]};
  uint64_t policy_end = (uint64_t)POLICY_CASES * ranks;
  if (c > policy_end && (c - policy_end) % VALUE_CASES == 0u) {
    /* The previous case was its rank's last value case. */
    arts_event_destroy((arts_guid_t)pv[P_OPAQUE]);
  }
  arts_guid_t finish;
  arts_guid_t event;
  if (c < policy_end) {
    unsigned int k = (unsigned int)(c % POLICY_CASES);
    finish =
        policy_case((unsigned int)(c / POLICY_CASES), k / 2u, k % 2u, &event);
  } else if (c < policy_end + (uint64_t)VALUE_CASES * ranks) {
    unsigned int rank = (unsigned int)((c - policy_end) / VALUE_CASES);
    unsigned int i = (unsigned int)((c - policy_end) % VALUE_CASES);
    if (i == 0u) {
      next[P_BLOCK] = (uint64_t)arts_guid_reserve(ARTS_GUID_DB, rank);
      next[P_OPAQUE] = (uint64_t)arts_event_create(&ARTS_EVENT_HINT_DEFAULTS);
    }
    const uint64_t payloads[VALUE_CASES] = {0,
                                            42,
                                            next[P_BLOCK],
                                            next[P_OPAQUE],
                                            UINT64_MAX,
                                            UINT64_C(0x4000000000000001)};
    finish = value_case(rank, payloads[i], &event);
  } else {
    arts_printf("PASS: event payloads survive remote creation and binding, "
                "and arrive as opaque values without an acquire\n");
    arts_shutdown();
    return;
  }
  next[P_EVENT] = (uint64_t)event;
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
  step((const uint64_t[P_COUNT]){0u, NULL_GUID, NULL_GUID, NULL_GUID});
}

int main(int argc, char **argv) { return arts_rt(argc, argv) != 0; }
