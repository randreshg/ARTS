/* SPDX-License-Identifier: Apache-2.0
 *
 * fam_no_acquire_first_use — a block created without acquiring it has ONE
 * store from the moment it exists, and a write then defines it.
 *
 * A create that acquires nothing hands back no pointer and writes nothing, so
 * the block's first use is the first acquire of it.  A block nobody has
 * written reads as zero there, on every arm: the storage a first use
 * materializes is zeroed, wherever it is materialized.  That VALUE is the
 * subject of the suite's first-use test; what this one asserts is the
 * property a single store gives and a per-rank one would not, which no value
 * check can separate from a per-rank copy that happens to be zeroed too:
 *
 *   consistency — every rank reading the block before any writer exists
 *                 computes the SAME digest over it, because there is one
 *                 store and every reader's copy came out of it;
 *   definedness — after one write turn, every later reader sees exactly the
 *                 bytes that turn left, whatever the block held before it.
 *
 * The readers of the first phase are held behind one gate so their turns
 * overlap, and the writer is ordered behind all of them by the phase's finish
 * scope, so no rank can be reading a written block in the first phase.
 *
 * Portable: it asserts COHERENCE alone and passes under every protocol.
 */

#include "arts.h"

#include "../test_failure_status.h"

#include <stdint.h>
#include <stdio.h>

#define N 64u
#define MAX_RANKS 16u
#define PATTERN(word) (0x5A000000u + (uint32_t)(word))

/* Digests reported by the first phase, one slot per rank, each written by the
 * single report EDT that rank sent and read only once every one of them has
 * been satisfied. */
static uint64_t g_digest[MAX_RANKS];
static unsigned int g_reported[MAX_RANKS];

static uint64_t digest_of(const uint32_t *p, uint32_t n) {
  uint64_t h = 1469598103934665603ull;
  for (uint32_t i = 0; i < n; i++) {
    h ^= (uint64_t)p[i];
    h *= 1099511628211ull;
  }
  return h;
}

/* Runs at the collector's rank, one per reporting rank. */
static void report_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  unsigned int from = (unsigned int)paramv[1];
  if (from >= MAX_RANKS) {
    (void)fprintf(stderr, "FAIL: fam_no_acquire_first_use rank %u is past the "
                          "collector's width\n",
                  from);
    arts_test_fail();
    return;
  }
  g_digest[from] = paramv[0];
  g_reported[from] = 1u;
}

/* depv[0] = the gate, depv[1] = the block, RO.  paramc: the phase's finish
 * scope and the rank the digests are collected on. */
static void first_read_edt(uint32_t paramc, const uint64_t *paramv,
                           uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  arts_guid_t scope = (arts_guid_t)paramv[0];
  unsigned int collector = (unsigned int)paramv[1];
  const uint32_t *p = (const uint32_t *)depv[1].ptr;
  if (p == NULL) {
    (void)fprintf(stderr, "FAIL: fam_no_acquire_first_use reader got no "
                          "storage\n");
    arts_test_fail();
    return;
  }
  uint64_t pv[2] = {digest_of(p, N), (uint64_t)arts_get_current_rank()};
  (void)arts_edt_create(report_edt, 2, pv, 0,
                        &(arts_edt_hint_t){.rank = collector,
                                           .finish_event = scope});
}

/* depv[0] = the writer's output event, depv[1] = the block, RO. */
static void verify_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  const uint32_t *p = (const uint32_t *)depv[1].ptr;
  if (p == NULL) {
    (void)fprintf(stderr, "FAIL: fam_no_acquire_first_use verifier got no "
                          "storage\n");
    arts_test_fail();
    return;
  }
  for (uint32_t i = 0; i < N; i++) {
    if (p[i] != PATTERN(i)) {
      (void)fprintf(stderr,
                    "FAIL: fam_no_acquire_first_use [%u] = 0x%X, want 0x%X "
                    "(rank %u)\n",
                    i, p[i], PATTERN(i), arts_get_current_rank());
      arts_test_fail();
      return;
    }
  }
}

/* depv[0] = the block, RW. */
static void writer_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  uint32_t *p = (uint32_t *)depv[0].ptr;
  if (p == NULL) {
    (void)fprintf(stderr, "FAIL: fam_no_acquire_first_use writer got no "
                          "storage\n");
    arts_test_fail();
    return;
  }
  for (uint32_t i = 0; i < N; i++) {
    p[i] = PATTERN(i);
  }
}

static void done_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("fam_no_acquire_first_use: one digest on every rank before the "
              "first write, and the written bytes after it — PASS\n");
  arts_shutdown();
}

/* Nothing but a gate: its completion satisfies the event the first phase's
 * readers wait on, so their turns start together. */
static void gate_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
}

/* paramv = {the block, the rank count}.  Gated on the first phase's finish
 * scope, so every digest has been reported by the time it runs. */
static void collect_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t g = (arts_guid_t)paramv[0];
  unsigned int nranks = (unsigned int)paramv[1];

  for (unsigned int r = 0; r < nranks; r++) {
    if (g_reported[r] == 0u) {
      (void)fprintf(stderr,
                    "FAIL: fam_no_acquire_first_use rank %u reported no "
                    "digest\n",
                    r);
      arts_test_fail();
      arts_shutdown();
      return;
    }
    if (g_digest[r] != g_digest[0]) {
      (void)fprintf(stderr,
                    "FAIL: fam_no_acquire_first_use rank %u read 0x%016llX "
                    "where rank 0 read 0x%016llX — the block has more than one "
                    "store\n",
                    r, (unsigned long long)g_digest[r],
                    (unsigned long long)g_digest[0]);
      arts_test_fail();
      arts_shutdown();
      return;
    }
  }

  arts_guid_t written = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
  arts_guid_t w = arts_edt_create(
      writer_edt, 0, NULL, 1,
      &(arts_edt_hint_t){.rank = 1u, .output_event = written});
  arts_add_dependence(g, w, 0, DB_MODE_RW);

  arts_guid_t fe = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  for (unsigned int r = 0; r < nranks; r++) {
    arts_guid_t v = arts_edt_create(
        verify_edt, 0, NULL, 2,
        &(arts_edt_hint_t){.rank = r, .finish_event = fe});
    arts_add_dependence(written, v, 0, DB_MODE_NULL);
    arts_add_dependence(g, v, 1, DB_MODE_RO);
  }
  arts_guid_t d = arts_edt_create(done_edt, 0, NULL, 1,
                                  &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(fe, d, 0, DB_MODE_NULL);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  unsigned int nranks = arts_get_total_ranks();
  if (nranks < 2u) {
    arts_printf("SKIP fam_no_acquire_first_use: needs >= 2 ranks (have %u)\n",
                nranks);
    arts_shutdown();
    return;
  }
  if (nranks > MAX_RANKS) {
    (void)fprintf(stderr, "FAIL: fam_no_acquire_first_use is built for at "
                          "most %u ranks (have %u)\n",
                  MAX_RANKS, nranks);
    arts_test_fail();
    arts_shutdown();
    return;
  }

  /* Acquiring nothing: the block exists, nobody holds it, and nothing has
   * written it. */
  void *q = NULL;
  arts_guid_t g =
      arts_db_create(&q, N * sizeof(uint32_t), ARTS_DB,
                     ARTS_DB_PROP_NO_ACQUIRE, &(arts_db_hint_t){.rank = 0u});
  if (g == NULL_GUID) {
    (void)fprintf(stderr, "FAIL: fam_no_acquire_first_use create\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }

  arts_guid_t start = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
  arts_guid_t fe = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  uint64_t rpv[2] = {(uint64_t)fe, 0u};
  for (unsigned int r = 0; r < nranks; r++) {
    arts_guid_t rd = arts_edt_create(
        first_read_edt, 2, rpv, 2,
        &(arts_edt_hint_t){.rank = r, .finish_event = fe});
    arts_add_dependence(start, rd, 0, DB_MODE_NULL);
    arts_add_dependence(g, rd, 1, DB_MODE_RO);
  }

  uint64_t cpv[2] = {(uint64_t)g, (uint64_t)nranks};
  arts_guid_t c = arts_edt_create(collect_edt, 2, cpv, 1,
                                  &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(fe, c, 0, DB_MODE_NULL);

  /* Wired last, so the gate cannot fire before every reader is waiting on
   * it. */
  (void)arts_edt_create(gate_edt, 0, NULL, 0,
                        &(arts_edt_hint_t){.rank = 0u, .output_event = start});
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
