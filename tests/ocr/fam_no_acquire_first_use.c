/* SPDX-License-Identifier: Apache-2.0
 *
 * fam_no_acquire_first_use — a block created without acquiring it can be
 * acquired by its first users on every rank, and one write then defines it
 * for all of them.
 *
 * A create that acquires nothing hands back no pointer and writes nothing, so
 * the block's first use is the first acquire of it, and its contents are
 * unspecified until a holder writes them.  What this test asserts holds
 * whatever those contents are:
 *
 *   first use   — every rank's first acquire of the never-written block is
 *                 served with storage, the ranks' turns overlapping;
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
#define PATTERN(word) (0x5A000000u + (uint32_t)(word))

/* depv[0] = the gate, depv[1] = the block, RO.  Its bytes are not read: the
 * block has not been written, so they carry no value to check. */
static void first_read_edt(uint32_t paramc, const uint64_t *paramv,
                           uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  if (depv[1].ptr == NULL) {
    (void)fprintf(stderr,
                  "FAIL: fam_no_acquire_first_use first reader got no storage "
                  "(rank %u)\n",
                  arts_get_current_rank());
    arts_test_fail();
  }
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
  arts_printf("fam_no_acquire_first_use: storage for every rank's first use, "
              "and the written bytes after the first write — PASS\n");
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
 * scope, so every first reader has released the block by the time it runs. */
static void write_phase_edt(uint32_t paramc, const uint64_t *paramv,
                            uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t g = (arts_guid_t)paramv[0];
  unsigned int nranks = (unsigned int)paramv[1];

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
  for (unsigned int r = 0; r < nranks; r++) {
    arts_guid_t rd = arts_edt_create(
        first_read_edt, 0, NULL, 2,
        &(arts_edt_hint_t){.rank = r, .finish_event = fe});
    arts_add_dependence(start, rd, 0, DB_MODE_NULL);
    arts_add_dependence(g, rd, 1, DB_MODE_RO);
  }

  uint64_t cpv[2] = {(uint64_t)g, (uint64_t)nranks};
  arts_guid_t c = arts_edt_create(write_phase_edt, 2, cpv, 1,
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
