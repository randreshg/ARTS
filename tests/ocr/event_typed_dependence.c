/* SPDX-License-Identifier: Apache-2.0
 *
 * The two typed registrations on an event: the entry a program calls states
 * what the dependent is, an EDT (arts_event_add_dependence_to_edt) or an event
 * (arts_event_add_dependence_to_event), and the event's home delivers by that
 * statement, which reaches it with the registration.
 *
 * A case is one source event, satisfied once, and one dependent that must
 * receive what it was satisfied with.  The cases are the product of:
 *   - the source's home: the calling rank, or another rank, where the
 *     registration crosses the fabric (one rank runs the first half alone);
 *   - the dependent: an EDT in each access mode (NULL carrying an opaque
 *     value, RW and RO carrying NULL_GUID, which keeps the mode and names no
 *     block), or an event that relays the value to an EDT;
 *   - when the registration is made:
 *       EARLY               registered, then satisfied, by one body;
 *       LATE                registered by an EDT the fire itself ran, so the
 *                           event has fired when its home takes it;
 *       UNBORN              registered on a reserved GUID, then created and
 *                           satisfied, by one body;
 *       CHANNEL_DEP_FIRST   a channel source, registered then satisfied;
 *       CHANNEL_DATA_FIRST  a channel source, satisfied then registered.
 * The checking EDT runs on the last rank and the relaying event lives on the
 * rank after the source's, so on four ranks a relayed case of the other-rank
 * half spans all four.
 *
 * Nothing orders two messages of one body at a home that dispatches on
 * several threads, so on another rank an EARLY, UNBORN or CHANNEL registration
 * may reach the home on the other side of the satisfy (or of the create); the
 * dependent receives the same value either way, which is what a case checks.
 * LATE is ordered by the fire on every run.
 *
 * Every checker is a member of one group, so the finale runs only when each
 * dependent was satisfied; a registration that is lost leaves the run waiting
 * until the test's timeout.  One delivered twice is a satisfy beyond the
 * EDT's dependence count: fatal while the checker is installed, and parked
 * for good once the checker has completed, which the teardown's walk reports
 * as a shutdown that left a message undone.  The run's own end leaves
 * nothing undone (the finale runs after every delivery and ends the run
 * itself), so the test refuses every marker of that walk.
 */
#include "arts.h"

#include "../test_failure_status.h"

#include <stdint.h>

enum shape { TO_EDT_VALUE, TO_EDT_RW, TO_EDT_RO, TO_EVENT, SHAPES };
enum timing {
  EARLY,
  LATE,
  UNBORN,
  CHANNEL_DEP_FIRST,
  CHANNEL_DATA_FIRST,
  TIMINGS
};
#define CASES_PER_HOME ((unsigned int)TIMINGS * (unsigned int)SHAPES)
#define MAX_EVENTS (2u * 2u * CASES_PER_HOME) /* a source and a relay each */

/* The checker's slot 0 is satisfied directly and its slot 1 by the case. */
#define MARK(id) ((arts_guid_t)(0x2000u + (id)))
#define VALUE(id) ((arts_guid_t)(0x1000u + (id)))

static arts_guid_t case_data(unsigned int id, enum shape s) {
  return (s == TO_EDT_RW || s == TO_EDT_RO) ? NULL_GUID : VALUE(id);
}

static arts_db_access_mode_t case_mode(enum shape s) {
  return s == TO_EDT_RW   ? DB_MODE_RW
         : s == TO_EDT_RO ? DB_MODE_RO
                          : DB_MODE_NULL;
}

/* The registration under test. */
static void bind(arts_guid_t source, arts_guid_t dependent, enum shape s) {
  if (s == TO_EVENT) {
    arts_event_add_dependence_to_event(source, dependent,
                                       ARTS_EVENT_LATCH_DECR_SLOT);
  } else {
    arts_event_add_dependence_to_edt(source, dependent, 1, case_mode(s));
  }
}

enum { C_CASE, C_DATA, C_MODE, C_COUNT };

static void checker(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                    arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  unsigned int id = (unsigned int)paramv[C_CASE];
  if (depv[0].guid != MARK(id) || depv[0].mode != DB_MODE_NULL ||
      depv[1].guid != (arts_guid_t)paramv[C_DATA] || depv[1].ptr != NULL ||
      (uint64_t)depv[1].mode != paramv[C_MODE]) {
    arts_printf("FAIL: event_typed_dependence case %u: slot 0 holds %lu mode "
                "%u, slot 1 holds %lu ptr %p mode %u, want %lu, then %lu with "
                "mode %u\n",
                id, (unsigned long)depv[0].guid, (unsigned)depv[0].mode,
                (unsigned long)depv[1].guid, depv[1].ptr,
                (unsigned)depv[1].mode, (unsigned long)MARK(id),
                (unsigned long)paramv[C_DATA], (unsigned)paramv[C_MODE]);
    arts_test_fail();
  }
}

enum { B_CASE, B_SOURCE, B_DEPENDENT, B_SHAPE, B_DATA, B_COUNT };

/* A dependent of the source itself: its body runs after the fire. */
static void late_binder(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  if (depv[0].guid != (arts_guid_t)paramv[B_DATA] ||
      depv[0].mode != DB_MODE_NULL) {
    arts_printf("FAIL: event_typed_dependence case %u: the binder holds %lu "
                "mode %u, want %lu\n",
                (unsigned)paramv[B_CASE], (unsigned long)depv[0].guid,
                (unsigned)depv[0].mode, (unsigned long)paramv[B_DATA]);
    arts_test_fail();
  }
  bind((arts_guid_t)paramv[B_SOURCE], (arts_guid_t)paramv[B_DEPENDENT],
       (enum shape)paramv[B_SHAPE]);
}

/* Runs once the group fired.  paramv: every event the cases made. */
static void finale(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)depc;
  (void)depv;
  for (uint32_t i = 0; i < paramc; i++) {
    arts_event_destroy((arts_guid_t)paramv[i]);
  }
  if (arts_test_status() == 0) {
    arts_printf("PASS: event_typed_dependence\n");
  }
  arts_shutdown();
}

/* Wires one case and appends the events it made to `events`. */
static void run_case(unsigned int id, unsigned int home, enum timing t,
                     enum shape s, arts_guid_t group, uint64_t *events,
                     uint32_t *event_count) {
  unsigned int ranks = arts_get_total_ranks();
  arts_guid_t data = case_data(id, s);
  uint64_t cv[C_COUNT] = {id, (uint64_t)data, (uint64_t)case_mode(s)};
  arts_guid_t sink =
      arts_edt_create(checker, C_COUNT, cv, 2,
                      &(arts_edt_hint_t){.rank = ranks - 1u, .group = group});
  arts_edt_satisfy_slot(sink, 0, MARK(id), DB_MODE_NULL);
  arts_guid_t dependent = sink;
  if (s == TO_EVENT) {
    arts_event_hint_t rh = ARTS_EVENT_HINT_DEFAULTS;
    rh.rank = (home + 1u) % ranks;
    dependent = arts_event_create(&rh);
    arts_event_add_dependence_to_edt(dependent, sink, 1, DB_MODE_NULL);
    events[(*event_count)++] = (uint64_t)dependent;
  }

  arts_event_hint_t sh = ARTS_EVENT_HINT_DEFAULTS;
  if (t == CHANNEL_DEP_FIRST || t == CHANNEL_DATA_FIRST) {
    sh = ARTS_EVENT_HINT_CHANNEL;
  }
  sh.rank = home;
  arts_guid_t source;
  switch (t) {
  case EARLY:
  case CHANNEL_DEP_FIRST:
    source = arts_event_create(&sh);
    bind(source, dependent, s);
    arts_event_satisfy(source, data);
    break;
  case CHANNEL_DATA_FIRST:
    source = arts_event_create(&sh);
    arts_event_satisfy(source, data);
    bind(source, dependent, s);
    break;
  case UNBORN:
    source = arts_guid_reserve(ARTS_GUID_EVENT, home);
    bind(source, dependent, s);
    sh.guid = source;
    (void)arts_event_create(&sh);
    arts_event_satisfy(source, data);
    break;
  default: { /* LATE */
    source = arts_event_create(&sh);
    uint64_t bv[B_COUNT] = {id, (uint64_t)source, (uint64_t)dependent,
                            (uint64_t)s, (uint64_t)data};
    arts_guid_t binder = arts_edt_create(
        late_binder, B_COUNT, bv, 1,
        &(arts_edt_hint_t){.rank = arts_get_current_rank(), .group = group});
    arts_event_add_dependence_to_edt(source, binder, 0, DB_MODE_NULL);
    arts_event_satisfy(source, data);
    break;
  }
  }
  events[(*event_count)++] = (uint64_t)source;
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int ranks = arts_get_total_ranks();
  unsigned int self = arts_get_current_rank();
  unsigned int homes = ranks > 1u ? 2u : 1u;
  arts_event_hint_t gh = ARTS_EVENT_HINT_LATCH(1);
  gh.auto_destroy = true;
  arts_guid_t group = arts_event_create(&gh);

  uint64_t events[MAX_EVENTS];
  uint32_t event_count = 0;
  unsigned int id = 0;
  for (unsigned int h = 0; h < homes; h++) {
    unsigned int home = (self + h) % ranks;
    for (unsigned int t = 0; t < (unsigned int)TIMINGS; t++) {
      for (unsigned int s = 0; s < (unsigned int)SHAPES; s++) {
        run_case(id++, home, (enum timing)t, (enum shape)s, group, events,
                 &event_count);
      }
    }
  }

  arts_guid_t f = arts_edt_create(finale, event_count, events, 1,
                                  &(arts_edt_hint_t){.rank = self});
  arts_event_add_dependence_to_edt(group, f, 0, DB_MODE_NULL);
  arts_event_satisfy_slot(group, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
