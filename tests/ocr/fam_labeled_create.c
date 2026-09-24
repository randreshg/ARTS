/* SPDX-License-Identifier: Apache-2.0
 *
 * fam_labeled_create — one label names one block, so it names one store.
 *
 * Each cycle one rank creates a pre-reserved label without acquiring it; the
 * creator rotates over the ranks, so the label's home creates some blocks
 * itself and has the others announced to it.
 *
 * Two verdicts.  The block's bytes: one write turn on a rank that is not the
 * home, and every rank reads back exactly what it left — a second store for
 * the label would show up as a reader seeing an unwritten block.  And the
 * pool: each cycle destroys its block, so only one is ever live, and a create
 * that allocated a store the block does not use would leave one behind per
 * cycle and exhaust a rank's share of the pool long before the last one.
 *
 * Portable: it asserts COHERENCE alone and passes under every protocol.
 */

#include "arts.h"

#include "../test_failure_status.h"

#include <stdint.h>
#include <stdio.h>

/* A quarter of a mebibyte per block, and more cycles than the smallest share a
 * registered topology gives one rank can hold blocks of that size.  Every
 * multinode configuration this test runs under leaves each rank a 16 MiB share
 * (a 32 MB pool over two ranks, 48 over three, 64 over four), so 64 blocks;
 * one block is live at a time, so a cycle that left its store behind dies
 * inside this count.  Both are read against the configurations the suite
 * registers: raising a pool raises the count this must clear. */
#define CYCLES 80u
#define N 65536u /* 256 KiB of uint32_t */

#define PATTERN(iter, word) (0xC3000000u + (uint32_t)(iter) + (uint32_t)(word))

static void iter_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]);

/* paramv = {the label}. */
static void create_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  void *p = arts_db_create_with_guid((arts_guid_t)paramv[0],
                                     (uint64_t)N * sizeof(uint32_t), ARTS_DB,
                                     ARTS_DB_PROP_NO_ACQUIRE, NULL);
  if (p != NULL) {
    (void)fprintf(stderr,
                  "FAIL: fam_labeled_create a create that acquires nothing "
                  "handed out a pointer (rank %u)\n",
                  arts_get_current_rank());
    arts_test_fail();
  }
}

/* depv[0] = the block, RW. */
static void writer_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint32_t iter = (uint32_t)paramv[0];
  uint32_t *p = (uint32_t *)depv[0].ptr;
  if (p == NULL) {
    (void)fprintf(stderr,
                  "FAIL: fam_labeled_create writer got no storage (cycle %u)\n",
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
                  "FAIL: fam_labeled_create reader got no storage (cycle %u)\n",
                  iter);
    arts_test_fail();
    return;
  }
  for (uint32_t i = 0; i < N; i++) {
    if (p[i] != PATTERN(iter, i)) {
      (void)fprintf(stderr,
                    "FAIL: fam_labeled_create [%u] = 0x%X, want 0x%X (cycle %u "
                    "rank %u)\n",
                    i, p[i], PATTERN(iter, i), iter, arts_get_current_rank());
      arts_test_fail();
      return;
    }
  }
}

/* depv[0] = the cycle's readers: every acquisition the block ever had is over
 * before it is destroyed, so the undefined case is not exercised here. */
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
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("fam_labeled_create: %u labels of %u KiB, each created by one "
              "of %u ranks in turn, read back on all of them and destroyed — "
              "PASS\n",
              CYCLES, (N * (unsigned int)sizeof(uint32_t)) / 1024u,
              arts_get_total_ranks());
  arts_shutdown();
}

/* paramv = {the range, the cycle, the rank count}.  Gated on the cycle's
 * create, so the label exists before a turn is asked for. */
static void turn_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t base = (arts_guid_t)paramv[0];
  unsigned int iter = (unsigned int)paramv[1];
  unsigned int nranks = (unsigned int)paramv[2];
  arts_guid_t g = arts_guid_from_index(base, iter);

  /* Never the home: the write turn has to be granted and purged back to the
   * label's one store. */
  unsigned int writer_rank = 1u + (iter % (nranks - 1u));
  arts_guid_t written = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
  uint64_t wpv[1] = {(uint64_t)iter};
  arts_guid_t w = arts_edt_create(
      writer_edt, 1, wpv, 1,
      &(arts_edt_hint_t){.rank = writer_rank, .output_event = written});
  arts_add_dependence(g, w, 0, DB_MODE_RW);

  arts_guid_t fe = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  for (unsigned int r = 0; r < nranks; r++) {
    arts_guid_t rd = arts_edt_create(
        reader_edt, 1, wpv, 2,
        &(arts_edt_hint_t){.rank = r, .finish_event = fe});
    arts_add_dependence(written, rd, 0, DB_MODE_NULL);
    arts_add_dependence(g, rd, 1, DB_MODE_RO);
  }

  /* The next cycle's label is created only once this one's destroy has run, so
   * the pool never holds two blocks for a reason of this test's own making. */
  arts_guid_t ddone = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
  uint64_t dpv[1] = {(uint64_t)g};
  arts_guid_t d =
      arts_edt_create(destroy_edt, 1, dpv, 1,
                      &(arts_edt_hint_t){.rank = 0u, .output_event = ddone});
  arts_add_dependence(fe, d, 0, DB_MODE_NULL);

  if (iter + 1u < CYCLES) {
    uint64_t npv[3] = {(uint64_t)base, (uint64_t)(iter + 1u), (uint64_t)nranks};
    arts_guid_t n = arts_edt_create(iter_edt, 3, npv, 1,
                                    &(arts_edt_hint_t){.rank = 0u});
    arts_add_dependence(ddone, n, 0, DB_MODE_NULL);
  } else {
    arts_guid_t f = arts_edt_create(done_edt, 0, NULL, 1,
                                    &(arts_edt_hint_t){.rank = 0u});
    arts_add_dependence(ddone, f, 0, DB_MODE_NULL);
  }
}

/* paramv = {the range, the cycle, the rank count}. */
static void iter_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t base = (arts_guid_t)paramv[0];
  unsigned int iter = (unsigned int)paramv[1];
  unsigned int nranks = (unsigned int)paramv[2];

  arts_guid_t fe = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  uint64_t cpv[1] = {(uint64_t)arts_guid_from_index(base, iter)};
  (void)arts_edt_create(
      create_edt, 1, cpv, 0,
      &(arts_edt_hint_t){.rank = iter % nranks, .finish_event = fe});
  uint64_t tpv[3] = {(uint64_t)base, (uint64_t)iter, (uint64_t)nranks};
  arts_guid_t t = arts_edt_create(turn_edt, 3, tpv, 1,
                                  &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(fe, t, 0, DB_MODE_NULL);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  unsigned int nranks = arts_get_total_ranks();
  if (nranks < 2u) {
    arts_printf("SKIP fam_labeled_create: needs >= 2 ranks (have %u)\n",
                nranks);
    arts_shutdown();
    return;
  }

  /* All homed on rank 0.  One label per cycle and never reused: a label
   * whose block was destroyed is not created again. */
  arts_guid_t base = arts_guid_reserve_range(ARTS_GUID_DB, CYCLES, 0u);
  if (base == NULL_GUID) {
    (void)fprintf(stderr, "FAIL: fam_labeled_create could not reserve its "
                          "labels\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  uint64_t pv[3] = {(uint64_t)base, 0u, (uint64_t)nranks};
  (void)arts_edt_create(iter_edt, 3, pv, 0, &(arts_edt_hint_t){.rank = 0u});
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
