/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* rendezvous_loser_parks — every rank creates one labeled sticky event.
 *
 * The rendezvous shape: each rank creates the label and binds its own
 * consumer to it, and the first rank satisfies it.  One create installs; the
 * others park at the label's home behind it.  Every consumer reads the value
 * of the one installed event.
 *
 * Once every consumer has read it, the home destroys the event, which admits
 * one parked create; the home then binds a consumer to the label and
 * satisfies it with the next value, and that consumer destroys it in turn —
 * one generation per create, so all ranks' creates install one after another
 * and each is destroyed.  Every message for a generation other than its
 * create is issued at the home after the previous generation's destroy, so it
 * reaches that generation whether it lands before or after the create.  The
 * run ends with nothing installed and nothing parked, so its shutdown is
 * quiescent.
 */

#include <stdint.h>

#include "arts.h"
#include "../test_failure_status.h"

#define VALUE(k) ((arts_guid_t)(0x7000u + (k)))

enum { P_LABEL, P_FINISH, P_LATCH, P_GEN, P_COUNT };

static void check_value(const arts_edt_dep_t *d, uint64_t k,
                        const char *who) {
  if (d->guid != VALUE(k)) {
    arts_printf("FAIL: rendezvous_loser_parks %s of generation %lu read "
                "0x%lx, want 0x%lx\n",
                who, (unsigned long)k, (unsigned long)d->guid,
                (unsigned long)VALUE(k));
    arts_test_fail();
  }
}

static void next_consumer(uint32_t paramc, const uint64_t *paramv,
                          uint32_t depc, arts_edt_dep_t depv[]);

/* At the home, after the previous generation's destroy: bind generation k's
 * consumer and satisfy it. */
static void start_generation(const uint64_t *paramv, uint64_t k) {
  arts_guid_t label = (arts_guid_t)paramv[P_LABEL];
  uint64_t pv[P_COUNT] = {paramv[P_LABEL], paramv[P_FINISH], paramv[P_LATCH],
                          k};
  arts_guid_t c = arts_edt_create(
      next_consumer, P_COUNT, pv, 1,
      &(arts_edt_hint_t){.rank = 0u,
                         .finish_event = (arts_guid_t)paramv[P_FINISH]});
  arts_add_dependence(label, c, 0, DB_MODE_NULL);
  arts_event_satisfy(label, VALUE(k));
}

static void next_consumer(uint32_t paramc, const uint64_t *paramv,
                          uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint64_t k = paramv[P_GEN];
  check_value(&depv[0], k, "the home consumer");
  arts_event_destroy((arts_guid_t)paramv[P_LABEL]);
  if (k + 1u < arts_get_total_ranks()) {
    start_generation(paramv, k + 1u);
  } else if (arts_test_status() == 0) {
    arts_printf("PASS: rendezvous_loser_parks %u creates, each installed and "
                "destroyed in turn\n",
                arts_get_total_ranks());
  }
}

/* Runs at the home once every rank's consumer has read the first generation. */
static void first_destroy(uint32_t paramc, const uint64_t *paramv,
                          uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_event_destroy((arts_guid_t)paramv[P_LATCH]);
  arts_event_destroy((arts_guid_t)paramv[P_LABEL]);
  if (arts_get_total_ranks() > 1u) {
    start_generation(paramv, 1u);
  } else if (arts_test_status() == 0) {
    arts_printf("PASS: rendezvous_loser_parks 1 creates, each installed and "
                "destroyed in turn\n");
  }
}

static void rank_consumer(uint32_t paramc, const uint64_t *paramv,
                          uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  check_value(&depv[0], 0u, "a rank's consumer");
  arts_event_satisfy((arts_guid_t)paramv[P_LATCH], NULL_GUID);
}

static void rank_creator(uint32_t paramc, const uint64_t *paramv,
                         uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t label = (arts_guid_t)paramv[P_LABEL];
  arts_event_hint_t h = ARTS_EVENT_HINT_STICKY;
  h.guid = label;
  (void)arts_event_create(&h);
  /* The consumer inherits this creator's scope: a scope named explicitly
   * must be homed on the creating rank. */
  arts_guid_t c = arts_edt_create(
      rank_consumer, P_COUNT, paramv, 1,
      &(arts_edt_hint_t){.rank = arts_get_current_rank()});
  arts_add_dependence(label, c, 0, DB_MODE_NULL);
  if (arts_get_current_rank() == 0u) {
    arts_event_satisfy(label, VALUE(0u));
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int ranks = arts_get_total_ranks();
  arts_guid_t label = arts_guid_reserve(ARTS_GUID_EVENT, 0u);
  arts_guid_t finish = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t read_all = arts_event_create(&ARTS_EVENT_HINT_LATCH(ranks));
  uint64_t pv[P_COUNT] = {(uint64_t)label, (uint64_t)finish,
                          (uint64_t)read_all, 0u};

  arts_guid_t d = arts_edt_create(
      first_destroy, P_COUNT, pv, 1,
      &(arts_edt_hint_t){.rank = 0u, .finish_event = finish});
  arts_add_dependence(read_all, d, 0, DB_MODE_NULL);
  for (unsigned int r = 0; r < ranks; r++) {
    (void)arts_edt_create(
        rank_creator, P_COUNT, pv, 0,
        &(arts_edt_hint_t){.rank = r, .finish_event = finish});
  }
  arts_event_wait(finish);
  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
