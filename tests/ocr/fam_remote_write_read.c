/* SPDX-License-Identifier: Apache-2.0
 *
 * fam_remote_write_read — a block written under one rank's write turn is read
 * back, on another rank, through the block's own store.
 *
 * The block is created where it is homed and acquires nothing, so no rank
 * holds it and nothing has written it when the writer asks.  The writer's turn
 * is delivered by a grant and ends at its own zero edge; the reader's turn is
 * delivered by a grant gated on the writer's output event, so the two turns are
 * ordered by the model and the value the reader sees is the value the writer
 * left.  Portable: it asserts COHERENCE alone and passes under every protocol.
 */

#include "arts.h"

#include "../test_failure_status.h"

#include <stdint.h>
#include <stdio.h>

#define N 64u
#define SEED 0x5100u

static void writer_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  uint32_t *p = (uint32_t *)depv[0].ptr;
  if (p == NULL) {
    (void)fprintf(stderr, "FAIL: fam_remote_write_read writer got no "
                          "storage\n");
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
  (void)paramv;
  (void)depc;
  const uint32_t *p = (const uint32_t *)depv[1].ptr;
  if (p == NULL) {
    (void)fprintf(stderr, "FAIL: fam_remote_write_read reader got no "
                          "storage\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  for (uint32_t i = 0; i < N; i++) {
    if (p[i] != SEED + i) {
      (void)fprintf(stderr,
                    "FAIL: fam_remote_write_read [%u] = 0x%X, want 0x%X "
                    "(rank %u)\n",
                    i, p[i], SEED + i, arts_get_current_rank());
      arts_test_fail();
      arts_shutdown();
      return;
    }
  }
  arts_printf("fam_remote_write_read: %u words read back on rank %u — PASS\n",
              N, arts_get_current_rank());
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
    arts_printf("SKIP fam_remote_write_read: needs >= 2 ranks (have %u)\n",
                nranks);
    arts_shutdown();
    return;
  }

  /* Homed here and acquiring nothing: the block exists, nobody holds it, and
   * its store carries whatever a block nobody has written carries. */
  void *q = NULL;
  arts_guid_t g =
      arts_db_create(&q, N * sizeof(uint32_t), ARTS_DB,
                     ARTS_DB_PROP_NO_ACQUIRE, &(arts_db_hint_t){.rank = 0u});
  if (g == NULL_GUID) {
    (void)fprintf(stderr, "FAIL: fam_remote_write_read create\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }

  arts_guid_t written = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
  arts_guid_t w = arts_edt_create(
      writer_edt, 0, NULL, 1,
      &(arts_edt_hint_t){.rank = 1u, .output_event = written});
  arts_add_dependence(g, w, 0, DB_MODE_RW);

  /* The reader goes on the last rank, except where that IS the writer: with
   * two ranks it reads at the block's home instead.  A reader on the writer's
   * own rank asserts nothing about a value leaving the rank that wrote it, and
   * on an arm that lets a writer keep its copy it would be served out of that
   * copy. */
  unsigned int reader_rank = (nranks > 2u) ? (nranks - 1u) : 0u;
  arts_guid_t r = arts_edt_create(reader_edt, 0, NULL, 2,
                                  &(arts_edt_hint_t){.rank = reader_rank});
  arts_add_dependence(written, r, 0, DB_MODE_NULL);
  arts_add_dependence(g, r, 1, DB_MODE_RO);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
