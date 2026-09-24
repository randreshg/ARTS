/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* edt_destroy_pending_scope — destroying an EDT that never ran balances the
 * finish scope it joined, as its completion would have.
 *
 * Two EDTs join one finish scope and each waits on a dependence nobody
 * satisfies: one homed on the first rank (the scope's own rank), one homed on
 * the last rank (on several ranks it joins the scope through a local proxy).
 * Both are destroyed; the scope must then drain, so the waiting EDT resumes
 * and the run ends with no work left undone.  A destroy that left the scope's
 * INCR open would hang the wait.
 *
 * Neither EDT carries an output event: the programming model does not say
 * what a destroyed EDT's output event does, so the runtime leaves it alone.
 */

#include <stdint.h>

#include "arts.h"
#include "../test_failure_status.h"

static void victim(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("FAIL: edt_destroy_pending_scope: a destroyed EDT ran\n");
  arts_test_fail();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int last = arts_get_total_ranks() - 1u;
  arts_guid_t scope = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t here = arts_edt_create(
      victim, 0, NULL, 1,
      &(arts_edt_hint_t){.rank = 0u, .finish_event = scope});
  arts_guid_t there = arts_edt_create(
      victim, 0, NULL, 1,
      &(arts_edt_hint_t){.rank = last, .finish_event = scope});
  arts_edt_destroy(here);
  arts_edt_destroy(there);
  arts_event_wait(scope);
  if (arts_test_status() == 0) {
    arts_printf("PASS: edt_destroy_pending_scope\n");
  }
  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
