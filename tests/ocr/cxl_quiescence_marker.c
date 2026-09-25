/* SPDX-License-Identifier: Apache-2.0
 *
 * cxl_quiescence_marker — six data blocks kept in the CXL store, every
 * one created, written, read and released, across every rank a run has.
 *
 * Two acquiring creators release their own write turn without ever being
 * granted; two blocks are written and read back on other ranks through a
 * granted turn; one gathers a two-reader cohort on one rank; the last chains
 * write, read, rewrite and read again to turn its slot over twice.  Every
 * block's last dependent feeds one latch, so the run ends only once every
 * turn this test took has also ended.  A clean run of this shape must leave
 * the arm's teardown walk silent: nothing here destroys a block with a
 * pending acquire, and nothing waits for a wake the runtime does not owe.
 * Portable: it asserts COHERENCE alone and passes under every protocol,
 * but the store's quiescence checks it exercises exist only in a CXL
 * build.
 */

#include "arts.h"

#include "../test_failure_status.h"

#include <stdint.h>
#include <stdio.h>

#define N 32u

static unsigned int rk(unsigned int nranks, unsigned int i) { return i % nranks; }

/* Ungated write: depv[0] = the block, RW. */
static void writer_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint32_t seed = (uint32_t)paramv[0];
  uint32_t *p = (uint32_t *)depv[0].ptr;
  if (p == NULL) {
    (void)fprintf(stderr,
                  "FAIL: cxl_quiescence_marker writer got no storage "
                  "(seed=0x%X)\n",
                  seed);
    arts_test_fail();
    return;
  }
  for (uint32_t i = 0; i < N; i++) {
    p[i] = seed + i;
  }
}

/* Gated write: depv[0] = a NULL ordering dependence, depv[1] = the block,
 * RW. */
static void writer_gated_edt(uint32_t paramc, const uint64_t *paramv,
                             uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint32_t seed = (uint32_t)paramv[0];
  uint32_t *p = (uint32_t *)depv[1].ptr;
  if (p == NULL) {
    (void)fprintf(stderr,
                  "FAIL: cxl_quiescence_marker rewrite got no storage "
                  "(seed=0x%X)\n",
                  seed);
    arts_test_fail();
    return;
  }
  for (uint32_t i = 0; i < N; i++) {
    p[i] = seed + i;
  }
}

/* Gated read: depv[0] = a NULL ordering dependence, depv[1] = the block,
 * RO. */
static void reader_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint32_t seed = (uint32_t)paramv[0];
  const uint32_t *p = (const uint32_t *)depv[1].ptr;
  if (p == NULL) {
    (void)fprintf(stderr,
                  "FAIL: cxl_quiescence_marker reader got no storage "
                  "(seed=0x%X)\n",
                  seed);
    arts_test_fail();
    return;
  }
  for (uint32_t i = 0; i < N; i++) {
    if (p[i] != seed + i) {
      (void)fprintf(stderr,
                    "FAIL: cxl_quiescence_marker [%u] = 0x%X, want 0x%X "
                    "(seed=0x%X rank %u)\n",
                    i, p[i], seed + i, seed, arts_get_current_rank());
      arts_test_fail();
      return;
    }
  }
}

/* Ungated read of a block this rank's own creator just released: depv[0] =
 * the block, RO. */
static void reader_solo_edt(uint32_t paramc, const uint64_t *paramv,
                            uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint32_t seed = (uint32_t)paramv[0];
  const uint32_t *p = (const uint32_t *)depv[0].ptr;
  if (p == NULL) {
    (void)fprintf(stderr,
                  "FAIL: cxl_quiescence_marker solo reader got no storage "
                  "(seed=0x%X)\n",
                  seed);
    arts_test_fail();
    return;
  }
  for (uint32_t i = 0; i < N; i++) {
    if (p[i] != seed + i) {
      (void)fprintf(stderr,
                    "FAIL: cxl_quiescence_marker solo [%u] = 0x%X, want "
                    "0x%X (seed=0x%X rank %u)\n",
                    i, p[i], seed + i, seed, arts_get_current_rank());
      arts_test_fail();
      return;
    }
  }
}

/* An acquiring create's own write turn, released without ever being
 * granted; then a solo reader elsewhere confirms it.  paramv: seed, home
 * rank, reader rank, the scenario's own done event. */
static void creator_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  uint32_t seed = (uint32_t)paramv[0];
  unsigned int home = (unsigned int)paramv[1];
  unsigned int reader_rank = (unsigned int)paramv[2];
  arts_guid_t ev = (arts_guid_t)paramv[3];

  void *p = NULL;
  arts_guid_t g = arts_db_create(&p, N * sizeof(uint32_t), ARTS_DB,
                                 ARTS_DB_PROP_NONE,
                                 &(arts_db_hint_t){.rank = home});
  if (g == NULL_GUID || p == NULL) {
    (void)fprintf(stderr, "FAIL: cxl_quiescence_marker create (seed=0x%X)\n",
                  seed);
    arts_test_fail();
    return;
  }
  uint32_t *w = (uint32_t *)p;
  for (uint32_t i = 0; i < N; i++) {
    w[i] = seed + i;
  }
  arts_db_release(g, DB_MODE_RW);

  uint64_t rpv[1] = {(uint64_t)seed};
  arts_guid_t r = arts_edt_create(reader_solo_edt, 1, rpv, 1,
                                  &(arts_edt_hint_t){.rank = reader_rank,
                                                     .output_event = ev});
  arts_add_dependence(g, r, 0, DB_MODE_RO);
}

/* Cohort join: two readers already checked their own values, this is just
 * the scenario's terminal signal. */
static void tail_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
}

static void done_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("cxl_quiescence_marker: six blocks released on %u ranks — "
              "PASS\n",
              arts_get_total_ranks());
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  unsigned int nranks = arts_get_total_ranks();
  if (nranks < 2u) {
    arts_printf("SKIP cxl_quiescence_marker: needs >= 2 ranks (have %u)\n",
                nranks);
    arts_shutdown();
    return;
  }

  arts_guid_t ev[6];

  /* S0, S3: an acquiring creator's own turn, released remotely. */
  ev[0] = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
  {
    uint64_t pv[4] = {0x91000000u, (uint64_t)rk(nranks, 0u),
                      (uint64_t)rk(nranks, nranks - 1u), (uint64_t)ev[0]};
    (void)arts_edt_create(creator_edt, 4, pv, 0,
                          &(arts_edt_hint_t){.rank = rk(nranks, 1u)});
  }
  ev[3] = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
  {
    uint64_t pv[4] = {0x94000000u, (uint64_t)rk(nranks, 1u),
                      (uint64_t)rk(nranks, 0u), (uint64_t)ev[3]};
    (void)arts_edt_create(creator_edt, 4, pv, 0,
                          &(arts_edt_hint_t){.rank = rk(nranks, nranks - 1u)});
  }

  /* S1, S4: a block written under one rank's granted turn and read back
   * under another. */
  {
    void *q = NULL;
    arts_guid_t g = arts_db_create(&q, N * sizeof(uint32_t), ARTS_DB,
                                   ARTS_DB_PROP_NO_ACQUIRE,
                                   &(arts_db_hint_t){.rank = rk(nranks, 0u)});
    arts_guid_t wdone = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
    uint64_t wpv[1] = {0x92000000u};
    arts_guid_t w = arts_edt_create(
        writer_edt, 1, wpv, 1,
        &(arts_edt_hint_t){.rank = rk(nranks, 1u), .output_event = wdone});
    arts_add_dependence(g, w, 0, DB_MODE_RW);

    ev[1] = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
    uint64_t rpv[1] = {0x92000000u};
    arts_guid_t r = arts_edt_create(
        reader_edt, 1, rpv, 2,
        &(arts_edt_hint_t){.rank = rk(nranks, nranks - 1u),
                           .output_event = ev[1]});
    arts_add_dependence(wdone, r, 0, DB_MODE_NULL);
    arts_add_dependence(g, r, 1, DB_MODE_RO);
  }
  {
    void *q = NULL;
    arts_guid_t g = arts_db_create(&q, N * sizeof(uint32_t), ARTS_DB,
                                   ARTS_DB_PROP_NO_ACQUIRE,
                                   &(arts_db_hint_t){.rank = rk(nranks, 1u)});
    arts_guid_t wdone = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
    uint64_t wpv[1] = {0x95000000u};
    arts_guid_t w = arts_edt_create(
        writer_edt, 1, wpv, 1,
        &(arts_edt_hint_t){.rank = rk(nranks, nranks - 1u),
                           .output_event = wdone});
    arts_add_dependence(g, w, 0, DB_MODE_RW);

    ev[4] = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
    uint64_t rpv[1] = {0x95000000u};
    arts_guid_t r = arts_edt_create(
        reader_edt, 1, rpv, 2,
        &(arts_edt_hint_t){.rank = rk(nranks, 0u), .output_event = ev[4]});
    arts_add_dependence(wdone, r, 0, DB_MODE_NULL);
    arts_add_dependence(g, r, 1, DB_MODE_RO);
  }

  /* S2: a two-reader cohort, both on the same rank. */
  {
    void *q = NULL;
    arts_guid_t g = arts_db_create(
        &q, N * sizeof(uint32_t), ARTS_DB, ARTS_DB_PROP_NO_ACQUIRE,
        &(arts_db_hint_t){.rank = rk(nranks, nranks - 1u)});
    arts_guid_t wdone = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
    uint64_t wpv[1] = {0x93000000u};
    arts_guid_t w = arts_edt_create(
        writer_edt, 1, wpv, 1,
        &(arts_edt_hint_t){.rank = rk(nranks, 0u), .output_event = wdone});
    arts_add_dependence(g, w, 0, DB_MODE_RW);

    ev[2] = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
    arts_guid_t tail =
        arts_edt_create(tail_edt, 0, NULL, 2,
                        &(arts_edt_hint_t){.rank = rk(nranks, 0u),
                                           .output_event = ev[2]});
    for (unsigned int i = 0; i < 2u; i++) {
      arts_guid_t rdone = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
      uint64_t rpv[1] = {0x93000000u};
      arts_guid_t r = arts_edt_create(
          reader_edt, 1, rpv, 2,
          &(arts_edt_hint_t){.rank = rk(nranks, 0u), .output_event = rdone});
      arts_add_dependence(wdone, r, 0, DB_MODE_NULL);
      arts_add_dependence(g, r, 1, DB_MODE_RO);
      arts_add_dependence(rdone, tail, i, DB_MODE_NULL);
    }
  }

  /* S5: write, read, rewrite, read again — the same slot changes hands
   * twice. */
  {
    void *q = NULL;
    arts_guid_t g = arts_db_create(&q, N * sizeof(uint32_t), ARTS_DB,
                                   ARTS_DB_PROP_NO_ACQUIRE,
                                   &(arts_db_hint_t){.rank = rk(nranks, 0u)});
    arts_guid_t w1done = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
    uint64_t w1pv[1] = {0x96000000u};
    arts_guid_t w1 = arts_edt_create(
        writer_edt, 1, w1pv, 1,
        &(arts_edt_hint_t){.rank = rk(nranks, 1u), .output_event = w1done});
    arts_add_dependence(g, w1, 0, DB_MODE_RW);

    arts_guid_t r1done = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
    uint64_t r1pv[1] = {0x96000000u};
    arts_guid_t r1 = arts_edt_create(
        reader_edt, 1, r1pv, 2,
        &(arts_edt_hint_t){.rank = rk(nranks, nranks - 1u),
                           .output_event = r1done});
    arts_add_dependence(w1done, r1, 0, DB_MODE_NULL);
    arts_add_dependence(g, r1, 1, DB_MODE_RO);

    arts_guid_t w2done = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
    uint64_t w2pv[1] = {0x96500000u};
    arts_guid_t w2 = arts_edt_create(
        writer_gated_edt, 1, w2pv, 2,
        &(arts_edt_hint_t){.rank = rk(nranks, 0u), .output_event = w2done});
    arts_add_dependence(r1done, w2, 0, DB_MODE_NULL);
    arts_add_dependence(g, w2, 1, DB_MODE_RW);

    ev[5] = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
    uint64_t r2pv[1] = {0x96500000u};
    arts_guid_t r2 = arts_edt_create(
        reader_edt, 1, r2pv, 2,
        &(arts_edt_hint_t){.rank = rk(nranks, 1u), .output_event = ev[5]});
    arts_add_dependence(w2done, r2, 0, DB_MODE_NULL);
    arts_add_dependence(g, r2, 1, DB_MODE_RO);
  }

  arts_guid_t tail = arts_edt_create(done_edt, 0, NULL, 6,
                                     &(arts_edt_hint_t){.rank = 0u});
  for (unsigned int i = 0; i < 6u; i++) {
    arts_add_dependence(ev[i], tail, i, DB_MODE_NULL);
  }
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
