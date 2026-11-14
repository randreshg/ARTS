/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* db_read_while_write_waits — a read acquisition on a rank whose write
 * request for the same block waits behind another rank's writer is served
 * without that writer's release.
 *
 * Blocks B and D are reserved on rank 1.  On rank H (1 with two ranks, 2 with
 * more) S creates B, fills it and, while its create hold keeps the write
 * turn, makes W1 {B: RW, D: RO} ready: W1's write acquire of B joins that
 * turn on the spot, and W1 then waits, holding it, for D, which does not
 * exist yet.  Only then does S release B and start T on rank 0.  T makes
 * Z0 {B: RW} ready, so rank 0's write request for B leaves and waits for
 * W1's release, and after it X0 {B: RO}, which checks a range of S's fill.
 * X0's completion readies C on rank H, which creates D, fills it and
 * releases it; that lets W1 run.  W1 and Z0 write ranges of B no other EDT
 * touches.  A latch counts W1, Z0 and C, whose completion follows X0's; it
 * readies the final EDT {B: RO, D: RO}, which checks every byte of both
 * blocks, destroys them and ends the run.  The final EDT waits for C rather
 * than for X0 alone because C writes D under its create hold, which W1's
 * read of D does not wait for.
 *
 * The program is legal: nothing the chain to D's create waits for is held by
 * W1 except through X0's read of B, and under VAL and INV a read acquisition
 * never waits for another EDT's release.  X0 reads only words S wrote before
 * X0 was created, so whichever version it is served, it sees S's fill.  Z0
 * waits for W1, and nothing waits for Z0 but the latch.
 *
 * Under EXCL a read waits behind another node's write turn, so the same
 * program blocks there by design: it is not an EXCL program.  Its write
 * acquisitions of B are unordered, which CDAG does not admit.
 *
 * Needs two ranks.  The verdict: exit status, PASS line, no FAIL line and no
 * QUIESCENCE-DEBUG marker.
 */

#include <stdint.h>

#include "arts.h"
#include "../test_failure_status.h"

#define B_WORDS 32u
#define D_WORDS 8u
#define SPAN 8u
#define X0_FIRST 0u
#define W1_FIRST 8u
#define Z0_FIRST 16u

#define S_MARK 0x5100u
#define W1_MARK 0x1100u
#define Z0_MARK 0x2100u
#define C_MARK 0xD100u

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
  if (i >= W1_FIRST && i < W1_FIRST + SPAN) {
    return W1_MARK + i;
  }
  if (i >= Z0_FIRST && i < Z0_FIRST + SPAN) {
    return Z0_MARK + i;
  }
  return S_MARK + i;
}

static uint64_t *storage(arts_edt_dep_t *dep, const char *who) {
  if (dep->ptr == NULL) {
    arts_printf("FAIL: db_read_while_write_waits %s got no storage\n", who);
    arts_test_fail();
  }
  return (uint64_t *)dep->ptr;
}

static void write_range(uint64_t *b, unsigned int first, uint64_t mark) {
  for (unsigned int i = first; i < first + SPAN; i++) {
    b[i] = mark + i;
  }
}

/* depv: B (RW), D (RO) */
static void w1(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  uint64_t *b = storage(&depv[0], "W1");
  if (b != NULL) {
    write_range(b, W1_FIRST, W1_MARK);
  }
}

/* depv: B (RW) */
static void z0(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  uint64_t *b = storage(&depv[0], "Z0");
  if (b != NULL) {
    write_range(b, Z0_FIRST, Z0_MARK);
  }
}

/* depv: B (RO) */
static void x0(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  const uint64_t *b = storage(&depv[0], "X0");
  if (b == NULL) {
    return;
  }
  for (unsigned int i = X0_FIRST; i < X0_FIRST + SPAN; i++) {
    if (b[i] != S_MARK + i) {
      arts_printf("FAIL: db_read_while_write_waits X0 read B[%u] = %#lx, "
                  "expected %#lx\n",
                  i, (unsigned long)b[i], (unsigned long)(S_MARK + i));
      arts_test_fail();
    }
  }
}

/* runs on rank H once X0 completed. paramv: D */
static void c_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t d = (arts_guid_t)paramv[0];
  uint64_t *p = (uint64_t *)arts_db_create_with_guid(
      d, D_WORDS * sizeof(uint64_t), ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  if (p == NULL) {
    arts_printf("FAIL: db_read_while_write_waits create of D gave no "
                "storage\n");
    arts_test_fail();
    return;
  }
  for (unsigned int i = 0; i < D_WORDS; i++) {
    p[i] = C_MARK + i;
  }
  arts_db_release(d);
}

/* runs on rank 0. paramv: B, latch, X0's completion event */
static void t_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t b = (arts_guid_t)paramv[0];
  arts_guid_t latch = (arts_guid_t)paramv[1];
  arts_guid_t xdone = (arts_guid_t)paramv[2];
  unsigned int here = arts_get_current_rank();
  /* Each acquire runs inside its call: Z0's write request leaves before X0
   * acquires. */
  arts_guid_t z = edt_on(z0, here, 0, NULL, 1, latch);
  arts_add_dependence(b, z, 0, DB_MODE_RW);
  arts_guid_t x = edt_on(x0, here, 0, NULL, 1, xdone);
  arts_add_dependence(b, x, 0, DB_MODE_RO);
}

/* runs on rank H. paramv: B, D, latch */
static void s_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t b = (arts_guid_t)paramv[0];
  arts_guid_t d = (arts_guid_t)paramv[1];
  arts_guid_t latch = (arts_guid_t)paramv[2];
  unsigned int here = arts_get_current_rank();
  uint64_t *p = (uint64_t *)arts_db_create_with_guid(
      b, B_WORDS * sizeof(uint64_t), ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  if (p == NULL) {
    arts_printf("FAIL: db_read_while_write_waits create of B gave no "
                "storage\n");
    arts_test_fail();
    return;
  }
  for (unsigned int i = 0; i < B_WORDS; i++) {
    p[i] = S_MARK + i;
  }
  arts_guid_t w = edt_on(w1, here, 0, NULL, 2, latch);
  arts_add_dependence(b, w, 0, DB_MODE_RW);
  /* This call readies W1 and runs its acquires: the write acquire of B joins
   * the turn this EDT's create hold keeps on this rank, and the read of D
   * waits for D's create. */
  arts_add_dependence(d, w, 1, DB_MODE_RO);
  arts_guid_t xdone = latch_on(here, 1);
  uint64_t cpv[1] = {(uint64_t)d};
  arts_guid_t c = edt_on(c_edt, here, 1, cpv, 1, latch);
  arts_add_dependence(xdone, c, 0, DB_MODE_NULL);
  arts_db_release(b);
  uint64_t tpv[3] = {(uint64_t)b, (uint64_t)latch, (uint64_t)xdone};
  (void)edt_on(t_edt, 0u, 3, tpv, 0, NULL_GUID);
}

/* depv: latch, B (RO), D (RO). paramv: B, D */
static void final_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  const uint64_t *b = storage(&depv[1], "the final EDT (B)");
  const uint64_t *d = storage(&depv[2], "the final EDT (D)");
  if (b != NULL) {
    for (unsigned int i = 0; i < B_WORDS; i++) {
      if (b[i] != b_expected(i)) {
        arts_printf("FAIL: db_read_while_write_waits B[%u] = %#lx, expected "
                    "%#lx\n",
                    i, (unsigned long)b[i], (unsigned long)b_expected(i));
        arts_test_fail();
      }
    }
  }
  if (d != NULL) {
    for (unsigned int i = 0; i < D_WORDS; i++) {
      if (d[i] != C_MARK + i) {
        arts_printf("FAIL: db_read_while_write_waits D[%u] = %#lx, expected "
                    "%#lx\n",
                    i, (unsigned long)d[i], (unsigned long)(C_MARK + i));
        arts_test_fail();
      }
    }
  }
  arts_db_destroy((arts_guid_t)paramv[0]);
  arts_db_destroy((arts_guid_t)paramv[1]);
  if (arts_test_status() == 0) {
    arts_printf("PASS: db_read_while_write_waits\n");
  }
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int n = arts_get_total_ranks();
  if (n < 2u) {
    arts_printf("FAIL: db_read_while_write_waits needs two ranks\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  unsigned int holder = n > 2u ? 2u : 1u;
  arts_guid_t range = arts_guid_reserve_range(ARTS_GUID_DB, 2u, 1u);
  arts_guid_t b = arts_guid_from_index(range, 0u);
  arts_guid_t d = arts_guid_from_index(range, 1u);
  arts_guid_t latch = latch_on(0u, 3);
  uint64_t fpv[2] = {(uint64_t)b, (uint64_t)d};
  arts_guid_t f = edt_on(final_edt, 0u, 2, fpv, 3, NULL_GUID);
  arts_add_dependence(latch, f, 0, DB_MODE_NULL);
  arts_add_dependence(b, f, 1, DB_MODE_RO);
  arts_add_dependence(d, f, 2, DB_MODE_RO);
  uint64_t spv[3] = {(uint64_t)b, (uint64_t)d, (uint64_t)latch};
  (void)edt_on(s_edt, holder, 3, spv, 0, NULL_GUID);
}

int main(int argc, char **argv) {
  return arts_rt(argc, argv);
}
