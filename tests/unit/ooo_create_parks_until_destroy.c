/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* ooo_create_parks_until_destroy — whitebox, single rank.
 *
 * A create of a GUID whose object is live parks on the slot's OoO list and
 * changes nothing; the occupant's destroy admits exactly one parked create,
 * which installs as the next generation.  Several parked creates form a queue
 * that drains one per destroy.
 *
 * Each generation is told apart by its satisfy data: the test satisfies the
 * occupant with a generation-specific value before destroying it, so the
 * object a lookup returns afterwards is the next generation exactly when it is
 * unfired.  (A pointer comparison could not tell: the freed occupant's storage
 * may be what the next generation is allocated in.)
 */

#include <stdint.h>

#include "arts.h"
#include "arts/gas/route_table.h"
#include "arts/runtime_types.h"
#include "../test_failure_status.h"

#define PARKED 3u

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

static void expect(bool ok, const char *what, unsigned int gen) {
  if (!ok) {
    arts_printf("FAIL: ooo_create_parks_until_destroy generation %u: %s\n",
                gen, what);
    arts_test_fail();
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_guid_t g = arts_guid_reserve(ARTS_GUID_EVENT, arts_get_current_rank());
  arts_event_hint_t h = ARTS_EVENT_HINT_STICKY;
  h.guid = g;

  expect(arts_event_create(&h) == g, "first create did not return the GUID",
         0);
  bool present;
  (void)installed_data(g, &present);
  expect(present, "first create installed nothing", 0);
  expect(parked_on(g) == 0u, "first create parked", 0);

  for (unsigned int k = 0; k < PARKED; k++) {
    expect(arts_event_create(&h) == g, "a parked create did not return the GUID",
           0);
  }
  expect(parked_on(g) == PARKED, "the creates of a live GUID did not park", 0);

  for (unsigned int gen = 0; gen <= PARKED; gen++) {
    arts_guid_t mark = (arts_guid_t)(0x100u + gen);
    arts_event_satisfy(g, mark);
    arts_guid_t d = installed_data(g, &present);
    expect(present && d == mark, "the occupant did not take its satisfy", gen);
    /* A parked create never displaces the occupant. */
    expect(parked_on(g) == PARKED - gen, "the parked queue changed", gen);

    arts_event_destroy(g);
    d = installed_data(g, &present);
    if (gen < PARKED) {
      expect(present, "the destroy admitted no parked create", gen);
      expect(d == NULL_GUID, "the object after the destroy is not a fresh one",
             gen);
      expect(parked_on(g) == PARKED - gen - 1u,
             "the destroy admitted more or fewer than one create", gen);
    } else {
      expect(!present, "an object survived the last destroy", gen);
      expect(parked_on(g) == 0u, "a create is still parked at the end", gen);
    }
  }

  if (arts_test_status() == 0) {
    arts_printf("PASS: ooo_create_parks_until_destroy %u parked creates "
                "installed one per destroy\n",
                PARKED);
  }
  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
