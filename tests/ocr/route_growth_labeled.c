/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* route_growth_labeled — every worker creates events named from one reserved
 * range, so their creates, the satisfies that race them and the bindings of
 * their consumers all reserve keys of one route-table chain at once while it
 * grows; each event must fire exactly once.
 *
 * The range is reserved on the first rank, so every member is homed there and
 * keyed in one block.  One creator per worker, spread over the ranks, creates
 * its share of the events under their labels and binds a consumer on the
 * first rank to each; one satisfier per worker satisfies a share created by
 * another worker, so a satisfy can reach an event before its create.  Run
 * with a tiny route_table_size the chain grows many times under the race.
 * Every consumer counts once; the final EDT requires exactly one firing per
 * event.
 */

#include <stdatomic.h>
#include <stdint.h>

#include "../test_failure_status.h"
#include "arts.h"

#define EVENTS 2048u

static _Atomic(unsigned int) g_fired;

/* paramv: done latch */
static void consumer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  atomic_fetch_add_explicit(&g_fired, 1u, memory_order_relaxed);
  arts_event_satisfy_slot((arts_guid_t)paramv[0], NULL_GUID,
                          ARTS_EVENT_LATCH_DECR_SLOT);
}

/* paramv: range, done latch, first index, stride */
static void creator(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                    arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t range = (arts_guid_t)paramv[0];
  uint64_t cpv[1] = {paramv[1]};
  for (uint64_t i = paramv[2]; i < EVENTS; i += paramv[3]) {
    arts_event_hint_t h = ARTS_EVENT_HINT_DEFAULTS;
    h.guid = arts_guid_from_index(range, (unsigned int)i);
    arts_guid_t e = arts_event_create(&h);
    arts_edt_hint_t ch = ARTS_EDT_HINT_DEFAULTS;
    ch.rank = 0;
    arts_guid_t c = arts_edt_create(consumer, 1, cpv, 1, &ch);
    arts_add_dependence(e, c, 0, DB_MODE_NULL);
  }
}

/* paramv: range, first index, stride */
static void satisfier(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t range = (arts_guid_t)paramv[0];
  for (uint64_t i = paramv[1]; i < EVENTS; i += paramv[2]) {
    arts_event_satisfy(arts_guid_from_index(range, (unsigned int)i), NULL_GUID);
  }
}

static void finish(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int fired = atomic_load_explicit(&g_fired, memory_order_relaxed);
  if (fired != EVENTS) {
    arts_printf("FAIL: route_growth_labeled %u of %u events fired\n", fired,
                EVENTS);
    arts_test_fail();
  } else {
    arts_printf("PASS: route_growth_labeled %u events fired once each\n",
                fired);
  }
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int nrank = arts_get_total_ranks();
  unsigned int workers = arts_get_total_workers();
  arts_guid_t range =
      arts_guid_reserve_range(ARTS_GUID_EVENT, EVENTS, ARTS_HINT_CURRENT_RANK);
  arts_event_hint_t lh = ARTS_EVENT_HINT_LATCH((int32_t)EVENTS);
  arts_guid_t done = arts_event_create(&lh);
  arts_edt_hint_t fh = ARTS_EDT_HINT_DEFAULTS;
  fh.rank = 0;
  arts_guid_t fin = arts_edt_create(finish, 0, NULL, 1, &fh);
  arts_add_dependence(done, fin, 0, DB_MODE_NULL);
  for (unsigned int w = 0; w < workers; w++) {
    arts_edt_hint_t h = ARTS_EDT_HINT_DEFAULTS;
    h.rank = w % nrank;
    uint64_t cpv[4] = {(uint64_t)range, (uint64_t)done, w, workers};
    (void)arts_edt_create(creator, 4, cpv, 0, &h);
    uint64_t spv[3] = {(uint64_t)range, (w + 1u) % workers, workers};
    h.rank = (w + 1u) % nrank;
    (void)arts_edt_create(satisfier, 3, spv, 0, &h);
  }
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
