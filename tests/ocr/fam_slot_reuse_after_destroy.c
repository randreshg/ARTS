/* SPDX-License-Identifier: Apache-2.0
 *
 * fam_slot_reuse_after_destroy — a destroyed block's store comes back, so a
 * create/destroy cycle runs on a fixed pool instead of consuming it.
 *
 * One block is live at a time and every block is created from one rank, so
 * every store is carved out of that rank's own share of the pool and the
 * arithmetic is a single rank's.  The iteration count clears the number of
 * blocks that share can hold at once, so a cycle that returns nothing
 * exhausts it and the run dies naming the pool's size, while a cycle that
 * returns each block's store exactly once at its teardown runs to the end.
 * Every iteration's destroy is gated on that iteration's reader, so no
 * acquisition is ever pending when a block is destroyed — the undefined case
 * is deliberately not exercised here.
 *
 * Portable: it asserts COHERENCE alone and passes under every protocol.
 */

#include "arts.h"

#include "../test_failure_status.h"

#include <stdint.h>
#include <stdio.h>

/* A quarter of a mebibyte per block, and more iterations than the smallest
 * share a registered topology gives one rank can hold blocks of that size.
 * Both are read against the configurations the suite registers: a share is
 * the pool's bytes past its header page divided by the ranks, so raising a
 * pool raises the count this must clear. */
#define FAM_REUSE_ITERS 144u
#define N 65536u /* 256 KiB of uint32_t */

#define PATTERN(iter, word) (0xA5000000u + (uint32_t)(iter) + (uint32_t)(word))

/* depv[0] = the block, RW. */
static void writer_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint32_t iter = (uint32_t)paramv[0];
  uint32_t *p = (uint32_t *)depv[0].ptr;
  if (p == NULL) {
    (void)fprintf(stderr,
                  "FAIL: fam_slot_reuse_after_destroy writer got no storage "
                  "(iteration %u)\n",
                  iter);
    arts_test_fail();
    return;
  }
  for (uint32_t i = 0; i < N; i++) {
    p[i] = PATTERN(iter, i);
  }
}

/* depv[0] = the writer's output event, depv[1] = the block, RO. */
static void reader_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint32_t iter = (uint32_t)paramv[0];
  const uint32_t *p = (const uint32_t *)depv[1].ptr;
  if (p == NULL) {
    (void)fprintf(stderr,
                  "FAIL: fam_slot_reuse_after_destroy reader got no storage "
                  "(iteration %u)\n",
                  iter);
    arts_test_fail();
    return;
  }
  for (uint32_t i = 0; i < N; i++) {
    if (p[i] != PATTERN(iter, i)) {
      (void)fprintf(stderr,
                    "FAIL: fam_slot_reuse_after_destroy [%u] = 0x%X, want "
                    "0x%X (iteration %u rank %u)\n",
                    i, p[i], PATTERN(iter, i), iter, arts_get_current_rank());
      arts_test_fail();
      return;
    }
  }
}

/* depv[0] = the reader's output event: every acquisition this block ever had
 * is over before it is destroyed. */
static void destroy_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_db_destroy((arts_guid_t)paramv[0]);
}

static void done_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_printf("fam_slot_reuse_after_destroy: %u blocks of %u KiB created and "
              "destroyed on %u ranks, %u distinct pointers — PASS\n",
              FAM_REUSE_ITERS, (N * (unsigned int)sizeof(uint32_t)) / 1024u,
              arts_get_total_ranks(), (unsigned int)paramv[0]);
  arts_shutdown();
}

/* paramv: iteration index, the pointer the previous iteration's block was
 * handed out at, how many distinct such pointers have been seen.  The count
 * is reported and never judged: the pointer a block hands out is the block's
 * store itself only on a residency that keeps no separate working copy. */
static void iter_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  uint32_t iter = (uint32_t)paramv[0];
  uint64_t prev_ptr = paramv[1];
  uint64_t distinct = paramv[2];
  unsigned int nranks = arts_get_total_ranks();

  void *p = NULL;
  arts_guid_t g = arts_db_create(
      &p, (uint64_t)N * sizeof(uint32_t), ARTS_DB, ARTS_DB_PROP_NONE,
      &(arts_db_hint_t){.rank = iter % nranks});
  if (g == NULL_GUID || p == NULL) {
    (void)fprintf(stderr,
                  "FAIL: fam_slot_reuse_after_destroy create returned nothing "
                  "(iteration %u)\n",
                  iter);
    arts_test_fail();
    arts_shutdown();
    return;
  }
  if ((uint64_t)(uintptr_t)p != prev_ptr) {
    distinct++;
  }

  arts_guid_t wdone = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
  uint64_t wpv[1] = {(uint64_t)iter};
  arts_guid_t w = arts_edt_create(
      writer_edt, 1, wpv, 1,
      &(arts_edt_hint_t){.rank = (iter + 1u) % nranks, .output_event = wdone});
  arts_add_dependence(g, w, 0, DB_MODE_RW);

  arts_guid_t rdone = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
  uint64_t rpv[1] = {(uint64_t)iter};
  arts_guid_t r = arts_edt_create(
      reader_edt, 1, rpv, 2,
      &(arts_edt_hint_t){.rank = (iter + 2u) % nranks, .output_event = rdone});
  arts_add_dependence(wdone, r, 0, DB_MODE_NULL);
  arts_add_dependence(g, r, 1, DB_MODE_RO);

  arts_guid_t ddone = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
  uint64_t dpv[1] = {(uint64_t)g};
  arts_guid_t d =
      arts_edt_create(destroy_edt, 1, dpv, 1,
                      &(arts_edt_hint_t){.rank = 0u, .output_event = ddone});
  arts_add_dependence(rdone, d, 0, DB_MODE_NULL);

  /* The next block is created only once this one's destroy has run, so the
   * pool never holds two of them for a reason of this test's own making. */
  if (iter + 1u < FAM_REUSE_ITERS) {
    uint64_t npv[3] = {(uint64_t)(iter + 1u), (uint64_t)(uintptr_t)p, distinct};
    arts_guid_t n = arts_edt_create(iter_edt, 3, npv, 1,
                                    &(arts_edt_hint_t){.rank = 0u});
    arts_add_dependence(ddone, n, 0, DB_MODE_NULL);
  } else {
    uint64_t fpv[1] = {distinct};
    arts_guid_t f = arts_edt_create(done_edt, 1, fpv, 1,
                                    &(arts_edt_hint_t){.rank = 0u});
    arts_add_dependence(ddone, f, 0, DB_MODE_NULL);
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  uint64_t pv[3] = {0u, 0u, 0u};
  (void)arts_edt_create(iter_edt, 3, pv, 0, &(arts_edt_hint_t){.rank = 0u});
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
