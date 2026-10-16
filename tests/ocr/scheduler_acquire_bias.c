/* SPDX-License-Identifier: Apache-2.0
 *
 * scheduler_acquire_bias — single-node stress for the +1 acquire bias in
 * arts_handle_ready_edt, and for the readiness wakes a dependence slot does
 * not carry.
 *
 * The +1 bias (arts_handle_ready_edt: rw_cursor=0, acquire_remaining=1, then
 * arts_db_acquire_all adds the real dep count) prevents a premature schedule
 * when every per-dep decrement races to completion BEFORE the seed loop
 * finishes.  An off-by-one would either double-schedule (UAF, EDT body runs
 * twice) or never-schedule (EDT parked forever → hang).  It is stressed with
 * many EDTs each carrying MANY DB deps, all of whose owners resolve
 * concurrently, and each EDT body must run EXACTLY ONCE.
 *
 * Beside them, a chain of EDTs takes one shared block for writing in turn,
 * each link woken by its predecessor through a NO_SLOT control satisfy: a
 * readiness decrement that names no dependence slot, counted in depc like
 * any other.  The resumed acquires push onto the resuming thread's own deque
 * while the multi-dep EDTs keep every worker stealing and popping.  Every
 * link must run exactly once too.
 *
 * Correctness (black-box): run count must equal the EDT count, double-run count
 * must be 0, every per-EDT bitset cell exactly 1.  A lost/never-scheduled EDT
 * stalls the all-done LATCH → ctest TIMEOUT (no in-test watchdog, no spin); a
 * double-schedule trips the double-run check.
 *
 * The EDTs record into the state block with no order among them, which the
 * DB-WRF model does not admit.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "arts.h"

#define MULTI_EDTS 48 /* EDTs exercising the multi-dep +1 bias race */
#define DEPS_PER 6    /* RW DBs each multi-dep EDT acquires (+ state) */
#define CHAIN_LEN 64  /* links of the NO_SLOT-woken write chain */
#define TOTAL_EDTS (MULTI_EDTS + CHAIN_LEN)

/* State DB layout (uint64_t):
 *   [0]                    — run count (atomic; must == TOTAL_EDTS)
 *   [1]                    — double-run count (atomic; must == 0)
 *   [2 .. 2+TOTAL_EDTS-1]  — per-EDT run bitset (multi EDTs, then links) */
#define ST_RUNS 0
#define ST_DOUBLE 1
#define ST_BITS 2
#define ST_NELEMS (ST_BITS + TOTAL_EDTS)

static void mark_run(uint64_t *state, unsigned idx) {
  uint64_t prev = atomic_fetch_add_explicit(
      (_Atomic(uint64_t) *)&state[ST_BITS + idx], 1u, memory_order_acq_rel);
  if (prev != 0u) {
    atomic_fetch_add_explicit((_Atomic(uint64_t) *)&state[ST_DOUBLE], 1u,
                              memory_order_acq_rel);
  }
  atomic_fetch_add_explicit((_Atomic(uint64_t) *)&state[ST_RUNS], 1u,
                            memory_order_acq_rel);
}

/* Multi-dep EDT: depv[0..DEPS_PER-1] = distinct RW DBs, depv[DEPS_PER] = state
 * DB (RW).  paramv: [state_db, my_index, latch]. */
static void multi_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  unsigned idx = (unsigned)paramv[1];
  arts_guid_t latch = (arts_guid_t)paramv[2];
  uint64_t *state = (uint64_t *)depv[DEPS_PER].ptr;
  /* Touch every acquired dep so a not-yet-resolved (NULL) dep would surface. */
  for (unsigned d = 0; d < DEPS_PER; d++) {
    uint64_t *cell = (uint64_t *)depv[d].ptr;
    if (cell != NULL) {
      cell[0] = (uint64_t)idx;
    }
  }
  if (state != NULL) {
    mark_run(state, idx);
  }
  arts_event_satisfy_slot(latch, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
}

/* Chain link: depv[0] = the shared chain DB (RW), depv[1] = state DB (RW); its
 * third readiness decrement is the predecessor's NO_SLOT wake, which has no
 * depv entry.  paramv: [my_index, next_guid_or_0, latch]. */
static void chain_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  unsigned idx = (unsigned)paramv[0];
  arts_guid_t next = (arts_guid_t)paramv[1];
  arts_guid_t latch = (arts_guid_t)paramv[2];
  uint64_t *chain = (uint64_t *)depv[0].ptr;
  uint64_t *state = (uint64_t *)depv[1].ptr;
  if (chain != NULL) {
    chain[0] = (uint64_t)idx;
  }
  if (state != NULL) {
    mark_run(state, MULTI_EDTS + idx);
  }
  arts_event_satisfy_slot(latch, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
  if (next != NULL_GUID) {
    arts_edt_satisfy_slot(next, (uint32_t)-1, (arts_guid_t)0, DB_MODE_NULL);
  }
}

/* verify_edt — bound to all-done LATCH (slot 0) + state DB RO (slot 1). */
static void verify_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  uint64_t *state = (uint64_t *)depv[1].ptr;
  uint64_t runs = atomic_load_explicit((_Atomic(uint64_t) *)&state[ST_RUNS],
                                       memory_order_acquire);
  uint64_t dbl = atomic_load_explicit((_Atomic(uint64_t) *)&state[ST_DOUBLE],
                                      memory_order_acquire);
  if (dbl != 0u) {
    (void)fprintf(stderr,
                  "FAIL: %llu EDT(s) double-scheduled — +1 bias off-by-one\n",
                  (unsigned long long)dbl);
    arts_abort(1);
  }
  if (runs != (uint64_t)TOTAL_EDTS) {
    (void)fprintf(stderr, "FAIL: %llu EDTs ran (want %d) — never-scheduled\n",
                  (unsigned long long)runs, TOTAL_EDTS);
    arts_abort(1);
  }
  for (unsigned i = 0; i < TOTAL_EDTS; i++) {
    uint64_t c = atomic_load_explicit((_Atomic(uint64_t) *)&state[ST_BITS + i],
                                      memory_order_acquire);
    if (c != 1u) {
      (void)fprintf(stderr, "FAIL: EDT %u ran %llu times (want 1)\n", i,
                    (unsigned long long)c);
      arts_abort(1);
    }
  }
  printf("scheduler_acquire_bias: %d multi-dep EDTs + %d chain links each ran "
         "once — PASS\n",
         MULTI_EDTS, CHAIN_LEN);
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== scheduler_acquire_bias (multi=%d x %d deps, chain=%d) ===\n",
              MULTI_EDTS, DEPS_PER, CHAIN_LEN);

  void *state_raw = NULL;
  arts_guid_t state_db =
      arts_db_create(&state_raw, ST_NELEMS * sizeof(uint64_t), ARTS_DB,
                     ARTS_DB_PROP_NONE, NULL);
  if (state_db == NULL_GUID) {
    (void)fprintf(stderr, "FAIL: state_db create NULL_GUID\n");
    arts_abort(1);
  }
  memset(state_raw, 0, ST_NELEMS * sizeof(uint64_t));
  arts_db_release(state_db, DB_MODE_RW);

  arts_event_hint_t latch_hint = ARTS_EVENT_HINT_LATCH(TOTAL_EDTS);
  latch_hint.rank = 0;
  arts_guid_t latch = arts_event_create(&latch_hint);
  if (latch == NULL_GUID) {
    (void)fprintf(stderr, "FAIL: LATCH create NULL_GUID\n");
    arts_abort(1);
  }

  /* Multi-dep EDTs: many DB deps each, all owners resolve concurrently so the
   * per-dep decrements race the +1-biased seed loop. */
  for (int i = 0; i < MULTI_EDTS; i++) {
    uint64_t pv[3] = {(uint64_t)state_db, (uint64_t)i, (uint64_t)latch};
    arts_guid_t e = arts_edt_create(multi_edt, 3, pv, DEPS_PER + 1, NULL);
    for (int d = 0; d < DEPS_PER; d++) {
      void *dp = NULL;
      arts_guid_t db = arts_db_create(&dp, sizeof(uint64_t), ARTS_DB,
                                      ARTS_DB_PROP_NONE, NULL);
      if (db == NULL_GUID) {
        (void)fprintf(stderr, "FAIL [multi=%d dep=%d]: db NULL_GUID\n", i, d);
        arts_abort(1);
      }
      *(uint64_t *)dp = 0u;
      arts_db_release(db, DB_MODE_RW);
      arts_add_dependence(db, e, (uint32_t)d, DB_MODE_RW);
    }
    arts_add_dependence(state_db, e, (uint32_t)DEPS_PER, DB_MODE_RW);
  }

  void *chain_raw = NULL;
  arts_guid_t chain_db = arts_db_create(&chain_raw, sizeof(uint64_t), ARTS_DB,
                                        ARTS_DB_PROP_NONE, NULL);
  if (chain_db == NULL_GUID) {
    (void)fprintf(stderr, "FAIL: chain_db create NULL_GUID\n");
    arts_abort(1);
  }
  *(uint64_t *)chain_raw = 0u;
  arts_db_release(chain_db, DB_MODE_RW);

  /* Pre-reserved link GUIDs, so each link can name its successor. */
  arts_guid_t link[CHAIN_LEN];
  for (int i = 0; i < CHAIN_LEN; i++) {
    link[i] = arts_guid_reserve(ARTS_GUID_EDT, 0);
  }
  for (int i = 0; i < CHAIN_LEN; i++) {
    arts_guid_t next = (i + 1 < CHAIN_LEN) ? link[i + 1] : NULL_GUID;
    uint64_t pv[3] = {(uint64_t)i, (uint64_t)next, (uint64_t)latch};
    /* depc = 3: the two data slots and the predecessor's NO_SLOT wake. */
    arts_edt_create(chain_edt, 3, pv, 3, &(arts_edt_hint_t){.guid = link[i]});
    arts_add_dependence(chain_db, link[i], 0, DB_MODE_RW);
    arts_add_dependence(state_db, link[i], 1, DB_MODE_RW);
  }
  arts_edt_satisfy_slot(link[0], (uint32_t)-1, (arts_guid_t)0, DB_MODE_NULL);

  arts_guid_t v =
      arts_edt_create(verify_edt, 0, NULL, 2, &(arts_edt_hint_t){.rank = 0});
  arts_add_dependence(latch, v, 0, DB_MODE_NULL);
  arts_add_dependence(state_db, v, 1, DB_MODE_RO);
}

int main(int argc, char **argv) {
  /* Non-zero when a rank this process spawned ended badly: their exit status
     reaches nobody else, and a run with a dead rank did not succeed. */
  return arts_rt(argc, argv) != 0 ? 1 : 0;
}
