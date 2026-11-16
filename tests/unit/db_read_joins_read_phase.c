/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* db_read_joins_read_phase — a read acquisition on a rank whose write
 * request for the same block waits behind another rank's readers joins the
 * read phase those readers keep open.
 *
 * Blocks B and E are two names of one range reserved on rank H (3), B the
 * lower.  On rank H, S creates B, fills it and releases it, then starts R0
 * {B: RO} on rank D (1).  R0 runs under rank D's read grant, and while it
 * holds B it makes RD {B: RO, E: RO} ready: RD's read of B is served on the
 * spot from the grant R0 holds, and RD's walk then waits, holding B, for E,
 * which does not exist yet.  So the home's read phase of B is open, and
 * stays open, before anything below starts.  Only then does R0 start T on
 * rank C (2).  T makes WC {B: RW} ready, so rank C's write request for B
 * leaves and waits for RD's release, and after it RC {B: RO}, which checks a
 * range of S's fill.  RC's completion readies X on rank H, which creates E,
 * fills it and releases it; that lets RD run, RD's release ends the read
 * phase, and WC writes a range of B.  A latch counts RD, WC and X, whose
 * completion follows RC's; it readies the final EDT {B: RO, E: RO}, which
 * checks every byte of both blocks, destroys them and ends the run.
 *
 * The program is legal.  E's create waits for RC, and RC holds nothing RD
 * waits for; RC reads B, which RD holds for reading too, and a read request
 * that arrives while a read phase is open is granted into it, whether or not
 * a writer waits.  WC waits for RD and RC, and nothing waits for WC but the
 * latch.
 *
 * Arrival orders at the home.  R0's grant and RD's hold precede T by
 * construction, so the read phase is open when rank C's requests leave.
 * Rank C's write request and read request reach the home in either order
 * (and each may first take a size round): a read request that arrives first
 * is counted into the open phase and the write request then waits behind
 * one more reader; one that arrives second extends the phase the write
 * request waits behind.  RD's read request for E waits at the home for E's
 * create in either case.  No order lets WC run before RD and RC released, so
 * RC and RD read S's fill alone.
 *
 * Needs four ranks: the home, the readers' rank and the writer's rank are
 * distinct, so no acquire of rank C or D is the home's own.  Under a
 * retention that leaves the block with its last writer, S on rank H keeps
 * it, so B's owner is rank H too, neither the readers' rank nor the
 * writer's.  The verdict:
 * exit status, PASS line, no FAIL line and no QUIESCENCE-DEBUG marker; a
 * runtime that leaves RC behind rank C's write request never ends the run.
 */

#include <stdint.h>

#include "arts.h"
#include "../test_failure_status.h"

#define B_WORDS 32u
#define E_WORDS 8u
#define SPAN 8u
#define RC_FIRST 0u
#define RD_FIRST 8u
#define WC_FIRST 16u

#define S_MARK 0x5100u
#define WC_MARK 0x2100u
#define X_MARK 0xE100u

#define RANK_D 1u
#define RANK_C 2u
#define RANK_H 3u

static arts_guid_t latch_on(unsigned int rank, int32_t n) {
  arts_event_hint_t h = ARTS_EVENT_HINT_LATCH(n);
  h.rank = rank;
  return arts_event_create(&h);
}

static arts_guid_t edt_on(arts_edt_t fn, unsigned int rank, uint32_t paramc,
                          const uint64_t *paramv, uint32_t depc,
                          arts_guid_t completion_event) {
  arts_edt_hint_t h = ARTS_EDT_HINT_DEFAULTS;
  h.rank = rank;
  h.completion_event = completion_event;
  return arts_edt_create(fn, paramc, paramv, depc, &h);
}

static uint64_t b_expected(unsigned int i) {
  if (i >= WC_FIRST && i < WC_FIRST + SPAN) {
    return WC_MARK + i;
  }
  return S_MARK + i;
}

static uint64_t *storage(arts_edt_dep_t *dep, const char *who) {
  if (dep->ptr == NULL) {
    arts_printf("FAIL: db_read_joins_read_phase %s got no storage\n", who);
    arts_test_fail();
  }
  return (uint64_t *)dep->ptr;
}

static void check_fill(const uint64_t *b, unsigned int first, const char *who) {
  for (unsigned int i = first; i < first + SPAN; i++) {
    if (b[i] != S_MARK + i) {
      arts_printf("FAIL: db_read_joins_read_phase %s read B[%u] = %#lx, "
                  "expected %#lx\n",
                  who, i, (unsigned long)b[i], (unsigned long)(S_MARK + i));
      arts_test_fail();
    }
  }
}

/* depv: B (RW) */
static void wc(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  uint64_t *b = storage(&depv[0], "WC");
  if (b != NULL) {
    for (unsigned int i = WC_FIRST; i < WC_FIRST + SPAN; i++) {
      b[i] = WC_MARK + i;
    }
  }
}

/* depv: B (RO) */
static void rc(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  const uint64_t *b = storage(&depv[0], "RC");
  if (b != NULL) {
    check_fill(b, RC_FIRST, "RC");
  }
}

/* depv: B (RO), E (RO) */
static void rd(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  const uint64_t *b = storage(&depv[0], "RD (B)");
  const uint64_t *e = storage(&depv[1], "RD (E)");
  if (b != NULL) {
    check_fill(b, RD_FIRST, "RD");
  }
  if (e != NULL) {
    for (unsigned int i = 0; i < E_WORDS; i++) {
      if (e[i] != X_MARK + i) {
        arts_printf("FAIL: db_read_joins_read_phase RD read E[%u] = %#lx, "
                    "expected %#lx\n",
                    i, (unsigned long)e[i], (unsigned long)(X_MARK + i));
        arts_test_fail();
      }
    }
  }
}

/* runs on rank H once RC completed. paramv: E */
static void x_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t e = (arts_guid_t)paramv[0];
  uint64_t *p = (uint64_t *)arts_db_create_with_guid(
      e, E_WORDS * sizeof(uint64_t), ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  if (p == NULL) {
    arts_printf("FAIL: db_read_joins_read_phase create of E gave no "
                "storage\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  for (unsigned int i = 0; i < E_WORDS; i++) {
    p[i] = X_MARK + i;
  }
  arts_db_release(e);
}

/* runs on rank C. paramv: B, latch, RC's completion event */
static void t_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t b = (arts_guid_t)paramv[0];
  arts_guid_t latch = (arts_guid_t)paramv[1];
  arts_guid_t rcdone = (arts_guid_t)paramv[2];
  unsigned int here = arts_get_current_rank();
  /* Each acquire runs inside its call: WC's write request leaves before RC
   * acquires. */
  arts_guid_t w = edt_on(wc, here, 0, NULL, 1, latch);
  arts_add_dependence(b, w, 0, DB_MODE_RW);
  arts_guid_t r = edt_on(rc, here, 0, NULL, 1, rcdone);
  arts_add_dependence(b, r, 0, DB_MODE_RO);
}

/* runs on rank D. depv: B (RO). paramv: B, E, latch, RC's completion event */
static void r0(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  arts_guid_t b = (arts_guid_t)paramv[0];
  arts_guid_t e = (arts_guid_t)paramv[1];
  arts_guid_t latch = (arts_guid_t)paramv[2];
  arts_guid_t rcdone = (arts_guid_t)paramv[3];
  unsigned int here = arts_get_current_rank();
  (void)storage(&depv[0], "R0");
  arts_guid_t r = edt_on(rd, here, 0, NULL, 2, latch);
  arts_add_dependence(b, r, 0, DB_MODE_RO);
  /* This call readies RD and runs its acquires: the read of B is served from
   * the read grant this EDT holds, and the read of E waits for E's create. */
  arts_add_dependence(e, r, 1, DB_MODE_RO);
  uint64_t tpv[3] = {(uint64_t)b, (uint64_t)latch, (uint64_t)rcdone};
  (void)edt_on(t_edt, RANK_C, 3, tpv, 0, NULL_GUID);
}

/* runs on rank H. paramv: B, E, latch */
static void s_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t b = (arts_guid_t)paramv[0];
  arts_guid_t e = (arts_guid_t)paramv[1];
  arts_guid_t latch = (arts_guid_t)paramv[2];
  unsigned int here = arts_get_current_rank();
  uint64_t *p = (uint64_t *)arts_db_create_with_guid(
      b, B_WORDS * sizeof(uint64_t), ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  if (p == NULL) {
    arts_printf("FAIL: db_read_joins_read_phase create of B gave no "
                "storage\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  for (unsigned int i = 0; i < B_WORDS; i++) {
    p[i] = S_MARK + i;
  }
  arts_db_release(b);
  arts_guid_t rcdone = latch_on(here, 1);
  uint64_t xpv[1] = {(uint64_t)e};
  arts_guid_t x = edt_on(x_edt, here, 1, xpv, 1, latch);
  arts_add_dependence(rcdone, x, 0, DB_MODE_NULL);
  uint64_t rpv[4] = {(uint64_t)b, (uint64_t)e, (uint64_t)latch,
                     (uint64_t)rcdone};
  arts_guid_t r = edt_on(r0, RANK_D, 4, rpv, 1, NULL_GUID);
  arts_add_dependence(b, r, 0, DB_MODE_RO);
}

/* depv: latch, B (RO), E (RO). paramv: B, E */
static void final_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  const uint64_t *b = storage(&depv[1], "the final EDT (B)");
  const uint64_t *e = storage(&depv[2], "the final EDT (E)");
  if (b != NULL) {
    for (unsigned int i = 0; i < B_WORDS; i++) {
      if (b[i] != b_expected(i)) {
        arts_printf("FAIL: db_read_joins_read_phase B[%u] = %#lx, expected "
                    "%#lx\n",
                    i, (unsigned long)b[i], (unsigned long)b_expected(i));
        arts_test_fail();
      }
    }
  }
  if (e != NULL) {
    for (unsigned int i = 0; i < E_WORDS; i++) {
      if (e[i] != X_MARK + i) {
        arts_printf("FAIL: db_read_joins_read_phase E[%u] = %#lx, expected "
                    "%#lx\n",
                    i, (unsigned long)e[i], (unsigned long)(X_MARK + i));
        arts_test_fail();
      }
    }
  }
  arts_db_destroy((arts_guid_t)paramv[0]);
  arts_db_destroy((arts_guid_t)paramv[1]);
  if (arts_test_status() == 0) {
    arts_printf("PASS: db_read_joins_read_phase\n");
  }
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (arts_get_total_ranks() < 4u) {
    arts_printf("FAIL: db_read_joins_read_phase needs four ranks\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  arts_guid_t range = arts_guid_reserve_range(ARTS_GUID_DB, 2u, RANK_H);
  arts_guid_t b = arts_guid_from_index(range, 0u);
  arts_guid_t e = arts_guid_from_index(range, 1u);
  if (!(b < e)) {
    arts_printf("FAIL: db_read_joins_read_phase needs B below E\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  arts_guid_t latch = latch_on(0u, 3);
  uint64_t fpv[2] = {(uint64_t)b, (uint64_t)e};
  arts_guid_t f = edt_on(final_edt, 0u, 2, fpv, 3, NULL_GUID);
  arts_add_dependence(latch, f, 0, DB_MODE_NULL);
  arts_add_dependence(b, f, 1, DB_MODE_RO);
  arts_add_dependence(e, f, 2, DB_MODE_RO);
  uint64_t spv[3] = {(uint64_t)b, (uint64_t)e, (uint64_t)latch};
  (void)edt_on(s_edt, RANK_H, 3, spv, 0, NULL_GUID);
}

int main(int argc, char **argv) {
  return arts_rt(argc, argv);
}
