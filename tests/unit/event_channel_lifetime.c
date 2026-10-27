/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/// @file event_channel_lifetime.c
/// @brief Verifies that CHANNEL events do NOT auto-destroy regardless of
///        the number of fire generations.  Only explicit arts_event_destroy
///        tears the event down, and a channel destroyed while it still
///        queues unpaired satisfies, or unpaired dependences, frees them
///        (a Debug build checks that its count matches what it frees).

#include "arts.h"
#include "arts/gas/route_table.h"

#include <stdio.h>

#include "../test_failure_status.h"

#define N_GENS 100
#define N_PAIRED 3
#define N_UNPAIRED 5

static void noop_dep(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
}

/* Gated on the noop EDTs' finish scope. */
static void finale(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== event_channel_lifetime ===\n");

  arts_guid_t fe = arts_event_create(&ARTS_EVENT_HINT_FINISH);

  arts_event_hint_t hint = ARTS_EVENT_HINT_CHANNEL;
  arts_guid_t ch = arts_event_create(&hint);

  /* Drive N fire generations: pair add_dep + satisfy each iteration. */
  for (int i = 0; i < N_GENS; i++) {
    arts_guid_t edt =
        arts_edt_create(noop_dep, 0, NULL, 1,
                        &(arts_edt_hint_t){.rank = 0, .finish_event = fe});
    arts_add_dependence(ch, edt, 0, DB_MODE_RW);
    arts_event_satisfy_slot(ch, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
  }

  /* Event must still be alive after N fires. */
  arts_shared_ptr_t h = arts_route_table_lookup_event(ch);
  struct arts_event_s *e = (struct arts_event_s *)arts_shared_get(h);
  if (!e) {
    arts_printf(
        "FAIL: CHANNEL destroyed after %d fires (expected persistent)\n",
        N_GENS);
    arts_test_fail();
    arts_shared_release(&h);
    arts_shutdown();
    return;
  }
  arts_shared_release(&h);

  /* Explicit destroy releases it. */
  arts_event_destroy(ch);

  h = arts_route_table_lookup_event(ch);
  e = (struct arts_event_s *)arts_shared_get(h);
  if (e) {
    arts_printf("FAIL: CHANNEL still alive after explicit destroy\n");
    arts_test_fail();
    arts_shared_release(&h);
    arts_shutdown();
    return;
  }
  arts_shared_release(&h);

  /* One channel left holding satisfies past its pairs, one holding
   * dependences.  The dependents are an event nobody fires, so nothing waits
   * on the pairs that never come. */
  arts_guid_t ch_sat = arts_event_create(&hint);
  for (int i = 0; i < N_PAIRED; i++) {
    arts_guid_t edt =
        arts_edt_create(noop_dep, 0, NULL, 1,
                        &(arts_edt_hint_t){.rank = 0, .finish_event = fe});
    arts_add_dependence(ch_sat, edt, 0, DB_MODE_RW);
  }
  for (int i = 0; i < N_PAIRED + N_UNPAIRED; i++) {
    arts_event_satisfy_slot(ch_sat, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
  }
  arts_event_hint_t sink_hint = ARTS_EVENT_HINT_LATCH(1);
  arts_guid_t sink = arts_event_create(&sink_hint);
  arts_guid_t ch_dep = arts_event_create(&hint);
  for (int i = 0; i < N_UNPAIRED; i++) {
    arts_add_dependence(ch_dep, sink, ARTS_EVENT_LATCH_DECR_SLOT, DB_MODE_NULL);
  }
  arts_event_destroy(ch_sat);
  arts_event_destroy(ch_dep);
  arts_event_destroy(sink);
  const arts_guid_t held[2] = {ch_sat, ch_dep};
  for (int i = 0; i < 2; i++) {
    h = arts_route_table_lookup_event(held[i]);
    e = (struct arts_event_s *)arts_shared_get(h);
    arts_shared_release(&h);
    if (e) {
      arts_printf("FAIL: CHANNEL holding unpaired %s still alive after "
                  "explicit destroy\n",
                  i == 0 ? "satisfies" : "dependences");
      arts_test_fail();
      arts_shutdown();
      return;
    }
  }

  arts_printf(
      "  PASS: CHANNEL persistent across %d fires; destroyed explicitly, "
      "also with %d unpaired satisfies or dependences queued\n",
      N_GENS, N_UNPAIRED);
  arts_guid_t f =
      arts_edt_create(finale, 0, NULL, 1, &(arts_edt_hint_t){.rank = 0});
  arts_add_dependence(fe, f, 0, DB_MODE_NULL);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
