/* SPDX-License-Identifier: Apache-2.0
 *
 * cxl_ro_cohort — one write turn, then a cohort of readers, two of them on one
 * rank.
 *
 * The four readers are gated on the writer's output event, so every one of
 * them is ordered after the write and must see the whole pattern.  Two share a
 * rank on purpose: that rank asks once and both readers are served by the one
 * turn it is granted, which is the join a reader admitted while its rank's copy
 * is still being filled would break.  Portable: it asserts COHERENCE alone and
 * passes under every protocol.
 */

#include "arts.h"

#include "../test_failure_status.h"

#include <stdint.h>
#include <stdio.h>

#define N 64u
#define SEED 0x3300u
#define READERS 4u

static void writer_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  uint32_t *p = (uint32_t *)depv[0].ptr;
  if (p == NULL) {
    (void)fprintf(stderr, "FAIL: cxl_ro_cohort writer got no storage\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  for (uint32_t i = 0; i < N; i++) {
    p[i] = SEED + i;
  }
}

/* depv[0] = the writer's output event, depv[1] = the block, RO. */
static void reader_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  unsigned int which = (unsigned int)paramv[0];
  const uint32_t *p = (const uint32_t *)depv[1].ptr;
  if (p == NULL) {
    (void)fprintf(stderr, "FAIL: cxl_ro_cohort reader %u got no storage\n",
                  which);
    arts_test_fail();
    arts_shutdown();
    return;
  }
  for (uint32_t i = 0; i < N; i++) {
    if (p[i] != SEED + i) {
      (void)fprintf(stderr,
                    "FAIL: cxl_ro_cohort reader %u [%u] = 0x%X, want 0x%X "
                    "(rank %u)\n",
                    which, i, p[i], SEED + i, arts_get_current_rank());
      arts_test_fail();
      arts_shutdown();
      return;
    }
  }
}

static void done_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("cxl_ro_cohort: %u readers over %u words — PASS\n", READERS, N);
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
    arts_printf("SKIP cxl_ro_cohort: needs >= 2 ranks (have %u)\n", nranks);
    arts_shutdown();
    return;
  }

  void *q = NULL;
  arts_guid_t g =
      arts_db_create(&q, N * sizeof(uint32_t), ARTS_DB,
                     ARTS_DB_PROP_NO_ACQUIRE, &(arts_db_hint_t){.rank = 0u});
  if (g == NULL_GUID) {
    (void)fprintf(stderr, "FAIL: cxl_ro_cohort create\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }

  arts_guid_t written = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
  arts_guid_t w = arts_edt_create(
      writer_edt, 0, NULL, 1,
      &(arts_edt_hint_t){.rank = 1u, .output_event = written});
  arts_add_dependence(g, w, 0, DB_MODE_RW);

  /* Two readers on rank 1, so one rank's cohort is a pair. */
  const unsigned int where[READERS] = {0u, 1u, 1u, nranks - 1u};
  arts_guid_t read[READERS];
  for (unsigned int i = 0; i < READERS; i++) {
    read[i] = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
    uint64_t pv[1] = {(uint64_t)i};
    arts_guid_t r = arts_edt_create(
        reader_edt, 1, pv, 2,
        &(arts_edt_hint_t){.rank = where[i], .output_event = read[i]});
    arts_add_dependence(written, r, 0, DB_MODE_NULL);
    arts_add_dependence(g, r, 1, DB_MODE_RO);
  }

  arts_guid_t tail = arts_edt_create(done_edt, 0, NULL, READERS,
                                     &(arts_edt_hint_t){.rank = 0u});
  for (unsigned int i = 0; i < READERS; i++) {
    arts_add_dependence(read[i], tail, i, DB_MODE_NULL);
  }
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
