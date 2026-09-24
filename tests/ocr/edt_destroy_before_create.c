/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* edt_destroy_before_create — a destroy of a reserved EDT GUID that reaches
 * the home before the create parks there; the create installs the EDT and the
 * install's replay of the destroy retires it at once.
 *
 *   - wire: the first rank destroys, then creates, an EDT GUID homed on the
 *     last rank (on one rank both are local).
 *   - home: an EDT on the last rank destroys, then creates, a GUID homed on
 *     its own rank, and then creates the GUID's next generation, which must
 *     install on the retired slot and run.  A destroy that failed to retire
 *     the first generation would leave the second parked behind it forever.
 *
 * The destroyed EDTs carry a dependence nobody satisfies and join no finish
 * scope, so they never run and owe no scope a decrement.
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
  arts_printf("FAIL: edt_destroy_before_create: a destroyed EDT ran\n");
  arts_test_fail();
}

static void next_generation(uint32_t paramc, const uint64_t *paramv,
                            uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("PASS: edt_destroy_before_create\n");
  arts_shutdown();
}

static void at_home(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                    arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_guid_t g = arts_guid_reserve(ARTS_GUID_EDT, arts_get_current_rank());
  arts_edt_destroy(g);
  (void)arts_edt_create(victim, 0, NULL, 1, &(arts_edt_hint_t){.guid = g});
  (void)arts_edt_create(next_generation, 0, NULL, 0,
                        &(arts_edt_hint_t){.guid = g});
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int last = arts_get_total_ranks() - 1u;
  arts_guid_t g = arts_guid_reserve(ARTS_GUID_EDT, last);
  arts_edt_destroy(g);
  (void)arts_edt_create(victim, 0, NULL, 1, &(arts_edt_hint_t){.guid = g});
  (void)arts_edt_create(at_home, 0, NULL, 0,
                        &(arts_edt_hint_t){.rank = last});
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
