/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* create_parks_until_retire — whitebox, single rank.
 *
 * A create of a GUID whose object is live parks on the slot's OoO list and
 * changes nothing; the occupant's retire admits exactly one parked create,
 * which installs as the next generation.
 *
 * Event lane: several parked creates form a queue that drains one per
 * destroy.  Each generation is told apart by its satisfy data: the occupant is
 * satisfied with a generation-specific value before it is destroyed, so the
 * object a lookup returns afterwards is the next generation exactly when it is
 * unfired.  (A pointer comparison could not tell: the freed occupant's storage
 * may be what the next generation is allocated in.)
 *
 * EDT lane: the occupant's completion retires the GUID, which admits the
 * parked create, and the occupant's completion signal (its finish scope) is
 * observed only after that install.  The next generation then runs with the
 * satisfy issued after that signal.  Generations are told apart by a tag in
 * their parameters, read from the EDT the route slot holds.
 */

#include "arts/utils/stdatomic_compat.h"
#include <stdint.h>

#include "../test_failure_status.h"
#include "arts.h"
#include "arts/gas/route_table.h"
#include "arts/runtime_types.h"

#define PARKED 3u
#define VALUE(tag) ((arts_guid_t)(0x900u + (tag)))
#define NO_EDT UINT64_MAX

static _Atomic uint64_t g_ran[2];

static void expect(bool ok, const char *lane, const char *what) {
  if (!ok) {
    arts_printf("FAIL: create_parks_until_retire %s: %s\n", lane, what);
    arts_test_fail();
  }
}

static unsigned int parked_on(arts_guid_t g) {
  arts_route_item_t *slot;
  arts_route_table_reserve_or_lookup(g, &slot);
  unsigned int n = 0;
  for (arts_lf_link_t *l =
           atomic_load_explicit(&slot->ooo_list.head, memory_order_acquire);
       l != NULL; l = atomic_load_explicit(&l->next, memory_order_relaxed)) {
    n++;
  }
  return n;
}

/* The satisfy data of the event installed at `g`; `*present` says whether one
 * is. */
static arts_guid_t installed_data(arts_guid_t g, bool *present) {
  arts_shared_ptr_t h = arts_route_table_lookup_event(g);
  const struct arts_event_s *e =
      (const struct arts_event_s *)arts_shared_get(h);
  *present = (e != NULL);
  arts_guid_t d = e ? e->simple.data : NULL_GUID;
  if (h) {
    arts_shared_release(&h);
  }
  return d;
}

/* The tag of the EDT installed at `g`, or NO_EDT. */
static uint64_t installed_tag(arts_guid_t g) {
  arts_shared_ptr_t h = arts_route_table_lookup_edt(g);
  const struct arts_edt_s *e = (const struct arts_edt_s *)arts_shared_get(h);
  uint64_t tag = e ? ((const uint64_t *)(e + 1))[0] : NO_EDT;
  if (h) {
    arts_shared_release(&h);
  }
  return tag;
}

static void event_lane(void) {
  const char *lane = "event";
  arts_guid_t g = arts_guid_reserve(ARTS_GUID_EVENT, arts_get_current_rank());
  arts_event_hint_t h = ARTS_EVENT_HINT_DEFAULTS;
  h.guid = g;

  expect(arts_event_create(&h) == g, lane,
         "first create did not return the GUID");
  bool present;
  (void)installed_data(g, &present);
  expect(present, lane, "first create installed nothing");
  expect(parked_on(g) == 0u, lane, "first create parked");

  for (unsigned int k = 0; k < PARKED; k++) {
    expect(arts_event_create(&h) == g, lane,
           "a parked create did not return the GUID");
  }
  expect(parked_on(g) == PARKED, lane,
         "the creates of a live GUID did not park");

  for (unsigned int gen = 0; gen <= PARKED; gen++) {
    arts_guid_t mark = (arts_guid_t)(0x100u + gen);
    arts_event_satisfy(g, mark);
    arts_guid_t d = installed_data(g, &present);
    expect(present && d == mark, lane, "the occupant did not take its satisfy");
    /* A parked create never displaces the occupant. */
    expect(parked_on(g) == PARKED - gen, lane, "the parked queue changed");

    arts_event_destroy(g);
    d = installed_data(g, &present);
    if (gen < PARKED) {
      expect(present, lane, "the destroy admitted no parked create");
      expect(d == NULL_GUID, lane,
             "the object after the destroy is not a fresh one");
      expect(parked_on(g) == PARKED - gen - 1u, lane,
             "the destroy admitted more or fewer than one create");
    } else {
      expect(!present, lane, "an object survived the last destroy");
      expect(parked_on(g) == 0u, lane, "a create is still parked at the end");
    }
  }
}

static void member(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  atomic_store_explicit(&g_ran[paramv[0]], (uint64_t)depv[0].guid,
                        memory_order_release);
}

static void done(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  const char *lane = "EDT";
  arts_guid_t g = (arts_guid_t)paramv[0];
  expect(atomic_load_explicit(&g_ran[1], memory_order_acquire) ==
             (uint64_t)VALUE(1),
         lane, "the second generation did not run with its own satisfy");
  expect(installed_tag(g) == NO_EDT, lane,
         "an EDT survived the last completion");
  expect(parked_on(g) == 0u, lane, "a create is still parked at the end");
  if (arts_test_status() == 0) {
    arts_printf("PASS: create_parks_until_retire %u parked event creates "
                "installed one per destroy, an EDT create installed at its "
                "occupant's retire\n",
                PARKED);
  }
  arts_shutdown();
}

/* Gated on the first generation's finish scope. */
static void check(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  const char *lane = "EDT";
  arts_guid_t g = (arts_guid_t)paramv[0];
  arts_guid_t second_scope = (arts_guid_t)paramv[1];
  expect(atomic_load_explicit(&g_ran[0], memory_order_acquire) ==
             (uint64_t)VALUE(0),
         lane, "the first generation did not run with its satisfy");
  expect(atomic_load_explicit(&g_ran[1], memory_order_acquire) == 0u, lane,
         "the second generation ran before its satisfy");
  expect(parked_on(g) == 0u, lane,
         "the retire of the first generation admitted nothing");
  expect(installed_tag(g) == 1u, lane,
         "the completion signal preceded the second generation's install");

  uint64_t pv[1] = {(uint64_t)g};
  arts_guid_t d = arts_edt_create(done, 1, pv, 1, NULL);
  arts_add_dependence(second_scope, d, 0, DB_MODE_NULL);
  arts_edt_satisfy_slot(g, 0, VALUE(1), DB_MODE_NULL);
}

static void edt_lane(void) {
  const char *lane = "EDT";
  arts_guid_t g = arts_guid_reserve(ARTS_GUID_EDT, arts_get_current_rank());
  arts_guid_t first_scope = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t second_scope = arts_event_create(&ARTS_EVENT_HINT_FINISH);

  uint64_t tag0 = 0u;
  uint64_t tag1 = 1u;
  (void)arts_edt_create(
      member, 1, &tag0, 1,
      &(arts_edt_hint_t){.guid = g, .finish_event = first_scope});
  expect(installed_tag(g) == 0u, lane, "the first create installed nothing");
  expect(parked_on(g) == 0u, lane, "the first create parked");

  (void)arts_edt_create(
      member, 1, &tag1, 1,
      &(arts_edt_hint_t){.guid = g, .finish_event = second_scope});
  expect(parked_on(g) == 1u, lane, "the create of a live GUID did not park");
  expect(installed_tag(g) == 0u, lane,
         "the parked create displaced the occupant");

  uint64_t pv[2] = {(uint64_t)g, (uint64_t)second_scope};
  arts_guid_t c = arts_edt_create(check, 2, pv, 1, NULL);
  arts_add_dependence(first_scope, c, 0, DB_MODE_NULL);
  arts_edt_satisfy_slot(g, 0, VALUE(0), DB_MODE_NULL);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  event_lane();
  edt_lane();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
