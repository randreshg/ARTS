/* SPDX-License-Identifier: Apache-2.0
 *
 * The EXCL arbiters (coherence/excl/arbiters.c), the PURE state-transition
 * functions of the release policy this tree compiles.
 *
 * PURGE: excl_compute_next(cur, op, &grant), the lock-state transition over
 * [td | state_bit | rwo | roo | owed | w | r].  Given the current packed
 * lock_state and an op (RW/RO acquire/release), it returns the next packed
 * state and sets *grant to NONE / ONE_RW / ALL_RO.  This test pins its FULL
 * truth table with explicit, hand-derived expected values — no oracle that
 * merely re-implements the function — covering:
 *   - none→rw / none→ro grants
 *   - RW arriving while RO held (w==0 && r>0): parks, bit→RO, no grant
 *   - rw→rw (w>0 acquire): bit UNCHANGED, no grant (held writer serves it)
 *   - D7: new RO grant while RW waiting (RO phase extends)
 *   - RO arriving while RW held: parks, bit→RW
 *   - D6: rw→rw chain on release (w-1>0 grants next writer)
 *   - rw→ro on release (w-1==0 && r>0 drains all RO)
 *   - ro→rw on release (r-1==0 && w>0 grants one writer)
 *   - state_bit normalization to 0 whenever a counter reaches 0;
 *   - the teardown mark: a destroy with releases owed only marks, the last
 *     release tears down, and the torn-down word counts and grants no
 *     acquire.
 * A CXL tree also pins the PURGE cache-word arbiter cache_compute_next.
 *
 * RETAIN: cache_owner_compute_next, the cache word's arbiter (the acquire's
 * count and decision in one transition, the parked populations a serving
 * transition owes, the sticky owner's migration), and the home directory's
 * lock_owner_compute_next.
 *
 * The arbiters are non-static (exposed for this test); the file is built
 * standalone by #including coherence/excl/arbiters.c.  Built and registered
 * in every EXCL tree.
 *
 * Build: -DARTS_PROTOCOL_EXCL=1 -DARTS_RELEASE_{PURGE,RETAIN}=1
 * -DARTS_UNIT_STANDALONE_SHIMS.
 */

#include <stdio.h>

#include "arts/coherence/excl/types.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef ARTS_RELEASE_PURGE
/* op + grant codes mirror the arbiter (kept in sync via the same header
 * defines for the packed-state macros). */
#define T_RW_ACQ 0
#define T_RO_ACQ 1
#define T_RW_REL 2
#define T_RO_REL 3
#define T_TEARDOWN 4
#define G_NONE 0
#define G_ONE_RW 1
#define G_ALL_RO 2
#define G_DESTROY 3

/* Forward decl of the SUT (defined in the arbiter TU #included below). */
uint64_t excl_compute_next(uint64_t cur, int op, uint32_t *out_grant);

static int g_rc = 0;
static int g_checks = 0;

/* Assert: applying `op` to MAKE_STATE(in_bit,in_w,in_r) yields
 * MAKE_STATE(ex_bit,ex_w,ex_r) and grant==ex_grant. */
static void chk(const char *name, uint32_t in_bit, uint32_t in_w, uint32_t in_r,
                int op, uint32_t ex_bit, uint32_t ex_w, uint32_t ex_r,
                uint32_t ex_grant) {
  uint64_t cur = LOCK_MAKE_STATE(in_bit, in_w, in_r);
  uint32_t grant = 0xffffffff;
  uint64_t next = excl_compute_next(cur, op, &grant);
  uint32_t nw = EXCL_STATE_W(next), nr = EXCL_STATE_R(next),
           nb = EXCL_STATE_BIT(next);
  g_checks++;
  if (nw != ex_w || nr != ex_r || nb != ex_bit || grant != ex_grant) {
    (void)fprintf(
        stderr,
        "FAIL [%s]: in(bit=%u w=%u r=%u) op=%d => out(bit=%u w=%u r=%u "
        "grant=%u) expected(bit=%u w=%u r=%u grant=%u)\n",
        name, in_bit, in_w, in_r, op, nb, nw, nr, grant, ex_bit, ex_w, ex_r,
        ex_grant);
    g_rc = 1;
  }
}

/* One step of a lock-word sequence, the teardown mark included: apply `op`,
 * check the word and the grant, carry the produced word on. */
static void step_lock(const char *name, uint64_t *cur, int op, uint32_t ex_td,
                      uint32_t ex_bit, uint32_t ex_w, uint32_t ex_r,
                      uint32_t ex_grant) {
  uint32_t grant = 0xffffffff;
  uint64_t next = excl_compute_next(*cur, op, &grant);
  uint64_t want = LOCK_MAKE_STATE_TD(ex_td, ex_bit, ex_w, ex_r);
  g_checks++;
  if (next != want || grant != ex_grant) {
    (void)fprintf(stderr,
                  "FAIL [%s]: in(td=%u bit=%u w=%u r=%u) op=%d => out(td=%u "
                  "bit=%u w=%u r=%u grant=%u) expected(td=%u bit=%u w=%u r=%u "
                  "grant=%u)\n",
                  name, EXCL_STATE_TEARDOWN(*cur), EXCL_STATE_BIT(*cur),
                  EXCL_STATE_W(*cur), EXCL_STATE_R(*cur), op,
                  EXCL_STATE_TEARDOWN(next), EXCL_STATE_BIT(next),
                  EXCL_STATE_W(next), EXCL_STATE_R(next), grant, ex_td, ex_bit,
                  ex_w, ex_r, ex_grant);
    g_rc = 1;
  }
  *cur = next;
}

/* One step of a lock-word sequence that carries the owed fields. */
static void step_full(const char *name, uint64_t *cur, int op, uint64_t want,
                      uint32_t ex_grant) {
  uint32_t grant = 0xffffffff;
  uint64_t next = excl_compute_next(*cur, op, &grant);
  g_checks++;
  if (next != want || grant != ex_grant) {
    (void)fprintf(stderr,
                  "FAIL [%s]: op=%d => out(%llx grant=%u) expected(%llx "
                  "grant=%u)\n",
                  name, op, (unsigned long long)next, grant,
                  (unsigned long long)want, ex_grant);
    g_rc = 1;
  }
  *cur = next;
}

#ifdef ARTS_USE_CXL
/* The parity and stub bits ride the word; the field checks below ignore
 * them (the request's parity is the sender's, not the arbiter's decision). */
#define CACHE_STICKY                                                           \
  (((uint64_t)1 << CACHE_RO_PAR_SHIFT) | ((uint64_t)1 << CACHE_STUB_SHIFT))
/* Assert: applying `op` to CACHE_MAKE(in_rws,in_ros,in_wc,in_rc) yields
 * CACHE_MAKE(ex_rws,ex_ros,ex_wc,ex_rc) and action == ex_act. */
static void chk_cache(const char *name, uint32_t in_rws, uint32_t in_ros,
                      uint32_t in_wc, uint32_t in_rc, int op, uint32_t ex_rws,
                      uint32_t ex_ros, uint32_t ex_wc, uint32_t ex_rc,
                      uint32_t ex_act) {
  uint64_t cur = CACHE_MAKE(in_rws, in_ros, in_wc, in_rc);
  uint32_t act = 0xffffffff;
  uint64_t next = cache_compute_next(cur, op, &act);
  uint64_t want = CACHE_MAKE(ex_rws, ex_ros, ex_wc, ex_rc);
  g_checks++;
  if ((next & ~CACHE_STICKY) != want || act != ex_act) {
    (void)fprintf(stderr,
                  "FAIL [%s]: in(%u,%u,%u,%u) op=%d => out(%u,%u,%u,%u "
                  "act=%u) expected(%u,%u,%u,%u act=%u)\n",
                  name, in_rws, in_ros, in_wc, in_rc, op, CACHE_RW_ST(next),
                  CACHE_RO_ST(next), CACHE_RW_CNT(next), CACHE_RO_CNT(next),
                  act, ex_rws, ex_ros, ex_wc, ex_rc, ex_act);
    g_rc = 1;
  }
}

/* One step of a SEQUENCE: apply `op` to the running word, check the result and
 * the action, then carry the word the arbiter actually produced into the next
 * step.  A single-round table cannot reach a state that takes several
 * transitions to build, which is where a reusable axis and an exactly-once
 * drain are decided. */
static void step_cache(const char *name, uint64_t *cur, int op, uint32_t ex_rws,
                       uint32_t ex_ros, uint32_t ex_wc, uint32_t ex_rc,
                       uint32_t ex_act) {
  uint32_t act = 0xffffffff;
  uint64_t next = cache_compute_next(*cur, op, &act);
  uint64_t want = CACHE_MAKE(ex_rws, ex_ros, ex_wc, ex_rc);
  g_checks++;
  if ((next & ~CACHE_STICKY) != want || act != ex_act) {
    (void)fprintf(
        stderr,
        "FAIL [%s]: in(%u,%u,%u,%u) op=%d => out(%u,%u,%u,%u "
        "act=%u) expected(%u,%u,%u,%u act=%u)\n",
        name, CACHE_RW_ST(*cur), CACHE_RO_ST(*cur), CACHE_RW_CNT(*cur),
        CACHE_RO_CNT(*cur), op, CACHE_RW_ST(next), CACHE_RO_ST(next),
        CACHE_RW_CNT(next), CACHE_RO_CNT(next), act, ex_rws, ex_ros, ex_wc,
        ex_rc, ex_act);
    g_rc = 1;
  }
  *cur = next;
}
#endif

int main(void) {
  const uint32_t RW = EXCL_PHASE_BIT_RW; /* 0 */
  const uint32_t RO = EXCL_PHASE_BIT_RO; /* 1 */

  /* ===== RW_ACQ ===== */
  /* none -> rw : w0 r0 => w1 r0, grant ONE_RW, bit normalized 0 (r==0). */
  chk("rwacq none->rw", 0, 0, 0, T_RW_ACQ, 0, 1, 0, G_ONE_RW);
  /* RW arriving while RO held (w==0 && r>0): parks, bit->RO, no grant. */
  chk("rwacq w0 r1 ->park RO", 0, 0, 1, T_RW_ACQ, RO, 1, 1, G_NONE);
  chk("rwacq w0 r3 ->park RO", RO, 0, 3, T_RW_ACQ, RO, 1, 3, G_NONE);
  /* rw->rw (already a writer participant, w>0): bit UNCHANGED, no grant. */
  chk("rwacq w1 r0 rw->rw", 0, 1, 0, T_RW_ACQ, 0, 2, 0, G_NONE);
  /* w>0 with readers present: bit must stay whatever it was (not forced RO). */
  chk("rwacq w1 r2 bit RW unchanged", RW, 1, 2, T_RW_ACQ, RW, 2, 2, G_NONE);
  chk("rwacq w2 r2 bit RO unchanged", RO, 2, 2, T_RW_ACQ, RO, 3, 2, G_NONE);

  /* ===== RO_ACQ ===== */
  /* none -> ro : w0 r0 => grant ALL_RO; bit set RO but normalized to 0 since
   * w==0 after (one counter zero => bit 0). */
  chk("roacq none->ro", 0, 0, 0, T_RO_ACQ, 0, 0, 1, G_ALL_RO);
  /* ro -> ro : w0 r1 => grant ALL_RO, bit normalized 0 (w==0). */
  chk("roacq ro->ro", RO, 0, 1, T_RO_ACQ, 0, 0, 2, G_ALL_RO);
  /* D7: RW waiting (w>0) but RO phase held (bit RO, r>0): RO extends, grant. */
  chk("roacq D7 extend RO", RO, 1, 1, T_RO_ACQ, RO, 1, 2, G_ALL_RO);
  /* RW held (w>0, bit RW): RO parks, bit->RW, no grant. */
  chk("roacq w1 r0 ->park RW", RW, 1, 0, T_RO_ACQ, RW, 1, 1, G_NONE);
  chk("roacq w2 bit RW park", RW, 2, 0, T_RO_ACQ, RW, 2, 1, G_NONE);

  /* ===== RW_REL ===== */
  /* rw->none : w1 r0 => w0 r0, no grant, bit 0. */
  chk("rwrel rw->none", 0, 1, 0, T_RW_REL, 0, 0, 0, G_NONE);
  /* D6 rw->rw : w2 r0 => w1, grant ONE_RW. */
  chk("rwrel D6 rw->rw", RW, 2, 0, T_RW_REL, 0, 1, 0, G_ONE_RW);
  /* rw->rw with readers waiting still grants the next writer (w-1>0). */
  chk("rwrel D6 rw->rw r>0", RW, 3, 2, T_RW_REL, RW, 2, 2, G_ONE_RW);
  /* rw->ro : w1 r2 => w0, drain ALL_RO, bit->RO normalized 0 (w==0). */
  chk("rwrel rw->ro drain", RW, 1, 2, T_RW_REL, 0, 0, 2, G_ALL_RO);

  /* ===== RO_REL ===== */
  /* ro->none : w0 r1 => w0 r0, no grant. */
  chk("rorel ro->none", 0, 0, 1, T_RO_REL, 0, 0, 0, G_NONE);
  /* ro still held (r-1>0): no grant. */
  chk("rorel ro->ro", RO, 0, 3, T_RO_REL, 0, 0, 2, G_NONE);
  /* ro->rw : last reader leaves (r-1==0) while w>0 => grant ONE_RW, bit->RW
   * normalized 0 (r==0). */
  chk("rorel ro->rw", RO, 2, 1, T_RO_REL, 0, 2, 0, G_ONE_RW);
  /* r-1==0 but w==0 => nothing. */
  chk("rorel r0 w0 nothing", 0, 0, 1, T_RO_REL, 0, 0, 0, G_NONE);

  /* ===== state_bit normalization invariant ===== */
  /* Any transition leaving one counter at 0 must clear bit to 0. */
  chk("norm rwrel leaves w0 bit0", RO, 1, 0, T_RW_REL, 0, 0, 0, G_NONE);
  chk("norm rorel leaves r0 bit0", RO, 0, 1, T_RO_REL, 0, 0, 0, G_NONE);

  /* ===== the teardown mark ===== */
  {
    /* A destroy that finds releases owed marks the word; requests that land
     * before the zero edge are still counted and served; the release that
     * reaches the zero edge tears down, and the torn-down word refuses every
     * later acquire, a duplicate destroy included. */
    uint64_t w = LOCK_MAKE_STATE(RW, 1, 0);
    step_lock("td: a reader parks behind the writer", &w, T_RO_ACQ, 0, RW, 1, 1,
              G_NONE);
    step_lock("td: the destroy marks", &w, T_TEARDOWN, 1, RW, 1, 1, G_NONE);
    step_lock("td: a writer that lands now is counted", &w, T_RW_ACQ, 1, RW, 2,
              1, G_NONE);
    step_lock("td: the writer's release passes the turn", &w, T_RW_REL, 1, RW,
              1, 1, G_ONE_RW);
    step_lock("td: the last writer's release opens the read phase", &w,
              T_RW_REL, 1, 0, 0, 1, G_ALL_RO);
    step_lock("td: the last release tears down", &w, T_RO_REL, 1, 0, 0, 0,
              G_DESTROY);
    step_lock("td: a writer after the teardown is refused", &w, T_RW_ACQ, 1, 0,
              0, 0, G_NONE);
    step_lock("td: a reader after the teardown is refused", &w, T_RO_ACQ, 1, 0,
              0, 0, G_NONE);
    step_lock("td: a duplicate destroy changes nothing", &w, T_TEARDOWN, 1, 0,
              0, 0, G_NONE);
  }
  {
    /* A destroy that finds nothing owed tears down at once. */
    uint64_t w = LOCK_MAKE_STATE(0, 0, 0);
    step_lock("td idle: the destroy tears down", &w, T_TEARDOWN, 1, 0, 0, 0,
              G_DESTROY);
    step_lock("td idle: a writer after it is refused", &w, T_RW_ACQ, 1, 0, 0, 0,
              G_NONE);
  }
  {
    /* A claimed first touch: the announce seeded the creator's hold (w 1)
     * and its pre-claim write and read requests (rwo, roo; owed 2). */
    uint64_t w = LOCK_MAKE_STATE_FULL(0, 0, 1, 1, 2, 1, 0);
    step_full("owed: the creator's release", &w, T_RW_REL,
              LOCK_MAKE_STATE_FULL(0, 0, 1, 1, 2, 0, 0), G_NONE);
    step_full("owed: the destroy only marks", &w, T_TEARDOWN,
              LOCK_MAKE_STATE_FULL(1, 0, 1, 1, 2, 0, 0), G_NONE);
    step_full("owed: the pre-claim write request is discarded", &w,
              EXCL_OP_RWO_ARRIVE, LOCK_MAKE_STATE_FULL(1, 0, 0, 1, 1, 0, 0),
              G_NONE);
    step_full("owed: the pre-claim read request tears down", &w,
              EXCL_OP_ROO_ARRIVE, LOCK_MAKE_STATE_FULL(1, 0, 0, 0, 0, 0, 0),
              G_DESTROY);
    step_full("owed: a reader after the teardown is refused", &w, T_RO_ACQ,
              LOCK_MAKE_STATE_FULL(1, 0, 0, 0, 0, 0, 0), G_NONE);
  }
  {
    /* A write request declares the rank's read request; its arrival settles
     * the count (the caller dismisses it), before or after the destroy. */
    uint64_t w = LOCK_MAKE_STATE_FULL(0, 0, 0, 0, 0, 0, 0);
    step_full("decl: owed", &w, EXCL_OP_OWE,
              LOCK_MAKE_STATE_FULL(0, 0, 0, 0, 1, 0, 0), G_NONE);
    step_full("decl: arrives before the destroy, processed", &w,
              EXCL_OP_ARRIVE, LOCK_MAKE_STATE_FULL(0, 0, 0, 0, 0, 0, 0), G_NONE);
    step_full("decl: owed again", &w, EXCL_OP_OWE,
              LOCK_MAKE_STATE_FULL(0, 0, 0, 0, 1, 0, 0), G_NONE);
    step_full("decl: a reader counted", &w, T_RO_ACQ,
              LOCK_MAKE_STATE_FULL(0, 0, 0, 0, 1, 0, 1), G_ALL_RO);
    step_full("decl: its release", &w, T_RO_REL,
              LOCK_MAKE_STATE_FULL(0, 0, 0, 0, 1, 0, 0), G_NONE);
    step_full("decl: the destroy waits for the owed request", &w, T_TEARDOWN,
              LOCK_MAKE_STATE_FULL(1, 0, 0, 0, 1, 0, 0), G_NONE);
    step_full("decl: its arrival tears down", &w, EXCL_OP_ARRIVE,
              LOCK_MAKE_STATE_FULL(1, 0, 0, 0, 0, 0, 0), G_DESTROY);
  }
  {
    /* A declaration that would overflow the owed field is refused, the word
     * unchanged. */
    uint64_t w = LOCK_MAKE_STATE_FULL(0, 0, 0, 0, 0xffffu, 0, 0);
    step_full("decl: a full owed field is refused", &w, EXCL_OP_OWE,
              LOCK_MAKE_STATE_FULL(0, 0, 0, 0, 0xffffu, 0, 0),
              LOCK_GRANT_FULL);
  }
  {
    /* An owed arrival under the mark with releases still owed settles the
     * count; the last release tears down. */
    uint64_t w = LOCK_MAKE_STATE_FULL(0, 0, 0, 0, 1, 1, 0);
    step_full("mark: destroy with a writer and one owed", &w, T_TEARDOWN,
              LOCK_MAKE_STATE_FULL(1, 0, 0, 0, 1, 1, 0), G_NONE);
    step_full("mark: the owed arrival is absorbed", &w, EXCL_OP_ARRIVE,
              LOCK_MAKE_STATE_FULL(1, 0, 0, 0, 0, 1, 0), G_NONE);
    step_full("mark: the last release tears down", &w, T_RW_REL,
              LOCK_MAKE_STATE_FULL(1, 0, 0, 0, 0, 0, 0), G_DESTROY);
  }

#ifdef ARTS_USE_CXL
  /* ===== cache-word grant (PURGE) =====
   * One grant answers one request: only an axis at REQ with its opener
   * counted takes it, and anything else is a grant for no request (LATE, the
   * word unchanged, raised by the commit site).  A read grant that finds its
   * readers already served by a write turn goes straight back. */
  const uint32_t I = CACHE_ST_IDLE, Q = CACHE_ST_REQ, G = CACHE_ST_GRANT;
  chk_cache("rwgrant req wc1", Q, I, 1, 0, CACHE_OP_GRANT_RW, G, I, 1, 0,
            CACHE_ACT_DRAIN_BOTH);
  chk_cache("rwgrant req wc1 with the read request out", Q, Q, 1, 2,
            CACHE_OP_GRANT_RW, G, Q, 1, 2, CACHE_ACT_DRAIN_BOTH);
  chk_cache("rwgrant with no writer counted is late", Q, I, 0, 2,
            CACHE_OP_GRANT_RW, Q, I, 0, 2, CACHE_ACT_LATE);
  chk_cache("rwgrant at idle is late", I, I, 0, 0, CACHE_OP_GRANT_RW, I, I, 0,
            0, CACHE_ACT_LATE);
  chk_cache("rwgrant at a live hold is late", G, I, 1, 0, CACHE_OP_GRANT_RW, G,
            I, 1, 0, CACHE_ACT_LATE);
  chk_cache("rogrant req rc2", I, Q, 0, 2, CACHE_OP_GRANT_RO, I, G, 0, 2,
            CACHE_ACT_DRAIN_RO);
  chk_cache("rogrant req under a pending rw request", Q, Q, 1, 2,
            CACHE_OP_GRANT_RO, Q, G, 1, 2, CACHE_ACT_DRAIN_RO);
  chk_cache("rogrant with nobody parked is late", I, Q, 0, 0,
            CACHE_OP_GRANT_RO, I, Q, 0, 0, CACHE_ACT_LATE);
  chk_cache("rogrant at idle is late", I, I, 0, 0, CACHE_OP_GRANT_RO, I, I, 0,
            0, CACHE_ACT_LATE);
  chk_cache("rogrant under a held write grant is late", G, Q, 1, 1,
            CACHE_OP_GRANT_RO, G, Q, 1, 1, CACHE_ACT_LATE);
  chk_cache("cts re-sends for waiting readers", I, Q, 0, 1, CACHE_OP_CTS_RO, I,
            Q, 0, 1, CACHE_ACT_RESEND_RO);
  chk_cache("cts closes a covered axis", I, Q, 0, 0, CACHE_OP_CTS_RO, I, I, 0,
            0, CACHE_ACT_NONE);
  chk_cache("cts closes under a write request", Q, Q, 1, 1, CACHE_OP_CTS_RO, Q,
            I, 1, 1, CACHE_ACT_NONE);
  chk_cache("cts with no read request out is late", I, I, 0, 0,
            CACHE_OP_CTS_RO, I, I, 0, 0, CACHE_ACT_LATE);
  chk_cache("the create's hold closes both requests", Q, Q, 0, 0,
            CACHE_OP_CREATE_HOLD, G, I, 1, 0, CACHE_ACT_DRAIN_BOTH);
  chk_cache("the create's hold on a full count is late", Q, I,
            (uint32_t)CACHE_RW_CNT_MASK, 0, CACHE_OP_CREATE_HOLD, Q, I,
            (uint32_t)CACHE_RW_CNT_MASK, 0, CACHE_ACT_LATE);

  {
    uint64_t w = CACHE_MAKE(I, I, 0, 0);
    step_cache("rw round: first writer requests", &w, CACHE_OP_ACQ_RW, Q, I, 1,
               0, CACHE_ACT_SEND_RW);
    step_cache("rw round: second writer parks", &w, CACHE_OP_ACQ_RW, Q, I, 2, 0,
               CACHE_ACT_PARK);
    step_cache("rw round: a reader rides the write request", &w,
               CACHE_OP_ACQ_RO, Q, I, 2, 1, CACHE_ACT_PARK);
    step_cache("rw round: the grant drains both", &w, CACHE_OP_GRANT_RW, G, I,
               2, 1, CACHE_ACT_DRAIN_BOTH);
    step_cache("rw round: the reader releases", &w, CACHE_OP_REL_RO, G, I, 2, 0,
               CACHE_ACT_NONE);
    step_cache("rw round: a writer releases", &w, CACHE_OP_REL_RW, G, I, 1, 0,
               CACHE_ACT_NONE);
    step_cache("rw round: the zero edge", &w, CACHE_OP_REL_RW, I, I, 0, 0,
               CACHE_ACT_REL_RW);
  }
  {
    uint64_t w = CACHE_MAKE(I, I, 0, 0);
    /* The write request leaves with the read request out; the write grant
     * lands first and serves the reader; the home answers the read request
     * with a dismiss, which closes the covered axis. */
    step_cache("cover: a reader requests", &w, CACHE_OP_ACQ_RO, I, Q, 0, 1,
               CACHE_ACT_SEND_RO);
    if (CACHE_RO_PAR(w) != 1u) {
      (void)fprintf(stderr, "FAIL [cover: the request flips the parity]\n");
      g_rc = 1;
    }
    step_cache("cover: a writer requests at once", &w, CACHE_OP_ACQ_RW, Q, Q, 1,
               1, CACHE_ACT_SEND_RW);
    step_cache("cover: the write grant serves writer and reader", &w,
               CACHE_OP_GRANT_RW, G, Q, 1, 1, CACHE_ACT_DRAIN_BOTH);
    step_cache("cover: the reader releases", &w, CACHE_OP_REL_RO, G, Q, 1, 0,
               CACHE_ACT_NONE);
    step_cache("cover: the zero edge", &w, CACHE_OP_REL_RW, I, Q, 0, 0,
               CACHE_ACT_REL_RW);
    step_cache("cover: the dismiss closes the axis", &w, CACHE_OP_CTS_RO, I, I,
               0, 0, CACHE_ACT_NONE);
  }
  {
    /* Every reply clears the stub bit: the home is installed and the claim is
     * elsewhere.  A write size round clears it only for a write request out. */
    const int ops[4] = {CACHE_OP_GRANT_RW, CACHE_OP_GRANT_RO, CACHE_OP_CTS_RO,
                        CACHE_OP_DECLAIM};
    const uint64_t ins[4] = {CACHE_MAKE_X(Q, I, 0u, 1u, 1, 0),
                             CACHE_MAKE_X(I, Q, 0u, 1u, 0, 1),
                             CACHE_MAKE_X(I, Q, 0u, 1u, 0, 0),
                             CACHE_MAKE_X(Q, I, 0u, 1u, 1, 0)};
    for (int i = 0; i < 4; i++) {
      uint32_t act = 0xffffffff;
      uint64_t next = cache_compute_next(ins[i], ops[i], &act);
      g_checks++;
      if (act == CACHE_ACT_LATE || CACHE_STUB(next) != 0u) {
        (void)fprintf(stderr, "FAIL [reply %d clears stub]: act=%u stub=%u\n",
                      i, act, CACHE_STUB(next));
        g_rc = 1;
      }
    }
    uint32_t act = 0xffffffff;
    (void)cache_compute_next(CACHE_MAKE_X(I, I, 0u, 1u, 0, 0),
                             CACHE_OP_DECLAIM, &act);
    g_checks++;
    if (act != CACHE_ACT_LATE) {
      (void)fprintf(stderr, "FAIL [declaim with no write request out]\n");
      g_rc = 1;
    }
  }
  {
    uint64_t w = CACHE_MAKE(I, I, 0, 0);
    step_cache("hold round: a writer requests", &w, CACHE_OP_ACQ_RW, Q, I, 1, 0,
               CACHE_ACT_SEND_RW);
    step_cache("hold round: the create takes the hold", &w,
               CACHE_OP_CREATE_HOLD, G, I, 2, 0, CACHE_ACT_DRAIN_BOTH);
    step_cache("hold round: a grant under the hold is late", &w,
               CACHE_OP_GRANT_RW, G, I, 2, 0, CACHE_ACT_LATE);
    step_cache("hold round: a holder releases", &w, CACHE_OP_REL_RW, G, I, 1, 0,
               CACHE_ACT_NONE);
    step_cache("hold round: the zero edge", &w, CACHE_OP_REL_RW, I, I, 0, 0,
               CACHE_ACT_REL_RW);
    step_cache("hold round: a hold is refused under a grant", &w,
               CACHE_OP_ACQ_RW, Q, I, 1, 0, CACHE_ACT_SEND_RW);
    step_cache("hold round: the next grant", &w, CACHE_OP_GRANT_RW, G, I, 1, 0,
               CACHE_ACT_DRAIN_BOTH);
    step_cache("hold round: refused", &w, CACHE_OP_CREATE_HOLD, G, I, 1, 0,
               CACHE_ACT_NONE);
  }
#endif

  if (g_rc != 0) {
    return 1;
  }
  printf("PASS excl_compute_next: %d truth-table transitions "
         "(RW/RO ACQ/REL, D6 rw->rw chain, D7 new-RO-while-RW-waiting, "
         "state_bit normalization, the teardown mark and the torn-down word"
#ifdef ARTS_USE_CXL
         ", cache-word grant: only a REQ axis takes it, committed hand-backs "
         "that clear the answered REQ, multi-round sequences"
#endif
         ") all correct\n",
         g_checks);
  return 0;
}
#endif /* ARTS_RELEASE_PURGE */

#ifdef ARTS_RELEASE_RETAIN
/* RETAIN's cache arbiter cache_owner_compute_next, its parked-population
 * reading, and the home arbiter lock_owner_compute_next. */
uint64_t cache_owner_compute_next(uint64_t cur, int op, uint32_t *out_action);
uint64_t lock_owner_compute_next(uint64_t cur, int op, unsigned int new_owner,
                                 uint32_t *out_action);
static inline uint32_t cache_owner_parked_rw(uint64_t s);
static inline uint32_t cache_owner_parked_ro(uint64_t s);

static int g_rc = 0;
static int g_checks = 0;

#define NT ARTS_EXCL_NO_TARGET
#define W(own, rws, ros, mt, wc, rc) CACHE_MAKE_FULL(own, rws, ros, mt, wc, rc)

/* One step of a cache-word sequence: apply `op`, check the whole word (the
 * sticky and spare bits included) and the action, carry the word on. */
static void step_owner(const char *name, uint64_t *cur, int op, uint64_t want,
                       uint32_t ex_act) {
  uint32_t act = 0xffffffff;
  uint64_t next = cache_owner_compute_next(*cur, op, &act);
  g_checks++;
  if (next != want || act != ex_act) {
    (void)fprintf(stderr,
                  "FAIL [%s]: in %llx op=%d => out %llx act=%u, expected %llx "
                  "act=%u\n",
                  name, (unsigned long long)*cur, op, (unsigned long long)next,
                  act, (unsigned long long)want, ex_act);
    g_rc = 1;
  }
  *cur = next;
}

static void chk_owner(const char *name, uint64_t cur, int op, uint64_t want,
                      uint32_t ex_act) {
  step_owner(name, &cur, op, want, ex_act);
}

static void chk_parked(const char *name, uint64_t s, uint32_t ex_rw,
                       uint32_t ex_ro) {
  g_checks++;
  if (cache_owner_parked_rw(s) != ex_rw || cache_owner_parked_ro(s) != ex_ro) {
    (void)fprintf(stderr,
                  "FAIL [%s]: parked rw=%u ro=%u, expected rw=%u ro=%u\n", name,
                  cache_owner_parked_rw(s), cache_owner_parked_ro(s), ex_rw,
                  ex_ro);
    g_rc = 1;
  }
}

static void chk_lock(const char *name, uint64_t cur, int op,
                     unsigned int new_owner, uint64_t want, uint32_t ex_act) {
  uint32_t act = 0xffffffff;
  uint64_t next = lock_owner_compute_next(cur, op, new_owner, &act);
  g_checks++;
  if (next != want || act != ex_act) {
    (void)fprintf(stderr,
                  "FAIL [%s]: in %llx op=%d => out %llx act=%u, expected %llx "
                  "act=%u\n",
                  name, (unsigned long long)cur, op, (unsigned long long)next,
                  act, (unsigned long long)want, ex_act);
    g_rc = 1;
  }
}

int main(void) {
  const uint32_t I = CACHE_ST_IDLE, Q = CACHE_ST_REQ, G = CACHE_ST_GRANT,
                 GP = CACHE_ST_GRANT_PURGE;
  const uint64_t PAR = CACHE_OWNER_RO_PAR_BIT, STUB = CACHE_OWNER_STUB_BIT,
                 LEASED = CACHE_OWNER_LEASED_MASK;
  const uint32_t D = 5u; /* a migrate target */

  /* ===== ACQ_RW: the count and the decision in one transition ===== */
  chk_owner("rwacq joins the held grant", W(1, G, I, NT, 0, 0), CACHE_OP_ACQ_RW,
            W(1, G, I, NT, 1, 0), CACHE_ACT_SELF_SERVE);
  chk_owner("rwacq parks on the request out", W(0, Q, I, NT, 1, 0),
            CACHE_OP_ACQ_RW, W(0, Q, I, NT, 2, 0), CACHE_ACT_PARK);
  chk_owner("rwacq: a resident rank without the permission requests",
            W(1, I, I, NT, 0, 0), CACHE_OP_ACQ_RW, W(1, Q, I, NT, 1, 0),
            CACHE_ACT_SEND_RW);
  chk_owner("rwacq: a non-owner requests", W(0, I, Q, NT, 0, 1) | PAR,
            CACHE_OP_ACQ_RW, W(0, Q, Q, NT, 1, 1) | PAR, CACHE_ACT_SEND_RW);
  chk_owner("rwacq: a full count is refused",
            W(1, G, I, NT, CACHE_OWNER_WC_MASK, 0), CACHE_OP_ACQ_RW,
            W(1, G, I, NT, CACHE_OWNER_WC_MASK, 0), CACHE_ACT_LATE);

  /* ===== ACQ_RO ===== */
  chk_owner("roacq: the resident copy serves", W(1, I, I, NT, 0, 0),
            CACHE_OP_ACQ_RO, W(1, I, I, NT, 0, 1), CACHE_ACT_SELF_SERVE);
  chk_owner("roacq: resident with a write request out, ro_st untouched",
            W(1, Q, I, NT, 1, 0), CACHE_OP_ACQ_RO, W(1, Q, I, NT, 1, 1),
            CACHE_ACT_SELF_SERVE);
  chk_owner("roacq: the write grant serves", W(1, G, I, NT, 1, 0),
            CACHE_OP_ACQ_RO, W(1, G, I, NT, 1, 1), CACHE_ACT_SELF_SERVE);
  chk_owner("roacq: a retained read grant serves", W(0, I, G, NT, 0, 0) | LEASED,
            CACHE_OP_ACQ_RO, W(0, I, G, NT, 0, 1) | LEASED,
            CACHE_ACT_SELF_SERVE);
  chk_owner("roacq: a grant marked for return serves",
            W(0, I, GP, NT, 0, 1) | LEASED, CACHE_OP_ACQ_RO,
            W(0, I, GP, NT, 0, 2) | LEASED, CACHE_ACT_SELF_SERVE);
  chk_owner("roacq: a non-owner opens a read request, parity flipped",
            W(0, I, I, NT, 0, 0), CACHE_OP_ACQ_RO, W(0, I, Q, NT, 0, 1) | PAR,
            CACHE_ACT_SEND_RO);
  chk_owner("roacq rides the write request out", W(0, Q, I, NT, 1, 0),
            CACHE_OP_ACQ_RO, W(0, Q, I, NT, 1, 1), CACHE_ACT_PARK);
  chk_owner("roacq coalesces on the read request out", W(0, I, Q, NT, 0, 1) | PAR,
            CACHE_OP_ACQ_RO, W(0, I, Q, NT, 0, 2) | PAR, CACHE_ACT_PARK);
  chk_owner("roacq: a full count is refused",
            W(1, I, I, NT, 0, CACHE_OWNER_RC_MASK), CACHE_OP_ACQ_RO,
            W(1, I, I, NT, 0, CACHE_OWNER_RC_MASK), CACHE_ACT_LATE);

  /* ===== the parked populations a serving transition owes ===== */
  chk_parked("parked: both requests out", W(0, Q, Q, NT, 2, 3), 2, 3);
  chk_parked("parked: readers ride the write request", W(0, Q, I, NT, 1, 2), 1,
             2);
  chk_parked("parked: resident, writers wait for the self-grant",
             W(1, Q, I, NT, 2, 1), 2, 0);
  chk_parked("parked: under the write grant every count is live",
             W(1, G, I, NT, 2, 1), 0, 0);
  chk_parked("parked: under a read grant the readers are live",
             W(0, I, G, NT, 0, 2) | LEASED, 0, 0);
  chk_parked("parked: under a marked read grant too",
             W(0, Q, GP, NT, 1, 2) | LEASED, 1, 0);
  chk_parked("parked: idle", W(0, I, I, NT, 0, 0), 0, 0);

  /* ===== a sticky owner's writers race a migration ===== */
  {
    /* Two writers join the retained grant, a migration is forwarded in, the
     * last release ships the bytes; a writer arriving after that opens a
     * request in the same transition that counts it, so the request always
     * has that writer waiting. */
    uint64_t w = W(1, G, I, NT, 0, 0);
    step_owner("sticky: X joins", &w, CACHE_OP_ACQ_RW, W(1, G, I, NT, 1, 0),
               CACHE_ACT_SELF_SERVE);
    step_owner("sticky: Y joins", &w, CACHE_OP_ACQ_RW, W(1, G, I, NT, 2, 0),
               CACHE_ACT_SELF_SERVE);
    w = W(1, G, I, D, 2, 0); /* the FORWARD installs the target */
    step_owner("sticky: X releases", &w, CACHE_OP_REL_RW, W(1, G, I, D, 1, 0),
               CACHE_ACT_NONE);
    step_owner("sticky: Y's release ships the bytes", &w, CACHE_OP_REL_RW,
               W(0, I, I, NT, 0, 0), CACHE_ACT_MIGRATE);
    step_owner("sticky: Z requests, counted with its request", &w,
               CACHE_OP_ACQ_RW, W(0, Q, I, NT, 1, 0), CACHE_ACT_SEND_RW);
    chk_parked("sticky: the migration back owes Z", w, 1, 0);
  }
  {
    /* Readers live under the write grant outlast the last writer: the grant
     * and the migration wait for the read count's zero edge. */
    uint64_t w = W(1, G, I, D, 1, 1);
    step_owner("rw-ro: the last writer leaves, a reader holds", &w,
               CACHE_OP_REL_RW, W(1, G, I, D, 0, 1), CACHE_ACT_NONE);
    step_owner("rw-ro: the reader's zero edge", &w, CACHE_OP_REL_RO,
               W(1, G, I, D, 0, 0), CACHE_ACT_REL_RO);
  }
  chk_owner("sticky: with no target the last release keeps the grant",
            W(1, G, I, NT, 1, 0), CACHE_OP_REL_RW, W(1, G, I, NT, 0, 0),
            CACHE_ACT_NONE);

  /* ===== the create's hold, the read reply, the read release ===== */
  {
    uint64_t w = W(0, Q, Q, NT, 1, 1) | PAR | STUB;
    chk_parked("hold: the claimed first touch's waiters", w, 1, 1);
    step_owner("hold: closes both axes, clears stub", &w, CACHE_OP_CREATE_HOLD,
               W(1, G, I, NT, 2, 1) | PAR, CACHE_ACT_DRAIN_BOTH);
    step_owner("hold: refused on a resident block", &w, CACHE_OP_CREATE_HOLD,
               W(1, G, I, NT, 2, 1) | PAR, CACHE_ACT_NONE);
  }
  chk_owner("hold: a full count is refused",
            W(0, Q, I, NT, CACHE_OWNER_WC_MASK, 0) | STUB, CACHE_OP_CREATE_HOLD,
            W(0, Q, I, NT, CACHE_OWNER_WC_MASK, 0) | STUB, CACHE_ACT_LATE);
  chk_owner("cts re-sends for waiting readers, parity flipped",
            W(0, I, Q, NT, 0, 1) | PAR | STUB, CACHE_OP_CTS_RO,
            W(0, I, Q, NT, 0, 1), CACHE_ACT_RESEND_RO);
  chk_owner("cts closes under a write request", W(0, Q, Q, NT, 1, 1) | PAR,
            CACHE_OP_CTS_RO, W(0, Q, I, NT, 1, 1) | PAR, CACHE_ACT_NONE);
  chk_owner("cts closes on a resident rank", W(1, G, Q, NT, 1, 1) | PAR,
            CACHE_OP_CTS_RO, W(1, G, I, NT, 1, 1) | PAR, CACHE_ACT_NONE);
  chk_owner("cts with no read request out is late", W(0, I, I, NT, 0, 0),
            CACHE_OP_CTS_RO, W(0, I, I, NT, 0, 0), CACHE_ACT_LATE);
  chk_owner("rorel: a marked grant returns at its zero edge",
            W(0, I, GP, NT, 0, 1) | LEASED, CACHE_OP_REL_RO,
            W(0, I, I, NT, 0, 0), CACHE_ACT_REL_RO);
  chk_owner("rorel: an unmarked grant is retained",
            W(0, I, G, NT, 0, 1) | LEASED, CACHE_OP_REL_RO,
            W(0, I, G, NT, 0, 0) | LEASED, CACHE_ACT_REL_RO);

  /* ===== the home directory ===== */
  chk_lock("home: a writer at idle migrates", LOCK_MAKE(EXCL_PHASE_IDLE, 0, 0, 0),
           EXCL_OP_RW_ACQ, 0, LOCK_MAKE(EXCL_PHASE_RW, 0, 1, 0),
           EXCL_ACTION_FORWARD_MIGRATE);
  chk_lock("home: a reader at idle is served", LOCK_MAKE(EXCL_PHASE_IDLE, 0, 0, 0),
           EXCL_OP_RO_ACQ, 0, LOCK_MAKE(EXCL_PHASE_RO, 0, 0, 1),
           EXCL_ACTION_FORWARD_SERVE_ONE);
  chk_lock("home: a reader in the write phase is held",
           LOCK_MAKE(EXCL_PHASE_RW, 0, 1, 0), EXCL_OP_RO_ACQ, 0,
           LOCK_MAKE(EXCL_PHASE_RW, 0, 1, 1), EXCL_ACTION_NONE);
  chk_lock("home: the confirm flips to the read phase",
           LOCK_MAKE(EXCL_PHASE_RW, 0, 1, 1), EXCL_OP_CONFIRM, 3,
           LOCK_MAKE(EXCL_PHASE_RO, 3, 0, 1), EXCL_ACTION_FORWARD_SERVE_ALL);
  chk_lock("home: the confirm chains the next writer",
           LOCK_MAKE(EXCL_PHASE_RW, 0, 2, 0), EXCL_OP_CONFIRM, 3,
           LOCK_MAKE(EXCL_PHASE_RW, 3, 1, 0), EXCL_ACTION_FORWARD_MIGRATE);
  chk_lock("home: the last read return flips to the write phase",
           LOCK_MAKE(EXCL_PHASE_RO, 3, 1, 1), EXCL_OP_RO_RET, 0,
           LOCK_MAKE(EXCL_PHASE_RW, 3, 1, 0), EXCL_ACTION_FORWARD_MIGRATE);
  chk_lock("home: the last read return idles", LOCK_MAKE(EXCL_PHASE_RO, 3, 0, 1),
           EXCL_OP_RO_RET, 0, LOCK_MAKE(EXCL_PHASE_IDLE, 3, 0, 0),
           EXCL_ACTION_NONE);

  if (g_rc != 0) {
    return 1;
  }
  printf("PASS excl_compute_next: %d transitions (RETAIN cache word: the "
         "acquire's count and decision in one transition, the parked "
         "populations, the sticky owner's migration, the create's hold, read "
         "replies and returns; the home directory) all correct\n",
         g_checks);
  return 0;
}
#endif /* ARTS_RELEASE_RETAIN */

#ifdef ARTS_UNIT_STANDALONE_SHIMS
void *arts_calloc(size_t nmemb, size_t size) { return calloc(nmemb, size); }
void arts_free(void *ptr) { free(ptr); }
void *arts_malloc(size_t size) { return malloc(size); }

/* The standalone build links no runtime: every runtime symbol an included
 * coherence translation unit may reference must resolve here. */
#include "arts/coherence/buffer.h"
#include "arts/utils/shared.h"
unsigned int arts_global_rank_id = 0;
void arts_transport_send_async(int rank, char *message, unsigned int length) {
  (void)rank;
  (void)message;
  (void)length;
}
arts_shared_ptr_t arts_db_buf_acquire(struct arts_db_cache_s *cache) {
  (void)cache;
  return NULL;
}
void arts_db_buf_release(arts_shared_ptr_t *h) { (void)h; }
struct arts_db_buffer_s *arts_db_buf_install(struct arts_db_cache_s *cache,
                                             uint64_t new_version,
                                             const void *data_payload,
                                             uint64_t db_size) {
  (void)cache;
  (void)new_version;
  (void)data_payload;
  (void)db_size;
  return NULL;
}
void arts_db_buf_write_inplace(struct arts_db_cache_s *cache, const void *data,
                               uint64_t db_size) {
  (void)cache;
  (void)data;
  (void)db_size;
}
void *arts_shared_get(arts_shared_ptr_t p) {
  (void)p;
  return NULL;
}
void arts_send_db_cache_destroy(unsigned int sharer_rank, arts_guid_t db_guid) {
  (void)sharer_rank;
  (void)db_guid;
}
#endif

#include "core/coherence/excl/arbiters.c"
