/// @file cxl_creator_roster.c
/// @brief A rank whose cache came from its own acquiring create is on the
/// block's destroy roster, so that cache dies with the block.
///
/// Whitebox, because both halves are internal state: the home's roster is a
/// directory field and a cache's store is cache state, and neither has a
/// public reader.
///
/// An acquiring create made away from the block's home caches the block from
/// the moment it is made: it holds the block before any request, so the
/// request path — the only other writer of the roster — never records it.
/// Without a roster bit its cache outlives the block and goes on naming a
/// slot the block no longer names, which no value oracle can see: a stale
/// reference is not lost memory.  So the two halves are asserted directly —
/// the home names the creator's rank once the create's announce is in, and
/// the creator's cache no longer names a slot once the block is destroyed.

#include "arts.h"

#include "arts/coherence/coherence.h"
#include "arts/coherence/directory.h"
#include "arts/coherence/excl/types.h"
#include "arts/gas/route_table.h"
#include "arts/utils/shared.h"

#include "../test_failure_status.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define N 64u
#define SEED 0x2B00u
/* The destroy fan-out is a message, so the creator's cache goes away after the
 * destroy rather than with it.  Long enough that only a notify that is never
 * sent runs it out. */
#define NOTIFY_WAIT_MS 5000u

static void fail(const char *what) {
  (void)fprintf(stderr, "FAIL: %s\n", what);
  arts_test_fail();
}

/* The block's store as this rank knows it, or 0 when this rank keeps no cache
 * for the block. */
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

struct roster_probe_s {
  unsigned int want;
  bool found;
};

static void roster_cb(unsigned int rank, void *ctx) {
  struct roster_probe_s *p = (struct roster_probe_s *)ctx;
  if (rank == p->want) {
    p->found = true;
  }
}

/* depv[0] = the creator's output event, depv[1] = the block, RO — the read
 * dependence is the ordering against the create: the home serves it only once
 * the announce has installed the object here.  paramv = {the block, the
 * creator's rank}. */
static void home_check_edt(uint32_t paramc, const uint64_t *paramv,
                           uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t g = (arts_guid_t)paramv[0];
  struct roster_probe_s probe = {.want = (unsigned int)paramv[1],
                                 .found = false};

  arts_shared_ptr_t h = arts_route_table_lookup_db(g);
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(h);
  if (db == NULL || !db->home_initialized) {
    fail("the home has no directory for a block it was announced");
  } else {
    arts_rank_bitset_for_each(&db->cached_ranks, roster_cb, &probe);
    if (!probe.found) {
      fail("an acquiring creator is not on the block's destroy roster");
    }
  }
  arts_shared_release(&h);
}

/* depv[0] = the home's check. */
static void destroy_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_db_destroy((arts_guid_t)paramv[0]);
}

/* depv[0] = the destroy.  Runs back on the creating rank. */
static void creator_check_edt(uint32_t paramc, const uint64_t *paramv,
                              uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t g = (arts_guid_t)paramv[0];
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (;;) {
    if (slot_of(g) == 0) {
      arts_printf("cxl_creator_roster: the creator's cache named a store and "
                  "gave it up with the block — PASS\n");
      break;
    }
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint64_t el = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull +
                  (uint64_t)(t1.tv_nsec - t0.tv_nsec);
    if (el >= (uint64_t)NOTIFY_WAIT_MS * 1000000ull) {
      fail("a destroyed block left its creator's cache naming a store");
      break;
    }
    struct timespec nap = {.tv_sec = 0, .tv_nsec = 1000000};
    (void)nanosleep(&nap, NULL);
  }
  arts_shutdown();
}

/* paramv = {the label}. */
static void creator_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t g = (arts_guid_t)paramv[0];
  void *p = arts_db_create_with_guid(g, N * sizeof(uint32_t), ARTS_DB,
                                     ARTS_DB_PROP_NONE, NULL);
  if (p == NULL) {
    fail("an acquiring create off-home returned nothing");
    arts_shutdown();
    return;
  }
  uint32_t *w = (uint32_t *)p;
  for (uint32_t i = 0; i < N; i++) {
    w[i] = SEED + i;
  }
  if (slot_of(g) == 0) {
    fail("an acquiring create off-home left its own cache naming no store");
  }
  arts_db_release(g, DB_MODE_RW);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  unsigned int nranks = arts_get_total_ranks();
  if (nranks < 2u) {
    arts_printf("SKIP cxl_creator_roster: needs >= 2 ranks (have %u)\n",
                nranks);
    arts_shutdown();
    return;
  }

  const unsigned int creator = 1u;
  arts_guid_t g = arts_guid_reserve(ARTS_GUID_DB, 0u);
  if (g == NULL_GUID) {
    fail("cxl_creator_roster could not reserve its label");
    arts_shutdown();
    return;
  }

  arts_guid_t made = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
  uint64_t cpv[1] = {(uint64_t)g};
  (void)arts_edt_create(
      creator_edt, 1, cpv, 0,
      &(arts_edt_hint_t){.rank = creator, .output_event = made});

  arts_guid_t checked = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
  uint64_t hpv[2] = {(uint64_t)g, (uint64_t)creator};
  arts_guid_t hc = arts_edt_create(
      home_check_edt, 2, hpv, 2,
      &(arts_edt_hint_t){.rank = 0u, .output_event = checked});
  arts_add_dependence(made, hc, 0, DB_MODE_NULL);
  arts_add_dependence(g, hc, 1, DB_MODE_RO);

  /* Every acquisition of the block is over before it is destroyed: the
   * creator's hold ended at its own release and the home's read at its EDT's
   * end, both before the event this destroy waits on. */
  arts_guid_t gone = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
  arts_guid_t d = arts_edt_create(
      destroy_edt, 1, cpv, 1,
      &(arts_edt_hint_t){.rank = 0u, .output_event = gone});
  arts_add_dependence(checked, d, 0, DB_MODE_NULL);

  arts_guid_t cc = arts_edt_create(creator_check_edt, 1, cpv, 1,
                                   &(arts_edt_hint_t){.rank = creator});
  arts_add_dependence(gone, cc, 0, DB_MODE_NULL);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
