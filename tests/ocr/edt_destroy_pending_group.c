/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* edt_destroy_pending_group — destroying an EDT that never ran balances the
 * group it joined, as its completion would have.
 *
 * Two EDTs join one group and each waits on a dependence nobody
 * satisfies: one homed on the first rank (the group's own rank), one homed on
 * the last rank (on several ranks it joins the group through a local proxy).
 * Both are destroyed; the group must then fire, so the continuation gated on
 * it runs and the run ends with no work left undone.  A destroy that left the
 * group's INCR open would leave that continuation unrun.
 *
 * Neither EDT carries a completion event: the programming model does not say
 * what a destroyed EDT's completion event does, so the runtime leaves it alone.
 */

#include <stdint.h>

#include "../test_failure_status.h"
#include "arts.h"

static void victim(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("FAIL: edt_destroy_pending_group: a destroyed EDT ran\n");
  arts_test_fail();
}

static void finale(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (arts_test_status() == 0) {
    arts_printf("PASS: edt_destroy_pending_group\n");
  }
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int last = arts_get_total_ranks() - 1u;
  arts_event_hint_t gh = ARTS_EVENT_HINT_LATCH(1);
  gh.auto_destroy = true;
  arts_guid_t group = arts_event_create(&gh);
  arts_guid_t here = arts_edt_create(
      victim, 0, NULL, 1, &(arts_edt_hint_t){.rank = 0u, .group = group});
  arts_guid_t there = arts_edt_create(
      victim, 0, NULL, 1, &(arts_edt_hint_t){.rank = last, .group = group});
  arts_edt_destroy(here);
  arts_edt_destroy(there);
  arts_guid_t f =
      arts_edt_create(finale, 0, NULL, 1, &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(group, f, 0, DB_MODE_NULL);
  arts_event_satisfy_slot(group, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
}

int main(int argc, char **argv) {
  return arts_rt(argc, argv);
}
