/* SPDX-License-Identifier: Apache-2.0
 *
 * fam_create_release_remote — a creator's own write turn, made away from the
 * block's home, ends without a round trip and its bytes are the block's.
 *
 * The create acquires, so the creating EDT holds the block from the moment it
 * is made and never receives a grant for it.  Its release is therefore the one
 * write turn that was never granted, and what it leaves behind must still be
 * what a reader on a third rank is given.  Portable: it asserts COHERENCE
 * alone and passes under every protocol.
 */

#include "arts.h"

#include "../test_failure_status.h"

#include <stdint.h>
#include <stdio.h>

#define N 64u
#define SEED 0x7700u

/* depv[0] = the creator's output event, depv[1] = the block, RO. */
static void reader_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  const uint32_t *p = (const uint32_t *)depv[1].ptr;
  if (p == NULL) {
    (void)fprintf(stderr, "FAIL: fam_create_release_remote reader got no "
                          "storage\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  for (uint32_t i = 0; i < N; i++) {
    if (p[i] != SEED + i) {
      (void)fprintf(stderr,
                    "FAIL: fam_create_release_remote [%u] = 0x%X, want 0x%X "
                    "(rank %u)\n",
                    i, p[i], SEED + i, arts_get_current_rank());
      arts_test_fail();
      arts_shutdown();
      return;
    }
  }
  arts_printf("fam_create_release_remote: %u words read back on rank %u — "
              "PASS\n",
              N, arts_get_current_rank());
  arts_shutdown();
}

static void creator_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  unsigned int last = (unsigned int)paramv[0];
  arts_guid_t done = (arts_guid_t)paramv[1];

  void *p = NULL;
  arts_guid_t g = arts_db_create(&p, N * sizeof(uint32_t), ARTS_DB,
                                 ARTS_DB_PROP_NONE,
                                 &(arts_db_hint_t){.rank = 0u});
  if (g == NULL_GUID || p == NULL) {
    (void)fprintf(stderr, "FAIL: fam_create_release_remote create returned "
                          "nothing\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  uint32_t *w = (uint32_t *)p;
  for (uint32_t i = 0; i < N; i++) {
    w[i] = SEED + i;
  }
  arts_db_release(g, DB_MODE_RW);

  arts_guid_t r = arts_edt_create(reader_edt, 0, NULL, 2,
                                  &(arts_edt_hint_t){.rank = last});
  arts_add_dependence(done, r, 0, DB_MODE_NULL);
  arts_add_dependence(g, r, 1, DB_MODE_RO);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  unsigned int nranks = arts_get_total_ranks();
  if (nranks < 2u) {
    arts_printf("SKIP fam_create_release_remote: needs >= 2 ranks (have %u)\n",
                nranks);
    arts_shutdown();
    return;
  }

  /* The creator's own output event is what orders the reader after the turn:
   * an EDT releases every block it holds before its post-event is satisfied. */
  arts_guid_t done = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
  uint64_t pv[2] = {(uint64_t)(nranks - 1u), (uint64_t)done};
  (void)arts_edt_create(creator_edt, 2, pv, 0,
                        &(arts_edt_hint_t){.rank = 1u, .output_event = done});
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
