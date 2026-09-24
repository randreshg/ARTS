/* SPDX-License-Identifier: Apache-2.0
 *
 * A fresh slot's bytes are reproducible under strict mode: a block created and
 * never written reads as the poison byte, whether its slot is new or recycled
 * from a destroyed block.  That is what makes strict mode an oracle -- a
 * missing flush shows up as poison instead of as plausible stale data.
 *
 * Single rank: the pool is this rank's own slice, and recycling is what the
 * rounds exercise -- asserted, not assumed: some later round must be handed
 * the first round's slot back.  A first leg reaches the pool directly and
 * proves the free itself poisons: a slot written, published and freed reads
 * the poison byte while it waits on the free list, before any allocation
 * could have re-poisoned it.  Registered against the strict cfg, so the knob
 * is never read from the program.
 */
#include "arts.h"
#include "arts/fam/pool.h"

#include "../test_failure_status.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define ROUNDS 20u
#define BYTES 4096u

static unsigned int g_round;
static void *g_first_slot;
static bool g_recycled;

static void check_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]);

static void one_round(void) {
  void *p = NULL;
  arts_guid_t g = arts_db_create(&p, BYTES, ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  if (!arts_fam_strict()) {
    /* The registration names a strict cfg; a run without the mode is a
     * misconfiguration, not a reason to pass quietly. */
    (void)fprintf(stderr, "FAIL fam_direct_fresh_slot: strict mode is off\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  if (p == NULL || !arts_fam_contains(p)) {
    (void)fprintf(stderr, "FAIL fam_direct_fresh_slot: round %u got no slot\n",
                  g_round);
    arts_test_fail();
    arts_shutdown();
    return;
  }
  if (g_round == 0) {
    g_first_slot = p;
  } else if (p == g_first_slot) {
    g_recycled = true;
  }
  const unsigned char *b = (const unsigned char *)p;
  for (unsigned int i = 0; i < BYTES; i++) {
    if (b[i] != (unsigned char)ARTS_FAM_POISON_BYTE) {
      (void)fprintf(stderr,
                    "FAIL fam_direct_fresh_slot: round %u byte %u reads 0x%02x, "
                    "not the poison byte\n",
                    g_round, i, b[i]);
      arts_test_fail();
      arts_shutdown();
      return;
    }
  }
  arts_db_release(g, DB_MODE_RW);
  arts_db_destroy(g); /* frees the slot, which the next round reuses */

  g_round++;
  if (g_round == ROUNDS) {
    if (!g_recycled) {
      (void)fprintf(stderr, "FAIL fam_direct_fresh_slot: no round was handed "
                            "the first round's slot back\n");
      arts_test_fail();
      arts_shutdown();
      return;
    }
    printf("PASS fam_direct_fresh_slot\n");
    arts_shutdown();
    return;
  }
  arts_guid_t nxt = arts_edt_create(check_edt, 0, NULL, 0, NULL);
  (void)nxt;
}

static void check_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  one_round();
}

/* The slot is on the free list when it is read, so the only writes it can
 * have seen since the pattern are the free's own. */
static bool freed_slot_reads_poison(void) {
  unsigned char *q = (unsigned char *)arts_fam_alloc(BYTES);
  memset(q, 0x5A, BYTES);
  arts_fam_flush_producer(q, BYTES);
  arts_fam_free(q);
  for (unsigned int i = 0; i < BYTES; i++) {
    if (q[i] != (unsigned char)ARTS_FAM_POISON_BYTE) {
      (void)fprintf(stderr,
                    "FAIL fam_direct_fresh_slot: freed slot byte %u reads "
                    "0x%02x, not the poison byte\n",
                    i, q[i]);
      return false;
    }
  }
  return true;
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (!arts_fam_strict()) {
    (void)fprintf(stderr, "FAIL fam_direct_fresh_slot: strict mode is off\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  if (!freed_slot_reads_poison()) {
    arts_test_fail();
    arts_shutdown();
    return;
  }
  one_round();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
