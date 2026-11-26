/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* create_guid_kind_refused — an event or an EDT is created under a
 * caller-given GUID of its own kind only: the route table picks an object's
 * deleter by its key's kind.  An event created under an EDT GUID and an EDT
 * created under an event GUID each end the run with status 1 and the
 * refusal's message, which is this test's expected verdict.  A create that is
 * let through, or a case name the program does not know, prints a FAIL line
 * and the run ends 0: the status is then not the refusal's.
 *
 * argv[1] names the case: event_under_edt or edt_under_event.
 */

#include <stdint.h>
#include <string.h>

#include "arts.h"

static void never_runs(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  int argc = (int)paramv[0];
  char **argv = (char **)paramv[1];
  const char *name = argc > 1 ? argv[1] : "";

  if (strcmp(name, "event_under_edt") == 0) {
    arts_event_hint_t h = ARTS_EVENT_HINT_DEFAULTS;
    h.guid = arts_guid_reserve(ARTS_GUID_EDT, 0u);
    (void)arts_event_create(&h);
  } else if (strcmp(name, "edt_under_event") == 0) {
    /* One dependence nothing satisfies, so a create let through never runs
     * its body. */
    arts_edt_hint_t h = ARTS_EDT_HINT_DEFAULTS;
    h.guid = arts_guid_reserve(ARTS_GUID_EVENT, 0u);
    (void)arts_edt_create(never_runs, 0, NULL, 1, &h);
  } else {
    arts_printf("FAIL: create_guid_kind_refused: unknown case '%s'\n", name);
    arts_shutdown();
    return;
  }
  arts_printf("FAIL: create_guid_kind_refused: %s was let through\n", name);
  arts_shutdown();
}

int main(int argc, char **argv) {
  return arts_rt(argc, argv);
}
