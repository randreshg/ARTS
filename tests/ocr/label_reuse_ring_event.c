/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* label_reuse_ring_event — one labeled sticky event reused for GENERATIONS
 * lifetimes.
 *
 * The label is homed on the first rank.  A producer on the last rank satisfies
 * generation k with its own value and at once creates generation k+1 of the
 * same label, while generation k is still live: that create parks at the home.
 * A consumer on the home reads generation k's value, destroys generation k —
 * which admits the parked create, so generation k+1 installs — then binds the
 * next consumer to the label and starts the producer's next step.
 *
 * Every message for generation k+1 other than its create (the next consumer's
 * binding, the producer's next satisfy) is issued after generation k's destroy
 * ran at the home, so each reaches generation k+1 whether it lands before or
 * after that generation's create.  Every generation's value must reach its
 * consumer, in order.  The run ends with the last generation destroyed and no
 * create parked, so its shutdown is quiescent.
 */

#include <stdint.h>

#include "arts.h"
#include "../test_failure_status.h"

#define GENERATIONS 64u
#define VALUE(k) ((arts_guid_t)(0x5000u + (k)))

enum { P_LABEL, P_FINISH, P_GEN, P_COUNT };

static void consumer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]);

static void producer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t label = (arts_guid_t)paramv[P_LABEL];
  uint64_t k = paramv[P_GEN];
  arts_event_hint_t h = ARTS_EVENT_HINT_STICKY;
  h.guid = label;
  if (k == 0u) {
    (void)arts_event_create(&h);
  }
  arts_event_satisfy(label, VALUE(k));
  if (k + 1u < GENERATIONS) {
    (void)arts_event_create(&h);
  }
}

/* Bind a consumer of generation k to the label and start its producer step. */
static void start_generation(arts_guid_t label, arts_guid_t finish,
                             uint64_t k) {
  unsigned int last = arts_get_total_ranks() - 1u;
  uint64_t pv[P_COUNT] = {(uint64_t)label, (uint64_t)finish, k};
  arts_guid_t c = arts_edt_create(
      consumer, P_COUNT, pv, 1,
      &(arts_edt_hint_t){.rank = 0u, .finish_event = finish});
  arts_add_dependence(label, c, 0, DB_MODE_NULL);
  (void)arts_edt_create(
      producer, P_COUNT, pv, 0,
      &(arts_edt_hint_t){.rank = last, .finish_event = finish});
}

static void consumer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  arts_guid_t label = (arts_guid_t)paramv[P_LABEL];
  uint64_t k = paramv[P_GEN];
  if (depv[0].guid != VALUE(k)) {
    arts_printf("FAIL: label_reuse_ring_event generation %lu read 0x%lx, "
                "want 0x%lx\n",
                (unsigned long)k, (unsigned long)depv[0].guid,
                (unsigned long)VALUE(k));
    arts_test_fail();
  }
  arts_event_destroy(label);
  if (k + 1u < GENERATIONS) {
    start_generation(label, (arts_guid_t)paramv[P_FINISH], k + 1u);
  } else if (arts_test_status() == 0) {
    arts_printf("PASS: label_reuse_ring_event %u generations in order\n",
                GENERATIONS);
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_guid_t label = arts_guid_reserve(ARTS_GUID_EVENT, 0u);
  arts_guid_t finish = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  start_generation(label, finish, 0u);
  arts_event_wait(finish);
  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
