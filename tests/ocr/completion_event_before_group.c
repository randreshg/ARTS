/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* completion_event_before_group — an EDT's completion event fires at its own
 * completion, while the group it belongs to is still open.
 *
 * X is a member of the group g and names E as its completion event.  X creates
 * a child C on the last rank, which inherits g and whose one dependence is the
 * event S.  E's successor marks a flag and then satisfies S, so C, and with it
 * g, can finish only after E has fired.  g's successor must find the flag set.
 * A completion event that waited for its EDT's group would leave C waiting on
 * S for ever, which the test's timeout reports.  X and both successors run on
 * rank 0.
 */

#include <stdatomic.h>
#include <stdint.h>

#include "../test_failure_status.h"
#include "arts.h"

static _Atomic(unsigned int) g_completion_fired;

static void child(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
}

static void x_body(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t s = (arts_guid_t)paramv[0];
  unsigned int last = arts_get_total_ranks() - 1u;
  arts_guid_t c =
      arts_edt_create(child, 0, NULL, 1, &(arts_edt_hint_t){.rank = last});
  arts_add_dependence(s, c, 0, DB_MODE_NULL);
}

static void completion_successor(uint32_t paramc, const uint64_t *paramv,
                                 uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t s = (arts_guid_t)paramv[0];
  atomic_store_explicit(&g_completion_fired, 1u, memory_order_release);
  arts_event_satisfy_slot(s, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
}

static void group_successor(uint32_t paramc, const uint64_t *paramv,
                            uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (atomic_load_explicit(&g_completion_fired, memory_order_acquire) != 1u) {
    arts_printf("FAIL: completion_event_before_group: the group fired before "
                "the completion event's successor ran\n");
    arts_test_fail();
  } else if (arts_test_status() == 0) {
    arts_printf("PASS: completion_event_before_group\n");
  }
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_event_hint_t gh = ARTS_EVENT_HINT_LATCH(1);
  arts_guid_t g = arts_event_create(&gh);
  arts_event_hint_t eh = ARTS_EVENT_HINT_LATCH(1);
  arts_guid_t e = arts_event_create(&eh);
  arts_event_hint_t sh = ARTS_EVENT_HINT_LATCH(1);
  uint64_t s = (uint64_t)arts_event_create(&sh);

  arts_guid_t es = arts_edt_create(completion_successor, 1, &s, 1,
                                   &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(e, es, 0, DB_MODE_NULL);
  arts_guid_t gs = arts_edt_create(group_successor, 0, NULL, 1,
                                   &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(g, gs, 0, DB_MODE_NULL);
  (void)arts_edt_create(
      x_body, 1, &s, 0,
      &(arts_edt_hint_t){.rank = 0u, .group = g, .completion_event = e});
  arts_event_satisfy_slot(g, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
