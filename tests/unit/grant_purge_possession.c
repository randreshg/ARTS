/* SPDX-License-Identifier: Apache-2.0
 *
 * Where a holder hands the write right back at its own idle edge, the home's
 * own possession is its whole directory.
 *
 * Such a home revokes no one, so it names no holder and is owed nothing
 * between a grant and its hand-back.  The right lives in two words, the
 * holder's and the home's, and no value oracle sees either: a holder that
 * kept the right computes the same answers, and a serve that kept the
 * transfer baton shows only as a hang.  So both words are read at two points
 * of every write turn:
 *   - while the holder is inside its turn: the home possesses nothing, its
 *     transfer baton is down and nothing is parked in its return latch;
 *   - after the turn: the holder's word carries neither possession nor a
 *     hold, so its next write turn has to ask, and the home has possession
 *     back, idle, with the baton down and the latch empty.  A release that
 *     hands the right back completes only once the home has applied the
 *     hand-back, so this holds as soon as the turn has completed.
 *
 * The home is rank 0 and the holder rank 1.  A turn keeps the right until the
 * home's probe has looked; the two meet through a file in the working
 * directory, which both ranks share because the local launcher runs them on
 * one host.
 */
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "arts.h"
#include "arts/coherence/types.h"
#include "arts/gas/route_table.h"
#include "arts/utils/atomics.h"
#include "arts/utils/shared.h"

#include "../test_failure_status.h"

#define TURNS 3
#define PROBE_WAIT_MS 30000

static void sleep_ms(long ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  (void)nanosleep(&ts, NULL);
}

static void flag_path(char *buf, size_t n, uint64_t token, uint64_t turn) {
  (void)snprintf(buf, n, "grant_purge_possession.%llu.%llu",
                 (unsigned long long)token, (unsigned long long)turn);
}

struct home_view {
  int found;
  unsigned int word;
  unsigned int baton;
  unsigned int latch;
};

/* The home's grant plane.  The thread that served or accepted puts the baton
 * down only after its send, and that send is what let the caller get here, so
 * the baton is given a bounded moment to go down. */
static struct home_view read_home(arts_guid_t db) {
  struct home_view v = {0, 0u, 0u, 0u};
  arts_shared_ptr_t h = arts_route_table_lookup_db(db);
  struct arts_db_s *d = (struct arts_db_s *)arts_shared_get(h);
  if (d != NULL && d->home_initialized) {
    v.found = 1;
    for (int i = 0; i < 1000; i++) {
      v.baton =
          atomic_load_explicit(&d->invalidate_in_flight, memory_order_acquire);
      if (v.baton == 0u) {
        break;
      }
      sleep_ms(1);
    }
    v.word = atomic_load_explicit(&d->cache.writer_count, memory_order_acquire);
    v.latch =
        atomic_load_explicit(&d->pending_return_from, memory_order_acquire);
  }
  arts_shared_release(&h);
  return v;
}

static void fail(const char *what, uint64_t turn) {
  arts_test_fail();
  arts_printf("FAIL grant_purge_possession turn %llu: %s\n",
              (unsigned long long)turn, what);
}

/* A check of the home's grant plane, printed with all three of its words. */
static void expect(int ok, const char *what, uint64_t turn, unsigned int word,
                   unsigned int baton, unsigned int latch) {
  if (ok) {
    return;
  }
  arts_test_fail();
  arts_printf("FAIL grant_purge_possession turn %llu: %s (word=0x%x baton=%u "
              "latch=%u)\n",
              (unsigned long long)turn, what, word, baton, latch);
}

/* Rank 0, while rank 1 is inside turn paramv[2]. */
static void probe_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t db = (arts_guid_t)paramv[0];
  uint64_t token = paramv[1];
  uint64_t turn = paramv[2];
  struct home_view v = read_home(db);
  if (!v.found) {
    fail("no home descriptor", turn);
  } else {
    expect(!ARTS_GRANT_OWN_OF(v.word) && ARTS_GRANT_COUNT_OF(v.word) == 0u,
           "the home possesses the right while another rank holds it", turn,
           v.word, v.baton, v.latch);
    expect(v.baton == 0u, "a transfer round is open while the holder writes",
           turn, v.word, v.baton, v.latch);
    expect(v.latch == ARTS_GRANT_NO_RETURNER,
           "a hand-back is parked before the holder has released", turn, v.word,
           v.baton, v.latch);
  }
  char path[128];
  flag_path(path, sizeof(path), token, turn);
  FILE *f = fopen(path, "w");
  if (f == NULL) {
    arts_test_fail();
    arts_printf("FAIL grant_purge_possession: cannot create %s\n", path);
    return;
  }
  (void)fclose(f);
}

/* Rank 1: one write turn, kept open until the probe has looked. */
static void turn_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  arts_guid_t probe_event = (arts_guid_t)paramv[0];
  uint64_t token = paramv[1];
  uint64_t turn = paramv[2];
  uint64_t *p = (uint64_t *)depv[0].ptr;
  arts_event_satisfy(probe_event, NULL_GUID);
  char path[128];
  flag_path(path, sizeof(path), token, turn);
  int seen = 0;
  for (int waited = 0; waited < PROBE_WAIT_MS; waited++) {
    if (access(path, F_OK) == 0) {
      seen = 1;
      break;
    }
    sleep_ms(1);
  }
  (void)unlink(path);
  if (!seen) {
    fail("the probe never ran while the turn held the right", turn);
  }
  if (p == NULL) {
    fail("the turn got no storage", turn);
    return;
  }
  *p += 1u;
}

/* Rank 1, after turn paramv[1] has completed. */
static void holder_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t db = (arts_guid_t)paramv[0];
  uint64_t turn = paramv[1];
  arts_shared_ptr_t h = arts_route_table_lookup_db(db);
  struct arts_db_s *d = (struct arts_db_s *)arts_shared_get(h);
  unsigned int word = (d != NULL) ? atomic_load_explicit(&d->cache.writer_count,
                                                         memory_order_acquire)
                                  : 0u;
  arts_shared_release(&h);
  if (d == NULL) {
    fail("the holder has no cache of the block", turn);
    return;
  }
  if (ARTS_GRANT_OWN_OF(word) || ARTS_GRANT_COUNT_OF(word) != 0u) {
    arts_test_fail();
    arts_printf("FAIL grant_purge_possession turn %llu: the holder kept the "
                "right past its idle edge (word=0x%x)\n",
                (unsigned long long)turn, word);
  }
}

/* Rank 0, after the holder's check of turn paramv[1]. */
static void home_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t db = (arts_guid_t)paramv[0];
  uint64_t turn = paramv[1];
  struct home_view v = read_home(db);
  if (!v.found) {
    fail("no home descriptor", turn);
    return;
  }
  expect(ARTS_GRANT_OWN_OF(v.word) && ARTS_GRANT_COUNT_OF(v.word) == 0u,
         "the right is not back at the home, idle, after the turn", turn,
         v.word, v.baton, v.latch);
  expect(v.baton == 0u, "a transfer round is left open after the turn", turn,
         v.word, v.baton, v.latch);
  expect(v.latch == ARTS_GRANT_NO_RETURNER,
         "a hand-back is left parked after the turn", turn, v.word, v.baton,
         v.latch);
}

static void value_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  const uint64_t *p = (const uint64_t *)depv[1].ptr;
  uint64_t got = (p != NULL) ? *p : 0u;
  if (got != (uint64_t)TURNS) {
    arts_test_fail();
    arts_printf("FAIL grant_purge_possession: value=%llu want %d\n",
                (unsigned long long)got, TURNS);
  }
  if (arts_test_status() == 0) {
    arts_printf("PASS grant_purge_possession: %d turns\n", TURNS);
  }
}

static void shutdown_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                         arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (arts_get_total_ranks() < 2u) {
    arts_test_fail();
    arts_printf("FAIL grant_purge_possession: needs a holder other than the "
                "home\n");
    arts_shutdown();
    return;
  }
  uint64_t token = (uint64_t)getpid();
  /* Every task joins one finish scope, and the shutdown waits for it, so the
   * run ends with nothing of the block's in flight. */
  arts_guid_t scope = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  void *p = NULL;
  arts_guid_t db =
      arts_db_create(&p, sizeof(uint64_t), ARTS_DB, ARTS_DB_PROP_NONE,
                     &(arts_db_hint_t){.rank = 0u});
  *(uint64_t *)p = 0u;
  arts_db_release(db, DB_MODE_RW);

  /* turn(i) -> holder check(i) -> home check(i) -> turn(i+1).  A turn's
   * acquire begins only once its gate is satisfied, so each after-turn check
   * reads a block nobody is acquiring, and each turn is granted from the home
   * the previous one handed the right back to. */
  arts_guid_t gate = NULL_GUID;
  for (uint64_t i = 0; i < TURNS; i++) {
    arts_guid_t probe_event = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
    uint64_t probe_args[3] = {(uint64_t)db, token, i};
    arts_guid_t probe =
        arts_edt_create(probe_edt, 3, probe_args, 1,
                        &(arts_edt_hint_t){.rank = 0u, .finish_event = scope});
    arts_add_dependence(probe_event, probe, 0, DB_MODE_NULL);

    arts_guid_t turned = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
    uint64_t turn_args[3] = {(uint64_t)probe_event, token, i};
    arts_guid_t t = arts_edt_create(
        turn_edt, 3, turn_args, (gate == NULL_GUID) ? 1u : 2u,
        &(arts_edt_hint_t){
            .rank = 1u, .finish_event = scope, .output_event = turned});
    arts_add_dependence(db, t, 0, DB_MODE_RW);
    if (gate != NULL_GUID) {
      arts_add_dependence(gate, t, 1, DB_MODE_NULL);
    }

    uint64_t check_args[2] = {(uint64_t)db, i};
    arts_guid_t held = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
    arts_guid_t hc = arts_edt_create(holder_edt, 2, check_args, 1,
                                     &(arts_edt_hint_t){.rank = 1u,
                                                        .finish_event = scope,
                                                        .output_event = held});
    arts_add_dependence(turned, hc, 0, DB_MODE_NULL);

    arts_guid_t homed = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
    arts_guid_t mc = arts_edt_create(home_edt, 2, check_args, 1,
                                     &(arts_edt_hint_t){.rank = 0u,
                                                        .finish_event = scope,
                                                        .output_event = homed});
    arts_add_dependence(held, mc, 0, DB_MODE_NULL);
    gate = homed;
  }
  arts_guid_t v =
      arts_edt_create(value_edt, 0, NULL, 2,
                      &(arts_edt_hint_t){.rank = 0u, .finish_event = scope});
  arts_add_dependence(gate, v, 0, DB_MODE_NULL);
  arts_add_dependence(db, v, 1, DB_MODE_RO);
  arts_guid_t end =
      arts_edt_create(shutdown_edt, 0, NULL, 1, &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(scope, end, 0, DB_MODE_NULL);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
