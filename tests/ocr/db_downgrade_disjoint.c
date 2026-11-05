/* SPDX-License-Identifier: Apache-2.0
 *
 * While a downgraded read hold lasts, a location no EDT concurrent with the
 * downgrader writes keeps the downgrader's view, even while another EDT,
 * ordered after the hand-off, writes other locations of the block.
 *
 * D creates the block on its home (rank 0), writes range A and range B,
 * downgrades, hands the block on, and then re-reads A in a loop and checks
 * every value, while writers ordered after the hand-off write B.  Both ranges
 * are viewed as _Atomic(uint64_t) once the block is published, since B's
 * words race with D's reads of B's last word (the writers' marker) and the
 * runtime's own copies may land a whole image, A included, under D's reads;
 * the oracle reads only A.
 *
 * Arguments: the placement, then whether D waits for the writers.
 *   local   one writer on D's rank writes B in place, again and again;
 *   remote  a chain of writers on rank 1, each publishing at its release;
 *   demote  a reader on rank 1 is served first (under exclusion with a
 *           retained right this demotes D's rank's write permission), then a
 *           writer on D's rank, ordered after that reader, writes B;
 *   wait    D reads until it sees the writers' last marker in B, which shows
 *           the writes reached the storage D reads while it read (a deadline
 *           fails the test), or
 *   nowait  D reads for a bounded time only: where the writers are admitted
 *           only after D's read hold, or their bytes never reach D's
 *           storage, waiting for them would never end.
 */
#include "arts.h"

#include "../test_failure_status.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define NAME "db_downgrade_disjoint"
#define WA 256u
#define WB 256u
#define WORDS (WA + WB)
#define MARK (WORDS - 1u)
#define CHAIN 8u
#define LOCAL_ROUNDS 64u
#define WAIT_MS 60000L
#define NOWAIT_MS 200L

enum { P_LOCAL, P_REMOTE, P_DEMOTE };

static long now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long)t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static uint64_t a_word(unsigned int j) { return 0xA0000000ull + j; }

static _Atomic(uint64_t) *view(void *p) { return (_Atomic(uint64_t) *)p; }

static int check_a(_Atomic(uint64_t) *v, const char *who) {
  for (unsigned int j = 0; j < WA; j++) {
    uint64_t x = atomic_load_explicit(&v[j], memory_order_relaxed);
    if (x != a_word(j)) {
      printf("FAIL: " NAME " %s: A word %u is 0x%llx, wrote 0x%llx\n", who, j,
             (unsigned long long)x, (unsigned long long)a_word(j));
      arts_test_fail();
      return 0;
    }
  }
  return 1;
}

static void write_b(_Atomic(uint64_t) *v, uint64_t m) {
  for (unsigned int j = WA; j < MARK; j++) {
    atomic_store_explicit(&v[j], m, memory_order_relaxed);
  }
  atomic_store_explicit(&v[MARK], m, memory_order_relaxed);
}

/* depv[0]: the block, RW.  paramv[0]: the marker this writer leaves (the
 * remote chain), or 0 for the local writer, which writes LOCAL_ROUNDS
 * markers. */
static void writer_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  if (depv[0].ptr == NULL) {
    printf("FAIL: " NAME " a writer has no pointer\n");
    arts_test_fail();
    return;
  }
  _Atomic(uint64_t) *v = view(depv[0].ptr);
  if (paramv[0] != 0u) {
    write_b(v, paramv[0]);
    return;
  }
  for (uint64_t m = 1; m <= LOCAL_ROUNDS; m++) {
    write_b(v, m);
  }
}

/* depv[0]: the block, RO. */
static void reader_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  if (depv[0].ptr == NULL) {
    printf("FAIL: " NAME " the reader has no pointer\n");
    arts_test_fail();
    return;
  }
  (void)check_a(view(depv[0].ptr), "the reader");
}

/* paramv: placement, wait, finale. */
static void downgrader_edt(uint32_t paramc, const uint64_t *paramv,
                           uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  unsigned int placement = (unsigned int)paramv[0];
  int wait = paramv[1] != 0u;
  arts_guid_t finale = (arts_guid_t)paramv[2];

  void *p = NULL;
  arts_db_hint_t h = ARTS_DB_HINT_DEFAULTS;
  h.rank = 0u;
  arts_guid_t db = arts_db_create(&p, WORDS * sizeof(uint64_t), ARTS_DB,
                                  ARTS_DB_PROP_NONE, &h);
  if (p == NULL) {
    printf("FAIL: " NAME " an acquiring create was handed no pointer\n");
    arts_test_fail();
    return;
  }
  _Atomic(uint64_t) *v = view(p);
  for (unsigned int j = 0; j < WA; j++) {
    atomic_init(&v[j], a_word(j));
  }
  for (unsigned int j = WA; j < WORDS; j++) {
    atomic_init(&v[j], 0u);
  }
  arts_db_downgrade(db);
  arts_edt_satisfy_slot(finale, 1, db, DB_MODE_NULL);

  uint64_t last = 0;
  arts_event_hint_t eh = ARTS_EVENT_HINT_LATCH(1);
  if (placement == P_LOCAL) {
    uint64_t zero = 0;
    arts_guid_t w = arts_edt_create(writer_edt, 1, &zero, 1,
                                    &(arts_edt_hint_t){.rank = 0u});
    arts_add_dependence(db, w, 0, DB_MODE_RW);
    last = LOCAL_ROUNDS;
  } else if (placement == P_REMOTE) {
    arts_guid_t prev = NULL_GUID;
    for (uint64_t i = 1; i <= CHAIN; i++) {
      arts_guid_t done = arts_event_create(&eh);
      arts_guid_t w = arts_edt_create(
          writer_edt, 1, &i, 2,
          &(arts_edt_hint_t){.rank = 1u, .completion_event = done});
      arts_add_dependence(db, w, 0, DB_MODE_RW);
      arts_add_dependence(prev, w, 1, DB_MODE_NULL);
      prev = done;
    }
    last = CHAIN;
  } else {
    arts_guid_t served = arts_event_create(&eh);
    arts_guid_t r = arts_edt_create(
        reader_edt, 0, NULL, 1,
        &(arts_edt_hint_t){.rank = 1u, .completion_event = served});
    arts_add_dependence(db, r, 0, DB_MODE_RO);
    uint64_t zero = 0;
    arts_guid_t w = arts_edt_create(writer_edt, 1, &zero, 2,
                                    &(arts_edt_hint_t){.rank = 0u});
    arts_add_dependence(db, w, 0, DB_MODE_RW);
    arts_add_dependence(served, w, 1, DB_MODE_NULL);
    last = LOCAL_ROUNDS;
  }

  long start = now_ms();
  unsigned long passes = 0;
  for (;;) {
    if (!check_a(v, "D under its read hold")) {
      return;
    }
    passes++;
    long el = now_ms() - start;
    if (wait) {
      if (atomic_load_explicit(&v[MARK], memory_order_relaxed) == last) {
        break;
      }
      if (el > WAIT_MS) {
        printf("FAIL: " NAME " D never saw the writers' last marker in %ld ms "
               "(B's last word 0x%llx)\n",
               el,
               (unsigned long long)atomic_load_explicit(&v[MARK],
                                                        memory_order_relaxed));
        arts_test_fail();
        return;
      }
    } else if (el > NOWAIT_MS) {
      break;
    }
  }
  if (!check_a(v, "D after the writers")) {
    return;
  }
  printf("D read A %lu times under its read hold\n", passes);
}

/* depv[0]: the group; depv[1]: the block. */
static void finale_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  arts_db_destroy(depv[1].guid);
  printf("PASS: " NAME "\n");
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)depc;
  (void)depv;
  int argc = paramc > 0u ? (int)paramv[0] : 0;
  char **argv = paramc > 1u ? (char **)paramv[1] : NULL;
  unsigned int placement = P_LOCAL;
  uint64_t wait = 0u;
  if (argc > 1 && strcmp(argv[1], "remote") == 0) {
    placement = P_REMOTE;
  } else if (argc > 1 && strcmp(argv[1], "demote") == 0) {
    placement = P_DEMOTE;
  } else if (!(argc > 1 && strcmp(argv[1], "local") == 0)) {
    printf("FAIL: " NAME " give the placement: local, remote or demote\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  if (argc > 2 && strcmp(argv[2], "wait") == 0) {
    wait = 1u;
  } else if (!(argc > 2 && strcmp(argv[2], "nowait") == 0)) {
    printf("FAIL: " NAME " give wait or nowait\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  if (placement != P_LOCAL && arts_get_total_ranks() < 2u) {
    printf("FAIL: " NAME " this placement needs two ranks\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }

  arts_event_hint_t gh = ARTS_EVENT_HINT_LATCH(1);
  gh.auto_destroy = true;
  arts_guid_t group = arts_event_create(&gh);
  arts_guid_t fin =
      arts_edt_create(finale_edt, 0, NULL, 2, &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(group, fin, 0, DB_MODE_NULL);
  uint64_t dparam[3] = {placement, wait, (uint64_t)fin};
  arts_edt_create(downgrader_edt, 3, dparam, 0,
                  &(arts_edt_hint_t){.rank = 0u, .group = group});
  arts_event_satisfy_slot(group, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
}

int main(int argc, char **argv) {
  return arts_rt(argc, argv);
}
