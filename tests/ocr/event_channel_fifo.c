/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* event_channel_fifo — a CHANNEL event pairs satisfies with dependences in
 * the order each arrived, whichever side arrives first.
 *
 * Two lanes, one channel each:
 *   - producer first: K satisfies, each carrying a fresh data block, are pushed
 *     before any dependence exists, so the channel buffers all of them; then K
 *     dependences are added in order, in RW mode;
 *   - consumer first: K dependences are added in order, in RO mode, before any
 *     satisfy, so the channel buffers the dependences; then K satisfies follow.
 * Generation g's block holds g, and the EDT that receives it must find g in it
 * and record the GUID it was delivered in its slot of a results block; once
 * every EDT is done, verify_edt compares each lane's recovered sequence with
 * the order the satisfies were pushed.  A delivery out of order shows as a
 * wrong value or a wrong GUID, a lost one as a never-firing finish scope.
 *
 * The receiving EDTs record into one block with no order among them, which the
 * DB-WRF model does not admit.
 *
 * verify_edt deps: slot 0 = finish event (DB_MODE_NULL), slot 1 = ref_db (RO),
 *                  slot 2 = results_db (RO).
 * counter_edt deps: slot 0 = the delivered data block, slot 1 = results_db
 * (RW).
 */

#include <stdint.h>
#include <stdio.h>

#include "arts.h"

#define LANES 2
#define K_GENS 64
#define TOTAL (LANES * K_GENS)

/* paramv: [lane, g].  depv[0] = the block the channel delivered; depv[1] =
 * results_db (RW). */
static void counter_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint64_t lane = paramv[0];
  uint64_t g = paramv[1];
  const uint64_t *value = (const uint64_t *)depv[0].ptr;
  if (value == NULL || *value != g) {
    arts_printf("FAIL: event_channel_fifo lane %llu gen %llu received a block "
                "holding %lld\n",
                (unsigned long long)lane, (unsigned long long)g,
                value ? (long long)*value : -1LL);
    arts_abort(1);
  }
  arts_guid_t *rdb = (arts_guid_t *)depv[1].ptr;
  if (rdb == NULL) {
    arts_printf("FAIL: counter_edt results_db ptr NULL\n");
    arts_abort(1);
  }
  rdb[lane * K_GENS + g] = depv[0].guid;
}

static void verify_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;

  const arts_guid_t *ref = (const arts_guid_t *)depv[1].ptr;
  const arts_guid_t *got = (const arts_guid_t *)depv[2].ptr;
  if (ref == NULL || got == NULL) {
    (void)fprintf(stderr, "FAIL: verify_edt got NULL DB ptr\n");
    arts_abort(1);
  }
  for (int i = 0; i < TOTAL; i++) {
    if (got[i] != ref[i]) {
      (void)fprintf(stderr,
                    "FAIL [lane=%d]: gen %d recovered=%lu, expected=%lu (FIFO "
                    "broken)\n",
                    i / K_GENS, i % K_GENS, (uint64_t)got[i], (uint64_t)ref[i]);
      arts_abort(1);
    }
  }
  arts_printf("event_channel_fifo: %d lanes x %d gens = %d deliveries (FIFO "
              "ordered) — PASS\n",
              LANES, K_GENS, TOTAL);
  arts_shutdown();
}

static arts_guid_t make_value(uint64_t g) {
  uint64_t *p = NULL;
  arts_guid_t db = arts_db_create((void **)&p, sizeof(uint64_t), ARTS_DB,
                                  ARTS_DB_PROP_NONE, NULL);
  *p = g;
  arts_db_release(db, DB_MODE_RW);
  return db;
}

static void add_consumer(arts_guid_t ev, int lane, int g,
                         arts_db_access_mode_t mode, arts_guid_t results_db,
                         arts_guid_t fe) {
  uint64_t pv[2] = {(uint64_t)lane, (uint64_t)g};
  arts_guid_t edt = arts_edt_create(counter_edt, 2, pv, 2,
                                    &(arts_edt_hint_t){.finish_event = fe});
  arts_add_dependence(ev, edt, 0, mode);
  arts_add_dependence(results_db, edt, 1, DB_MODE_RW);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_guid_t *ref = NULL;
  arts_guid_t ref_db =
      arts_db_create((void **)&ref, sizeof(arts_guid_t) * TOTAL, ARTS_DB,
                     ARTS_DB_PROP_NONE, NULL);

  arts_guid_t *res = NULL;
  arts_guid_t results_db =
      arts_db_create((void **)&res, sizeof(arts_guid_t) * TOTAL, ARTS_DB,
                     ARTS_DB_PROP_NONE, NULL);
  for (int i = 0; i < TOTAL; i++) {
    res[i] = NULL_GUID;
  }
  arts_db_release(results_db, DB_MODE_RW);

  arts_guid_t fe = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_event_hint_t h = ARTS_EVENT_HINT_CHANNEL;

  /* Producer first: every satisfy is buffered before its dependence. */
  arts_guid_t pushed = arts_event_create(&h);
  for (int g = 0; g < K_GENS; g++) {
    ref[g] = make_value((uint64_t)g);
    arts_event_satisfy(pushed, ref[g]);
  }
  for (int g = 0; g < K_GENS; g++) {
    add_consumer(pushed, 0, g, DB_MODE_RW, results_db, fe);
  }

  /* Consumer first: every dependence is buffered before its satisfy. */
  arts_guid_t pulled = arts_event_create(&h);
  for (int g = 0; g < K_GENS; g++) {
    add_consumer(pulled, 1, g, DB_MODE_RO, results_db, fe);
  }
  for (int g = 0; g < K_GENS; g++) {
    ref[K_GENS + g] = make_value((uint64_t)g);
    arts_event_satisfy_slot(pulled, ref[K_GENS + g],
                            ARTS_EVENT_LATCH_DECR_SLOT);
  }

  arts_db_release(ref_db, DB_MODE_RW);

  /* The finish event orders every counter_edt's release of results_db before
   * verify_edt's read. */
  arts_guid_t v = arts_edt_create(verify_edt, 0, NULL, 3, NULL);
  arts_add_dependence(fe, v, 0, DB_MODE_NULL);
  arts_add_dependence(ref_db, v, 1, DB_MODE_RO);
  arts_add_dependence(results_db, v, 2, DB_MODE_RO);
}

int main(int argc, char **argv) {
  /* Non-zero when a rank this process spawned ended badly: their exit status
     reaches nobody else, and a run with a dead rank did not succeed. */
  return arts_rt(argc, argv) != 0 ? 1 : 0;
}
