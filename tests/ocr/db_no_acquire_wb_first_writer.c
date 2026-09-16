/* A datablock created without the creator's auto-acquire whose FIRST USER is
 * a remote WRITER.
 *
 * This is the shape in which the block's payload never exists at its home:
 * the create allocates nothing, and the first thing that happens to the block
 * is an ownership request from another rank, which the home answers without
 * bytes because it has none.  Whoever ends up holding the block must
 * therefore mint its first image itself, and there is exactly one version
 * that first image may carry — the same one every other first image carries,
 * so that the writer's own first release does not mint a colliding stamp and
 * make its real bytes look stale.
 *
 * Two kinds of reader pin the two ways that can go wrong:
 *
 *   - UNORDERED readers, nothing ordering their reads against the release.
 *     Either value is legal for them (the OCR model allows the race); a NULL
 *     pointer is not, because the block has a declared size and its holder is
 *     entitled to storage of that size whatever its contents.  A fan of them
 *     is issued alongside the writer so their requests land WHILE the
 *     ownership round is moving — the window in which the directory still
 *     names a rank that has already given the block up — and one more from
 *     inside the writer, which fixes the shape where the writer provably got
 *     the block first;
 *   - an ORDERED reader, gated on the writer's output event, which the
 *     runtime satisfies only after the writer's datablocks are released.  It
 *     must see the sentinel: if a first image were minted at the version the
 *     writer's release goes on to use, the release's install would retreat as
 *     stale and this reader would read zeroes forever.
 *
 * The readers run two ranks along from the home so the ordered read is served
 * across the directory; on a two-rank run that is the home itself, which
 * exercises the same shape with the read served locally.  GUIDs travel in
 * paramv: an EDT body runs on whichever rank the hint named, and a file-scope
 * variable assigned by the creating EDT exists only in that rank's image.
 */

#include "arts.h"
#include "../test_failure_status.h"

#include <stdint.h>

#define DB_ELEMS 256u
#define DB_BYTES (DB_ELEMS * sizeof(uint64_t))
#define STAMP(i) ((uint64_t)(i) * 11u + 5u)
/* Unordered readers issued with the writer, so requests fall inside the
 * ownership round rather than after it.  Placed round-robin over the ranks
 * that are NEITHER the home nor the writer: a read on the home would be
 * served from the home's own idle-owner copy — materializing it there and
 * erasing the very shape this test is about — and concurrent reads on one
 * rank are combined into a single request by design, so spreading them over
 * the eligible ranks is what puts more than one arrival in the window. */
#define RACERS 6u

static void fail(const char *what) {
  arts_printf("FAIL: %s (rank %u)\n", what, arts_get_current_rank());
  arts_test_fail();
  arts_shutdown();
}

void wb_shutdown_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("PASS db_no_acquire_wb_first_writer\n");
  arts_shutdown();
}

/* paramv = { done event }.  depv[0] = the block, RO — unordered against the
 * write, so only the POINTER is asserted. */
void wb_unordered_reader_edt(uint32_t paramc, const uint64_t *paramv,
                             uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  arts_guid_t done = (arts_guid_t)paramv[0];
  if (depv[0].ptr == NULL) {
    fail("an unordered RO acquire of a datablock held by a remote writer "
         "returned no storage");
    return;
  }
  const volatile uint64_t *p = (const volatile uint64_t *)depv[0].ptr;
  uint64_t sink = 0;
  for (unsigned i = 0; i < DB_ELEMS; i++) {
    sink += p[i]; /* either value is legal; the read must be addressable */
  }
  (void)sink;
  arts_event_satisfy(done, NULL_GUID);
}

/* paramv = { done event }.  depv[0] = the writer's output event,
 * depv[1] = the block, RO — ordered after the write, so the VALUE is
 * asserted. */
void wb_ordered_reader_edt(uint32_t paramc, const uint64_t *paramv,
                           uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  arts_guid_t done = (arts_guid_t)paramv[0];
  if (depv[1].ptr == NULL) {
    fail("an RO acquire ordered after the writer returned no storage");
    return;
  }
  const uint64_t *p = (const uint64_t *)depv[1].ptr;
  for (unsigned i = 0; i < DB_ELEMS; i++) {
    if (p[i] != STAMP(i)) {
      arts_printf("FAIL: element %u reads %llu, expected the writer's %llu "
                  "(rank %u)\n",
                  i, (unsigned long long)p[i], (unsigned long long)STAMP(i),
                  arts_get_current_rank());
      arts_test_fail();
      arts_shutdown();
      return;
    }
  }
  arts_event_satisfy(done, NULL_GUID);
}

/* paramv = { db, done event, home rank }.  depv[0] = the block, RW.  This is
 * the block's first user. */
void wb_writer_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  arts_guid_t db = (arts_guid_t)paramv[0];
  arts_guid_t done = (arts_guid_t)paramv[1];
  unsigned int home = (unsigned int)paramv[2];

  if (depv[0].ptr == NULL) {
    fail("the first RW acquire of a datablock created without an acquire "
         "returned no storage");
    return;
  }

  /* The unordered reader is created from HERE, while this EDT still holds the
   * write turn: its dependence is satisfied at once, so its acquire races
   * this release rather than following it. */
  unsigned int n = arts_get_total_ranks();
  arts_edt_hint_t uh = ARTS_EDT_HINT_DEFAULTS;
  uh.rank = (home + 2u) % n;
  uint64_t upv[1] = {(uint64_t)done};
  arts_guid_t u = arts_edt_create(wb_unordered_reader_edt, 1, upv, 1, &uh);
  arts_add_dependence(db, u, 0, DB_MODE_RO);

  uint64_t *p = (uint64_t *)depv[0].ptr;
  for (unsigned i = 0; i < DB_ELEMS; i++) {
    p[i] = STAMP(i);
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  unsigned int home = arts_get_current_rank();
  unsigned int n = arts_get_total_ranks();

  arts_db_hint_t dh = ARTS_DB_HINT_DEFAULTS;
  dh.rank = home;
  void *addr = NULL;
  arts_guid_t db = arts_db_create(&addr, DB_BYTES, ARTS_DB_DEFAULT,
                                  ARTS_DB_PROP_NO_ACQUIRE, &dh);
  if (db == NULL_GUID) {
    fail("could not create the datablock");
    return;
  }

  /* Both readers report in before the run ends, so neither can be left
   * outstanding at teardown. */
  arts_guid_t done = arts_event_create(&ARTS_EVENT_HINT_LATCH(RACERS + 2u));
  arts_edt_hint_t sh = ARTS_EDT_HINT_DEFAULTS;
  sh.rank = home;
  arts_guid_t s = arts_edt_create(wb_shutdown_edt, 0, NULL, 1, &sh);
  arts_add_dependence(done, s, 0, DB_MODE_NULL);

  arts_guid_t written = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
  arts_edt_hint_t wh = ARTS_EDT_HINT_DEFAULTS;
  wh.rank = (home + 1u) % n;
  wh.output_event = written;
  uint64_t wpv[3] = {(uint64_t)db, (uint64_t)done, (uint64_t)home};
  arts_guid_t w = arts_edt_create(wb_writer_edt, 3, wpv, 1, &wh);
  arts_add_dependence(db, w, 0, DB_MODE_RW);

  arts_edt_hint_t vh = ARTS_EDT_HINT_DEFAULTS;
  vh.rank = (home + 2u) % n;
  uint64_t vpv[1] = {(uint64_t)done};
  arts_guid_t v = arts_edt_create(wb_ordered_reader_edt, 1, vpv, 2, &vh);
  arts_add_dependence(written, v, 0, DB_MODE_NULL);
  arts_add_dependence(db, v, 1, DB_MODE_RO);

  /* Issued with the writer, not after it: these reads race the ownership
   * round, which is the only time the directory can send a reader to a rank
   * that has already handed the block on. */
  for (unsigned i = 0; i < RACERS; i++) {
    arts_edt_hint_t rh = ARTS_EDT_HINT_DEFAULTS;
    /* Two ranks leave nothing eligible (the home and the writer are the whole
     * machine); the read then rides with the writer, where the pointer check
     * still holds even though the directory window cannot be reached. */
    rh.rank = (n > 2u) ? ((home + 2u + (i % (n - 2u))) % n)
                       : ((home + 1u) % n);
    uint64_t rpv[1] = {(uint64_t)done};
    arts_guid_t r = arts_edt_create(wb_unordered_reader_edt, 1, rpv, 1, &rh);
    arts_add_dependence(db, r, 0, DB_MODE_RO);
  }
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
