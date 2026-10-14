/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* event_satisfy_adddep_window.
 *
 * Target: the single-fire event's satisfy <-> addDep race window
 *   - handler_event_satisfy_slot: prev==1 is the UNIQUE fire trigger — only
 *     that thread writes simple.data and CAS-sets fired.
 *   - arts_add_dependence (event source): push onto the Treiber stack, then
 *     re-load fired (race rescue) and drain — but it MUST NOT CAS fired and
 *     MUST NOT write data.
 *
 * Per iteration, N_SATISFIERS satisfies, each carrying its own data block,
 * race N_DEPS dependences added from other EDTs, all of them depc=0 EDTs so
 * the worker pool runs them concurrently: dependences pushed before the fire,
 * during it, and after it (delivered at once from the stored data).  Even
 * iterations create the dependence adders first and odd ones the satisfiers,
 * so both the waiters pushed ahead of the fire and the late binders are
 * plentiful.  Because
 * the latch starts at 1, exactly one DECR satisfy (prev==1) fires and writes
 * the data; every later one is prev<=0 and is absorbed WITHOUT touching it.
 * So every consumer of an iteration must see the same GUID, and that GUID
 * must be one of the iteration's payloads.  If addDep CAS-set fired or an
 * over-satisfy rewrote the data, consumers within an iteration would
 * disagree; a lost waiter leaves the latch short (ctest TIMEOUT).
 *
 * The events linger after the fire, so the late binders' delivery is defined
 * however the race goes.  The consumers record into one block with no order
 * among them, which the DB-WRF model does not admit.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "arts.h"

#define M_ITERS 64
#define N_DEPS 8
#define N_SATISFIERS 8
#define TOTAL (M_ITERS * N_DEPS)

/* State DB (uint64_t):
 *   [0]                    — delivered_count (atomic)
 *   [ST_CDATA + it*N_DEPS + idx]          — what consumer idx received
 *   [ST_PAYLOAD + it*N_SATISFIERS + s]    — satisfier s's payload
 */
#define ST_DELIVERED 0
#define ST_CDATA 1
#define ST_PAYLOAD (ST_CDATA + TOTAL)
#define ST_NELEMS (ST_PAYLOAD + M_ITERS * N_SATISFIERS)

/* counter_edt — paramv: [state_db, latch, it, idx].
 * depv[0] = event data (RW), depv[1] = state DB (RW). */
static void counter_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  arts_guid_t latch = (arts_guid_t)paramv[1];
  uint64_t it = paramv[2];
  uint64_t idx = paramv[3];
  uint64_t *state = (uint64_t *)depv[1].ptr;

  atomic_store_explicit(
      (_Atomic uint64_t *)&state[ST_CDATA + it * N_DEPS + idx],
      (uint64_t)depv[0].guid, memory_order_release);
  atomic_fetch_add_explicit((_Atomic uint64_t *)&state[ST_DELIVERED], 1u,
                            memory_order_acq_rel);
  arts_event_satisfy_slot(latch, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
}

/* satisfier_edt — paramv: [event, data]. */
static void satisfier_edt(uint32_t paramc, const uint64_t *paramv,
                          uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_event_satisfy((arts_guid_t)paramv[0], (arts_guid_t)paramv[1]);
}

/* depper_edt — register one counter onto the event.
 * paramv: [event, state_db, latch, it, idx]. */
static void depper_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t event = (arts_guid_t)paramv[0];
  uint64_t cpv[4] = {paramv[1], paramv[2], paramv[3], paramv[4]};
  arts_guid_t edt = arts_edt_create(counter_edt, 4, cpv, 2, NULL);
  arts_add_dependence(event, edt, 0, DB_MODE_RW);
  arts_add_dependence((arts_guid_t)paramv[1], edt, 1, DB_MODE_RW);
}

static void verify_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  uint64_t *state = (uint64_t *)depv[1].ptr;

  uint64_t got = atomic_load_explicit((_Atomic uint64_t *)&state[ST_DELIVERED],
                                      memory_order_acquire);
  if (got != (uint64_t)TOTAL) {
    (void)fprintf(stderr, "FAIL: delivered=%llu want %d\n",
                  (unsigned long long)got, TOTAL);
    arts_abort(1);
  }
  for (int it = 0; it < M_ITERS; it++) {
    arts_guid_t first = (arts_guid_t)atomic_load_explicit(
        (_Atomic uint64_t *)&state[ST_CDATA + it * N_DEPS + 0],
        memory_order_acquire);
    int payload = -1;
    for (int s = 0; s < N_SATISFIERS; s++) {
      if ((arts_guid_t)state[ST_PAYLOAD + it * N_SATISFIERS + s] == first) {
        payload = s;
      }
    }
    if (payload < 0) {
      (void)fprintf(stderr,
                    "FAIL [it=%d]: delivered data=%lu is none of the "
                    "iteration's satisfy payloads\n",
                    it, (uint64_t)first);
      arts_abort(1);
    }
    for (int i = 1; i < N_DEPS; i++) {
      arts_guid_t ci = (arts_guid_t)atomic_load_explicit(
          (_Atomic uint64_t *)&state[ST_CDATA + it * N_DEPS + i],
          memory_order_acquire);
      if (ci != first) {
        (void)fprintf(stderr,
                      "FAIL [it=%d]: consumer %d got %lu, consumer 0 got %lu "
                      "(data written more than once)\n",
                      it, i, (uint64_t)ci, (uint64_t)first);
        arts_abort(1);
      }
    }
  }
  printf("event_satisfy_adddep_window: %d iters x %d deps x %d satisfiers = "
         "%d deliveries, data written once by the unique fire — PASS\n",
         M_ITERS, N_DEPS, N_SATISFIERS, TOTAL);
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
  if (latch == NULL_GUID || state_db == NULL_GUID) {
    (void)fprintf(stderr, "FAIL: create NULL_GUID\n");
    arts_abort(1);
  }

  for (int it = 0; it < M_ITERS; it++) {
    arts_event_hint_t h = ARTS_EVENT_HINT_DEFAULTS;
    arts_guid_t ev = arts_event_create(&h);
    if (ev == NULL_GUID) {
      (void)fprintf(stderr, "FAIL [it=%d]: event create\n", it);
      arts_abort(1);
    }

    /* Deppers created first push their waiters before the fire; satisfiers
       created first fire before most of the pushes. */
    for (int half = 0; half < 2; half++) {
      if ((half == 0) == ((it & 1) == 0)) {
        for (int i = 0; i < N_DEPS; i++) {
          uint64_t dpv[5] = {(uint64_t)ev, (uint64_t)state_db, (uint64_t)latch,
                             (uint64_t)it, (uint64_t)i};
          arts_edt_create(depper_edt, 5, dpv, 0, NULL);
        }
      } else {
        for (int s = 0; s < N_SATISFIERS; s++) {
          void *dp = NULL;
          arts_guid_t data = arts_db_create(&dp, sizeof(uint64_t), ARTS_DB,
                                            ARTS_DB_PROP_NONE, NULL);
          state[ST_PAYLOAD + it * N_SATISFIERS + s] = (uint64_t)data;
          uint64_t spv[2] = {(uint64_t)ev, (uint64_t)data};
          arts_edt_create(satisfier_edt, 2, spv, 0, NULL);
        }
      }
    }
  }

  arts_db_release(state_db, DB_MODE_RW);

  arts_guid_t v = arts_edt_create(verify_edt, 0, NULL, 2, NULL);
  arts_add_dependence(latch, v, 0, DB_MODE_NULL);
  arts_add_dependence(state_db, v, 1, DB_MODE_RO);
}

int main(int argc, char **argv) {
  /* Non-zero when a rank this process spawned ended badly: their exit status
     reaches nobody else, and a run with a dead rank did not succeed. */
  return arts_rt(argc, argv) != 0 ? 1 : 0;
}
