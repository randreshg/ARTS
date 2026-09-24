/* SPDX-License-Identifier: Apache-2.0
 *
 * fam_ro_turn_overlap — back-to-back read turns of one block on its home rank.
 *
 * One task on the home creates the block and writes a pattern through its
 * create's write turn.  Two chains of readers then run on that same rank: each
 * reader acquires the block read-only, checks every byte, and creates its
 * successor gated on its own output event, which is satisfied after its
 * release.  With two chains the rank's read count keeps falling to zero while
 * the other chain's next read is being asked for, so one read turn ends while
 * the next begins on the same rank, with the request, the grant and the fetch
 * all home-local.  Every reader must see the whole pattern.  Portable: it
 * asserts coherence alone and passes under every protocol.  On a single
 * coherent host the byte check cannot see a missing flush, since there is no
 * second view of memory to go stale; here it checks the read-turn plumbing --
 * every chain completes, the values stay intact, nothing hangs -- and the
 * flush ordering itself is pinned by the recording test fam_edge_flush_order.
 */

#include "arts.h"

#include "../test_failure_status.h"

#include <stdint.h>
#include <stdio.h>

#define BYTES (64u * 1024u)
#define CHAINS 2u
#define LENGTH 1500u

static inline uint8_t pattern(uint32_t i) { return (uint8_t)(i * 31u + 7u); }

static void fail(const char *what, uint32_t at, unsigned got) {
  (void)fprintf(stderr,
                "FAIL fam_ro_turn_overlap: %s at byte %u (0x%02x) on rank %u\n",
                what, at, got, arts_get_current_rank());
  arts_test_fail();
  arts_shutdown();
}

/* paramv = {block, index in the chain, this reader's output event}; depv[0]
 * is the block, RO; depv[1], when present, is the predecessor's output event. */
static void reader_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  const uint8_t *p = (const uint8_t *)depv[0].ptr;
  if (p == NULL) {
    fail("a reader got no storage", 0u, 0u);
    return;
  }
  for (uint32_t i = 0; i < BYTES; i++) {
    if (p[i] != pattern(i)) {
      fail("a reader saw a byte the writer did not write", i, p[i]);
      return;
    }
  }
  if (paramv[1] + 1u < LENGTH) {
    arts_guid_t out = arts_event_create(NULL);
    uint64_t pv[3] = {paramv[0], paramv[1] + 1u, out};
    arts_guid_t r = arts_edt_create(
        reader_edt, 3, pv, 2,
        &(arts_edt_hint_t){.rank = arts_get_current_rank(),
                           .output_event = out});
    arts_add_dependence((arts_guid_t)paramv[0], r, 0, DB_MODE_RO);
    arts_add_dependence((arts_guid_t)paramv[2], r, 1, DB_MODE_NULL);
  }
}

static void creator_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  uint8_t *p = NULL;
  arts_guid_t db =
      arts_db_create((void **)&p, BYTES, ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  if (db == NULL_GUID || p == NULL) {
    fail("the create returned no block", 0u, 0u);
    return;
  }
  for (uint32_t i = 0; i < BYTES; i++) {
    p[i] = pattern(i);
  }
  /* The chains' first readers are ordered after the write by the create's
   * own write turn, which ends when this task releases. */
  for (unsigned c = 0; c < CHAINS; c++) {
    arts_guid_t out = arts_event_create(NULL);
    uint64_t pv[3] = {db, 0u, out};
    arts_guid_t r = arts_edt_create(
        reader_edt, 3, pv, 1,
        &(arts_edt_hint_t){.rank = arts_get_current_rank(),
                           .output_event = out});
    arts_add_dependence(db, r, 0, DB_MODE_RO);
  }
}

static void done_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("PASS fam_ro_turn_overlap: %u chains of %u readers\n", CHAINS,
              LENGTH);
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned home = arts_get_total_ranks() - 1u;
  arts_guid_t done = arts_edt_create(done_edt, 0, NULL, 1, NULL);
  arts_guid_t scope = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_add_dependence(scope, done, 0, DB_MODE_NULL);
  arts_edt_create(creator_edt, 0, NULL, 0,
                  &(arts_edt_hint_t){.rank = home, .finish_event = scope});
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
