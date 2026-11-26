/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* db_read_while_write_waits — a read acquisition on a rank whose write
 * request for the same block waits behind another rank's writer is served
 * without that writer's release.
 *
 * Two blocks of one size are homed on rank 1.  C is the lower name and B the
 * higher, so a walk that acquires both takes C first.  On rank H (1 with two
 * ranks, 2 with more) S0 creates both, fills them, releases them and starts
 * G0 {B: RW} on rank 0.  G0 holds B's write turn and starts L1 {C: RW} on
 * rank H.  L1, holding C's write turn, makes Y {C: RW, B: RW} ready: Y's
 * write of C joins that turn on the spot, and Y's walk then waits for B,
 * which rank 0 holds.  Only then does L1 release C and start T on rank 0,
 * which sets the flag G0 waits for.  G0 then makes Z0 {C: RW} ready, so
 * rank 0's write request for C leaves and waits for Y's release, and after it
 * X0 {B: RW, C: RO}: X0's read of C leaves, and its write of B joins the turn
 * G0 holds.  So when X0's read is asked, rank H holds C's turn through Y, Y
 * waits for B, and rank 0 keeps B until X0 has run: a runtime that leaves
 * X0's read behind rank 0's write request parks every EDT and never ends the
 * run.  X0 checks a range of S0's fill of C; Y writes a range of each block;
 * Z0 writes a range of C.  A latch counts X0, Y and Z0; it readies the final
 * EDT {B: RO, C: RO} on rank 0, which checks every word of both blocks,
 * destroys them and ends the run.
 *
 * S0 on rank H, not main_edt on rank 0, creates the blocks, so rank 0 has
 * never held C's bytes when X0 reads it: a valid copy there would serve the
 * read with no request of its own.  X0 holds B and writes nothing in it: the
 * hold is what keeps Y from B until X0 ran, and G0's release of B, which
 * X0's hold outlives, may copy rank 0's buffer of B to the home where the
 * home is the read source, a copy a write of X0 would race.
 *
 * The program is legal: Y waits for X0's release of B, X0 only for its read
 * of C, Z0 for Y's release of C, and nothing but the latch waits for Z0.
 * Under VAL and INV a read acquisition waits for no release, so every chain
 * of waits ends.  G0's wait for the flag orders the set-up only: T depends on
 * no contested acquisition, so the flag is set on every runtime, and the
 * wait's bound is not the verdict.  The final EDT's destroy of B follows G0's
 * release: Y, which the latch counts, gets B only once rank 0's turn ended,
 * G0's share included.
 *
 * Under EXCL every acquisition is in the name-ordered walk: X0 would wait at
 * C, the lower name, holding nothing, G0 would end, Y would get B and the run
 * would end without showing the rule, so the test registers on VAL and INV
 * only.  Its write acquisitions are unordered, which CDAG does not admit.
 *
 * Needs two ranks.  The verdict: the run ends within the registration's
 * TIMEOUT, with exit status 0, the PASS line, no FAIL line and no
 * QUIESCENCE-DEBUG marker.
 */

#include <stdint.h>
#include <time.h>

#include "arts.h"
#include "arts/utils/stdatomic_compat.h"
#include "../test_failure_status.h"

#define WORDS 32u
#define SPAN 8u
#define X0_FIRST 0u
#define Y_FIRST 8u
#define Z0_FIRST 16u

#define SB_MARK 0x5B00u
#define SC_MARK 0x5C00u
#define Y_MARK 0x1100u
#define Z0_MARK 0x2100u

#define HOME_RANK 1u
#define WAIT_MS 20000L

/* Set once by T (release) after L1 readied Y; read by G0 (acquire), which
 * runs on T's rank. */
static _Atomic(int) g_in_place;

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

static long now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long)t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static uint64_t b_expected(unsigned int i) {
  if (i >= Y_FIRST && i < Y_FIRST + SPAN) {
    return Y_MARK + i;
  }
  return SB_MARK + i;
}

static uint64_t c_expected(unsigned int i) {
  if (i >= Y_FIRST && i < Y_FIRST + SPAN) {
    return Y_MARK + i;
  }
  if (i >= Z0_FIRST && i < Z0_FIRST + SPAN) {
    return Z0_MARK + i;
  }
  return SC_MARK + i;
}

static uint64_t *storage(arts_edt_dep_t *dep, const char *who) {
  if (dep->ptr == NULL) {
    arts_printf("FAIL: db_read_while_write_waits %s got no storage\n", who);
    arts_test_fail();
  }
  return (uint64_t *)dep->ptr;
}

static void write_range(uint64_t *p, unsigned int first, uint64_t mark) {
  for (unsigned int i = first; i < first + SPAN; i++) {
    p[i] = mark + i;
  }
}

static void check_block(const uint64_t *p, uint64_t (*expected)(unsigned int),
                        const char *what) {
  for (unsigned int i = 0; i < WORDS; i++) {
    if (p[i] != expected(i)) {
      arts_printf("FAIL: db_read_while_write_waits %s[%u] = %#lx, expected "
                  "%#lx\n",
                  what, i, (unsigned long)p[i], (unsigned long)expected(i));
      arts_test_fail();
    }
  }
}

/* runs on rank 0. depv: B (RW), C (RO) */
static void x0(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)storage(&depv[0], "X0 (B)");
  const uint64_t *c = storage(&depv[1], "X0 (C)");
  if (c == NULL) {
    return;
  }
  for (unsigned int i = X0_FIRST; i < X0_FIRST + SPAN; i++) {
    if (c[i] != SC_MARK + i) {
      arts_printf("FAIL: db_read_while_write_waits X0 read C[%u] = %#lx, "
                  "expected %#lx\n",
                  i, (unsigned long)c[i], (unsigned long)(SC_MARK + i));
      arts_test_fail();
    }
  }
}

/* runs on rank H. depv: C (RW), B (RW) */
static void y_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  uint64_t *c = storage(&depv[0], "Y (C)");
  uint64_t *b = storage(&depv[1], "Y (B)");
  if (c != NULL) {
    write_range(c, Y_FIRST, Y_MARK);
  }
  if (b != NULL) {
    write_range(b, Y_FIRST, Y_MARK);
  }
}

/* runs on rank 0. depv: C (RW) */
static void z0(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  uint64_t *c = storage(&depv[0], "Z0");
  if (c != NULL) {
    write_range(c, Z0_FIRST, Z0_MARK);
  }
}

/* runs on rank 0 */
static void t_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  atomic_store_explicit(&g_in_place, 1, memory_order_release);
}

/* runs on rank H. depv: C (RW). paramv: B, C, latch */
static void l1(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  arts_guid_t b = (arts_guid_t)paramv[0];
  arts_guid_t c = (arts_guid_t)paramv[1];
  arts_guid_t latch = (arts_guid_t)paramv[2];
  (void)storage(&depv[0], "L1");
  arts_guid_t w = edt_on(y_edt, arts_get_current_rank(), 0, NULL, 2, latch);
  arts_add_dependence(c, w, 0, DB_MODE_RW);
  /* This call readies Y and runs its acquires: its write of C joins the turn
   * this EDT holds, and its walk then waits for B. */
  arts_add_dependence(b, w, 1, DB_MODE_RW);
  arts_db_release(depv[0].guid);
  (void)edt_on(t_edt, 0u, 0, NULL, 0, NULL_GUID);
}

/* runs on rank 0. depv: B (RW). paramv: B, C, latch, H */
static void g0(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  arts_guid_t b = (arts_guid_t)paramv[0];
  arts_guid_t c = (arts_guid_t)paramv[1];
  arts_guid_t latch = (arts_guid_t)paramv[2];
  unsigned int holder = (unsigned int)paramv[3];
  unsigned int here = arts_get_current_rank();
  (void)storage(&depv[0], "G0");
  uint64_t lpv[3] = {(uint64_t)b, (uint64_t)c, (uint64_t)latch};
  arts_guid_t l = edt_on(l1, holder, 3, lpv, 1, NULL_GUID);
  arts_add_dependence(c, l, 0, DB_MODE_RW);
  const struct timespec nap = {.tv_sec = 0, .tv_nsec = 1000000L};
  long start = now_ms();
  while (atomic_load_explicit(&g_in_place, memory_order_acquire) == 0) {
    if (now_ms() - start > WAIT_MS) {
      arts_printf("FAIL: db_read_while_write_waits the set-up did not "
                  "arrive: rank %u's writer of C was not reported parked "
                  "within %ld ms\n",
                  holder, WAIT_MS);
      arts_test_fail();
      arts_shutdown();
      return;
    }
    (void)nanosleep(&nap, NULL);
  }
  /* Each acquire runs inside the call that readies its EDT: Z0's write
   * request for C leaves before X0's read of C. */
  arts_guid_t z = edt_on(z0, here, 0, NULL, 1, latch);
  arts_add_dependence(c, z, 0, DB_MODE_RW);
  arts_guid_t x = edt_on(x0, here, 0, NULL, 2, latch);
  arts_add_dependence(c, x, 1, DB_MODE_RO);
  /* This call readies X0: its read of C leaves, and its write of B joins the
   * turn this EDT holds. */
  arts_add_dependence(b, x, 0, DB_MODE_RW);
}

/* depv: latch, B (RO), C (RO). paramv: B, C */
static void final_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  const uint64_t *b = storage(&depv[1], "the final EDT (B)");
  const uint64_t *c = storage(&depv[2], "the final EDT (C)");
  if (b != NULL) {
    check_block(b, b_expected, "B");
  }
  if (c != NULL) {
    check_block(c, c_expected, "C");
  }
  arts_db_destroy((arts_guid_t)paramv[0]);
  arts_db_destroy((arts_guid_t)paramv[1]);
  if (arts_test_status() == 0) {
    arts_printf("PASS: db_read_while_write_waits\n");
  }
  arts_shutdown();
}

/* runs on rank H. paramv: latch, H */
static void s0(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t latch = (arts_guid_t)paramv[0];
  arts_db_hint_t h = ARTS_DB_HINT_DEFAULTS;
  h.rank = HOME_RANK;
  void *first_p = NULL;
  void *second_p = NULL;
  arts_guid_t first = arts_db_create(&first_p, WORDS * sizeof(uint64_t),
                                     ARTS_DB, ARTS_DB_PROP_NONE, &h);
  arts_guid_t second = arts_db_create(&second_p, WORDS * sizeof(uint64_t),
                                      ARTS_DB, ARTS_DB_PROP_NONE, &h);
  if (first_p == NULL || second_p == NULL) {
    arts_printf("FAIL: db_read_while_write_waits a create gave no storage\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  int first_lower = first < second;
  arts_guid_t c = first_lower ? first : second;
  arts_guid_t b = first_lower ? second : first;
  uint64_t *pc = (uint64_t *)(first_lower ? first_p : second_p);
  uint64_t *pb = (uint64_t *)(first_lower ? second_p : first_p);
  for (unsigned int i = 0; i < WORDS; i++) {
    pc[i] = SC_MARK + i;
    pb[i] = SB_MARK + i;
  }
  arts_db_release(c);
  arts_db_release(b);
  uint64_t fpv[2] = {(uint64_t)b, (uint64_t)c};
  arts_guid_t f = edt_on(final_edt, 0u, 2, fpv, 3, NULL_GUID);
  arts_add_dependence(latch, f, 0, DB_MODE_NULL);
  arts_add_dependence(b, f, 1, DB_MODE_RO);
  arts_add_dependence(c, f, 2, DB_MODE_RO);
  uint64_t gpv[4] = {(uint64_t)b, (uint64_t)c, (uint64_t)latch, paramv[1]};
  arts_guid_t g = edt_on(g0, 0u, 4, gpv, 1, NULL_GUID);
  arts_add_dependence(b, g, 0, DB_MODE_RW);
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
  arts_guid_t latch = latch_on(0u, 3);
  uint64_t spv[2] = {(uint64_t)latch, (uint64_t)holder};
  (void)edt_on(s0, holder, 2, spv, 0, NULL_GUID);
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
