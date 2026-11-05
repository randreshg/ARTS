/* SPDX-License-Identifier: Apache-2.0
 *
 * EDT completion_event + arts_edt_set_result delivery and ordering.
 *
 * Property under test (arts_edt_set_result + run-end completion_event satisfy)
 * ----------------------------------------------------------------------
 * An EDT created with hint.completion_event has a per-EDT result channel.  The
 * run path satisfies that event AFTER the EDT's data blocks have been released,
 * carrying the GUID the body registered via arts_edt_set_result (NULL_GUID if
 * it registered none).  Two facts must hold:
 *
 *   1) Result delivery: a consumer wired on the producer's completion_event
 *      receives the exact GUID the producer passed to arts_edt_set_result.
 *   2) NULL-result default: a producer that never calls arts_edt_set_result
 *      still fires its completion_event, delivering NULL_GUID.
 *   3) Release-before-satisfy ordering (load-bearing for any consumer wired
 *      through a completion event): when the producer registers a DB as its
 *      result and the consumer acquires that DB RO via the completion-event
 *      dependency, the consumer must observe the producer's writes — the
 *      completion event fires only after the producer's DB release published
 *      them.
 *
 * Topology
 * --------
 *   producer (rank 0): writes a result DB, registers it via set_result, has a
 *     completion_event OE_RESULT; the consumer acquires the result DB RO via
 *     OE.
 *   producer_null (rank 0 or remote): no set_result; completion_event OE_NULL
 *     must fire with NULL_GUID, verified by the null-consumer.
 * Multinode: when >1 rank, the result consumer is placed on a remote rank so
 * the completion-event satisfy + DB acquire cross the wire.
 *
 * Both consumers join a group; a collector gated on it verifies a shared
 * pass/fail tally and shuts down.  A stranded wait is caught by ctest TIMEOUT.
 */

#include "../test_failure_status.h"
#include "arts.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#define RESULT_MAGIC 0x5151ACEDu

typedef struct {
  _Atomic(unsigned int) result_ok; /* consumer saw the producer's result+data */
  _Atomic(unsigned int) null_ok;   /* null-consumer saw NULL_GUID */
} tally_t;

/* paramv[0] = tally DB guid (for all bodies). */
#define PV_TALLY 0

/* Producer with a result: depv[0]=result DB (RW), writes magic + registers it
 * as the result GUID.  The runtime releases the DB then satisfies
 * completion_event with this GUID. */
void producer(uint32_t pc, const uint64_t *pv, uint32_t dc,
              arts_edt_dep_t dv[]) {
  (void)pc;
  (void)dc;
  (void)pv;
  unsigned int *d = (unsigned int *)dv[0].ptr;
  if (d) {
    d[0] = RESULT_MAGIC;
  }
  /* Register the result DB as this EDT's result; delivered via
   * completion_event. */
  arts_edt_set_result(dv[0].guid);
}

/* Producer that registers no result: completion_event must deliver NULL_GUID.
 */
void producer_null(uint32_t pc, const uint64_t *pv, uint32_t dc,
                   arts_edt_dep_t dv[]) {
  (void)pc;
  (void)pv;
  (void)dc;
  (void)dv;
  /* deliberately no arts_edt_set_result */
}

/* Consumer of the result: depv[0] = the result DB delivered via the producer's
 * completion_event (RO).  Must observe the magic (release-before-satisfy). */
void result_consumer(uint32_t pc, const uint64_t *pv, uint32_t dc,
                     arts_edt_dep_t dv[]) {
  (void)pc;
  (void)dc;
  tally_t *t = (tally_t *)dv[1].ptr;
  unsigned int *d = (unsigned int *)dv[0].ptr;
  bool ok = (dv[0].guid != NULL_GUID && d != NULL && d[0] == RESULT_MAGIC);
  if (ok) {
    atomic_fetch_add_explicit(&t->result_ok, 1u, memory_order_relaxed);
  } else {
    arts_test_fail();
    arts_printf("FAIL: result_consumer guid=%ld ptr=%p val=%x\n",
                (long)dv[0].guid, (void *)d, d ? d[0] : 0u);
  }
  (void)pv;
}

/* Consumer of the NULL output: depv[0] = completion-event payload (must be
 * NULL_GUID). */
void null_consumer(uint32_t pc, const uint64_t *pv, uint32_t dc,
                   arts_edt_dep_t dv[]) {
  (void)pc;
  (void)dc;
  tally_t *t = (tally_t *)dv[1].ptr;
  if (dv[0].guid == NULL_GUID) {
    atomic_fetch_add_explicit(&t->null_ok, 1u, memory_order_relaxed);
  } else {
    arts_test_fail();
    arts_printf("FAIL: null_consumer expected NULL_GUID got %ld\n",
                (long)dv[0].guid);
  }
  (void)pv;
}

void collector(uint32_t pc, const uint64_t *pv, uint32_t dc,
               arts_edt_dep_t dv[]) {
  (void)pc;
  (void)pv;
  (void)dc;
  tally_t *t = (tally_t *)dv[1].ptr;
  unsigned int r = atomic_load_explicit(&t->result_ok, memory_order_relaxed);
  unsigned int n = atomic_load_explicit(&t->null_ok, memory_order_relaxed);
  arts_printf("edt_completion_event: result_ok=%u null_ok=%u\n", r, n);
  if (r == 1u && n == 1u) {
    arts_printf("PASS edt_completion_event\n");
  } else {
    arts_printf("FAIL edt_completion_event\n");
    arts_abort(1);
  }
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== edt_completion_event ===\n");

  unsigned int nranks = arts_get_total_ranks();
  unsigned int consumer_rank = (nranks > 1) ? 1u : 0u;

  void *tp = NULL;
  arts_guid_t tally =
      arts_db_create(&tp, sizeof(tally_t), ARTS_DB, ARTS_DB_PROP_NONE,
                     &(arts_db_hint_t){.rank = 0});
  tally_t *t = (tally_t *)tp;
  atomic_init(&t->result_ok, 0u);
  atomic_init(&t->null_ok, 0u);
  arts_db_release(tally);

  uint64_t pv[1] = {(uint64_t)tally};

  arts_event_hint_t gh = ARTS_EVENT_HINT_LATCH(1);
  gh.auto_destroy = true;
  arts_guid_t group = arts_event_create(&gh);

  /* --- Result path --- */
  {
    /* Result DB the producer writes and registers. */
    void *rp = NULL;
    arts_guid_t rdb =
        arts_db_create(&rp, sizeof(unsigned int), ARTS_DB, ARTS_DB_PROP_NONE,
                       &(arts_db_hint_t){.rank = 0});
    ((unsigned int *)rp)[0] = 0u;
    arts_db_release(rdb);

    arts_guid_t oe = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));

    arts_guid_t prod = arts_edt_create(
        producer, 1, pv, 1,
        &(arts_edt_hint_t){.rank = 0, .completion_event = oe, .group = group});
    arts_add_dependence(rdb, prod, 0, DB_MODE_RW);

    /* Consumer: slot0 = result DB via completion event (RO), slot1 = tally
     * (RW). */
    arts_guid_t cons = arts_edt_create(
        result_consumer, 1, pv, 2,
        &(arts_edt_hint_t){.rank = consumer_rank, .group = group});
    arts_add_dependence(oe, cons, 0, DB_MODE_RO);
    arts_add_dependence(tally, cons, 1, DB_MODE_RW);
  }

  /* --- NULL-result path --- */
  {
    arts_guid_t oe = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));

    arts_guid_t prod = arts_edt_create(
        producer_null, 1, pv, 0,
        &(arts_edt_hint_t){.rank = 0, .completion_event = oe, .group = group});
    (void)prod;

    arts_guid_t cons = arts_edt_create(
        null_consumer, 1, pv, 2, &(arts_edt_hint_t){.rank = 0, .group = group});
    arts_add_dependence(oe, cons, 0, DB_MODE_NULL);
    arts_add_dependence(tally, cons, 1, DB_MODE_RW);
  }

  /* Collector gated on the group. */
  arts_guid_t coll =
      arts_edt_create(collector, 1, pv, 2, &(arts_edt_hint_t){.rank = 0});
  arts_add_dependence(group, coll, 0, DB_MODE_NULL);
  arts_add_dependence(tally, coll, 1, DB_MODE_RO);
  arts_event_satisfy_slot(group, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
}

int main(int argc, char **argv) {
  /* Two verdicts to merge: what arts_rt saw of the ranks it spawned (their exit
     status reaches nobody else) and what this rank's own checks found. */
  return arts_rt(argc, argv);
}
