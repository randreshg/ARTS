/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* route_fresh_path_taken — a create of a GUID it minted itself reserves the
 * GUID's slot by the fresh path, and every other create by the general one.
 *
 * The oracle is the slot's claim_key.  A general reservation publishes a key
 * by a claim, whose owner writes the key into claim_key first, and the slot
 * is reclaimable only after the key's object retires, so claim_key == G for
 * as long as G lives in a slot a claim published.  The fresh path never
 * writes claim_key, and G was never claimed anywhere, so a fresh slot's
 * claim_key is never G.  Each GUID the program made is checked while its
 * object lives, on the rank whose slot holds it, by an EDT the creator hands
 * it to (an EDT checks its own slot):
 *   - fresh: EDTs, events and data blocks (acquiring and NO_ACQUIRE) created
 *     for this rank with minted GUIDs, and the creator's own descriptor of a
 *     data block homed on another rank;
 *   - general: creates of reserved GUIDs, and each EDT, event and data block
 *     a create on another rank homed here.
 * Only GUIDs the program made are judged, so the runtime's own creates do
 * not enter the oracle.
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "../test_failure_status.h"
#include "arts.h"
#include "arts/gas/route_table.h"

enum { FRESH = 0, GENERAL = 1 };

static _Atomic(unsigned int) g_checked;

/* Judge `g` on this rank: its slot's claim_key against the path it took.  The
 * object must be installed here: a miss would make the lookup below reserve
 * the GUID itself, which reads as the general path whatever the create did. */
static void judge(arts_guid_t g, int path, const char *what) {
  arts_shared_ptr_t h = arts_route_table_lookup(g);
  if (h == NULL) {
    arts_printf("FAIL: route_fresh_path_taken %s %lu is not installed on rank "
                "%u\n",
                what, (unsigned long)g, arts_get_current_rank());
    arts_test_fail();
    return;
  }
  arts_route_item_t *slot = NULL;
  arts_route_table_reserve_or_lookup(g, &slot);
  arts_guid_t ck = atomic_load_explicit(&slot->claim_key, memory_order_relaxed);
  bool general = ck == g;
  if (general != (path == GENERAL)) {
    arts_printf("FAIL: route_fresh_path_taken %s %lu took the %s path on rank "
                "%u\n",
                what, (unsigned long)g, general ? "general" : "fresh",
                arts_get_current_rank());
    arts_test_fail();
  }
  arts_shared_release(&h);
  atomic_fetch_add_explicit(&g_checked, 1u, memory_order_relaxed);
}

/* paramv: path, what (0 EDT) */
static void self_check(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  judge(arts_edt_get_current_guid(), (int)paramv[0],
        paramv[1] ? "reserved EDT" : "EDT");
}

/* paramv: guid, path, kind (1 event, 2 data block), destroy, then (an event
 * satisfied once the check is done, or NULL_GUID) */
static void check_other(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t g = (arts_guid_t)paramv[0];
  judge(g, (int)paramv[1], paramv[2] == 1u ? "event" : "data block");
  if (paramv[3]) {
    if (paramv[2] == 1u) {
      arts_event_destroy(g);
    } else {
      arts_db_destroy(g);
    }
  }
  if ((arts_guid_t)paramv[4] != NULL_GUID) {
    arts_event_satisfy((arts_guid_t)paramv[4], NULL_GUID);
  }
}

static arts_guid_t edt_on(arts_edt_t fn, unsigned int rank, uint32_t paramc,
                          const uint64_t *paramv, uint32_t depc,
                          arts_guid_t guid, arts_guid_t scope) {
  arts_edt_hint_t h = ARTS_EDT_HINT_DEFAULTS;
  h.rank = rank;
  h.guid = guid;
  h.finish_event = scope;
  return arts_edt_create(fn, paramc, paramv, depc, &h);
}

static void check_later(arts_guid_t g, int path, uint64_t kind,
                        unsigned int rank, arts_guid_t scope, bool destroy) {
  uint64_t pv[5] = {(uint64_t)g, (uint64_t)path, kind, destroy ? 1u : 0u,
                    (uint64_t)NULL_GUID};
  (void)edt_on(check_other, rank, 5, pv, 0, NULL_GUID, scope);
}

static void finish(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("PASS: route_fresh_path_taken %u checks on rank 0\n",
              atomic_load_explicit(&g_checked, memory_order_relaxed));
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int here = arts_get_current_rank();
  unsigned int n = arts_get_total_ranks();
  arts_event_hint_t fh = ARTS_EVENT_HINT_FINISH;
  arts_guid_t scope = arts_event_create(&fh);
  arts_guid_t fin = edt_on(finish, here, 0, NULL, 1, NULL_GUID, NULL_GUID);
  arts_add_dependence(scope, fin, 0, DB_MODE_NULL);

  /* EDTs: minted for this rank, and created under a reserved GUID. */
  uint64_t fresh_pv[2] = {FRESH, 0u};
  uint64_t general_pv[2] = {GENERAL, 1u};
  (void)edt_on(self_check, here, 2, fresh_pv, 0, NULL_GUID, scope);
  (void)edt_on(self_check, here, 2, general_pv, 0,
               arts_guid_reserve(ARTS_GUID_EDT, here), scope);

  /* Events: minted, and labeled from a reserved range. */
  arts_event_hint_t eh = ARTS_EVENT_HINT_DEFAULTS;
  check_later(arts_event_create(&eh), FRESH, 1u, here, scope, true);
  arts_guid_t range =
      arts_guid_reserve_range(ARTS_GUID_EVENT, 4, ARTS_HINT_CURRENT_RANK);
  for (unsigned int i = 0; i < 4; i++) {
    arts_event_hint_t lh = ARTS_EVENT_HINT_DEFAULTS;
    lh.guid = arts_guid_from_index(range, i);
    check_later(arts_event_create(&lh), GENERAL, 1u, here, scope, true);
  }

  /* Data blocks: minted (acquiring and NO_ACQUIRE), and reserved. */
  void *p = NULL;
  arts_db_hint_t dh = ARTS_DB_HINT_DEFAULTS;
  dh.rank = here;
  arts_guid_t d1 = arts_db_create(&p, 64, ARTS_DB, ARTS_DB_PROP_NONE, &dh);
  arts_db_release(d1, DB_MODE_RW);
  check_later(d1, FRESH, 2u, here, scope, true);
  arts_guid_t d2 =
      arts_db_create(&p, 64, ARTS_DB, ARTS_DB_PROP_NO_ACQUIRE, &dh);
  check_later(d2, FRESH, 2u, here, scope, true);
  arts_db_hint_t rh = ARTS_DB_HINT_DEFAULTS;
  rh.guid = arts_guid_reserve(ARTS_GUID_DB, here);
  arts_guid_t d3 = arts_db_create(&p, 64, ARTS_DB, ARTS_DB_PROP_NONE, &rh);
  arts_db_release(d3, DB_MODE_RW);
  check_later(d3, GENERAL, 2u, here, scope, true);

  /* Every other rank: a minted EDT, event and data block homed there. */
  for (unsigned int r = 0; r < n; r++) {
    if (r == here) {
      continue;
    }
    uint64_t remote_pv[2] = {GENERAL, 0u};
    (void)edt_on(self_check, r, 2, remote_pv, 0, NULL_GUID, scope);

    arts_event_hint_t reh = ARTS_EVENT_HINT_DEFAULTS;
    reh.rank = r;
    arts_guid_t ev = arts_event_create(&reh);
    /* Checked at its home once it fired there, so its create has
     * installed it. */
    uint64_t epv[5] = {(uint64_t)ev, GENERAL, 1u, 1u, (uint64_t)NULL_GUID};
    arts_guid_t chk = edt_on(check_other, r, 5, epv, 1, NULL_GUID, scope);
    arts_add_dependence(ev, chk, 0, DB_MODE_NULL);
    arts_event_satisfy(ev, NULL_GUID);

    arts_db_hint_t xh = ARTS_DB_HINT_DEFAULTS;
    xh.rank = r;
    arts_guid_t dr = arts_db_create(&p, 64, ARTS_DB, ARTS_DB_PROP_NONE, &xh);
    arts_db_release(dr, DB_MODE_RW);
    /* The creator's own descriptor is checked here first; then the home's
     * checker, whose read of the block waits for the home's install, looks
     * at the home's and destroys the block, which also retires the
     * creator's descriptor. */
    arts_event_hint_t ah = ARTS_EVENT_HINT_DEFAULTS;
    ah.rank = r;
    arts_guid_t after = arts_event_create(&ah);
    uint64_t hpv[5] = {(uint64_t)dr, GENERAL, 2u, 1u, (uint64_t)NULL_GUID};
    arts_guid_t home = edt_on(check_other, r, 5, hpv, 2, NULL_GUID, scope);
    arts_add_dependence(after, home, 0, DB_MODE_NULL);
    arts_add_dependence(dr, home, 1, DB_MODE_RO);
    uint64_t cpv[5] = {(uint64_t)dr, FRESH, 2u, 0u, (uint64_t)after};
    (void)edt_on(check_other, here, 5, cpv, 0, NULL_GUID, scope);
  }
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
