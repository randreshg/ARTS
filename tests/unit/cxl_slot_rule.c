/// @file cxl_slot_rule.c
/// @brief One block, one slot: the rank that MADE the block minted its slot in
/// the CXL store before the create left it, whether or not the create
/// acquires, and every rank that knows the block names that same address.
///
/// Whitebox, because a slot is cache state with no public reader: the checks
/// resolve the block through the route table and read the cache the runtime
/// keeps there.
///
/// Three legs: a create that acquires nothing at the block's home; the same
/// from a non-home rank, whose slot the home records; and an acquiring create
/// made off-home, whose announce the home must record unchanged.

#include "arts.h"

#include "arts/coherence/coherence.h"
#include "arts/coherence/excl/types.h"
#include "arts/cxl/store.h"
#include "arts/gas/route_table.h"
#include "arts/utils/shared.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* The verdict is the exit status: the check that decides it must have RUN,
 * not merely not failed, or a shutdown that wins the race against it would
 * pass an empty run.  The checks run on the home, rank 0, so the home's
 * status fails a run in which they never ran, however the run ended; any
 * rank's status fails a run in which a check failed. */
static _Atomic(int) g_check_ran;
static _Atomic(unsigned int) g_failures;

int arts_app_exit_status(void) {
  if (atomic_load_explicit(&g_failures, memory_order_relaxed) != 0u) {
    return 1;
  }
  if (arts_get_current_rank() == 0u &&
      !atomic_load_explicit(&g_check_ran, memory_order_acquire)) {
    return 1;
  }
  return 0;
}

#define N 64

static void fail(const char *what) {
  (void)fprintf(stderr, "FAIL: %s\n", what);
  atomic_fetch_add_explicit(&g_failures, 1u, memory_order_relaxed);
}

/* The block's slot as this rank knows it, or 0 when this rank keeps no cache
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

/* The slot exists and lies in the CXL store; `what` names a missing one. */
static void check_slot(uint64_t addr, const char *what) {
  if (addr == 0) {
    fail(what);
    return;
  }
  if (!arts_cxl_store_contains((const void *)(uintptr_t)addr)) {
    fail("a recorded slot is not in the CXL store");
  }
}

static void pass_and_shut_down(void) {
  atomic_store_explicit(&g_check_ran, 1, memory_order_release);
  (void)fprintf(stderr, "PASS cxl_slot_rule: home checks ran\n");
  arts_shutdown();
}

/* The run's checks at the home.  paramv = {the creator's slot, the acquiring
 * create's guid, the guid of the create that acquired nothing}.  The two read
 * dependences are the checks' ordering against the creates: the home serves
 * them only once each block's announce has installed the object here.  The
 * home's status fails a run in which this check never ran, so a run that
 * never reached it cannot pass. */
static void home_check_edt(uint32_t paramc, const uint64_t *paramv,
                           uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  uint64_t want_addr = paramv[0];
  uint64_t got_addr = slot_of((arts_guid_t)paramv[1]);
  if (got_addr != want_addr) {
    fail("the home recorded a different slot than the creator allocated");
  }
  /* The create that acquired nothing minted its slot on the creating rank
   * and named it on the announce. */
  check_slot(slot_of((arts_guid_t)paramv[2]),
             "a create that acquires nothing off-home left its home with no "
             "slot");
  pass_and_shut_down();
}

static void creator_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  /* An acquiring create whose home is rank 0, made on rank 1. */
  void *p = NULL;
  arts_guid_t g = arts_db_create(&p, N * sizeof(unsigned int), ARTS_DB,
                                 ARTS_DB_PROP_NONE,
                                 &(arts_db_hint_t){.rank = 0u});
  if (g == NULL_GUID || p == NULL) {
    fail("an acquiring create off-home returned nothing");
    arts_shutdown();
    return;
  }
  uint64_t addr = slot_of(g);
  check_slot(addr, "an acquiring create off-home allocated no slot");
  arts_db_release(g);

  /* A create that acquires nothing, made away from the block's home: this rank
   * keeps no cache for it, but it mints the block's slot and names it on the
   * announce.  Checked at the home. */
  void *np = NULL;
  arts_guid_t ng = arts_db_create(&np, N * sizeof(unsigned int), ARTS_DB,
                                  ARTS_DB_PROP_NO_ACQUIRE,
                                  &(arts_db_hint_t){.rank = 0u});
  if (np != NULL) {
    fail("a create that acquires nothing handed out a pointer");
  }

  uint64_t pv[3] = {addr, (uint64_t)g, (uint64_t)ng};
  arts_guid_t check = arts_edt_create(home_check_edt, 3, pv, 2,
                                      &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(g, check, 0, DB_MODE_RO);
  arts_add_dependence(ng, check, 1, DB_MODE_RO);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  /* A create that acquires nothing, at the block's home: the home is the
   * creator and records the slot it minted. */
  void *q = NULL;
  arts_guid_t b =
      arts_db_create(&q, N * sizeof(unsigned int), ARTS_DB,
                     ARTS_DB_PROP_NO_ACQUIRE, &(arts_db_hint_t){.rank = 0u});
  check_slot(slot_of(b), "a create that acquires nothing allocated no slot at "
                        "its home");
  if (arts_get_total_ranks() < 2u) {
    atomic_store_explicit(&g_check_ran, 1, memory_order_release);
    (void)fprintf(stderr, "PASS cxl_slot_rule: one-rank check ran\n");
    arts_shutdown();
    return;
  }
  (void)arts_edt_create(creator_edt, 0, NULL, 0,
                        &(arts_edt_hint_t){.rank = 1u});
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
