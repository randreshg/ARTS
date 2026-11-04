/* SPDX-License-Identifier: Apache-2.0
 *
 * Two writers of one block share their rank's write turn, and one of them
 * downgrades while the other still writes.
 *
 * D holds the block RW and makes W1 ready on its own rank with an RW
 * dependence on it, so both hold it for writing at once.  W1 writes range
 * B1 and raises its flag in the block's last word; D waits for it,
 * downgrades, raises its own flag, hands the block to a reader on its rank,
 * and re-reads A (its own bytes) and B1 (W1's, written before the flag D
 * read); W1 waits for D's flag, writes range B2 and ends.  D no longer writes
 * the block once it downgraded, so its flag is in process memory, which the
 * two share on one rank.  B1, B2 and W1's flag are touched only through
 * _Atomic views.  A last reader, ordered after both writers, checks A and B2
 * with no writer live; it runs on another rank when there is one.
 *
 * Two write acquisitions of one block overlap here, which DB-WRF forbids.
 */
#include "arts.h"

#include "../test_failure_status.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define NAME "db_downgrade_cowriter"
#define WA 64u
#define WB 64u
#define B1 WA
#define B2 (WA + WB)
#define F_W1 (WA + 2u * WB)
#define WORDS (F_W1 + 1u)
#define WAIT_MS 60000L

static long now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long)t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static uint64_t a_word(unsigned int j) { return 0xAA000000ull + j; }
static uint64_t b1_word(unsigned int j) { return 0xB1000000ull + j; }
static uint64_t b2_word(unsigned int j) { return 0xB2000000ull + j; }

static _Atomic(uint64_t) *view(void *p) { return (_Atomic(uint64_t) *)p; }

static _Atomic(uint64_t) g_d_flag;

static void check_range(_Atomic(uint64_t) *v, unsigned int from,
                        uint64_t (*want)(unsigned int), const char *range,
                        const char *who) {
  for (unsigned int j = 0; j < WB; j++) {
    uint64_t x = atomic_load_explicit(&v[from + j], memory_order_relaxed);
    if (x != want(j)) {
      printf("FAIL: " NAME " %s: %s word %u is 0x%llx, expected 0x%llx\n", who,
             range, j, (unsigned long long)x, (unsigned long long)want(j));
      arts_test_fail();
      return;
    }
  }
}

/* Waits for a flag the other writer raises; 0 at the deadline. */
static int await_flag(_Atomic(uint64_t) *f, const char *who) {
  long start = now_ms();
  while (atomic_load_explicit(f, memory_order_acquire) == 0u) {
    if (now_ms() - start > WAIT_MS) {
      printf("FAIL: " NAME " %s never saw the other writer's flag\n", who);
      arts_test_fail();
      return 0;
    }
  }
  return 1;
}

/* depv[0]: the block, RW, shared with D's hold. */
static void w1_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  if (depv[0].ptr == NULL) {
    printf("FAIL: " NAME " W1 has no pointer\n");
    arts_test_fail();
    return;
  }
  _Atomic(uint64_t) *v = view(depv[0].ptr);
  for (unsigned int j = 0; j < WB; j++) {
    atomic_store_explicit(&v[B1 + j], b1_word(j), memory_order_relaxed);
  }
  atomic_store_explicit(&v[F_W1], 1u, memory_order_release);
  if (!await_flag(&g_d_flag, "W1")) {
    return;
  }
  for (unsigned int j = 0; j < WB; j++) {
    atomic_store_explicit(&v[B2 + j], b2_word(j), memory_order_relaxed);
  }
}

/* depv[0]: the block, RO. */
static void handoff_reader_edt(uint32_t paramc, const uint64_t *paramv,
                               uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  if (depv[0].ptr == NULL) {
    printf("FAIL: " NAME " the hand-off reader has no pointer\n");
    arts_test_fail();
    return;
  }
  check_range(view(depv[0].ptr), 0, a_word, "A", "the hand-off reader");
}

/* depv[0]: the block, RO; depv[1]: both writers' completions. */
static void last_reader_edt(uint32_t paramc, const uint64_t *paramv,
                            uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  if (depv[0].ptr == NULL) {
    printf("FAIL: " NAME " the last reader has no pointer\n");
    arts_test_fail();
    return;
  }
  _Atomic(uint64_t) *v = view(depv[0].ptr);
  check_range(v, 0, a_word, "A", "the last reader");
  check_range(v, B2, b2_word, "B2", "the last reader");
}

/* paramv[0]: the writers' completion event.  depv[0]: the block, RW. */
static void d_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  arts_guid_t done = (arts_guid_t)paramv[0];
  arts_guid_t db = depv[0].guid;
  if (depv[0].ptr == NULL) {
    printf("FAIL: " NAME " D has no pointer\n");
    arts_test_fail();
    return;
  }
  _Atomic(uint64_t) *v = view(depv[0].ptr);
  for (unsigned int j = 0; j < WA; j++) {
    atomic_store_explicit(&v[j], a_word(j), memory_order_relaxed);
  }
  unsigned int here = arts_get_current_rank();
  arts_guid_t w1 = arts_edt_create(
      w1_edt, 0, NULL, 1,
      &(arts_edt_hint_t){.rank = here, .completion_event = done});
  arts_add_dependence(db, w1, 0, DB_MODE_RW);
  if (!await_flag(&v[F_W1], "D")) {
    atomic_store_explicit(&g_d_flag, 1u, memory_order_release);
    return;
  }
  arts_db_downgrade(db);
  atomic_store_explicit(&g_d_flag, 1u, memory_order_release);
  arts_guid_t r = arts_edt_create(handoff_reader_edt, 0, NULL, 1,
                                  &(arts_edt_hint_t){.rank = here});
  arts_add_dependence(db, r, 0, DB_MODE_RO);
  check_range(v, 0, a_word, "A", "D after its downgrade");
  check_range(v, B1, b1_word, "B1", "D after its downgrade");
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
  unsigned int n = arts_get_total_ranks();
  unsigned int dr = n - 1u;
  arts_event_hint_t gh = ARTS_EVENT_HINT_LATCH(1);
  gh.auto_destroy = true;
  arts_guid_t group = arts_event_create(&gh);

  void *p = NULL;
  arts_db_hint_t h = ARTS_DB_HINT_DEFAULTS;
  h.rank = 0u;
  arts_guid_t db = arts_db_create(&p, WORDS * sizeof(uint64_t), ARTS_DB,
                                  ARTS_DB_PROP_NONE, &h);
  if (p != NULL) {
    for (unsigned int j = 0; j < WORDS; j++) {
      ((uint64_t *)p)[j] = 0;
    }
  }
  arts_db_release(db);

  uint64_t fparam = (uint64_t)db;
  arts_guid_t fin = arts_edt_create(finale_edt, 1, &fparam, 1,
                                    &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(group, fin, 0, DB_MODE_NULL);

  arts_event_hint_t ch = ARTS_EVENT_HINT_LATCH(2);
  arts_guid_t done = arts_event_create(&ch);
  arts_guid_t last = arts_edt_create(
      last_reader_edt, 0, NULL, 2,
      &(arts_edt_hint_t){.rank = n > 1u ? 0u : dr, .group = group});
  arts_add_dependence(db, last, 0, DB_MODE_RO);
  arts_add_dependence(done, last, 1, DB_MODE_NULL);

  uint64_t dparam = (uint64_t)done;
  arts_guid_t d = arts_edt_create(
      d_edt, 1, &dparam, 1,
      &(arts_edt_hint_t){.rank = dr, .group = group, .completion_event = done});
  arts_add_dependence(db, d, 0, DB_MODE_RW);
  arts_event_satisfy_slot(group, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
