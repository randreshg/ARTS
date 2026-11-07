/* SPDX-License-Identifier: Apache-2.0
 *
 * Writers sharing one rank's write turn on a block release it together, round
 * after round, and hand their bytes to readers.
 *
 * Every EDT runs on rank 0, the block's home.  main_edt creates the block,
 * writes round 0's words into every stripe, downgrades its hold, re-reads the
 * block and releases it.  Each round r then makes K writers ready at once,
 * each with an RW dependence on the block and a stripe of its own: a writer
 * checks that its stripe holds round r-1's words, writes round r's, waits
 * until every writer of the round has written (so the round's releases
 * overlap), and ends its write hold one of three ways by its index: a
 * downgrade, after which it re-reads its stripe and hands the block to a
 * reader that checks the stripe; arts_db_release; or the end of its body.  A
 * checker ordered after the round's writers and hand-off readers reads every
 * stripe and starts round r+1.  No two EDTs touch one word unless an event
 * orders them, so every access is plain.
 */
#include "arts.h"

#include "../test_failure_status.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define NAME "db_shared_turn_release"
#define ROUNDS 32u
#define WRITERS 8u
#define STRIPE 32u
#define WAIT_MS 60000L

/* paramv of a writer and a hand-off reader: round, index, the round's latch;
 * of a checker: round, group. */
enum { P_ROUND, P_INDEX, P_LATCH, P_COUNT };

/* Writes made so far, across rounds; only its own value (the overlap wait). */
static _Atomic(uint64_t) g_written;

static long now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long)t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static uint64_t word(uint64_t r, uint64_t i, uint64_t j) {
  return 0x5A00000000000000ull | (r << 24) | (i << 16) | j;
}

static int downgrades(unsigned int i) { return i % 3u == 0u; }

static unsigned int handoff_readers(void) {
  unsigned int n = 0;
  for (unsigned int i = 0; i < WRITERS; i++) {
    n += (unsigned int)downgrades(i);
  }
  return n;
}

/* 1 when stripe i holds round r's words; reports the first that does not. */
static int check_stripe(const uint64_t *b, unsigned int r, unsigned int i,
                        const char *who) {
  for (unsigned int j = 0; j < STRIPE; j++) {
    uint64_t x = b[i * STRIPE + j];
    if (x != word(r, i, j)) {
      printf("FAIL: " NAME " %s, round %u: stripe %u word %u is 0x%llx, "
             "expected 0x%llx\n",
             who, r, i, j, (unsigned long long)x,
             (unsigned long long)word(r, i, j));
      arts_test_fail();
      return 0;
    }
  }
  return 1;
}

static void start_round(unsigned int r, arts_guid_t db, arts_guid_t group);

/* depv[0]: the block, RO. */
static void handoff_reader_edt(uint32_t paramc, const uint64_t *paramv,
                               uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  if (depv[0].ptr == NULL) {
    printf("FAIL: " NAME " a hand-off reader has no pointer\n");
    arts_test_fail();
    return;
  }
  (void)check_stripe((const uint64_t *)depv[0].ptr,
                     (unsigned int)paramv[P_ROUND],
                     (unsigned int)paramv[P_INDEX], "a hand-off reader");
}

/* depv[0]: the block, RW, shared with the round's other writers.  A writer
 * that finds a wrong word goes on, so that its round still ends. */
static void writer_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  unsigned int r = (unsigned int)paramv[P_ROUND];
  unsigned int i = (unsigned int)paramv[P_INDEX];
  arts_guid_t latch = (arts_guid_t)paramv[P_LATCH];
  arts_guid_t db = depv[0].guid;
  uint64_t *b = (uint64_t *)depv[0].ptr;
  if (b == NULL) {
    printf("FAIL: " NAME " a writer has no pointer\n");
    arts_test_fail();
    if (downgrades(i)) {
      arts_event_satisfy_slot(latch, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
    }
    return;
  }
  (void)check_stripe(b, r - 1u, i, "a writer before its writes");
  for (unsigned int j = 0; j < STRIPE; j++) {
    b[i * STRIPE + j] = word(r, i, j);
  }
  uint64_t all = (uint64_t)r * WRITERS;
  (void)atomic_fetch_add_explicit(&g_written, 1u, memory_order_relaxed);
  long start = now_ms();
  while (atomic_load_explicit(&g_written, memory_order_relaxed) < all) {
    if (now_ms() - start > WAIT_MS) {
      printf("FAIL: " NAME " round %u: writer %u never saw the round's other "
             "writers share the write turn\n",
             r, i);
      arts_test_fail();
      break;
    }
  }
  if (downgrades(i)) {
    arts_db_downgrade(db);
    (void)check_stripe(b, r, i, "a writer after its downgrade");
    arts_guid_t reader = arts_edt_create(
        handoff_reader_edt, P_COUNT, paramv, 1,
        &(arts_edt_hint_t){.rank = arts_get_current_rank(),
                           .completion_event = latch});
    arts_add_dependence(db, reader, 0, DB_MODE_RO);
  } else if (i % 3u == 1u) {
    arts_db_release(db);
  }
}

/* depv[0]: the block, RO; depv[1]: the round's latch. */
static void checker_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  unsigned int r = (unsigned int)paramv[0];
  arts_guid_t db = depv[0].guid;
  const uint64_t *b = (const uint64_t *)depv[0].ptr;
  if (b == NULL) {
    printf("FAIL: " NAME " a checker has no pointer\n");
    arts_test_fail();
    return;
  }
  for (unsigned int i = 0; i < WRITERS; i++) {
    (void)check_stripe(b, r, i, "the checker");
  }
  arts_db_release(db);
  if (r < ROUNDS) {
    start_round(r + 1u, db, (arts_guid_t)paramv[1]);
  }
}

static void start_round(unsigned int r, arts_guid_t db, arts_guid_t group) {
  unsigned int here = arts_get_current_rank();
  arts_event_hint_t lh =
      ARTS_EVENT_HINT_LATCH((int32_t)(WRITERS + handoff_readers()));
  lh.auto_destroy = true;
  arts_guid_t latch = arts_event_create(&lh);
  uint64_t cparam[2] = {r, (uint64_t)group};
  arts_guid_t checker =
      arts_edt_create(checker_edt, 2, cparam, 2,
                      &(arts_edt_hint_t){.rank = here, .group = group});
  arts_add_dependence(db, checker, 0, DB_MODE_RO);
  arts_add_dependence(latch, checker, 1, DB_MODE_NULL);
  for (unsigned int i = 0; i < WRITERS; i++) {
    uint64_t wparam[P_COUNT] = {r, i, (uint64_t)latch};
    arts_guid_t w = arts_edt_create(
        writer_edt, P_COUNT, wparam, 1,
        &(arts_edt_hint_t){
            .rank = here, .group = group, .completion_event = latch});
    arts_add_dependence(db, w, 0, DB_MODE_RW);
  }
}

/* paramv[0]: the block. */
static void finale_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_db_destroy((arts_guid_t)paramv[0]);
  printf("PASS: " NAME " %u rounds x %u writers\n", ROUNDS, WRITERS);
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_event_hint_t gh = ARTS_EVENT_HINT_LATCH(1);
  gh.auto_destroy = true;
  arts_guid_t group = arts_event_create(&gh);

  void *p = NULL;
  arts_db_hint_t h = ARTS_DB_HINT_DEFAULTS;
  h.rank = 0u;
  arts_guid_t db = arts_db_create(&p, WRITERS * STRIPE * sizeof(uint64_t),
                                  ARTS_DB, ARTS_DB_PROP_NONE, &h);
  uint64_t *b = (uint64_t *)p;
  if (b == NULL) {
    printf("FAIL: " NAME " the create returned no pointer\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  for (unsigned int i = 0; i < WRITERS; i++) {
    for (unsigned int j = 0; j < STRIPE; j++) {
      b[i * STRIPE + j] = word(0u, i, j);
    }
  }
  arts_db_downgrade(db);
  for (unsigned int i = 0; i < WRITERS; i++) {
    (void)check_stripe(b, 0u, i, "main_edt after its downgrade");
  }
  arts_db_release(db);

  uint64_t fparam = (uint64_t)db;
  arts_guid_t fin = arts_edt_create(finale_edt, 1, &fparam, 1,
                                    &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(group, fin, 0, DB_MODE_NULL);
  start_round(1u, db, group);
  arts_event_satisfy_slot(group, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
}

int main(int argc, char **argv) {
  return arts_rt(argc, argv);
}
