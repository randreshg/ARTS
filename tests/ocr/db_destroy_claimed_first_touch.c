/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* db_destroy_claimed_first_touch — a block whose create, on a rank other than
 * its home, claims the first-touch cache its own dependences made is
 * destroyed once every hold is released, and the shutdown leaves nothing
 * parked.
 *
 * Per block g (pre-reserved, home H): a setup EDT on rank C != H makes one to
 * three consumer EDTs on C ready on g first -- read-only, read-write, or both
 * modes at once -- so their acquires find no copy on C and ask H for the
 * block; only then does it create g on C, whose hold serves those consumers
 * while their requests are still on their way to H.  The setup EDT and every
 * consumer count down a latch, and a destroyer on H or on C (alternating)
 * destroys g.  With the argument `wave2` a second wave of consumers acquires
 * g on C after the first wave and the create have released, so requests sent
 * after the claim travel beside the ones sent before it.  Sizes cycle through
 * sized blocks and a zero-sized one.  A consumer's read races the setup EDT's
 * write, so no value is checked: the verdict is the exit status, no FAIL
 * line, and a quiescent shutdown (no QUIESCENCE-DEBUG marker).
 *
 * Needs two ranks.  Its read-write consumers share one rank's turn with the
 * creator unordered, which DB-WRF does not admit.
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "arts.h"
#include "../test_failure_status.h"

#define NBLOCKS 64u

static bool g_wave2 = false;

static arts_guid_t latch_on(unsigned int rank, int32_t n) {
  arts_event_hint_t h = ARTS_EVENT_HINT_LATCH(n);
  h.rank = rank;
  return arts_event_create(&h);
}

static arts_guid_t edt_on(arts_edt_t fn, unsigned int rank, uint32_t paramc,
                          const uint64_t *paramv, uint32_t depc,
                          arts_guid_t output_event) {
  arts_edt_hint_t h = ARTS_EDT_HINT_DEFAULTS;
  h.rank = rank;
  h.output_event = output_event;
  return arts_edt_create(fn, paramc, paramv, depc, &h);
}

static arts_db_access_mode_t cohort_mode(unsigned int cohort, unsigned int i) {
  return (cohort == 0u)   ? DB_MODE_RO
         : (cohort == 1u) ? DB_MODE_RW
                          : ((i & 1u) ? DB_MODE_RW : DB_MODE_RO);
}

/* paramv: g, size */
static void consumer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  if (depv[0].guid != (arts_guid_t)paramv[0]) {
    arts_printf("FAIL: db_destroy_claimed_first_touch slot guid mismatch\n");
    arts_test_fail();
    return;
  }
  if (paramv[1] >= sizeof(uint32_t)) {
    if (depv[0].ptr == NULL) {
      arts_printf("FAIL: db_destroy_claimed_first_touch consumer got no "
                  "storage\n");
      arts_test_fail();
      return;
    }
    (void)atomic_load_explicit((_Atomic(uint32_t) *)depv[0].ptr,
                               memory_order_relaxed);
  }
}

/* paramv: g, all */
static void destroyer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_db_destroy((arts_guid_t)paramv[0]);
  arts_event_satisfy((arts_guid_t)paramv[1], NULL_GUID);
}

/* runs on C after the first wave. paramv: g, size, nconsumers, cohort,
 * destroyer rank, all */
static void wave2(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t g = (arts_guid_t)paramv[0];
  unsigned int nc = (unsigned int)paramv[2];
  unsigned int cohort = (unsigned int)paramv[3];
  unsigned int here = arts_get_current_rank();
  unsigned int where = (unsigned int)paramv[4];
  arts_guid_t latch2 = latch_on(where, (int32_t)nc);
  for (unsigned int i = 0; i < nc; i++) {
    uint64_t cpv[2] = {(uint64_t)g, paramv[1]};
    arts_guid_t x = edt_on(consumer, here, 2, cpv, 1, latch2);
    arts_add_dependence(g, x, 0, cohort_mode(cohort, i));
  }
  uint64_t dpv[2] = {(uint64_t)g, paramv[5]};
  arts_guid_t d = edt_on(destroyer, where, 2, dpv, 1, NULL_GUID);
  arts_add_dependence(latch2, d, 0, DB_MODE_NULL);
}

/* paramv: g, size, nconsumers, cohort (0 RO, 1 RW, 2 both), latch,
 *         destroyer rank, all, wave2 */
static void setup(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t g = (arts_guid_t)paramv[0];
  uint64_t size = paramv[1];
  unsigned int nc = (unsigned int)paramv[2];
  unsigned int cohort = (unsigned int)paramv[3];
  arts_guid_t latch = (arts_guid_t)paramv[4];
  unsigned int here = arts_get_current_rank();
  for (unsigned int i = 0; i < nc; i++) {
    uint64_t cpv[2] = {(uint64_t)g, size};
    arts_guid_t x = edt_on(consumer, here, 2, cpv, 1, latch);
    arts_add_dependence(g, x, 0, cohort_mode(cohort, i));
  }
  void *p = arts_db_create_with_guid(g, size, ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  if (size >= sizeof(uint32_t)) {
    if (p == NULL) {
      arts_printf("FAIL: db_destroy_claimed_first_touch create gave no "
                  "storage\n");
      arts_test_fail();
      return;
    }
    atomic_store_explicit((_Atomic(uint32_t) *)p, 0x5EEDu,
                          memory_order_relaxed);
  }
  if (paramv[7] != 0u) {
    uint64_t wpv[6] = {(uint64_t)g, size, nc, (cohort + 1u) % 3u, paramv[5],
                       paramv[6]};
    arts_guid_t w2 = edt_on(wave2, here, 6, wpv, 1, NULL_GUID);
    arts_add_dependence(latch, w2, 0, DB_MODE_NULL);
    return;
  }
  uint64_t dpv[2] = {(uint64_t)g, paramv[6]};
  arts_guid_t d = edt_on(destroyer, (unsigned int)paramv[5], 2, dpv, 1,
                         NULL_GUID);
  arts_add_dependence(latch, d, 0, DB_MODE_NULL);
}

static void terminal(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (arts_test_status() == 0) {
    arts_printf("PASS: db_destroy_claimed_first_touch %u blocks%s\n", NBLOCKS,
                g_wave2 ? " (second wave)" : "");
  }
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  int argc = (int)paramv[0];
  char **argv = (char **)paramv[1];
  g_wave2 = argc > 1 && strcmp(argv[1], "wave2") == 0;
  unsigned int n = arts_get_total_ranks();
  if (n < 2u) {
    arts_printf("FAIL: db_destroy_claimed_first_touch needs two ranks\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  static const uint64_t sizes[4] = {64u, 0u, 4096u, 8u};
  arts_guid_t all = latch_on(0u, (int32_t)NBLOCKS);
  arts_guid_t t = edt_on(terminal, 0u, 0, NULL, 1, NULL_GUID);
  arts_add_dependence(all, t, 0, DB_MODE_NULL);
  for (unsigned int b = 0; b < NBLOCKS; b++) {
    unsigned int h = b % n;
    unsigned int c = (h + 1u + (b / n) % (n - 1u)) % n;
    unsigned int nc = 1u + b % 3u;
    unsigned int cohort = (b / 4u) % 3u;
    unsigned int where = (b & 1u) ? c : h;
    arts_guid_t g = arts_guid_reserve(ARTS_GUID_DB, h);
    /* The setup EDT's own release counts too. */
    arts_guid_t latch = latch_on(where, (int32_t)(nc + 1u));
    uint64_t pv[8] = {(uint64_t)g,      sizes[b % 4u], nc,
                      cohort,           (uint64_t)latch, where,
                      (uint64_t)all,    g_wave2 ? 1u : 0u};
    (void)edt_on(setup, c, 8, pv, 0, latch);
  }
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
