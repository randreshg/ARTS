/// @file fam_slot_rule.c
/// @brief One block, one slot: the rank that MADE the block allocated it, and
/// every rank that knows the block names that same address.
///
/// Whitebox, because a slot is cache state with no public reader: the checks
/// resolve the block through the route table and read the cache the runtime
/// keeps there.  The owner of an address is a pure function of the address, so
/// there is no second word to compare it against.
///
/// Six legs: a create that acquires nothing at the block's home; the same from
/// a non-home rank, where the home is the one that must allocate; two ranks
/// creating ONE label with no acquisition, where one create installs and the
/// other waits for the block's destroy and then installs with a store of its
/// own; an acquiring create made off-home, whose announce the home must record
/// unchanged; a create of a label a dependence on this rank has already
/// touched; and a create of a label whose block is live, which waits at the
/// home and has minted nothing until it installs.

#include "arts.h"

#include "arts/coherence/coherence.h"
#include "arts/coherence/excl/types.h"
#include "arts/fam/pool.h"
#include "arts/gas/route_table.h"
#include "arts/utils/shared.h"

#include "../test_failure_status.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* The verdict is the exit status: the check that decides it must have RUN,
 * not merely not failed, or a shutdown that wins the race against it would
 * pass an empty run. */
static _Atomic int g_check_ran;

#define N 64
/* Labels two ranks create at once.  One is enough to state the rule and far
 * too few to meet it: the two creates have to overlap inside the home's own
 * install window, so the leg is a population, not a single try. */
#define RACE_LABELS 128u

static void fail(const char *what) {
  (void)fprintf(stderr, "FAIL: %s\n", what);
  arts_test_fail();
}

/* The block's slot as this rank knows it, or 0 when this rank keeps no cache
 * for the block. */
static uint64_t slot_of(arts_guid_t g) {
  uint64_t addr = 0;
  arts_shared_ptr_t h = arts_route_table_lookup_db(g);
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(h);
  if (db != NULL) {
    addr = arts_db_fam_slot_addr(&db->cache);
  }
  arts_shared_release(&h);
  return addr;
}

static void check_owned_here(uint64_t addr, const char *what) {
  if (addr == 0) {
    fail(what);
    return;
  }
  if (!arts_fam_contains((const void *)(uintptr_t)addr)) {
    fail("a recorded slot is not pool memory");
    return;
  }
  if (arts_fam_owner_of((const void *)(uintptr_t)addr) !=
      arts_get_current_rank()) {
    fail("the allocating rank does not own its slot's slice");
  }
}

/* The reader whose dependence makes the first touch; it only has to run. */
static void mark_reader_edt(uint32_t paramc, const uint64_t *paramv,
                            uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
}

static void creator_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]);

/* Both ranks create every label of the range, with no acquisition and no
 * ordering between them.  One create of each label installs; the other waits,
 * parked, until that block is destroyed, and installs then.
 *
 * The peer's sweep is started from the off-home rank, one message ahead of
 * its own: the home's create of a label and the peer's announce of it then
 * begin the range in step and stay within microseconds of each other across
 * it. */
static void race_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t base = (arts_guid_t)paramv[0];
  unsigned int n = (unsigned int)paramv[1];
  if (paramv[2] != 0u) {
    uint64_t pv[3] = {(uint64_t)base, (uint64_t)n, 0u};
    (void)arts_edt_create(race_edt, 3, pv, 0,
                          &(arts_edt_hint_t){.rank = 0u});
  }
  for (unsigned int i = 0; i < n; i++) {
    void *p = arts_db_create_with_guid(arts_guid_from_index(base, i),
                                       N * sizeof(unsigned int), ARTS_DB,
                                       ARTS_DB_PROP_NO_ACQUIRE, NULL);
    if (p != NULL) {
      fail("a create that acquires nothing handed out a pointer");
      return;
    }
  }
}

/* Runs at the range's home once both racers are done.  Every label has a
 * block here by then: this rank's own create of it installed one or is
 * waiting behind the other's.  Each label is destroyed, the waiting create
 * installs with a store of its own, and that block is destroyed too. */
static void race_check_edt(uint32_t paramc, const uint64_t *paramv,
                           uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t base = (arts_guid_t)paramv[0];
  unsigned int n = (unsigned int)paramv[1];
  for (unsigned int i = 0; i < n; i++) {
    arts_guid_t g = arts_guid_from_index(base, i);
    check_owned_here(slot_of(g), "a label two ranks created without "
                                 "acquiring it kept no slot at its home");
    arts_db_destroy(g);
    check_owned_here(slot_of(g),
                     "the create that waited for a label's destroy installed "
                     "no block with a slot at its home");
    arts_db_destroy(g);
  }
  (void)arts_edt_create(creator_edt, 0, NULL, 0,
                        &(arts_edt_hint_t){.rank = 1u});
}

/* The run's last act on every rank count.  paramv = {the creator's slot, the
 * acquiring create's guid, the creator's rank, the guid of the create that
 * acquired nothing, the first-touched label, its slot}.  The two read
 * dependences are the checks' ordering against the creates: the home serves
 * them only once each block's announce has installed the object here.  The
 * third slot is the waiting-create leg's completion where that leg runs, so
 * no leg is still outstanding when this one shuts the run down and none of
 * them can be skipped by losing a race to the shutdown.  Its PASS line is
 * what the registration requires, so a run that never reached these checks
 * cannot pass. */
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
  if (got_addr != 0 && arts_fam_owner_of((const void *)(uintptr_t)got_addr) !=
                           (unsigned int)paramv[2]) {
    fail("the home derives the wrong owner for the creator's slot");
  }
  /* The create that acquired nothing named no store, so the home is the rank
   * that had to allocate one. */
  check_owned_here(slot_of((arts_guid_t)paramv[3]),
                   "a create that acquires nothing off-home left its home "
                   "with no slot");
  if (arts_get_total_ranks() >= 3u) {
    /* The waiting create of the first-touched label minted nothing: the live
     * block still names the slot its creator allocated.  Once that block is
     * destroyed the waiting create installs, and the home, which it named no
     * store to, allocates one. */
    arts_guid_t lg = (arts_guid_t)paramv[4];
    if (slot_of(lg) != paramv[5]) {
      fail("a create waiting behind a live block changed that block's slot");
    }
    arts_db_destroy(lg);
    check_owned_here(slot_of(lg),
                     "a create that waited for its label's destroy installed "
                     "no block with a slot at its home");
    arts_db_destroy(lg);
  }
  __atomic_store_n(&g_check_ran, 1, __ATOMIC_RELEASE);
  (void)fprintf(stderr, "PASS fam_slot_rule: home checks ran\n");
  arts_shutdown();
}

/* A create of a label whose block is live waits for that block's destroy.
 * Made from a rank that already knows the block's store (slot 0 is the block,
 * acquired read only and released before the create runs), acquiring
 * nothing, so the announce is the whole of the create and it waits at the
 * home. */
static void recreate_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                         arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t dg = (arts_guid_t)paramv[0];
  arts_db_release(dg, DB_MODE_RO);
  uint64_t before = slot_of(dg);
  void *dp = arts_db_create_with_guid(dg, N * sizeof(unsigned int), ARTS_DB,
                                      ARTS_DB_PROP_NO_ACQUIRE, NULL);
  if (dp != NULL) {
    fail("a create that acquires nothing handed out a pointer");
  }
  if (slot_of(dg) != before) {
    fail("a create of a live label changed the block's slot here");
  }
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
  check_owned_here(addr, "an acquiring create off-home allocated no slot");
  arts_db_release(g, DB_MODE_RW);

  /* First touch, then a create of the same label, on ONE rank.  The dependence
   * is asked for FIRST, so a first touch may install the cache here and park;
   * the create then either wins its own install or finds that cache and
   * claims it.  Which one is a race this test does not control, and need
   * not: on both paths this create MAKES the block -- the label's home has no
   * object for the GUID until this create's announce installs one, so no grant
   * can precede it -- and the invariant asserted is the one an admitted
   * waiter's pointer depends on: a non-NULL pointer and a slot out of this
   * rank's own slice.  The cache a first touch leaves behind declares no size,
   * so a create that failed to declare one before allocating would be handed
   * nothing. */
  arts_guid_t lg = arts_guid_reserve(ARTS_GUID_DB, 0u);
  arts_guid_t consumer =
      arts_edt_create(mark_reader_edt, 0, NULL, 1,
                      &(arts_edt_hint_t){.rank = arts_get_current_rank()});
  arts_add_dependence(lg, consumer, 0, DB_MODE_RO);
  void *lp = arts_db_create_with_guid(lg, N * sizeof(unsigned int), ARTS_DB,
                                      ARTS_DB_PROP_NONE, NULL);
  uint64_t laddr = slot_of(lg);
  if (lp == NULL) {
    fail("a create after a first touch of the same label made nothing");
  }
  check_owned_here(laddr, "a create after a first touch allocated no slot");
  arts_db_release(lg, DB_MODE_RW);

  /* A create that acquires nothing, made away from the block's home: this rank
   * keeps no cache for it and names no store, so the home is the only rank
   * left that can allocate one.  Checked at the home. */
  arts_guid_t ng = arts_guid_reserve(ARTS_GUID_DB, 0u);
  void *np = arts_db_create_with_guid(ng, N * sizeof(unsigned int), ARTS_DB,
                                      ARTS_DB_PROP_NO_ACQUIRE, NULL);
  if (np != NULL) {
    fail("a create that acquires nothing handed out a pointer");
  }

  uint64_t pv[6] = {addr,        (uint64_t)g, (uint64_t)arts_get_current_rank(),
                    (uint64_t)ng, (uint64_t)lg, laddr};
  arts_guid_t check = arts_edt_create(home_check_edt, 6, pv, 3,
                                      &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(g, check, 0, DB_MODE_RO);
  arts_add_dependence(ng, check, 1, DB_MODE_RO);

  /* The waiting-create leg needs a third rank: a home, this creator, and a
   * rank the block is granted to.  Its output event fires only after it has
   * run, and that is what the home's check waits on; with fewer ranks there
   * is no such leg and nothing to wait for. */
  if (arts_get_total_ranks() >= 3u) {
    arts_guid_t done = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
    uint64_t dpv[1] = {(uint64_t)lg};
    arts_guid_t again = arts_edt_create(
        recreate_edt, 1, dpv, 1,
        &(arts_edt_hint_t){.rank = 2u, .output_event = done});
    arts_add_dependence(done, check, 2, DB_MODE_NULL);
    arts_add_dependence(lg, again, 0, DB_MODE_RO);
  } else {
    arts_edt_satisfy_slot(check, 2, NULL_GUID, DB_MODE_NULL);
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  /* A create that acquires nothing, at the block's home: the home allocates,
   * out of its own slice. */
  void *q = NULL;
  arts_guid_t b =
      arts_db_create(&q, N * sizeof(unsigned int), ARTS_DB,
                     ARTS_DB_PROP_NO_ACQUIRE, &(arts_db_hint_t){.rank = 0u});
  check_owned_here(slot_of(b),
                   "a create that acquires nothing allocated no slot at its "
                   "home");
  if (arts_get_total_ranks() < 2u) {
    __atomic_store_n(&g_check_ran, 1, __ATOMIC_RELEASE);
    (void)fprintf(stderr, "PASS fam_slot_rule: one-rank check ran\n");
    arts_shutdown();
    return;
  }
  /* The two-rank leg runs first and the rest of the run hangs off its check. */
  arts_guid_t base = arts_guid_reserve_range(ARTS_GUID_DB, RACE_LABELS, 0u);
  uint64_t rv[3] = {(uint64_t)base, (uint64_t)RACE_LABELS, 1u};
  arts_guid_t fe = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t chk = arts_edt_create(race_check_edt, 2, rv, 1,
                                    &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(fe, chk, 0, DB_MODE_NULL);
  (void)arts_edt_create(race_edt, 3, rv, 0,
                        &(arts_edt_hint_t){.rank = 1u, .finish_event = fe});
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  if (rc) {
    return 1;
  }
  /* The checks run on the home, rank 0, whose status is the one the launcher
   * reports; every other rank contributes only through arts_test_status. */
  if (arts_get_current_rank() == 0u &&
      !__atomic_load_n(&g_check_ran, __ATOMIC_ACQUIRE)) {
    (void)fprintf(stderr, "FAIL fam_slot_rule: the deciding check never ran\n");
    return 1;
  }
  return arts_test_status();
}
