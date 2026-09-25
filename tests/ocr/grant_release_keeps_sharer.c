/* SPDX-License-Identifier: Apache-2.0
 *
 * A rank that holds a durable reader copy, then takes the write right and
 * releases, is still a sharer: the NEXT owner's first release must retire
 * it.  The shape here is the one where the next requester is queued while
 * this rank still holds — the writer satisfies the event that lets the next
 * writer request before it has written — so the hand-over is decided while
 * the ex-holder's own release round is still to come, and any registration
 * that round could erase is exactly what this test would catch.
 *
 * Y (a non-home rank) reads (copy), then writes with the next writer Z
 * already asking; Z writes; Y reads, event-ordered after both.  Under the
 * OCR model's exclusive RW turn the only legal value is two increments;
 * the two writers are ordered by that turn alone, never by happens-before,
 * so the program is outside DB-WRF and is not built for the FLUSH arm.
 * Failure is reported by exit status.  Two ranks are enough for the shape
 * (Z is then the home); with three or more Z is another non-home rank, the
 * case where Z's CONFIRM travels the wire.
 */
#include "arts.h"

#include <stdint.h>
#include <stdio.h>
#include <time.h>

static arts_guid_t g_db;

static uint64_t now_ns(void) {
  struct timespec ts;
  (void)clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* paramv[0] = the event that lets the next writer ask.  Satisfied FIRST,
 * then a hold long enough for that request to reach the home, then the
 * write: the hand-over is decided while this rank still holds. */
static void holder_writer_edt(uint32_t paramc, const uint64_t *paramv,
                              uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  int *data = (int *)depv[0].ptr;
  if (data == NULL) {
    (void)fprintf(stderr,
                  "FAIL grant_release_keeps_sharer: holder got NULL ptr\n");
    arts_abort(1);
  }
  arts_event_satisfy((arts_guid_t)paramv[0], NULL_GUID);
  uint64_t t0 = now_ns();
  while (now_ns() - t0 < 3000000ull) {
  }
  data[0] = data[0] + 1;
}

static void next_writer_edt(uint32_t paramc, const uint64_t *paramv,
                            uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  int *data = (int *)depv[0].ptr;
  if (data == NULL) {
    (void)fprintf(stderr, "FAIL grant_release_keeps_sharer: next got NULL ptr\n");
    arts_abort(1);
  }
  data[0] = data[0] + 1;
}

/* paramv[0] = expected value, paramv[1] = 1 when this read ends the run. */
static void reader_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  int want = (int)paramv[0];
  const int *data = (const int *)depv[0].ptr;
  if (data == NULL) {
    (void)fprintf(stderr, "FAIL grant_release_keeps_sharer: reader got NULL ptr\n");
    arts_abort(1);
  }
  if (data[0] != want) {
    (void)fprintf(stderr,
                  "FAIL grant_release_keeps_sharer: ex-holder read %d, "
                  "expected %d — its copy survived the next owner's release "
                  "round\n",
                  data[0], want);
    arts_abort(1);
  }
  if (paramv[1]) {
    printf("PASS grant_release_keeps_sharer: ex-holder was retired and "
           "refetched (%d)\n", data[0]);
    arts_shutdown();
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int nranks = arts_get_total_ranks();
  if (nranks < 2) {
    printf("SKIP grant_release_keeps_sharer: needs 2+ ranks (the copy has to "
           "live off the home)\n");
    arts_shutdown();
    return;
  }
  unsigned int y = 1u;
  unsigned int z = nranks > 2u ? 2u : 0u;

  void *ptr = NULL;
  g_db = arts_db_create(&ptr, sizeof(int), ARTS_DB, ARTS_DB_PROP_NONE,
                        &(arts_db_hint_t){.rank = 0});
  ((int *)ptr)[0] = 0;
  arts_db_release(g_db, DB_MODE_RW);

  /* Y takes a durable copy. */
  arts_guid_t f_read = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  {
    uint64_t p[2] = {0u, 0u};
    arts_guid_t r = arts_edt_create(
        reader_edt, 2, p, 1,
        &(arts_edt_hint_t){.rank = y, .finish_event = f_read});
    arts_add_dependence(g_db, r, 0, DB_MODE_RO);
  }

  /* Y writes, letting Z ask before it has written. */
  arts_event_hint_t sticky = ARTS_EVENT_HINT_STICKY;
  arts_guid_t ask = arts_event_create(&sticky);
  arts_guid_t f_hold = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  {
    uint64_t p[1] = {(uint64_t)ask};
    arts_guid_t w = arts_edt_create(
        holder_writer_edt, 1, p, 2,
        &(arts_edt_hint_t){.rank = y, .finish_event = f_hold});
    arts_add_dependence(g_db, w, 0, DB_MODE_RW);
    arts_add_dependence(f_read, w, 1, DB_MODE_NULL);
  }

  /* Z asks while Y holds, and writes once it has the right. */
  arts_guid_t f_next = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  {
    arts_guid_t w = arts_edt_create(
        next_writer_edt, 0, NULL, 2,
        &(arts_edt_hint_t){.rank = z, .finish_event = f_next});
    arts_add_dependence(g_db, w, 0, DB_MODE_RW);
    arts_add_dependence(ask, w, 1, DB_MODE_NULL);
  }

  /* Y reads after both: the ex-holder's copy is one round behind. */
  {
    uint64_t p[2] = {2u, 1u};
    arts_guid_t r = arts_edt_create(reader_edt, 2, p, 3,
                                    &(arts_edt_hint_t){.rank = y});
    arts_add_dependence(g_db, r, 0, DB_MODE_RO);
    arts_add_dependence(f_hold, r, 1, DB_MODE_NULL);
    arts_add_dependence(f_next, r, 2, DB_MODE_NULL);
  }
}

int main(int argc, char **argv) {
  /* Non-zero when a rank this process spawned ended badly: their exit status
     reaches nobody else, and a run with a dead rank did not succeed. */
  return arts_rt(argc, argv) != 0 ? 1 : 0;
}
