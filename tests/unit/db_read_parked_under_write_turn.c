/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* db_read_parked_under_write_turn — on EXCL a rank's read request that its
 * home holds behind that rank's own write turn is closed by its own answer,
 * and the rank's write permission serves its writers alone.
 *
 * Blocks B, E and F are names of one range reserved on rank H (0), in that
 * order, so a walk acquires B first.  On rank H, main creates B, fills it
 * and releases it, then starts W0 {B: RW} on rank A (1).  W0 holds B's write
 * turn and, while it holds it, makes WA {B: RW, E: RO} ready: WA's write of B
 * joins the turn W0 holds, and WA's walk then waits, holding the turn, for E,
 * which does not exist yet.  So rank A holds B's write turn, and holds it,
 * before anything below starts.  Only then does W0 start T on rank H.  T makes
 * WC {B: RW} ready and then RC {B: RO}.  Each acquire runs inside the call that
 * readies its EDT, and rank H is B's home, so rank H's write request and then
 * its read request run at the home inside T's body, in that order and behind
 * rank A's turn, whatever the timing.  T's completion readies X on rank H,
 * which creates E; WA runs, writes a range of B and releases it.  What follows
 * is the retention's:
 *
 * PURGE: rank A's release ends its turn and the home grants the next write
 * turn to rank H, inline.  That grant finds RC parked on rank H's read
 * request: it serves WC alone and sets RC aside.  WC, holding the turn,
 * starts WD {B: RW} on rank A and waits until the home, its own rank, counts
 * WD's write request behind the turn: the home's lock word is the one place
 * that shows a request from another rank has arrived.  WC then writes its
 * range and returns.  Its release ends rank H's turn with a write request
 * queued, so the home grants rank A's turn first, WD writes its range, and
 * only WD's release opens the read phase in which the home grants rank H's
 * read request, whose answer returns RC: RC reads WD's range as WD wrote it.
 * The verdict rests on that range.  A reader served under rank H's write turn
 * holds B under that turn, so the turn ends, and WD writes, only after the
 * reader returned: such a reader cannot find WD's bytes, whatever the timing.
 * No captured counter tells the two serves apart: every acquisition on a
 * block's home rank is a local hit, and the home's own requests and grants
 * are no messages.
 *
 * RETAIN: rank A's release migrates B to rank H.  The migration's DELIVER sets
 * RC aside and serves WC, and rank H's EXCL_CONFIRM, inline at the home,
 * opens the read phase with rank H as the owner and forwards RC's held read
 * request to rank H itself: that forward is the request's answer, and RC
 * resumes while WC holds the turn.  Here WC is {B: RW, F: RO}, and F is
 * created by Y only once RC completed: WC holds B's turn while its walk waits
 * for F, so a runtime that served RC only after rank H's turn never ends the
 * run (the verdict is the test's TIMEOUT).  F is created without the
 * creator's hold, since WC's read of it would join a hold of its own rank, and
 * WC reads none of its bytes.  RC reads only ranges WC does not write.  The
 * census tells the answers apart: RC is a local hit (its own rank's answer),
 * where a serve by the migration would count it remote.
 *
 * A latch counts the EDTs whose holds the destroys follow; it readies the
 * final EDT {B: RO, E: RO} on rank H, which checks both blocks, destroys B, E
 * (and F) and ends the run.
 *
 * The program is legal on both retentions: E's create waits for T, which holds
 * nothing; under RETAIN F's create waits for RC, which the home answers by
 * rank H's own forward; under PURGE WC waits, in its body, only for a message
 * of rank A, which waits for no EDT.  Under PURGE the RETAIN shape (WC waiting
 * for F) is the stated limit of that retention and is not run.
 *
 * The shape is the tree's retention's.  Needs two ranks.  The verdict: exit
 * status, PASS line, no FAIL line and no QUIESCENCE-DEBUG marker; where the
 * tree captures the acquisition census, the registration checks each rank's
 * record.
 */

#include <stdint.h>

#include "arts.h"
#include "../test_failure_status.h"

#ifdef ARTS_RETENTION_PURGE
#include "arts/utils/stdatomic_compat.h"
#include <time.h>

#include "arts/coherence/excl/types.h"
#include "arts/gas/route_table.h"
#include "arts/utils/shared.h"
#endif

#define B_WORDS 32u
#define E_WORDS 8u
#define F_WORDS 8u
#define SPAN 8u
#define RC_FIRST 0u
#define WA_FIRST 8u
#define WC_FIRST 16u
#define WD_FIRST 24u

#define S_MARK 0x5100u
#define WA_MARK 0xA100u
#define WC_MARK 0xC100u
#define WD_MARK 0xD100u
#define X_MARK 0xE100u

#define RANK_H 0u
#define RANK_A 1u

#ifdef ARTS_RETENTION_RETAIN
#define RETENTION "retain"
#else
#define RETENTION "purge"
#define WAIT_MS 20000L
#endif

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

static uint64_t *storage(arts_edt_dep_t *dep, const char *who) {
  if (dep->ptr == NULL) {
    arts_printf("FAIL: db_read_parked_under_write_turn %s got no storage\n",
                who);
    arts_test_fail();
  }
  return (uint64_t *)dep->ptr;
}

static void check_range(const uint64_t *p, unsigned int first,
                        unsigned int count, uint64_t mark, const char *who,
                        const char *what) {
  for (unsigned int i = first; i < first + count; i++) {
    if (p[i] != mark + i) {
      arts_printf("FAIL: db_read_parked_under_write_turn %s read %s[%u] = "
                  "%#lx, expected %#lx\n",
                  who, what, i, (unsigned long)p[i],
                  (unsigned long)(mark + i));
      arts_test_fail();
    }
  }
}

static void fill(uint64_t *p, unsigned int first, unsigned int count,
                 uint64_t mark) {
  for (unsigned int i = first; i < first + count; i++) {
    p[i] = mark + i;
  }
}

/* Creates `g` of `words` words on this rank, fills it and releases it. */
static void create_filled(arts_guid_t g, unsigned int words, uint64_t mark,
                          const char *who) {
  uint64_t *p = (uint64_t *)arts_db_create_with_guid(
      g, words * sizeof(uint64_t), ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  if (p == NULL) {
    arts_printf("FAIL: db_read_parked_under_write_turn %s's create gave no "
                "storage\n",
                who);
    arts_test_fail();
    arts_shutdown();
    return;
  }
  fill(p, 0u, words, mark);
  arts_db_release(g);
}

#ifdef ARTS_RETENTION_PURGE
static long now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long)t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

/* runs on rank A. depv: B (RW) */
static void wd(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  uint64_t *b = storage(&depv[0], "WD");
  if (b != NULL) {
    fill(b, WD_FIRST, SPAN, WD_MARK);
  }
}

/* Returns once the home of `b`, this rank, counts a second write request:
 * the caller holds this rank's write turn, which is the first. */
static void await_queued_writer(arts_guid_t b) {
  arts_shared_ptr_t h = arts_route_table_lookup_db(b);
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(h);
  if (db == NULL) {
    arts_printf("FAIL: db_read_parked_under_write_turn WC found no home "
                "descriptor of B\n");
    arts_test_fail();
    return;
  }
  long start = now_ms();
  while (EXCL_STATE_W(atomic_load_explicit(&db->lock_state,
                                           memory_order_acquire)) != 2u) {
    if (now_ms() - start > WAIT_MS) {
      arts_printf("FAIL: db_read_parked_under_write_turn the home never "
                  "counted rank %u's write request behind rank %u's turn\n",
                  RANK_A, RANK_H);
      arts_test_fail();
      break;
    }
  }
  arts_shared_release(&h);
}
#endif

/* depv: B (RW); under RETAIN also F (RO). paramv under PURGE: latch */
static void wc(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint64_t *b = storage(&depv[0], "WC");
#ifdef ARTS_RETENTION_PURGE
  arts_guid_t w = edt_on(wd, RANK_A, 0, NULL, 1, (arts_guid_t)paramv[0]);
  arts_add_dependence(depv[0].guid, w, 0, DB_MODE_RW);
  await_queued_writer(depv[0].guid);
#else
  (void)paramv;
  (void)storage(&depv[1], "WC (F)");
#endif
  if (b != NULL) {
    fill(b, WC_FIRST, SPAN, WC_MARK);
  }
}

/* depv: B (RO) */
static void rc(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  const uint64_t *b = storage(&depv[0], "RC");
  if (b == NULL) {
    return;
  }
  check_range(b, RC_FIRST, SPAN, S_MARK, "RC", "B");
  check_range(b, WA_FIRST, SPAN, WA_MARK, "RC", "B");
#ifdef ARTS_RETENTION_PURGE
  if (b[WD_FIRST] != WD_MARK + WD_FIRST) {
    arts_printf("FAIL: db_read_parked_under_write_turn RC ran before rank "
                "%u's writer WD, whose turn follows rank %u's: its read was "
                "served under rank %u's write turn\n",
                RANK_A, arts_get_current_rank(), arts_get_current_rank());
    arts_test_fail();
  }
  check_range(b, WC_FIRST, SPAN, WC_MARK, "RC", "B");
  check_range(b, WD_FIRST, SPAN, WD_MARK, "RC", "B");
#endif
}

/* runs on rank H once T completed. paramv: E */
static void x_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  create_filled((arts_guid_t)paramv[0], E_WORDS, X_MARK, "X");
}

#ifdef ARTS_RETENTION_RETAIN
/* runs on rank H once RC completed. paramv: F */
static void y_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  (void)arts_db_create_with_guid((arts_guid_t)paramv[0],
                                 F_WORDS * sizeof(uint64_t), ARTS_DB,
                                 ARTS_DB_PROP_NO_ACQUIRE, NULL);
}
#endif

/* runs on rank H. paramv: B, F, latch */
static void t_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t b = (arts_guid_t)paramv[0];
  arts_guid_t latch = (arts_guid_t)paramv[2];
  unsigned int here = arts_get_current_rank();
  /* Each acquire runs inside the call that readies its EDT, and this rank is
   * B's home: rank H's write request reaches the home before its read
   * request, both behind rank A's write turn. */
#ifdef ARTS_RETENTION_RETAIN
  arts_guid_t f = (arts_guid_t)paramv[1];
  arts_guid_t rcdone = latch_on(here, 1);
  uint64_t ypv[1] = {(uint64_t)f};
  arts_guid_t y = edt_on(y_edt, here, 1, ypv, 1, latch);
  arts_add_dependence(rcdone, y, 0, DB_MODE_NULL);
  arts_guid_t w = edt_on(wc, here, 0, NULL, 2, latch);
  arts_add_dependence(b, w, 0, DB_MODE_RW);
  arts_add_dependence(f, w, 1, DB_MODE_RO);
#else
  arts_guid_t rcdone = latch;
  uint64_t wpv[1] = {(uint64_t)latch};
  arts_guid_t w = edt_on(wc, here, 1, wpv, 1, latch);
  arts_add_dependence(b, w, 0, DB_MODE_RW);
#endif
  arts_guid_t r = edt_on(rc, here, 0, NULL, 1, rcdone);
  arts_add_dependence(b, r, 0, DB_MODE_RO);
}

/* runs on rank A. depv: B (RW), E (RO) */
static void wa(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  uint64_t *b = storage(&depv[0], "WA (B)");
  const uint64_t *e = storage(&depv[1], "WA (E)");
  if (b != NULL) {
    fill(b, WA_FIRST, SPAN, WA_MARK);
  }
  if (e != NULL) {
    check_range(e, 0u, E_WORDS, X_MARK, "WA", "E");
  }
}

/* runs on rank A. depv: B (RW). paramv: B, E, F, latch */
static void w0(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  arts_guid_t b = (arts_guid_t)paramv[0];
  arts_guid_t e = (arts_guid_t)paramv[1];
  arts_guid_t f = (arts_guid_t)paramv[2];
  arts_guid_t latch = (arts_guid_t)paramv[3];
  unsigned int here = arts_get_current_rank();
  (void)storage(&depv[0], "W0");
  arts_guid_t a = edt_on(wa, here, 0, NULL, 2, latch);
  arts_add_dependence(b, a, 0, DB_MODE_RW);
  /* This call readies WA and runs its acquires: its write of B joins the
   * turn this EDT holds, and its read of E waits for E's create. */
  arts_add_dependence(e, a, 1, DB_MODE_RO);
  arts_guid_t tdone = latch_on(RANK_H, 1);
  uint64_t xpv[1] = {(uint64_t)e};
  arts_guid_t x = edt_on(x_edt, RANK_H, 1, xpv, 1, latch);
  arts_add_dependence(tdone, x, 0, DB_MODE_NULL);
  uint64_t tpv[3] = {(uint64_t)b, (uint64_t)f, (uint64_t)latch};
  (void)edt_on(t_edt, RANK_H, 3, tpv, 0, tdone);
}

/* depv: latch, B (RO), E (RO). paramv: B, E, F */
static void final_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  const uint64_t *b = storage(&depv[1], "the final EDT (B)");
  const uint64_t *e = storage(&depv[2], "the final EDT (E)");
  if (b != NULL) {
    check_range(b, RC_FIRST, SPAN, S_MARK, "the final EDT", "B");
    check_range(b, WA_FIRST, SPAN, WA_MARK, "the final EDT", "B");
    check_range(b, WC_FIRST, SPAN, WC_MARK, "the final EDT", "B");
#ifdef ARTS_RETENTION_PURGE
    check_range(b, WD_FIRST, SPAN, WD_MARK, "the final EDT", "B");
#else
    check_range(b, WD_FIRST, SPAN, S_MARK, "the final EDT", "B");
#endif
  }
  if (e != NULL) {
    check_range(e, 0u, E_WORDS, X_MARK, "the final EDT", "E");
  }
  arts_db_destroy((arts_guid_t)paramv[0]);
  arts_db_destroy((arts_guid_t)paramv[1]);
#ifdef ARTS_RETENTION_RETAIN
  arts_db_destroy((arts_guid_t)paramv[2]);
#endif
  if (arts_test_status() == 0) {
    arts_printf("PASS: db_read_parked_under_write_turn " RETENTION "\n");
  }
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (arts_get_total_ranks() < 2u || arts_get_current_rank() != RANK_H) {
    arts_printf("FAIL: db_read_parked_under_write_turn needs two ranks, "
                "main on rank %u\n",
                RANK_H);
    arts_test_fail();
    arts_shutdown();
    return;
  }
  arts_guid_t range = arts_guid_reserve_range(ARTS_GUID_DB, 3u, RANK_H);
  arts_guid_t b = arts_guid_from_index(range, 0u);
  arts_guid_t e = arts_guid_from_index(range, 1u);
  arts_guid_t f = arts_guid_from_index(range, 2u);
  if (!(b < e && e < f)) {
    arts_printf("FAIL: db_read_parked_under_write_turn needs B below E "
                "below F\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  /* WA, WC, X; RC and WD under PURGE, Y (which follows RC) under RETAIN. */
#ifdef ARTS_RETENTION_RETAIN
  arts_guid_t latch = latch_on(RANK_H, 4);
#else
  arts_guid_t latch = latch_on(RANK_H, 5);
#endif
  uint64_t fpv[3] = {(uint64_t)b, (uint64_t)e, (uint64_t)f};
  arts_guid_t fin = edt_on(final_edt, RANK_H, 3, fpv, 3, NULL_GUID);
  arts_add_dependence(latch, fin, 0, DB_MODE_NULL);
  arts_add_dependence(b, fin, 1, DB_MODE_RO);
  arts_add_dependence(e, fin, 2, DB_MODE_RO);
  uint64_t *p = (uint64_t *)arts_db_create_with_guid(
      b, B_WORDS * sizeof(uint64_t), ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  if (p == NULL) {
    arts_printf("FAIL: db_read_parked_under_write_turn create of B gave no "
                "storage\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  fill(p, 0u, B_WORDS, S_MARK);
  arts_db_release(b);
  uint64_t wpv[4] = {(uint64_t)b, (uint64_t)e, (uint64_t)f, (uint64_t)latch};
  arts_guid_t w = edt_on(w0, RANK_A, 4, wpv, 1, NULL_GUID);
  arts_add_dependence(b, w, 0, DB_MODE_RW);
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
