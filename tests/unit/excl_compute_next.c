/* SPDX-License-Identifier: Apache-2.0
 *
 * T067 — EXCL excl_compute_next (coherence/excl/arbiters.c), the PURE
 *        lock-state transition function over [state_bit | w:31 | r:31].
 *
 * excl_compute_next(cur, op, &grant) is a deterministic pure function: given
 * the current packed lock_state and an op (RW/RO acquire/release), it returns
 * the next packed state and sets *grant to NONE / ONE_RW / ALL_RO.  This test
 * pins its FULL truth table with explicit, hand-derived expected values — no
 * oracle that merely re-implements the function — covering:
 *   - none→rw / none→ro grants
 *   - RW arriving while RO held (w==0 && r>0): parks, bit→RO, no grant
 *   - rw→rw (w>0 acquire): bit UNCHANGED, no grant (held writer serves it)
 *   - D7: new RO grant while RW waiting (RO phase extends)
 *   - RO arriving while RW held: parks, bit→RW
 *   - D6: rw→rw chain on release (w-1>0 grants next writer)
 *   - rw→ro on release (w-1==0 && r>0 drains all RO)
 *   - ro→rw on release (r-1==0 && w>0 grants one writer)
 *   - state_bit normalization to 0 whenever a counter reaches 0.
 *
 * excl_compute_next is non-static (exposed for this test); the file is built
 * standalone by #including coherence/excl/arbiters.c, which also carries the
 * PURGE cache-word arbiter cache_compute_next this test pins alongside it.
 * EXCL-only; self-skips elsewhere.
 *
 * Build: -DARTS_PROTOCOL_EXCL=1 -DARTS_UNIT_STANDALONE_SHIMS.
 */

#include <stdio.h>

#if !defined(ARTS_PROTOCOL_EXCL) || !defined(ARTS_RELEASE_PURGE)
int main(void) {
  printf(
      "PASS excl_compute_next: skipped (HOME EXCL-only; the HOME lock_state "
      "machine [state_bit|w|r] exists only in the EXCL+HOME build)\n");
  return 0;
}
#else

#include "arts/coherence/excl/types.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* op + grant codes mirror the arbiter (kept in sync via the same header
 * defines for the packed-state macros). */
#define T_RW_ACQ 0
#define T_RO_ACQ 1
#define T_RW_REL 2
#define T_RO_REL 3
#define G_NONE 0
#define G_ONE_RW 1
#define G_ALL_RO 2

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

#ifdef ARTS_FAM
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
  if (next != want || act != ex_act) {
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
  if (next != want || act != ex_act) {
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

#ifdef ARTS_FAM
  /* ===== cache-word claim / commit (PURGE, fabric-memory arm) =====
   * A grant is CLAIMED into FETCH before the block's bytes are read in and
   * COMMITTED to GRANT after, so no turn is ever admitted over a copy in
   * flight.  Every case below is hand-derived from the word alone. */
  const uint32_t I = CACHE_ST_IDLE, Q = CACHE_ST_REQ, G = CACHE_ST_GRANT,
                 F = CACHE_ST_FETCH;
  /* The state value fits the field: four values, and the word round-trips
   * FETCH on either axis with the widest counts. */
  if (CACHE_ST_FETCH != 3u ||
      CACHE_RW_ST(CACHE_MAKE(F, I, 0x3fffffffu, 0x3fffffffu)) != F ||
      CACHE_RO_ST(CACHE_MAKE(I, F, 0x3fffffffu, 0x3fffffffu)) != F ||
      CACHE_RW_CNT(CACHE_MAKE(F, F, 0x3fffffffu, 0x3fffffffu)) != 0x3fffffffu) {
    (void)fprintf(stderr, "FAIL: CACHE_ST_FETCH does not fit the word\n");
    g_rc = 1;
  }
  /* RW claim: a cohort is claimed; every other answer is a COMMITTED
   * hand-back — the refused axis's own REQ is cleared, the other axis is
   * not, and no claim outcome is CACHE_ACT_NONE. */
  chk_cache("rwclaim req wc1", Q, I, 1, 0, CACHE_OP_GRANT_RW_CLAIM, F, I, 1, 0,
            CACHE_ACT_FETCH);
  chk_cache("rwclaim req readers only", Q, I, 0, 2, CACHE_OP_GRANT_RW_CLAIM, F,
            I, 0, 2, CACHE_ACT_FETCH);
  chk_cache("rwclaim empty cohort", Q, I, 0, 0, CACHE_OP_GRANT_RW_CLAIM, I, I,
            0, 0, CACHE_ACT_REL_RW_EMPTY);
  /* The late grant: a create hold served this request's cohort and its
   * holders have released, so the axis rests IDLE.  Legal, handed back,
   * never an error. */
  chk_cache("rwclaim at idle hands back", I, I, 0, 0, CACHE_OP_GRANT_RW_CLAIM,
            I, I, 0, 0, CACHE_ACT_REL_RW_EMPTY);
  /* A grant for a right already held or being fetched is a second answer to
   * one request: the word stands and the caller must report it. */
  chk_cache("rwclaim at a live hold is invalid", G, I, 1, 0,
            CACHE_OP_GRANT_RW_CLAIM, G, I, 1, 0, CACHE_ACT_INVALID);
  chk_cache("rwclaim at its own claim is invalid", F, I, 1, 0,
            CACHE_OP_GRANT_RW_CLAIM, F, I, 1, 0, CACHE_ACT_INVALID);
  chk_cache("rwclaim req refused by ro grant clears req", Q, G, 1, 1,
            CACHE_OP_GRANT_RW_CLAIM, I, G, 1, 1, CACHE_ACT_REL_RW_EMPTY);
  chk_cache("rwclaim req refused by ro fetch clears req", Q, F, 1, 1,
            CACHE_OP_GRANT_RW_CLAIM, I, F, 1, 1, CACHE_ACT_REL_RW_EMPTY);
  /* RO claim: the same shape, plus the phantom. */
  chk_cache("roclaim req rc2", I, Q, 0, 2, CACHE_OP_GRANT_RO_CLAIM, I, F, 0, 2,
            CACHE_ACT_FETCH);
  chk_cache("roclaim req under a pending rw request", Q, Q, 1, 2,
            CACHE_OP_GRANT_RO_CLAIM, Q, F, 1, 2, CACHE_ACT_FETCH);
  chk_cache("roclaim phantom", I, Q, 0, 0, CACHE_OP_GRANT_RO_CLAIM, I, I, 0, 0,
            CACHE_ACT_REL_RO);
  chk_cache("roclaim at idle hands back", I, I, 0, 0, CACHE_OP_GRANT_RO_CLAIM,
            I, I, 0, 0, CACHE_ACT_REL_RO);
  chk_cache("roclaim req refused by rw grant clears req", G, Q, 1, 1,
            CACHE_OP_GRANT_RO_CLAIM, G, I, 1, 1, CACHE_ACT_REL_RO);
  chk_cache("roclaim req refused by rw claim clears req", F, Q, 1, 1,
            CACHE_OP_GRANT_RO_CLAIM, F, I, 1, 1, CACHE_ACT_REL_RO);
  chk_cache("roclaim at a live read grant is invalid", I, G, 0, 1,
            CACHE_OP_GRANT_RO_CLAIM, I, G, 0, 1, CACHE_ACT_INVALID);
  chk_cache("roclaim at its own claim is invalid", I, F, 0, 2,
            CACHE_OP_GRANT_RO_CLAIM, I, F, 0, 2, CACHE_ACT_INVALID);
  /* Commit: the counts at the CAS are the population to drain. */
  chk_cache("commit rw", F, I, 2, 1, CACHE_OP_GRANT_COMMIT, G, I, 2, 1,
            CACHE_ACT_DRAIN_BOTH);
  chk_cache("commit rw no readers", F, I, 1, 0, CACHE_OP_GRANT_COMMIT, G, I, 1,
            0, CACHE_ACT_DRAIN_BOTH);
  chk_cache("commit rw with a pending ro request", F, Q, 1, 1,
            CACHE_OP_GRANT_COMMIT, G, Q, 1, 1, CACHE_ACT_DRAIN_BOTH);
  chk_cache("commit ro", I, F, 0, 3, CACHE_OP_GRANT_COMMIT, I, G, 0, 3,
            CACHE_ACT_DRAIN_RO);
  chk_cache("commit with no claim is invalid", I, I, 0, 0,
            CACHE_OP_GRANT_COMMIT, I, I, 0, 0, CACHE_ACT_INVALID);
  chk_cache("commit under a live grant is invalid", G, I, 1, 0,
            CACHE_OP_GRANT_COMMIT, G, I, 1, 0, CACHE_ACT_INVALID);
  /* A release cannot precede the commit that admitted its holder: nothing is
   * admitted while an axis fetches, and only the commit clears FETCH. */
  chk_cache("rel rw at a fetching axis is invalid", F, I, 1, 0,
            CACHE_OP_REL_RW, F, I, 1, 0, CACHE_ACT_INVALID);
  chk_cache("rel ro at a fetching axis is invalid", I, F, 0, 1,
            CACHE_OP_REL_RO, I, F, 0, 1, CACHE_ACT_INVALID);
  /* A reader that joined under a held write turn owes its release to that
   * turn, not to the read axis, so it decrements even while the read axis
   * fetches. */
  chk_cache("rel ro of a joiner under a write hold decrements", G, F, 1, 1,
            CACHE_OP_REL_RO, G, F, 1, 0, CACHE_ACT_NONE);
  /* An acquire parks at FETCH, exactly as at REQ — never self-serves. */
  chk_cache("acq rw at fetch parks", F, I, 1, 0, CACHE_OP_ACQ_RW, F, I, 2, 0,
            CACHE_ACT_PARK);
  chk_cache("acq ro under rw fetch parks", F, I, 1, 0, CACHE_OP_ACQ_RO, F, I, 1,
            1, CACHE_ACT_PARK);
  chk_cache("acq ro at ro fetch parks", I, F, 0, 1, CACHE_OP_ACQ_RO, I, F, 0, 2,
            CACHE_ACT_PARK);
  /* An RW acquire consults its OWN axis only: a read copy in flight no more
   * stops it opening a write request than an in-flight read request does. */
  chk_cache("acq rw under ro fetch opens its own request", I, F, 0, 1,
            CACHE_OP_ACQ_RW, Q, F, 1, 1, CACHE_ACT_SEND_RW);
  /* A create hold is refused at FETCH, exactly as at GRANT. */
  chk_cache("create hold refused at rw fetch", F, I, 1, 0, CACHE_OP_CREATE_HOLD,
            F, I, 1, 0, CACHE_ACT_NONE);
  chk_cache("create hold refused at ro fetch", I, F, 0, 1, CACHE_OP_CREATE_HOLD,
            I, F, 0, 1, CACHE_ACT_NONE);

  /* ===== sequences =====
   * Several rounds over ONE word, so the states each case starts from are
   * states the arbiter itself produced. */
  {
    uint64_t w = CACHE_MAKE(I, I, 0, 0);
    /* A write round: request, claim, arrivals park under the copy, commit
     * drains exactly them, and the zero edge ends the turn. */
    step_cache("rw round: first writer requests", &w, CACHE_OP_ACQ_RW, Q, I, 1,
               0, CACHE_ACT_SEND_RW);
    step_cache("rw round: second writer parks", &w, CACHE_OP_ACQ_RW, Q, I, 2, 0,
               CACHE_ACT_PARK);
    step_cache("rw round: the grant is claimed", &w, CACHE_OP_GRANT_RW_CLAIM, F,
               I, 2, 0, CACHE_ACT_FETCH);
    step_cache("rw round: a writer parks under the copy", &w, CACHE_OP_ACQ_RW,
               F, I, 3, 0, CACHE_ACT_PARK);
    step_cache("rw round: a reader parks under the copy", &w, CACHE_OP_ACQ_RO,
               F, I, 3, 1, CACHE_ACT_PARK);
    step_cache("rw round: the commit drains both", &w, CACHE_OP_GRANT_COMMIT, G,
               I, 3, 1, CACHE_ACT_DRAIN_BOTH);
    step_cache("rw round: an arrival under the grant self-serves", &w,
               CACHE_OP_ACQ_RW, G, I, 4, 1, CACHE_ACT_SELF_SERVE);
    step_cache("rw round: the reader releases", &w, CACHE_OP_REL_RO, G, I, 4, 0,
               CACHE_ACT_NONE);
    step_cache("rw round: a writer releases", &w, CACHE_OP_REL_RW, G, I, 3, 0,
               CACHE_ACT_NONE);
    step_cache("rw round: a writer releases", &w, CACHE_OP_REL_RW, G, I, 2, 0,
               CACHE_ACT_NONE);
    step_cache("rw round: a writer releases", &w, CACHE_OP_REL_RW, G, I, 1, 0,
               CACHE_ACT_NONE);
    step_cache("rw round: the zero edge", &w, CACHE_OP_REL_RW, I, I, 0, 0,
               CACHE_ACT_REL_RW);
    /* A second round on the same word: the axis is reusable, not one-shot. */
    step_cache("rw round 2: requests again", &w, CACHE_OP_ACQ_RW, Q, I, 1, 0,
               CACHE_ACT_SEND_RW);
    step_cache("rw round 2: claims again", &w, CACHE_OP_GRANT_RW_CLAIM, F, I, 1,
               0, CACHE_ACT_FETCH);
    step_cache("rw round 2: commits again", &w, CACHE_OP_GRANT_COMMIT, G, I, 1,
               0, CACHE_ACT_DRAIN_BOTH);
    step_cache("rw round 2: the zero edge", &w, CACHE_OP_REL_RW, I, I, 0, 0,
               CACHE_ACT_REL_RW);
  }
  {
    uint64_t w = CACHE_MAKE(I, I, 0, 0);
    /* A read round taken while this rank's own write request is outstanding:
     * the read turn does not answer it, and the write turn follows. */
    step_cache("ro round: a reader requests", &w, CACHE_OP_ACQ_RO, I, Q, 0, 1,
               CACHE_ACT_SEND_RO);
    step_cache("ro round: a writer requests", &w, CACHE_OP_ACQ_RW, Q, Q, 1, 1,
               CACHE_ACT_SEND_RW);
    step_cache("ro round: a second reader parks", &w, CACHE_OP_ACQ_RO, Q, Q, 1,
               2, CACHE_ACT_PARK);
    step_cache("ro round: the read grant is claimed", &w,
               CACHE_OP_GRANT_RO_CLAIM, Q, F, 1, 2, CACHE_ACT_FETCH);
    step_cache("ro round: a reader parks under the copy", &w, CACHE_OP_ACQ_RO,
               Q, F, 1, 3, CACHE_ACT_PARK);
    step_cache("ro round: the commit drains the readers", &w,
               CACHE_OP_GRANT_COMMIT, Q, G, 1, 3, CACHE_ACT_DRAIN_RO);
    step_cache("ro round: a reader releases", &w, CACHE_OP_REL_RO, Q, G, 1, 2,
               CACHE_ACT_NONE);
    step_cache("ro round: a reader releases", &w, CACHE_OP_REL_RO, Q, G, 1, 1,
               CACHE_ACT_NONE);
    step_cache("ro round: the read zero edge", &w, CACHE_OP_REL_RO, Q, I, 1, 0,
               CACHE_ACT_REL_RO);
    step_cache("ro round: the write grant is claimed next", &w,
               CACHE_OP_GRANT_RW_CLAIM, F, I, 1, 0, CACHE_ACT_FETCH);
    step_cache("ro round: the write commit", &w, CACHE_OP_GRANT_COMMIT, G, I, 1,
               0, CACHE_ACT_DRAIN_BOTH);
    step_cache("ro round: the write zero edge", &w, CACHE_OP_REL_RW, I, I, 0, 0,
               CACHE_ACT_REL_RW);
  }
  {
    uint64_t w = CACHE_MAKE(I, I, 0, 0);
    /* A read grant arriving during a write copy is refused AND its axis
     * cleared: the write turn serves those readers, and a later reader opens
     * its own request rather than waiting behind one nobody will answer. */
    step_cache("mixed: a reader requests", &w, CACHE_OP_ACQ_RO, I, Q, 0, 1,
               CACHE_ACT_SEND_RO);
    step_cache("mixed: a writer requests", &w, CACHE_OP_ACQ_RW, Q, Q, 1, 1,
               CACHE_ACT_SEND_RW);
    step_cache("mixed: the write grant is claimed first", &w,
               CACHE_OP_GRANT_RW_CLAIM, F, Q, 1, 1, CACHE_ACT_FETCH);
    step_cache("mixed: the read grant is refused and its axis cleared", &w,
               CACHE_OP_GRANT_RO_CLAIM, F, I, 1, 1, CACHE_ACT_REL_RO);
    step_cache("mixed: the commit serves writer and reader", &w,
               CACHE_OP_GRANT_COMMIT, G, I, 1, 1, CACHE_ACT_DRAIN_BOTH);
    step_cache("mixed: the reader releases", &w, CACHE_OP_REL_RO, G, I, 1, 0,
               CACHE_ACT_NONE);
    step_cache("mixed: the zero edge", &w, CACHE_OP_REL_RW, I, I, 0, 0,
               CACHE_ACT_REL_RW);
    step_cache("mixed: a later reader opens its own request", &w,
               CACHE_OP_ACQ_RO, I, Q, 0, 1, CACHE_ACT_SEND_RO);
    step_cache("mixed: its grant is claimed", &w, CACHE_OP_GRANT_RO_CLAIM, I, F,
               0, 1, CACHE_ACT_FETCH);
    step_cache("mixed: its commit drains the readers", &w,
               CACHE_OP_GRANT_COMMIT, I, G, 0, 1, CACHE_ACT_DRAIN_RO);
    step_cache("mixed: the read zero edge", &w, CACHE_OP_REL_RO, I, I, 0, 0,
               CACHE_ACT_REL_RO);
  }
  {
    uint64_t w = CACHE_MAKE(I, I, 0, 0);
    /* The create hold and the claim exclude each other through the word
     * alone, so a block's two drains can never run against the same counts. */
    step_cache("hold round: a writer requests", &w, CACHE_OP_ACQ_RW, Q, I, 1, 0,
               CACHE_ACT_SEND_RW);
    step_cache("hold round: the create takes the hold", &w,
               CACHE_OP_CREATE_HOLD, G, I, 2, 0, CACHE_ACT_DRAIN_BOTH);
    step_cache("hold round: a grant while the hold is live is invalid", &w,
               CACHE_OP_GRANT_RW_CLAIM, G, I, 2, 0, CACHE_ACT_INVALID);
    step_cache("hold round: a holder releases", &w, CACHE_OP_REL_RW, G, I, 1, 0,
               CACHE_ACT_NONE);
    step_cache("hold round: the zero edge", &w, CACHE_OP_REL_RW, I, I, 0, 0,
               CACHE_ACT_REL_RW);
    step_cache("hold round: a grant at the idle axis still goes back", &w,
               CACHE_OP_GRANT_RW_CLAIM, I, I, 0, 0, CACHE_ACT_REL_RW_EMPTY);
  }
  {
    uint64_t w = CACHE_MAKE(I, I, 0, 0);
    /* The mirror: a hold is refused while a claimed copy runs and while the
     * turn it commits is held, and is taken again once the turn has ended. */
    step_cache("exclusion: a writer requests", &w, CACHE_OP_ACQ_RW, Q, I, 1, 0,
               CACHE_ACT_SEND_RW);
    step_cache("exclusion: the grant is claimed", &w, CACHE_OP_GRANT_RW_CLAIM,
               F, I, 1, 0, CACHE_ACT_FETCH);
    step_cache("exclusion: a create hold is refused over the copy", &w,
               CACHE_OP_CREATE_HOLD, F, I, 1, 0, CACHE_ACT_NONE);
    step_cache("exclusion: the commit", &w, CACHE_OP_GRANT_COMMIT, G, I, 1, 0,
               CACHE_ACT_DRAIN_BOTH);
    step_cache("exclusion: a create hold is refused under the grant", &w,
               CACHE_OP_CREATE_HOLD, G, I, 1, 0, CACHE_ACT_NONE);
    step_cache("exclusion: the zero edge", &w, CACHE_OP_REL_RW, I, I, 0, 0,
               CACHE_ACT_REL_RW);
    step_cache("exclusion: the hold is taken once the turn has ended", &w,
               CACHE_OP_CREATE_HOLD, G, I, 1, 0, CACHE_ACT_DRAIN_BOTH);
  }
  {
    uint64_t w = CACHE_MAKE(I, I, 0, 0);
    /* A duplicate grant is refused WITHOUT touching the word, so the claim it
     * duplicates still commits normally. */
    step_cache("duplicate: a writer requests", &w, CACHE_OP_ACQ_RW, Q, I, 1, 0,
               CACHE_ACT_SEND_RW);
    step_cache("duplicate: the grant is claimed", &w, CACHE_OP_GRANT_RW_CLAIM,
               F, I, 1, 0, CACHE_ACT_FETCH);
    step_cache("duplicate: a second grant leaves the word alone", &w,
               CACHE_OP_GRANT_RW_CLAIM, F, I, 1, 0, CACHE_ACT_INVALID);
    step_cache("duplicate: a release before the commit is invalid", &w,
               CACHE_OP_REL_RW, F, I, 1, 0, CACHE_ACT_INVALID);
    step_cache("duplicate: the claim still commits", &w, CACHE_OP_GRANT_COMMIT,
               G, I, 1, 0, CACHE_ACT_DRAIN_BOTH);
    step_cache("duplicate: a second commit is invalid", &w,
               CACHE_OP_GRANT_COMMIT, G, I, 1, 0, CACHE_ACT_INVALID);
    step_cache("duplicate: the zero edge", &w, CACHE_OP_REL_RW, I, I, 0, 0,
               CACHE_ACT_REL_RW);
  }
#endif

  if (g_rc != 0) {
    return 1;
  }
  printf("PASS excl_compute_next: %d truth-table transitions "
         "(RW/RO ACQ/REL, D6 rw->rw chain, D7 new-RO-while-RW-waiting, "
         "state_bit normalization"
#ifdef ARTS_FAM
         ", cache-word claim/commit: REQ->FETCH->GRANT, committed hand-backs, "
         "acquire parks at FETCH, create hold refused at FETCH, a duplicate "
         "grant / a claimless commit / a release under a copy reported with "
         "the word untouched, multi-round sequences"
#endif
         ") all correct\n",
         g_checks);
  return 0;
}

#ifdef ARTS_UNIT_STANDALONE_SHIMS
void *arts_calloc(size_t nmemb, size_t size) { return calloc(nmemb, size); }
void arts_free(void *ptr) { free(ptr); }
void *arts_malloc(size_t size) { return malloc(size); }

/* The standalone build links no runtime: every runtime symbol an included
 * coherence translation unit may reference must resolve here. */
#include "arts/coherence/buffer.h"
#include "arts/utils/shared.h"
unsigned int arts_global_rank_id = 0;
void mark_edt_ready_by_guid(arts_guid_t edt_guid, unsigned int slot) {
  (void)edt_guid;
  (void)slot;
}
void arts_transport_send_async(int rank, char *message, unsigned int length) {
  (void)rank;
  (void)message;
  (void)length;
}
void arts_transport_loopback_post(const void *packet, unsigned int size) {
  (void)packet;
  (void)size;
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

#endif /* ARTS_PROTOCOL_EXCL */

#if defined(ARTS_PROTOCOL_EXCL)
#include "core/coherence/excl/arbiters.c"
#endif
