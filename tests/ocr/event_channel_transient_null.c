/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* event_channel_transient_null — a CHANNEL event pairs every satisfy with
 * exactly one dependence, however the two queues are contended.
 *
 * A producer links its node into the channel's queue before it bumps the
 * matching count, so a drainer can pop a node whose successor link is still
 * in flight and read it as NULL.  It must retry, never drop the partner it
 * already popped: a dropped node strands a consumer, a count decremented
 * without its node wraps and spins the drainer.
 *
 * Two storms on channel events, all satisfies and dependences issued by EDTs
 * with no dependences, so the workers run them concurrently:
 *  (1) M_ITERS events, each with K_PAIRS single-satisfy EDTs (each its own
 *      data block) and K_PAIRS single-dependence EDTs;
 *  (2) one event with P_SAT EDTs each issuing BURST satisfies of one data
 *      block, and P_DEP EDTs each adding BURST dependences, so several
 *      pushes of one producer are in flight back to back.
 * Each dependence's EDT checks that the GUID it received is one its storm
 * satisfied, counts itself, and decrements a LATCH over every pairing; the
 * verifier runs when the LATCH fires and requires the exact count.  A dropped
 * node leaves the LATCH unfired (the run times out).
 *
 * The counting EDTs increment one block with no order among them, which DB-WRF
 * does not admit.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../test_failure_status.h"
#include "arts.h"

#define M_ITERS 24
#define K_PAIRS 64
#define P_SAT 8
#define P_DEP 8
#define BURST 64
#define PAIR_TOTAL (M_ITERS * K_PAIRS)
#define BURST_TOTAL (P_SAT * BURST) /* == P_DEP * BURST */
#define TOTAL (PAIR_TOTAL + BURST_TOTAL)

/* State block (uint64_t elements): delivered count, foreign-GUID count, the
 * PAIR_TOTAL data GUIDs of storm (1), then the P_SAT data GUIDs of storm (2).
 */
#define ST_DELIVERED 0
#define ST_BAD 1
#define ST_SAT_OFF 2
#define ST_BURST_OFF (ST_SAT_OFF + PAIR_TOTAL)
#define ST_NELEMS (ST_BURST_OFF + P_SAT)

/* paramv: [state_db, latch, first slot of the valid GUID set, set length].
 * depv[0] = channel payload, depv[1] = state block (RW).  Its output event is
 * the LATCH, decremented once its blocks are released, so the verifier gated
 * on the LATCH is ordered after every count. */
static void counter_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint64_t off = paramv[2];
  uint64_t len = paramv[3];
  uint64_t *state = (uint64_t *)depv[1].ptr;
  arts_guid_t received = depv[0].guid;

  int found = 0;
  for (uint64_t g = 0; g < len; g++) {
    _Atomic uint64_t *s = (_Atomic uint64_t *)&state[off + g];
    if ((arts_guid_t)atomic_load_explicit(s, memory_order_acquire) ==
        received) {
      found = 1;
      break;
    }
  }
  if (!found) {
    atomic_fetch_add_explicit((_Atomic uint64_t *)&state[ST_BAD], 1u,
                              memory_order_acq_rel);
  }
  atomic_fetch_add_explicit((_Atomic uint64_t *)&state[ST_DELIVERED], 1u,
                            memory_order_acq_rel);
}

/* paramv: [event, data, count]. */
static void satisfier_edt(uint32_t paramc, const uint64_t *paramv,
                          uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  for (uint64_t i = 0; i < paramv[2]; i++) {
    arts_event_satisfy((arts_guid_t)paramv[0], (arts_guid_t)paramv[1]);
  }
}

/* paramv: [event, count, state_db, latch, set offset, set length]. */
static void depper_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t event = (arts_guid_t)paramv[0];
  uint64_t cpv[4] = {paramv[2], paramv[3], paramv[4], paramv[5]};
  for (uint64_t i = 0; i < paramv[1]; i++) {
    arts_guid_t edt = arts_edt_create(
        counter_edt, 4, cpv, 2,
        &(arts_edt_hint_t){.rank = ARTS_HINT_ANY_RANK,
                           .output_event = (arts_guid_t)paramv[3]});
    arts_add_dependence(event, edt, 0, DB_MODE_RW);
    arts_add_dependence((arts_guid_t)paramv[2], edt, 1, DB_MODE_RW);
  }
}

static void verify_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  uint64_t *state = (uint64_t *)depv[1].ptr;
  if (state == NULL) {
    arts_printf("FAIL: event_channel_transient_null verifier got a NULL "
                "state block\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  uint64_t got = atomic_load_explicit((_Atomic uint64_t *)&state[ST_DELIVERED],
                                      memory_order_acquire);
  uint64_t bad = atomic_load_explicit((_Atomic uint64_t *)&state[ST_BAD],
                                      memory_order_acquire);
  if (got != (uint64_t)TOTAL) {
    arts_printf("FAIL: event_channel_transient_null delivered=%llu want %d\n",
                (unsigned long long)got, TOTAL);
    arts_test_fail();
  }
  if (bad != 0) {
    arts_printf("FAIL: event_channel_transient_null %llu deliveries carried a "
                "GUID their storm never satisfied\n",
                (unsigned long long)bad);
    arts_test_fail();
  }
  if (arts_test_status() == 0) {
    arts_printf("event_channel_transient_null: %d single pairs + %d burst "
                "pairs = %d matched deliveries, no drop — PASS\n",
                PAIR_TOTAL, BURST_TOTAL, TOTAL);
  }
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  uint64_t *state = NULL;
  arts_guid_t state_db =
      arts_db_create((void **)&state, ST_NELEMS * sizeof(uint64_t), ARTS_DB,
                     ARTS_DB_PROP_NONE, NULL);
  memset(state, 0, ST_NELEMS * sizeof(uint64_t));

  arts_event_hint_t lh = ARTS_EVENT_HINT_LATCH(TOTAL);
  lh.rank = 0;
  arts_guid_t latch = arts_event_create(&lh);

  for (int it = 0; it < M_ITERS; it++) {
    arts_event_hint_t ch = ARTS_EVENT_HINT_CHANNEL;
    arts_guid_t ev = arts_event_create(&ch);
    uint64_t off = ST_SAT_OFF + (uint64_t)it * K_PAIRS;
    for (int g = 0; g < K_PAIRS; g++) {
      void *dbp = NULL;
      state[off + g] = (uint64_t)arts_db_create(&dbp, sizeof(uint64_t), ARTS_DB,
                                                ARTS_DB_PROP_NONE, NULL);
    }
    for (int g = 0; g < K_PAIRS; g++) {
      uint64_t spv[3] = {(uint64_t)ev, state[off + g], 1};
      arts_edt_create(satisfier_edt, 3, spv, 0, NULL);
      uint64_t dpv[6] = {(uint64_t)ev,    1,   (uint64_t)state_db,
                         (uint64_t)latch, off, K_PAIRS};
      arts_edt_create(depper_edt, 6, dpv, 0, NULL);
    }
  }

  arts_event_hint_t bh = ARTS_EVENT_HINT_CHANNEL;
  arts_guid_t evb = arts_event_create(&bh);
  for (int p = 0; p < P_SAT; p++) {
    void *dbp = NULL;
    state[ST_BURST_OFF + p] = (uint64_t)arts_db_create(
        &dbp, sizeof(uint64_t), ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  }
  for (int p = 0; p < P_SAT; p++) {
    uint64_t spv[3] = {(uint64_t)evb, state[ST_BURST_OFF + p], BURST};
    arts_edt_create(satisfier_edt, 3, spv, 0, NULL);
  }
  for (int p = 0; p < P_DEP; p++) {
    uint64_t dpv[6] = {(uint64_t)evb,   BURST,        (uint64_t)state_db,
                       (uint64_t)latch, ST_BURST_OFF, P_SAT};
    arts_edt_create(depper_edt, 6, dpv, 0, NULL);
  }

  /* The counting EDTs acquire the state block once this creator hold ends. */
  arts_db_release(state_db, DB_MODE_RW);

  arts_guid_t v = arts_edt_create(verify_edt, 0, NULL, 2, NULL);
  arts_add_dependence(latch, v, 0, DB_MODE_NULL);
  arts_add_dependence(state_db, v, 1, DB_MODE_RO);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
