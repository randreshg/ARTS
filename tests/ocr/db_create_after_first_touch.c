/* SPDX-License-Identifier: Apache-2.0
 *
 * A create whose rank already has a cache for the label — made by a
 * dependence's first touch, not by another create — is still the block's
 * creator: it is handed a pointer, and what it writes through that pointer is
 * what the next holder reads.  The two are raced on purpose; the verdict does
 * not depend on which of them goes first.
 */
#include "arts.h"

#include "../test_failure_status.h"

#include <stdio.h>

#define ROUNDS 16u
#define SENTINEL 0xC0FFEEu

static arts_guid_t g_round_db[ROUNDS];

static void toucher_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  /* A sized, live block owes its holder storage.  No claim about the VALUE:
   * this read may be ordered before the creator's release, which the model
   * leaves to the program to order and this one deliberately does not. */
  if (depv[0].ptr == NULL) {
    (void)fprintf(stderr, "FAIL db_create_after_first_touch: a first-touch "
                          "reader was resolved to NULL\n");
    arts_test_fail();
  }
}

static void creator_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t g = (arts_guid_t)paramv[0];
  arts_db_hint_t h = ARTS_DB_HINT_DEFAULTS;
  h.guid = g;
  void *p = NULL;
  (void)arts_db_create(&p, sizeof(unsigned int), ARTS_DB, ARTS_DB_PROP_NONE,
                       &h);
  if (p == NULL) {
    (void)fprintf(stderr, "FAIL db_create_after_first_touch: an acquiring "
                          "create was handed no pointer\n");
    arts_test_fail();
    return;
  }
  ((unsigned int *)p)[0] = SENTINEL;
}

static void checker_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  const unsigned int *d = (const unsigned int *)depv[0].ptr;
  if (d == NULL || d[0] != SENTINEL) {
    (void)fprintf(stderr,
                  "FAIL db_create_after_first_touch: the creator's bytes did "
                  "not reach the next holder (got 0x%x)\n",
                  d ? d[0] : 0u);
    arts_test_fail();
  }
}

static void done_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  printf("PASS db_create_after_first_touch\n");
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  /* The creating rank must not be the block's home, so that the create takes
   * the remote-creator branch where a first touch can have made the cache.
   * With one rank every create is local and the properties hold trivially. */
  unsigned int ranks = arts_get_total_ranks();
  unsigned int creator_rank = (ranks > 1u) ? 1u : 0u;

  arts_guid_t prev = NULL_GUID;
  for (unsigned int r = 0; r < ROUNDS; r++) {
    g_round_db[r] = arts_guid_reserve(ARTS_GUID_DB, 0);

    arts_guid_t touched = arts_event_create(&ARTS_EVENT_HINT_FINISH);
    arts_guid_t made = arts_event_create(&ARTS_EVENT_HINT_FINISH);
    unsigned int gate = (prev == NULL_GUID) ? 0u : 1u;

    arts_edt_hint_t th = {.finish_event = touched, .rank = creator_rank};
    arts_guid_t t = arts_edt_create(toucher_edt, 0, NULL, 1u + gate, &th);

    uint64_t cpv[1] = {(uint64_t)g_round_db[r]};
    arts_edt_hint_t ch = {.finish_event = made, .rank = creator_rank};
    arts_guid_t c = arts_edt_create(creator_edt, 1, cpv, gate, &ch);

    arts_guid_t seen = arts_event_create(&ARTS_EVENT_HINT_FINISH);
    arts_edt_hint_t kh = {.finish_event = seen, .rank = creator_rank};
    arts_guid_t k = arts_edt_create(checker_edt, 0, NULL, 3, &kh);

    /* The checker is ordered after BOTH racers, so its read is not a race. */
    arts_add_dependence(g_round_db[r], k, 0, DB_MODE_RO);
    arts_add_dependence(touched, k, 1, DB_MODE_NULL);
    arts_add_dependence(made, k, 2, DB_MODE_NULL);

    /* The two racers: the toucher's acquire fires the moment its gate is
     * satisfied, which is what installs the stub before any create. */
    if (gate != 0u) {
      arts_add_dependence(prev, t, 1, DB_MODE_NULL);
      arts_add_dependence(prev, c, 0, DB_MODE_NULL);
    }
    arts_add_dependence(g_round_db[r], t, 0, DB_MODE_RO);

    prev = seen;
  }

  arts_guid_t fin = arts_edt_create(done_edt, 0, NULL, 1, NULL);
  arts_add_dependence(prev, fin, 0, DB_MODE_NULL);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
