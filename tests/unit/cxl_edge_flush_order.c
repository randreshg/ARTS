/// @file cxl_edge_flush_order.c
/// @brief The CXL store's two flushes, at exactly their two edges.
///
/// A store that is not coherent across hosts changes only how a turn's bytes
/// move: when a rank's first acquire of a block is admitted, a consumer flush
/// of the block's slot precedes the copy (or, where a turn works in the slot
/// itself, the first access); when the rank's last holder releases, a
/// producer flush of the slot follows the copy back.  Nothing else in a turn
/// flushes, and a read turn has no producer flush.  Whitebox: the runtime is
/// linked with the adapter that records every flush it performs, and each
/// check reads its own rank's record.
///
/// One block, homed on rank A, through these turns, each ordered after the
/// last by an event:
///   1. the create's write turn on A
///   2. one write turn on B holding two same-rank acquires at once (one on a
///      rank with a single worker)
///   3. a write turn on A again
///   4. a read turn on B
/// where B is another rank when there is one.  At every point a task checks
/// that its rank's record for the block is exactly the flushes the turns so
/// far imply for that rank, in that order, each covering the whole slot.

#include "arts.h"

#include "arts/coherence/coherence.h"
#include "arts/cxl/store.h"
#include "arts/gas/route_table.h"
#include "arts/system/topology.h"
#include "arts/utils/shared.h"

#include "../test_failure_status.h"

#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef ARTS_CXL_FLUSH_RECORDER
#error "this test reads the flush record; link the recording runtime"
#endif

#define WORDS 512u
#define MAX_WRITERS 2u
#define SEED 0x5eedu
/* How long a writer waits for its siblings before calling the run failed. */
#define HANDSHAKE_SPINS 50000000u

/* The writers on B hold the block at once: a rank's write holders share its
 * turn, and each writer stays inside its body until every sibling has entered
 * its own, so the rank's hold count cannot reach zero while any of them is
 * still to be admitted.  Each writer occupies a worker while it waits, so a
 * rank with one worker runs the turn with one writer, and there the check
 * after the handshake pins nothing beyond the one before it. */
static _Atomic unsigned g_writers_in;

static unsigned writers(void) {
  return arts_get_workers_per_rank() >= MAX_WRITERS ? MAX_WRITERS : 1u;
}

/* The turns' flushes in the order they happen: (rank, kind), P = producer,
 * C = consumer.  A check at phase k expects the first k of them. */
enum { R_A, R_B };
static const struct {
  int who;
  char kind;
} g_flushes[] = {{R_A, 'P'}, {R_B, 'C'}, {R_B, 'P'},
                 {R_A, 'C'}, {R_A, 'P'}, {R_B, 'C'}};
#define AT_CREATE 0u
#define AT_B_WRITE 2u
#define AT_A_WRITE 4u
#define AT_B_READ 6u

static unsigned rank_a(void) { return 0u; }
static unsigned rank_b(void) { return arts_get_total_ranks() > 1u ? 1u : 0u; }

static void fail(const char *phase, const char *what) {
  (void)fprintf(stderr, "FAIL cxl_edge_flush_order (%s, rank %u): %s\n",
                phase, arts_get_current_rank(), what);
  arts_test_fail();
}

static uint64_t slot_of(arts_guid_t g) {
  uint64_t addr = 0;
  arts_shared_ptr_t h = arts_route_table_lookup_db(g);
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(h);
  if (db != NULL) {
    addr = arts_db_cxl_slot_addr(&db->cache);
  }
  arts_shared_release(&h);
  return addr;
}

/* This rank's record for [slot, slot + bytes) must spell the first `upto`
 * entries of g_flushes that belong to this rank. */
static bool check_record(const char *phase, uint64_t slot, size_t bytes,
                         unsigned upto) {
  char want[16] = {0};
  size_t nw = 0;
  unsigned me = arts_get_current_rank();
  for (unsigned i = 0; i < upto; i++) {
    unsigned who = g_flushes[i].who == R_A ? rank_a() : rank_b();
    if (who == me) {
      want[nw++] = g_flushes[i].kind;
    }
  }
  char got[64] = {0};
  size_t ng = 0;
  uint64_t n = arts_cxl_flush_record_count();
  if (n > ARTS_CXL_FLUSH_RECORD_CAP) {
    fail(phase, "the flush record overflowed");
    return false;
  }
  for (uint64_t i = 0; i < n; i++) {
    struct arts_cxl_flush_record_s r;
    unsigned spins = 0;
    while (!arts_cxl_flush_record_get(i, &r)) {
      if (++spins > 1000000u) {
        fail(phase, "a flush record never completed");
        return false;
      }
    }
    if (r.seq != i) {
      fail(phase, "a flush record is out of its place");
      return false;
    }
    uint64_t lo = (uint64_t)r.addr;
    uint64_t hi = lo + (uint64_t)r.bytes;
    if (hi <= slot || lo >= slot + bytes) {
      continue; /* another range: another block's slot */
    }
    if (lo != slot || r.bytes != bytes) {
      fail(phase, "a flush covered part of the block's slot, not all of it");
      return false;
    }
    if (ng + 1u >= sizeof(got)) {
      fail(phase, "too many flushes of the block");
      return false;
    }
    got[ng++] = r.producer ? 'P' : 'C';
  }
  if (strcmp(got, want) != 0) {
    char msg[160];
    (void)snprintf(msg, sizeof(msg),
                   "the block's flushes on this rank are \"%s\", the turns so "
                   "far imply \"%s\"",
                   got, want);
    fail(phase, msg);
    return false;
  }
  return true;
}

static bool check_slot(const char *phase, arts_guid_t db, uint64_t slot) {
  uint64_t here = slot_of(db);
  if (here != slot) {
    fail(phase, "this rank names a different slot for the block");
    return false;
  }
  return true;
}

/* paramv = {block, slot}; depv[0] = the read turn's end. */
static void check_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  if (check_record("after the read turn", paramv[1], WORDS * sizeof(uint64_t),
                   AT_B_READ)) {
    arts_printf("cxl_edge_flush_order: rank %u's flushes of the block are "
                "as the turns imply\n",
                arts_get_current_rank());
  }
}

/* paramv = {block, slot}; depv[0] = the block, RO; depv[1] = the second
 * write turn's end. */
static void reader_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  const char *phase = "read turn on B";
  const uint64_t *w = (const uint64_t *)depv[0].ptr;
  if (!check_slot(phase, (arts_guid_t)paramv[0], paramv[1])) {
    return;
  }
  check_record(phase, paramv[1], WORDS * sizeof(uint64_t), AT_B_READ);
  for (unsigned i = 0; i < writers(); i++) {
    if (w[1u + i] != 100u + i) {
      fail(phase, "a write from B's turn is missing");
    }
  }
  if (w[0] != SEED || w[1u + writers()] != 200u) {
    fail(phase, "a write from A's turns is missing");
  }
}

/* paramv = {block, slot}; depv[0] = the block, RW; depv[1..writers()] = the
 * workers' output events. */
static void rewrite_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  const char *phase = "second write turn on A";
  uint64_t *w = (uint64_t *)depv[0].ptr;
  check_record(phase, paramv[1], WORDS * sizeof(uint64_t), AT_A_WRITE);
  for (unsigned i = 0; i < writers(); i++) {
    if (w[1u + i] != 100u + i) {
      fail(phase, "a write from B's turn is missing");
    }
  }
  w[1u + writers()] = 200u;
}

/* paramv = {block, slot, index}; depv[0] = the block, RW; depv[1] = the
 * create turn's end. */
static void worker_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  const char *phase = "write turn on B";
  uint64_t *w = (uint64_t *)depv[0].ptr;
  if (!check_slot(phase, (arts_guid_t)paramv[0], paramv[1])) {
    return;
  }
  check_record(phase, paramv[1], WORDS * sizeof(uint64_t), AT_B_WRITE);
  if (w[0] != SEED) {
    fail(phase, "the create's write is missing");
  }
  w[1u + paramv[2]] = 100u + paramv[2];
  atomic_fetch_add_explicit(&g_writers_in, 1u, memory_order_acq_rel);
  unsigned spins = 0;
  while (atomic_load_explicit(&g_writers_in, memory_order_acquire) <
         writers()) {
    if (++spins > HANDSHAKE_SPINS) {
      fail(phase, "a sibling writer never joined the turn");
      return;
    }
    (void)sched_yield();
  }
  check_record(phase, paramv[1], WORDS * sizeof(uint64_t), AT_B_WRITE);
}

/* paramv = {the create turn's end}. */
static void creator_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  const char *phase = "create turn on A";
  uint64_t *w = NULL;
  arts_guid_t db =
      arts_db_create((void **)&w, WORDS * sizeof(uint64_t), ARTS_DB,
                     ARTS_DB_PROP_NONE, &(arts_db_hint_t){.rank = rank_a()});
  uint64_t slot = slot_of(db);
  if (db == NULL_GUID || w == NULL || slot == 0 ||
      !arts_cxl_store_contains((const void *)(uintptr_t)slot)) {
    fail(phase, "the create has no slot in the store");
    arts_shutdown();
    return;
  }
  check_record(phase, slot, WORDS * sizeof(uint64_t), AT_CREATE);
  w[0] = SEED;

  arts_guid_t made = (arts_guid_t)paramv[0];
  arts_guid_t worked[MAX_WRITERS];
  for (unsigned i = 0; i < writers(); i++) {
    worked[i] = arts_event_create(NULL);
    uint64_t pv[3] = {db, slot, i};
    arts_guid_t t = arts_edt_create(
        worker_edt, 3, pv, 2,
        &(arts_edt_hint_t){.rank = rank_b(), .output_event = worked[i]});
    arts_add_dependence(db, t, 0, DB_MODE_RW);
    arts_add_dependence(made, t, 1, DB_MODE_NULL);
  }

  uint64_t pv[2] = {db, slot};
  arts_guid_t rewritten = arts_event_create(NULL);
  arts_guid_t a2 = arts_edt_create(
      rewrite_edt, 2, pv, 1u + writers(),
      &(arts_edt_hint_t){.rank = rank_a(), .output_event = rewritten});
  arts_add_dependence(db, a2, 0, DB_MODE_RW);
  for (unsigned i = 0; i < writers(); i++) {
    arts_add_dependence(worked[i], a2, 1u + i, DB_MODE_NULL);
  }

  arts_guid_t read = arts_event_create(NULL);
  arts_guid_t r = arts_edt_create(
      reader_edt, 2, pv, 2,
      &(arts_edt_hint_t){.rank = rank_b(), .output_event = read});
  arts_add_dependence(db, r, 0, DB_MODE_RO);
  arts_add_dependence(rewritten, r, 1, DB_MODE_NULL);

  /* After the read turn's end, on both ranks: a read turn's edge flushes
   * nothing, and nothing flushes the block once no turn is open. */
  const unsigned where[2] = {rank_a(), rank_b()};
  for (unsigned i = 0; i < (rank_a() == rank_b() ? 1u : 2u); i++) {
    arts_guid_t c = arts_edt_create(check_edt, 2, pv, 1,
                                    &(arts_edt_hint_t){.rank = where[i]});
    arts_add_dependence(read, c, 0, DB_MODE_NULL);
  }
}

static void done_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("PASS cxl_edge_flush_order: %u ranks\n", arts_get_total_ranks());
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_guid_t done = arts_edt_create(done_edt, 0, NULL, 1, NULL);
  arts_guid_t scope = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_add_dependence(scope, done, 0, DB_MODE_NULL);
  arts_guid_t made = arts_event_create(NULL);
  uint64_t pv[1] = {made};
  arts_edt_create(creator_edt, 1, pv, 0,
                  &(arts_edt_hint_t){.rank = rank_a(),
                                     .finish_event = scope,
                                     .output_event = made});
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
