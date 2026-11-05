/* SPDX-License-Identifier: Apache-2.0
 *
 * A downgrade of a create's hold on a cache the creating rank had already
 * made by a first touch.
 *
 * On rank 1, D makes a reader R1 ready with an RO dependence on a reserved
 * GUID homed on rank 0, before the block exists: R1's acquire runs in D's
 * call and leaves a first-touch cache and a read request on rank 1.  D then
 * creates the block under that GUID, which claims the cache (where the arm
 * serves parked readers from the create's hold, R1 is admitted under it).  D
 * writes, downgrades, hands the block to R2 on rank 0 and re-reads its bytes.
 * R1 runs concurrently with D's writes, so it does not read the payload.  A
 * finale ordered after D, R1 and R2 destroys the block.
 */
#include "arts.h"

#include "../test_failure_status.h"

#include <stdint.h>
#include <stdio.h>

#define NAME "db_downgrade_claimed"
#define WORDS 128u

static uint64_t word(unsigned int j) { return 0xC1A10000ull + j; }

static void check(const uint64_t *p, const char *who) {
  for (unsigned int j = 0; j < WORDS; j++) {
    if (p == NULL || p[j] != word(j)) {
      printf("FAIL: " NAME " %s: word %u is 0x%llx, expected 0x%llx\n", who, j,
             p ? (unsigned long long)p[j] : 0ull, (unsigned long long)word(j));
      arts_test_fail();
      return;
    }
  }
}

/* depv[0]: the block, RO, unordered with D's writes. */
static void r1_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  if (depv[0].ptr == NULL) {
    printf("FAIL: " NAME " R1 has no pointer\n");
    arts_test_fail();
  }
}

/* depv[0]: the block, RO, after D's downgrade. */
static void r2_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  check((const uint64_t *)depv[0].ptr, "R2");
}

/* paramv: the reserved GUID, the latch over D's, R1's and R2's completions,
 * the event that wakes R2. */
static void d_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t g = (arts_guid_t)paramv[0];
  arts_guid_t done = (arts_guid_t)paramv[1];
  arts_guid_t wake = (arts_guid_t)paramv[2];
  arts_guid_t r1 =
      arts_edt_create(r1_edt, 0, NULL, 1,
                      &(arts_edt_hint_t){.rank = arts_get_current_rank(),
                                         .completion_event = done});
  arts_add_dependence(g, r1, 0, DB_MODE_RO);

  arts_db_hint_t h = ARTS_DB_HINT_DEFAULTS;
  h.guid = g;
  void *p = NULL;
  (void)arts_db_create(&p, WORDS * sizeof(uint64_t), ARTS_DB, ARTS_DB_PROP_NONE,
                       &h);
  if (p == NULL) {
    printf("FAIL: " NAME " an acquiring create was handed no pointer\n");
    arts_test_fail();
    return;
  }
  uint64_t *d = (uint64_t *)p;
  for (unsigned int j = 0; j < WORDS; j++) {
    d[j] = word(j);
  }
  arts_db_downgrade(g);
  arts_event_satisfy(wake, g);
  check(d, "D after its downgrade");
}

/* paramv[0]: the block. */
static void finale_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_db_destroy((arts_guid_t)paramv[0]);
  printf("PASS: " NAME "\n");
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (arts_get_total_ranks() < 2u) {
    printf("FAIL: " NAME " needs two ranks\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  arts_guid_t g = arts_guid_reserve(ARTS_GUID_DB, 0);
  arts_event_hint_t ch = ARTS_EVENT_HINT_LATCH(3);
  arts_guid_t done = arts_event_create(&ch);
  uint64_t fparam = (uint64_t)g;
  arts_guid_t fin = arts_edt_create(finale_edt, 1, &fparam, 1,
                                    &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(done, fin, 0, DB_MODE_NULL);

  arts_event_hint_t eh = ARTS_EVENT_HINT_LATCH(1);
  arts_guid_t wake = arts_event_create(&eh);
  arts_guid_t r2 =
      arts_edt_create(r2_edt, 0, NULL, 1,
                      &(arts_edt_hint_t){.rank = 0u, .completion_event = done});
  arts_add_dependence(wake, r2, 0, DB_MODE_RO);
  uint64_t dparam[3] = {(uint64_t)g, (uint64_t)done, (uint64_t)wake};
  arts_edt_create(d_edt, 3, dparam, 0,
                  &(arts_edt_hint_t){.rank = 1u, .completion_event = done});
}

int main(int argc, char **argv) {
  return arts_rt(argc, argv);
}
