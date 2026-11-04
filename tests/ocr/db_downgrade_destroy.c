/* SPDX-License-Identifier: Apache-2.0
 *
 * A hold arts_db_downgrade leaves is a hold, and every destroy is ordered
 * after its release.
 *
 *   (a) D downgrades a block it created and an RW dependence and wakes two
 *       readers of both, R1 on another rank and R2 on D's rank; a destroyer
 *       gated on the three completions destroys both blocks.
 *   (b) D2 downgrades a created block and an RW dependence, reads them, and
 *       destroys them itself: the destroy releases the read hold first.
 *   (c) D3 creates a block homed on another rank, downgrades it, reads it and
 *       destroys it itself (where the create's turn was never granted, the
 *       wait for the home's answer runs inside that destroy).
 *
 * (a) and (b) on one rank, (a)-(c) on two or more.
 */
#include "arts.h"

#include "../test_failure_status.h"

#include <stdint.h>
#include <stdio.h>

#define NAME "db_downgrade_destroy"
#define WORDS 128u
#define SZ (WORDS * sizeof(uint64_t))

static void fill(uint64_t *p, uint64_t base) {
  for (unsigned int j = 0; j < WORDS; j++) {
    p[j] = base + j;
  }
}

static void check(const uint64_t *p, uint64_t base, const char *who) {
  for (unsigned int j = 0; j < WORDS; j++) {
    if (p == NULL || p[j] != base + j) {
      printf("FAIL: " NAME " %s: word %u is 0x%llx, expected 0x%llx\n", who, j,
             p ? (unsigned long long)p[j] : 0ull,
             (unsigned long long)(base + j));
      arts_test_fail();
      return;
    }
  }
}

static arts_guid_t create_filled(unsigned int home, uint64_t base,
                                 uint64_t **out) {
  arts_db_hint_t h = ARTS_DB_HINT_DEFAULTS;
  h.rank = home;
  void *p = NULL;
  arts_guid_t g = arts_db_create(&p, SZ, ARTS_DB, ARTS_DB_PROP_NONE, &h);
  if (p == NULL) {
    printf("FAIL: " NAME " an acquiring create was handed no pointer\n");
    arts_test_fail();
  } else {
    fill((uint64_t *)p, base);
  }
  *out = (uint64_t *)p;
  return g;
}

/* depv[0]: the created block; depv[1]: the dependence. */
static void reader_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  check((const uint64_t *)depv[0].ptr, 0xA000u, "a reader");
  check((const uint64_t *)depv[1].ptr, 0xA100u, "a reader");
}

/* (a).  paramv: destroyer, then the events of R1's and R2's slots (created,
 * dependence, created, dependence).  depv[0]: the RW dependence. */
static void d_a_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                    arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint64_t *c = NULL;
  arts_guid_t cre = create_filled(arts_get_current_rank(), 0xA000u, &c);
  uint64_t *d = (uint64_t *)depv[0].ptr;
  if (d == NULL) {
    printf("FAIL: " NAME " (a) the dependence has no pointer\n");
    arts_test_fail();
    return;
  }
  fill(d, 0xA100u);
  arts_db_downgrade(cre);
  arts_db_downgrade(depv[0].guid);
  arts_edt_satisfy_slot((arts_guid_t)paramv[0], 1, cre, DB_MODE_NULL);
  for (unsigned int r = 0; r < 2u; r++) {
    arts_event_satisfy((arts_guid_t)paramv[1 + 2 * r], cre);
    arts_event_satisfy((arts_guid_t)paramv[2 + 2 * r], depv[0].guid);
  }
  if (c != NULL) {
    check(c, 0xA000u, "(a) D");
  }
  check(d, 0xA100u, "(a) D");
}

/* (a)'s destroyer.  paramv[0]: the dependence; depv[0]: the three
 * completions; depv[1]: the created block. */
static void destroyer_edt(uint32_t paramc, const uint64_t *paramv,
                          uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  arts_db_destroy((arts_guid_t)paramv[0]);
  arts_db_destroy(depv[1].guid);
}

/* (b).  depv[0]: the RW dependence. */
static void d_b_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                    arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  uint64_t *c = NULL;
  arts_guid_t cre = create_filled(arts_get_current_rank(), 0xB000u, &c);
  uint64_t *d = (uint64_t *)depv[0].ptr;
  arts_guid_t dg = depv[0].guid;
  if (d == NULL) {
    printf("FAIL: " NAME " (b) the dependence has no pointer\n");
    arts_test_fail();
    return;
  }
  fill(d, 0xB100u);
  arts_db_downgrade(cre);
  arts_db_downgrade(dg);
  if (c != NULL) {
    check(c, 0xB000u, "(b) D2");
  }
  check(d, 0xB100u, "(b) D2");
  arts_db_destroy(cre);
  arts_db_destroy(dg);
  if (depv[0].mode != DB_MODE_NULL || depv[0].guid != NULL_GUID) {
    printf("FAIL: " NAME " (b) the destroy left the slot held\n");
    arts_test_fail();
  }
}

/* (c).  paramv[0]: a rank other than this one. */
static void d_c_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                    arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  uint64_t *c = NULL;
  arts_guid_t cre = create_filled((unsigned int)paramv[0], 0xC000u, &c);
  arts_db_downgrade(cre);
  if (c != NULL) {
    check(c, 0xC000u, "(c) D3");
  }
  arts_db_destroy(cre);
}

static void finale_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  printf("PASS: " NAME " shapes %s\n", paramv[0] ? "a-c" : "a-b");
  arts_shutdown();
}

static arts_guid_t released_block(unsigned int home) {
  arts_db_hint_t h = ARTS_DB_HINT_DEFAULTS;
  h.rank = home;
  void *p = NULL;
  arts_guid_t g = arts_db_create(&p, SZ, ARTS_DB, ARTS_DB_PROP_NONE, &h);
  if (p != NULL) {
    fill((uint64_t *)p, 0);
  }
  arts_db_release(g);
  return g;
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int n = arts_get_total_ranks();
  unsigned int dr = n - 1u;
  unsigned int other = (n > 1u) ? 0u : dr;

  arts_event_hint_t gh = ARTS_EVENT_HINT_LATCH(1);
  gh.auto_destroy = true;
  arts_guid_t group = arts_event_create(&gh);
  uint64_t three = n > 1u ? 1u : 0u;
  arts_guid_t fin =
      arts_edt_create(finale_edt, 1, &three, 1, &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(group, fin, 0, DB_MODE_NULL);

  /* (a) */
  arts_guid_t a_dep = released_block(0u);
  arts_event_hint_t ch = ARTS_EVENT_HINT_LATCH(3);
  arts_guid_t a_done = arts_event_create(&ch);
  uint64_t a_dparam = (uint64_t)a_dep;
  arts_guid_t destroyer =
      arts_edt_create(destroyer_edt, 1, &a_dparam, 2,
                      &(arts_edt_hint_t){.rank = 0u, .group = group});
  arts_add_dependence(a_done, destroyer, 0, DB_MODE_NULL);
  uint64_t a_param[5];
  a_param[0] = (uint64_t)destroyer;
  arts_event_hint_t eh = ARTS_EVENT_HINT_LATCH(1);
  for (unsigned int k = 0; k < 2u; k++) {
    arts_guid_t r =
        arts_edt_create(reader_edt, 0, NULL, 2,
                        &(arts_edt_hint_t){.rank = k == 0u ? other : dr,
                                           .group = group,
                                           .completion_event = a_done});
    for (unsigned int slot = 0; slot < 2u; slot++) {
      arts_guid_t e = arts_event_create(&eh);
      arts_add_dependence(e, r, slot, DB_MODE_RO);
      a_param[1 + 2 * k + slot] = (uint64_t)e;
    }
  }
  arts_guid_t da = arts_edt_create(
      d_a_edt, 5, a_param, 1,
      &(arts_edt_hint_t){
          .rank = dr, .group = group, .completion_event = a_done});
  arts_add_dependence(a_dep, da, 0, DB_MODE_RW);

  /* (b) */
  arts_guid_t b_dep = released_block(0u);
  arts_guid_t db2 = arts_edt_create(
      d_b_edt, 0, NULL, 1, &(arts_edt_hint_t){.rank = dr, .group = group});
  arts_add_dependence(b_dep, db2, 0, DB_MODE_RW);

  /* (c) */
  if (n > 1u) {
    uint64_t home = 0u;
    arts_edt_create(d_c_edt, 1, &home, 0,
                    &(arts_edt_hint_t){.rank = dr, .group = group});
  }
  arts_event_satisfy_slot(group, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
