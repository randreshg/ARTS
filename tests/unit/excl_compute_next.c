/* SPDX-License-Identifier: Apache-2.0
 *
 * The EXCL arbiters (coherence/excl/arbiters.c), the PURE state-transition
 * functions of the retention this tree compiles.
 *
 * PURGE: excl_compute_next(cur, op, &grant), the lock-state transition over
 * [td | state_bit | rwo | w | r].  Given the current packed lock_state and an
 * op, it returns the next packed state and sets *grant to NONE / ONE_RW /
 * ALL_RO / DESTROY.  This test pins its FULL truth table with explicit,
 * hand-derived expected values — no oracle that merely re-implements the
 * function — covering:
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
 *     acquire; the claiming creator's pre-claim write request, which the
 *     teardown waits for;
 * and walks every word reachable within a small bound.  cache_compute_next,
 * the cache word's arbiter: every row, sequences driven as the commit sites
 * drive them (ro_aside beside the word), and every reachable (word,
 * ro_aside) with ghost counts of the parked and holding acquires.
 *
 * RETAIN: cache_owner_compute_next, the cache word's arbiter: every row, the
 * parked populations a serving transition owes, sequences driven as
 * retain.c's commit sites drive them (ro_aside beside the word), and every
 * (word, ro_aside) reachable from an idle cache, a sticky owner, a resident
 * rank without the write permission and a claimable first touch, with ghost
 * counts of the parked and holding acquires and the direct transitions of
 * owner_try_execute, owner_serve_readers and the recall modelled on the word
 * they commit; and the home directory's lock_owner_compute_next.
 *
 * The arbiters are static to the unit that includes them; this file reaches
 * them the same way, by #including coherence/excl/arbiters.c.  Built and
 * registered in every EXCL tree.
 *
 * Build: -DARTS_VALIDITY_EXCL=1 -DARTS_RETENTION_{PURGE,RETAIN}=1
 * -DARTS_UNIT_STANDALONE_SHIMS.
 */

#include <stdio.h>

#include "arts/coherence/excl/types.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef ARTS_RETENTION_PURGE
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

/* Forward decls of the SUT (static in the arbiter TU #included below). */
static uint64_t excl_compute_next(uint64_t cur, int op, uint32_t *out_grant);
static uint64_t cache_compute_next(uint64_t cur, int op, uint32_t taken,
                                   uint32_t *out_action);
static inline uint32_t cache_set_aside(uint64_t pre);

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

/* One step of a lock-word sequence: apply `op`, check the whole word (the
 * mark, rwo and the spare bits included) and the grant, carry the produced
 * word on. */
static void step_lock(const char *name, uint64_t *cur, int op, uint64_t want,
                      uint32_t ex_grant) {
  uint32_t grant = 0xffffffff;
  uint64_t next = excl_compute_next(*cur, op, &grant);
  g_checks++;
  if (next != want || grant != ex_grant) {
    (void)fprintf(stderr,
                  "FAIL [%s]: in %llx op=%d => out(%llx grant=%u) "
                  "expected(%llx grant=%u)\n",
                  name, (unsigned long long)*cur, op, (unsigned long long)next,
                  grant, (unsigned long long)want, ex_grant);
    g_rc = 1;
  }
  *cur = next;
}

/* Every lock word reachable from a fresh home (idle, or seeded with the
 * creator's hold, with and without the pre-claim write request awaited),
 * with at most LW_MAX ranks counted per axis.  Each produced word keeps its
 * spare bits zero and its phase bit normalized; the teardown is returned
 * exactly on the transition that leaves a marked word with nothing counted
 * or awaited, and the torn-down word counts and grants no acquire. */
#define LW_MAX 3u
static void lock_walk(void) {
  enum { CAP = 4096 };
  static uint64_t seen[CAP];
  int n = 0, head = 0;
  seen[n++] = LOCK_MAKE_STATE(0u, 0u, 0u);
  seen[n++] = LOCK_MAKE_STATE(0u, 1u, 0u);
  seen[n++] = LOCK_MAKE_STATE_FULL(0u, 0u, 1u, 1u, 0u);
  const uint64_t layout =
      LOCK_MAKE_STATE_FULL(1u, 1u, 1u, EXCL_STATE_W_MASK, EXCL_STATE_R_MASK);
  while (head < n) {
    uint64_t cur = seen[head++];
    uint32_t w = EXCL_STATE_W(cur), r = EXCL_STATE_R(cur);
    bool torn = EXCL_STATE_TEARDOWN(cur) && w == 0u && r == 0u &&
                EXCL_STATE_RWO(cur) == 0u;
    for (int op = EXCL_OP_RW_ACQ; op <= EXCL_OP_RWO_ARRIVE; op++) {
      if ((op == EXCL_OP_RW_ACQ && w == LW_MAX) ||
          (op == EXCL_OP_RO_ACQ && r == LW_MAX) ||
          (op == EXCL_OP_RW_REL && w == 0u) ||
          (op == EXCL_OP_RO_REL && r == 0u) ||
          (op == EXCL_OP_RWO_ARRIVE && EXCL_STATE_RWO(cur) == 0u)) {
        continue;
      }
      uint32_t grant = 0xffffffff;
      uint64_t next = excl_compute_next(cur, op, &grant);
      uint32_t nw = EXCL_STATE_W(next), nr = EXCL_STATE_R(next);
      bool edge = EXCL_STATE_TEARDOWN(next) && nw == 0u && nr == 0u &&
                  EXCL_STATE_RWO(next) == 0u;
      g_checks++;
      bool ok = (next & ~layout) == 0u &&
                !((nw == 0u || nr == 0u) && EXCL_STATE_BIT(next) != 0u) &&
                (grant == LOCK_GRANT_DESTROY) == (edge && !torn) &&
                !(torn && (next != cur || grant != LOCK_GRANT_NONE));
      /* A grant is given only into the phase it belongs to. */
      if (grant == LOCK_GRANT_ONE_RW) {
        ok = ok && nw > 0u && (nr == 0u || op == EXCL_OP_RW_REL);
      } else if (grant == LOCK_GRANT_ALL_RO) {
        ok = ok && nr > 0u &&
             (nw == 0u || EXCL_STATE_BIT(next) == EXCL_PHASE_BIT_RO);
      }
      if (!ok) {
        (void)fprintf(stderr, "FAIL [lock walk]: %llx op=%d => %llx grant=%u\n",
                      (unsigned long long)cur, op, (unsigned long long)next,
                      grant);
        g_rc = 1;
        return;
      }
      int i = 0;
      while (i < n && seen[i] != next) {
        i++;
      }
      if (i == n) {
        if (n == CAP) {
          (void)fprintf(stderr, "FAIL [lock walk]: too many words\n");
          g_rc = 1;
          return;
        }
        seen[n++] = next;
      }
    }
  }
}

/* Assert: applying `op` (with `taken` readers returned from ro_aside) to
 * CACHE_MAKE(in_rws,in_ros,in_wc,in_rc) yields
 * CACHE_MAKE(ex_rws,ex_ros,ex_wc,ex_rc), action == ex_act, and sets
 * ex_aside readers aside. */
static void chk_cache(const char *name, uint32_t in_rws, uint32_t in_ros,
                      uint32_t in_wc, uint32_t in_rc, int op, uint32_t taken,
                      uint32_t ex_rws, uint32_t ex_ros, uint32_t ex_wc,
                      uint32_t ex_rc, uint32_t ex_act, uint32_t ex_aside) {
  uint64_t cur = CACHE_MAKE(in_rws, in_ros, in_wc, in_rc);
  uint32_t act = 0xffffffff;
  uint64_t next = cache_compute_next(cur, op, taken, &act);
  uint64_t want = CACHE_MAKE(ex_rws, ex_ros, ex_wc, ex_rc);
  uint32_t aside = act == CACHE_ACT_DRAIN_RW ? cache_set_aside(cur) : 0u;
  g_checks++;
  if (next != want || act != ex_act || aside != ex_aside) {
    (void)fprintf(stderr,
                  "FAIL [%s]: in(%u,%u,%u,%u) op=%d taken=%u => "
                  "out(%u,%u,%u,%u act=%u aside=%u) "
                  "expected(%u,%u,%u,%u act=%u aside=%u)\n",
                  name, in_rws, in_ros, in_wc, in_rc, op, taken,
                  CACHE_RW_ST(next), CACHE_RO_ST(next), CACHE_RW_CNT(next),
                  CACHE_RO_CNT(next), act, aside, ex_rws, ex_ros, ex_wc, ex_rc,
                  ex_act, ex_aside);
    g_rc = 1;
  }
}

/* A cache as its commit sites drive it: the word, and ro_aside, which the
 * committer of a write-permission transition adds to and the committer of a
 * read grant takes whole before its CAS. */
struct cache_sim {
  uint64_t word;
  uint32_t aside;
};

/* One step of a SEQUENCE on a simulated cache: apply `op` as its commit site
 * does, check the word (the stub bit included), ro_aside and the action,
 * and carry the result into the next step.  A single-round table cannot
 * reach a state that takes several transitions to build, which is where a
 * reusable axis and an exactly-once drain are decided. */
static void step_cache(const char *name, struct cache_sim *c, int op,
                       uint64_t want, uint32_t ex_aside, uint32_t ex_act) {
  uint32_t act = 0xffffffff;
  uint32_t taken = op == CACHE_OP_GRANT_RO ? c->aside : 0u;
  uint64_t next = cache_compute_next(c->word, op, taken, &act);
  uint32_t aside = c->aside - taken;
  if (act == CACHE_ACT_DRAIN_RW) {
    aside += cache_set_aside(c->word);
  }
  g_checks++;
  if (next != want || act != ex_act || aside != ex_aside) {
    (void)fprintf(stderr,
                  "FAIL [%s]: in %llx aside=%u op=%d => out(%llx aside=%u "
                  "act=%u) expected(%llx aside=%u act=%u)\n",
                  name, (unsigned long long)c->word, c->aside, op,
                  (unsigned long long)next, aside, act,
                  (unsigned long long)want, ex_aside, ex_act);
    g_rc = 1;
  }
  c->word = next;
  c->aside = aside;
}

/* Every (word, ro_aside) reachable from an idle cache and from a claimable
 * first touch, with at most CW_MAX acquires per axis outstanding, driven as
 * the commit sites drive it.  Ghost counts follow each acquire: parked on
 * its axis's request, or holding.  Checked on every transition:
 *   - the read count is the parked readers while the read request is out
 *     and no write grant is held, the holders otherwise, and ro_aside is the
 *     parked readers that count leaves out; ro_aside is non-zero only while
 *     the read request is out;
 *   - the write count is the parked writers at REQ, the holders at GRANT;
 *   - a request that is out has a waiter parked on it, so no word counts
 *     readers at IDLE without the write grant held;
 *   - each drain is owed exactly the waiters parked on its axis;
 *   - the two grants are never held together.
 * A grant, a size round or a release is applied only where the arbiter
 * takes it: one it calls LATE is a message the home never sends.  The walk
 * must reach a write turn's zero edge with readers still parked on the read
 * request (set aside), and a local hit under the write grant while the read
 * request is out. */
#define CW_MAX 3u
struct cw_node {
  uint64_t word;
  uint32_t aside;
  uint8_t parked_r, live_r, parked_w, live_w;
};

static bool cw_invariant(const struct cw_node *s) {
  uint32_t rws = CACHE_RW_ST(s->word), ros = CACHE_RO_ST(s->word);
  uint32_t wc = CACHE_RW_CNT(s->word), rc = CACHE_RO_CNT(s->word);
  bool rc_parked = ros == CACHE_ST_REQ && rws != CACHE_ST_GRANT;
  return s->parked_r == (rc_parked ? rc : 0u) + s->aside &&
         s->live_r == (rc_parked ? 0u : rc) &&
         s->parked_w == (rws == CACHE_ST_REQ ? wc : 0u) &&
         s->live_w == (rws == CACHE_ST_GRANT ? wc : 0u) &&
         (s->aside == 0u || ros == CACHE_ST_REQ) &&
         (ros == CACHE_ST_REQ) == (s->parked_r != 0u) &&
         (rws == CACHE_ST_REQ) == (s->parked_w != 0u) &&
         !(ros == CACHE_ST_IDLE && rc != 0u && rws != CACHE_ST_GRANT) &&
         !(rws == CACHE_ST_GRANT && ros == CACHE_ST_GRANT) &&
         !(rws == CACHE_ST_GRANT && wc == 0u && rc == 0u) &&
         !(ros == CACHE_ST_GRANT && rc == 0u) && rws != 3u && ros != 3u &&
         (s->word & ((uint64_t)1 << 59)) == 0u;
}

static void cache_walk(void) {
  enum { CAP = 8192 };
  static struct cw_node seen[CAP];
  int n = 0, head = 0;
  bool turn_ended_with_readers_aside = false;
  bool hit_under_write_grant_with_request_out = false;
  bool read_grant_under_write_request = false;
  memset(seen, 0, sizeof(seen));
  seen[n++].word = CACHE_MAKE(CACHE_ST_IDLE, CACHE_ST_IDLE, 0u, 0u);
  seen[n++].word = CACHE_MAKE_X(CACHE_ST_IDLE, CACHE_ST_IDLE, 1u, 0u, 0u);
  while (head < n) {
    const struct cw_node cur = seen[head++];
    uint32_t rws = CACHE_RW_ST(cur.word), ros = CACHE_RO_ST(cur.word);
    for (int op = CACHE_OP_ACQ_RW; op <= CACHE_OP_DECLAIM; op++) {
      bool acquire = op == CACHE_OP_ACQ_RW || op == CACHE_OP_ACQ_RO;
      bool release = op == CACHE_OP_REL_RW || op == CACHE_OP_REL_RO;
      if (((op == CACHE_OP_ACQ_RW || op == CACHE_OP_CREATE_HOLD) &&
           cur.parked_w + cur.live_w == CW_MAX) ||
          (op == CACHE_OP_ACQ_RO && cur.parked_r + cur.live_r == CW_MAX) ||
          (op == CACHE_OP_REL_RW && cur.live_w == 0u) ||
          (op == CACHE_OP_REL_RO && cur.live_r == 0u)) {
        continue;
      }
      struct cw_node nx = cur;
      uint32_t act = 0xffffffff;
      uint32_t taken = op == CACHE_OP_GRANT_RO ? cur.aside : 0u;
      nx.word = cache_compute_next(cur.word, op, taken, &act);
      g_checks++;
      bool ok = true;
      if (act == CACHE_ACT_LATE) {
        /* Refused, the word unchanged: never an acquire inside the bound or
         * a holder's release. */
        ok = nx.word == cur.word && !acquire && !release;
      } else if (act == CACHE_ACT_SELF_SERVE) {
        ok = acquire;
        if (op == CACHE_OP_ACQ_RW) {
          nx.live_w++;
        } else {
          nx.live_r++;
          hit_under_write_grant_with_request_out |=
              rws == CACHE_ST_GRANT && ros == CACHE_ST_REQ;
        }
      } else if (act == CACHE_ACT_SEND_RW || act == CACHE_ACT_SEND_RO ||
                 act == CACHE_ACT_PARK) {
        /* The request opens exactly when its own axis is idle. */
        if (op == CACHE_OP_ACQ_RW) {
          ok = act ==
               (rws == CACHE_ST_IDLE ? CACHE_ACT_SEND_RW : CACHE_ACT_PARK);
          nx.parked_w++;
        } else {
          ok = op == CACHE_OP_ACQ_RO &&
               act ==
                   (ros == CACHE_ST_IDLE ? CACHE_ACT_SEND_RO : CACHE_ACT_PARK);
          nx.parked_r++;
        }
      } else if (act == CACHE_ACT_DRAIN_RW) {
        ok = (op == CACHE_OP_GRANT_RW || op == CACHE_OP_CREATE_HOLD) &&
             CACHE_RW_CNT(cur.word) == cur.parked_w &&
             cache_set_aside(cur.word) == cur.parked_r - cur.aside;
        nx.aside += cache_set_aside(cur.word);
        nx.live_w += nx.parked_w;
        nx.parked_w = 0u;
        if (op == CACHE_OP_CREATE_HOLD) {
          nx.live_w++;
        }
      } else if (act == CACHE_ACT_DRAIN_RO) {
        ok = op == CACHE_OP_GRANT_RO && CACHE_RO_CNT(nx.word) == cur.parked_r;
        nx.aside = 0u;
        nx.live_r += nx.parked_r;
        nx.parked_r = 0u;
        read_grant_under_write_request |= rws == CACHE_ST_REQ;
      } else if (act == CACHE_ACT_RESEND_RO) {
        ok = op == CACHE_OP_CTS_RO && ros == CACHE_ST_REQ;
      } else if (release) {
        ok = act == CACHE_ACT_NONE || act == CACHE_ACT_REL_RW ||
             act == CACHE_ACT_REL_RO;
        if (op == CACHE_OP_REL_RW) {
          nx.live_w--;
        } else {
          nx.live_r--;
        }
        if (act != CACHE_ACT_NONE) {
          /* A grant goes back when its last holder leaves. */
          ok = ok && nx.live_w == 0u && nx.live_r == 0u;
          turn_ended_with_readers_aside |=
              act == CACHE_ACT_REL_RW && nx.aside != 0u;
        }
      } else {
        /* A write size round, or a create's hold refused under a grant. */
        ok = act == CACHE_ACT_NONE &&
             (op == CACHE_OP_DECLAIM ||
              (op == CACHE_OP_CREATE_HOLD && nx.word == cur.word &&
               (rws == CACHE_ST_GRANT || ros == CACHE_ST_GRANT)));
      }
      if (!ok || !cw_invariant(&nx)) {
        (void)fprintf(stderr,
                      "FAIL [cache walk]: %llx aside=%u op=%d => %llx "
                      "aside=%u act=%u (parked r=%u w=%u, live r=%u w=%u)\n",
                      (unsigned long long)cur.word, cur.aside, op,
                      (unsigned long long)nx.word, nx.aside, act, nx.parked_r,
                      nx.parked_w, nx.live_r, nx.live_w);
        g_rc = 1;
        return;
      }
      int i = 0;
      while (i < n && !(seen[i].word == nx.word && seen[i].aside == nx.aside)) {
        i++;
      }
      if (i == n) {
        if (n == CAP) {
          (void)fprintf(stderr, "FAIL [cache walk]: too many words\n");
          g_rc = 1;
          return;
        }
        seen[n++] = nx;
      }
    }
  }
  if (!turn_ended_with_readers_aside ||
      !hit_under_write_grant_with_request_out ||
      !read_grant_under_write_request) {
    (void)fprintf(stderr,
                  "FAIL [cache walk]: not reached: a write turn ending with "
                  "readers set aside (%d), a local hit under the write grant "
                  "with the read request out (%d), a read grant with the "
                  "write request out (%d)\n",
                  (int)turn_ended_with_readers_aside,
                  (int)hit_under_write_grant_with_request_out,
                  (int)read_grant_under_write_request);
    g_rc = 1;
  }
}

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
    step_lock("td: a reader parks behind the writer", &w, T_RO_ACQ,
              LOCK_MAKE_STATE_TD(0, RW, 1, 1), G_NONE);
    step_lock("td: the destroy marks", &w, T_TEARDOWN,
              LOCK_MAKE_STATE_TD(1, RW, 1, 1), G_NONE);
    step_lock("td: a writer that lands now is counted", &w, T_RW_ACQ,
              LOCK_MAKE_STATE_TD(1, RW, 2, 1), G_NONE);
    step_lock("td: the writer's release passes the turn", &w, T_RW_REL,
              LOCK_MAKE_STATE_TD(1, RW, 1, 1), G_ONE_RW);
    step_lock("td: the last writer's release opens the read phase", &w,
              T_RW_REL, LOCK_MAKE_STATE_TD(1, 0, 0, 1), G_ALL_RO);
    step_lock("td: the last release tears down", &w, T_RO_REL,
              LOCK_MAKE_STATE_TD(1, 0, 0, 0), G_DESTROY);
    step_lock("td: a writer after the teardown is refused", &w, T_RW_ACQ,
              LOCK_MAKE_STATE_TD(1, 0, 0, 0), G_NONE);
    step_lock("td: a reader after the teardown is refused", &w, T_RO_ACQ,
              LOCK_MAKE_STATE_TD(1, 0, 0, 0), G_NONE);
    step_lock("td: a duplicate destroy changes nothing", &w, T_TEARDOWN,
              LOCK_MAKE_STATE_TD(1, 0, 0, 0), G_NONE);
  }
  {
    /* A destroy that finds nothing owed tears down at once. */
    uint64_t w = LOCK_MAKE_STATE(0, 0, 0);
    step_lock("td idle: the destroy tears down", &w, T_TEARDOWN,
              LOCK_MAKE_STATE_TD(1, 0, 0, 0), G_DESTROY);
    step_lock("td idle: a writer after it is refused", &w, T_RW_ACQ,
              LOCK_MAKE_STATE_TD(1, 0, 0, 0), G_NONE);
  }
  {
    /* A claimed first touch: the announce seeded the creator's hold (w 1)
     * and its pre-claim write request (rwo), which the teardown waits for.
     * The home is not torn down while it does: a read request that lands
     * meanwhile is counted and granted as a read. */
    uint64_t w = LOCK_MAKE_STATE_FULL(0, 0, 1, 1, 0);
    step_lock("rwo: the creator's release", &w, T_RW_REL,
              LOCK_MAKE_STATE_FULL(0, 0, 1, 0, 0), G_NONE);
    step_lock("rwo: the destroy only marks", &w, T_TEARDOWN,
              LOCK_MAKE_STATE_FULL(1, 0, 1, 0, 0), G_NONE);
    step_lock("rwo: a reader that lands now is counted", &w, T_RO_ACQ,
              LOCK_MAKE_STATE_FULL(1, 0, 1, 0, 1), G_ALL_RO);
    step_lock("rwo: its release does not tear down", &w, T_RO_REL,
              LOCK_MAKE_STATE_FULL(1, 0, 1, 0, 0), G_NONE);
    step_lock("rwo: the pre-claim write request tears down", &w,
              EXCL_OP_RWO_ARRIVE, LOCK_MAKE_STATE_FULL(1, 0, 0, 0, 0),
              G_DESTROY);
    step_lock("rwo: a reader after the teardown is refused", &w, T_RO_ACQ,
              LOCK_MAKE_STATE_FULL(1, 0, 0, 0, 0), G_NONE);
  }
  {
    /* The pre-claim write request arrives before the destroy: nothing is
     * awaited any more, and the destroy tears down at the zero edge. */
    uint64_t w = LOCK_MAKE_STATE_FULL(0, 0, 1, 1, 0);
    step_lock("rwo first: the request is discarded", &w, EXCL_OP_RWO_ARRIVE,
              LOCK_MAKE_STATE_FULL(0, 0, 0, 1, 0), G_NONE);
    step_lock("rwo first: the destroy marks under the hold", &w, T_TEARDOWN,
              LOCK_MAKE_STATE_FULL(1, 0, 0, 1, 0), G_NONE);
    step_lock("rwo first: the creator's release tears down", &w, T_RW_REL,
              LOCK_MAKE_STATE_FULL(1, 0, 0, 0, 0), G_DESTROY);
  }
  {
    /* The pre-claim write request arrives under the mark with a release
     * still owed: the last release tears down. */
    uint64_t w = LOCK_MAKE_STATE_FULL(0, 0, 1, 1, 0);
    step_lock("mark: destroy with a writer and the request awaited", &w,
              T_TEARDOWN, LOCK_MAKE_STATE_FULL(1, 0, 1, 1, 0), G_NONE);
    step_lock("mark: the request is discarded", &w, EXCL_OP_RWO_ARRIVE,
              LOCK_MAKE_STATE_FULL(1, 0, 0, 1, 0), G_NONE);
    step_lock("mark: the last release tears down", &w, T_RW_REL,
              LOCK_MAKE_STATE_FULL(1, 0, 0, 0, 0), G_DESTROY);
  }
  lock_walk();

  /* ===== the cache word =====
   * Each axis has its own request: an acquire opens or joins the request of
   * its own axis whatever the other axis holds, and one grant answers one
   * request.  Write permission serves writers and sets the readers parked on
   * the read request aside; the read grant returns them. */
  const uint32_t I = CACHE_ST_IDLE, Q = CACHE_ST_REQ, G = CACHE_ST_GRANT;
  const uint32_t RMAX = (uint32_t)CACHE_CNT_MASK;
  const uint32_t WMAX = (uint32_t)CACHE_RW_CNT_MASK;

  chk_cache("roacq opens the read request", I, I, 0, 0, CACHE_OP_ACQ_RO, 0, I,
            Q, 0, 1, CACHE_ACT_SEND_RO, 0);
  chk_cache("roacq opens it with the write request out", Q, I, 1, 0,
            CACHE_OP_ACQ_RO, 0, Q, Q, 1, 1, CACHE_ACT_SEND_RO, 0);
  chk_cache("roacq joins the read request", I, Q, 0, 1, CACHE_OP_ACQ_RO, 0, I,
            Q, 0, 2, CACHE_ACT_PARK, 0);
  chk_cache("roacq joins it with the write request out", Q, Q, 1, 1,
            CACHE_OP_ACQ_RO, 0, Q, Q, 1, 2, CACHE_ACT_PARK, 0);
  chk_cache("roacq under the write grant is a local hit", G, I, 1, 0,
            CACHE_OP_ACQ_RO, 0, G, I, 1, 1, CACHE_ACT_SELF_SERVE, 0);
  chk_cache("roacq under the write grant, the read request out", G, Q, 1, 0,
            CACHE_OP_ACQ_RO, 0, G, Q, 1, 1, CACHE_ACT_SELF_SERVE, 0);
  chk_cache("roacq under the read grant is a local hit", I, G, 0, 1,
            CACHE_OP_ACQ_RO, 0, I, G, 0, 2, CACHE_ACT_SELF_SERVE, 0);
  chk_cache("roacq under the read grant, the write request out", Q, G, 1, 1,
            CACHE_OP_ACQ_RO, 0, Q, G, 1, 2, CACHE_ACT_SELF_SERVE, 0);
  chk_cache("roacq on a full count is late", I, G, 0, RMAX, CACHE_OP_ACQ_RO, 0,
            I, G, 0, RMAX, CACHE_ACT_LATE, 0);

  chk_cache("rwacq opens the write request", I, I, 0, 0, CACHE_OP_ACQ_RW, 0, Q,
            I, 1, 0, CACHE_ACT_SEND_RW, 0);
  chk_cache("rwacq opens it with the read request out", I, Q, 0, 2,
            CACHE_OP_ACQ_RW, 0, Q, Q, 1, 2, CACHE_ACT_SEND_RW, 0);
  chk_cache("rwacq opens it under the read grant", I, G, 0, 2, CACHE_OP_ACQ_RW,
            0, Q, G, 1, 2, CACHE_ACT_SEND_RW, 0);
  chk_cache("rwacq joins the write request", Q, I, 1, 0, CACHE_OP_ACQ_RW, 0, Q,
            I, 2, 0, CACHE_ACT_PARK, 0);
  chk_cache("rwacq under the write grant is a local hit", G, I, 1, 0,
            CACHE_OP_ACQ_RW, 0, G, I, 2, 0, CACHE_ACT_SELF_SERVE, 0);
  chk_cache("rwacq on a full count is late", G, I, WMAX, 0, CACHE_OP_ACQ_RW, 0,
            G, I, WMAX, 0, CACHE_ACT_LATE, 0);

  chk_cache("rwgrant req wc1", Q, I, 1, 0, CACHE_OP_GRANT_RW, 0, G, I, 1, 0,
            CACHE_ACT_DRAIN_RW, 0);
  chk_cache("rwgrant sets the parked readers aside", Q, Q, 1, 2,
            CACHE_OP_GRANT_RW, 0, G, Q, 1, 0, CACHE_ACT_DRAIN_RW, 2);
  chk_cache("rwgrant with no writer counted is late", Q, Q, 0, 2,
            CACHE_OP_GRANT_RW, 0, Q, Q, 0, 2, CACHE_ACT_LATE, 0);
  chk_cache("rwgrant at idle is late", I, I, 0, 0, CACHE_OP_GRANT_RW, 0, I, I,
            0, 0, CACHE_ACT_LATE, 0);
  chk_cache("rwgrant at a live hold is late", G, I, 1, 0, CACHE_OP_GRANT_RW, 0,
            G, I, 1, 0, CACHE_ACT_LATE, 0);
  chk_cache("rwgrant under a held read grant is late", Q, G, 1, 1,
            CACHE_OP_GRANT_RW, 0, Q, G, 1, 1, CACHE_ACT_LATE, 0);
  chk_cache("rwgrant with readers counted on no request is late", Q, I, 1, 1,
            CACHE_OP_GRANT_RW, 0, Q, I, 1, 1, CACHE_ACT_LATE, 0);

  chk_cache("rogrant req rc2", I, Q, 0, 2, CACHE_OP_GRANT_RO, 0, I, G, 0, 2,
            CACHE_ACT_DRAIN_RO, 0);
  chk_cache("rogrant with the write request out", Q, Q, 1, 2, CACHE_OP_GRANT_RO,
            0, Q, G, 1, 2, CACHE_ACT_DRAIN_RO, 0);
  chk_cache("rogrant returns the readers set aside", I, Q, 0, 0,
            CACHE_OP_GRANT_RO, 2, I, G, 0, 2, CACHE_ACT_DRAIN_RO, 0);
  chk_cache("rogrant adds them to the readers parked since", I, Q, 0, 1,
            CACHE_OP_GRANT_RO, 2, I, G, 0, 3, CACHE_ACT_DRAIN_RO, 0);
  chk_cache("rogrant with nobody parked is late", I, Q, 0, 0, CACHE_OP_GRANT_RO,
            0, I, Q, 0, 0, CACHE_ACT_LATE, 0);
  chk_cache("rogrant at idle is late", I, I, 0, 0, CACHE_OP_GRANT_RO, 0, I, I,
            0, 0, CACHE_ACT_LATE, 0);
  chk_cache("rogrant at a held read grant is late", I, G, 0, 1,
            CACHE_OP_GRANT_RO, 0, I, G, 0, 1, CACHE_ACT_LATE, 0);
  chk_cache("rogrant under a held write grant is late", G, Q, 1, 1,
            CACHE_OP_GRANT_RO, 0, G, Q, 1, 1, CACHE_ACT_LATE, 0);
  chk_cache("rogrant under a held write grant, readers aside, is late", G, Q, 1,
            0, CACHE_OP_GRANT_RO, 1, G, Q, 1, 0, CACHE_ACT_LATE, 0);
  chk_cache("rogrant past the count's limit is late", I, Q, 0, RMAX,
            CACHE_OP_GRANT_RO, 1, I, Q, 0, RMAX, CACHE_ACT_LATE, 0);

  chk_cache("cts re-sends for the parked readers", I, Q, 0, 1, CACHE_OP_CTS_RO,
            0, I, Q, 0, 1, CACHE_ACT_RESEND_RO, 0);
  chk_cache("cts re-sends with the readers set aside", I, Q, 0, 0,
            CACHE_OP_CTS_RO, 0, I, Q, 0, 0, CACHE_ACT_RESEND_RO, 0);
  chk_cache("cts re-sends with the write request out", Q, Q, 1, 1,
            CACHE_OP_CTS_RO, 0, Q, Q, 1, 1, CACHE_ACT_RESEND_RO, 0);
  chk_cache("cts re-sends under the write grant", G, Q, 1, 0, CACHE_OP_CTS_RO,
            0, G, Q, 1, 0, CACHE_ACT_RESEND_RO, 0);
  chk_cache("cts with no read request out is late", I, I, 0, 0, CACHE_OP_CTS_RO,
            0, I, I, 0, 0, CACHE_ACT_LATE, 0);
  chk_cache("cts under the read grant is late", I, G, 0, 1, CACHE_OP_CTS_RO, 0,
            I, G, 0, 1, CACHE_ACT_LATE, 0);

  chk_cache("the create's hold on an idle cache", I, I, 0, 0,
            CACHE_OP_CREATE_HOLD, 0, G, I, 1, 0, CACHE_ACT_DRAIN_RW, 0);
  chk_cache("the create's hold closes the write request", Q, I, 1, 0,
            CACHE_OP_CREATE_HOLD, 0, G, I, 2, 0, CACHE_ACT_DRAIN_RW, 0);
  chk_cache("the create's hold leaves the read request out", I, Q, 0, 1,
            CACHE_OP_CREATE_HOLD, 0, G, Q, 1, 0, CACHE_ACT_DRAIN_RW, 1);
  chk_cache("the create's hold with both requests out", Q, Q, 1, 2,
            CACHE_OP_CREATE_HOLD, 0, G, Q, 2, 0, CACHE_ACT_DRAIN_RW, 2);
  chk_cache("the create's hold is refused under the write grant", G, I, 1, 0,
            CACHE_OP_CREATE_HOLD, 0, G, I, 1, 0, CACHE_ACT_NONE, 0);
  chk_cache("the create's hold is refused under the read grant", Q, G, 1, 1,
            CACHE_OP_CREATE_HOLD, 0, Q, G, 1, 1, CACHE_ACT_NONE, 0);
  chk_cache("the create's hold on a full count is late", Q, I, WMAX, 0,
            CACHE_OP_CREATE_HOLD, 0, Q, I, WMAX, 0, CACHE_ACT_LATE, 0);
  chk_cache("the create's hold with readers on no request is late", Q, I, 1, 1,
            CACHE_OP_CREATE_HOLD, 0, Q, I, 1, 1, CACHE_ACT_LATE, 0);

  chk_cache("declaim keeps the write request out", Q, Q, 1, 1, CACHE_OP_DECLAIM,
            0, Q, Q, 1, 1, CACHE_ACT_NONE, 0);
  chk_cache("declaim with no write request out is late", I, Q, 0, 1,
            CACHE_OP_DECLAIM, 0, I, Q, 0, 1, CACHE_ACT_LATE, 0);

  chk_cache("rwrel with a writer left", G, I, 2, 0, CACHE_OP_REL_RW, 0, G, I, 1,
            0, CACHE_ACT_NONE, 0);
  chk_cache("rwrel with a reader left under the turn", G, I, 1, 1,
            CACHE_OP_REL_RW, 0, G, I, 0, 1, CACHE_ACT_NONE, 0);
  chk_cache("rwrel at the zero edge", G, I, 1, 0, CACHE_OP_REL_RW, 0, I, I, 0,
            0, CACHE_ACT_REL_RW, 0);
  chk_cache("rwrel at the zero edge, the read request out", G, Q, 1, 0,
            CACHE_OP_REL_RW, 0, I, Q, 0, 0, CACHE_ACT_REL_RW, 0);
  chk_cache("rorel of the last joiner ends the write turn", G, I, 0, 1,
            CACHE_OP_REL_RO, 0, I, I, 0, 0, CACHE_ACT_REL_RW, 0);
  chk_cache("rorel of the last joiner, the read request out", G, Q, 0, 1,
            CACHE_OP_REL_RO, 0, I, Q, 0, 0, CACHE_ACT_REL_RW, 0);
  chk_cache("rorel with a reader left", I, G, 0, 2, CACHE_OP_REL_RO, 0, I, G, 0,
            1, CACHE_ACT_NONE, 0);
  chk_cache("rorel at the zero edge", I, G, 0, 1, CACHE_OP_REL_RO, 0, I, I, 0,
            0, CACHE_ACT_REL_RO, 0);
  chk_cache("rorel at the zero edge, the write request out", Q, G, 1, 1,
            CACHE_OP_REL_RO, 0, Q, I, 1, 0, CACHE_ACT_REL_RO, 0);

  {
    /* Every reply and the create's hold clear the stub bit: the home is
     * installed, or the claim is here.  An acquire keeps it. */
    const int ops[5] = {CACHE_OP_GRANT_RW, CACHE_OP_GRANT_RO, CACHE_OP_CTS_RO,
                        CACHE_OP_DECLAIM, CACHE_OP_CREATE_HOLD};
    const uint64_t ins[5] = {
        CACHE_MAKE_X(Q, I, 1u, 1, 0), CACHE_MAKE_X(I, Q, 1u, 0, 1),
        CACHE_MAKE_X(I, Q, 1u, 0, 1), CACHE_MAKE_X(Q, I, 1u, 1, 0),
        CACHE_MAKE_X(I, I, 1u, 0, 0)};
    for (int i = 0; i < 5; i++) {
      uint32_t act = 0xffffffff;
      uint64_t next = cache_compute_next(ins[i], ops[i], 0u, &act);
      g_checks++;
      if (act == CACHE_ACT_LATE || CACHE_STUB(next) != 0u) {
        (void)fprintf(stderr, "FAIL [reply %d clears stub]: act=%u stub=%u\n",
                      i, act, CACHE_STUB(next));
        g_rc = 1;
      }
    }
    for (int op = CACHE_OP_ACQ_RW; op <= CACHE_OP_ACQ_RO; op++) {
      uint32_t act = 0xffffffff;
      uint64_t next =
          cache_compute_next(CACHE_MAKE_X(I, I, 1u, 0, 0), op, 0u, &act);
      g_checks++;
      if (CACHE_STUB(next) != 1u) {
        (void)fprintf(stderr, "FAIL [acquire %d keeps stub]\n", op);
        g_rc = 1;
      }
    }
  }
  {
    /* The write turn arrives while the read request is out: the grant wakes
     * the writers alone, the parked readers go aside, a reader that arrives
     * under the grant is a local hit, and the turn ends without the readers
     * set aside.  The read request stays out; its grant returns them, with
     * the reader parked since. */
    struct cache_sim c = {CACHE_MAKE(I, I, 0, 0), 0u};
    step_cache("turn: a reader requests", &c, CACHE_OP_ACQ_RO,
               CACHE_MAKE(I, Q, 0, 1), 0, CACHE_ACT_SEND_RO);
    step_cache("turn: a second reader joins", &c, CACHE_OP_ACQ_RO,
               CACHE_MAKE(I, Q, 0, 2), 0, CACHE_ACT_PARK);
    step_cache("turn: a writer requests at once", &c, CACHE_OP_ACQ_RW,
               CACHE_MAKE(Q, Q, 1, 2), 0, CACHE_ACT_SEND_RW);
    step_cache("turn: the write grant sets the readers aside", &c,
               CACHE_OP_GRANT_RW, CACHE_MAKE(G, Q, 1, 0), 2,
               CACHE_ACT_DRAIN_RW);
    step_cache("turn: a reader under the grant is a local hit", &c,
               CACHE_OP_ACQ_RO, CACHE_MAKE(G, Q, 1, 1), 2,
               CACHE_ACT_SELF_SERVE);
    step_cache("turn: it releases", &c, CACHE_OP_REL_RO, CACHE_MAKE(G, Q, 1, 0),
               2, CACHE_ACT_NONE);
    step_cache("turn: the zero edge, two readers still parked", &c,
               CACHE_OP_REL_RW, CACHE_MAKE(I, Q, 0, 0), 2, CACHE_ACT_REL_RW);
    step_cache("turn: a reader after the turn joins the request", &c,
               CACHE_OP_ACQ_RO, CACHE_MAKE(I, Q, 0, 1), 2, CACHE_ACT_PARK);
    step_cache("turn: the read grant finds all three", &c, CACHE_OP_GRANT_RO,
               CACHE_MAKE(I, G, 0, 3), 0, CACHE_ACT_DRAIN_RO);
    step_cache("turn: a reader releases", &c, CACHE_OP_REL_RO,
               CACHE_MAKE(I, G, 0, 2), 0, CACHE_ACT_NONE);
    step_cache("turn: a reader releases", &c, CACHE_OP_REL_RO,
               CACHE_MAKE(I, G, 0, 1), 0, CACHE_ACT_NONE);
    step_cache("turn: the read grant goes back", &c, CACHE_OP_REL_RO,
               CACHE_MAKE(I, I, 0, 0), 0, CACHE_ACT_REL_RO);
  }
  {
    /* The read request waits through two write turns: ro_aside sums the
     * readers each turn found parked. */
    struct cache_sim c = {CACHE_MAKE(Q, Q, 1, 1), 0u};
    step_cache("two turns: the first grant", &c, CACHE_OP_GRANT_RW,
               CACHE_MAKE(G, Q, 1, 0), 1, CACHE_ACT_DRAIN_RW);
    step_cache("two turns: the first turn ends", &c, CACHE_OP_REL_RW,
               CACHE_MAKE(I, Q, 0, 0), 1, CACHE_ACT_REL_RW);
    step_cache("two turns: a reader joins the request", &c, CACHE_OP_ACQ_RO,
               CACHE_MAKE(I, Q, 0, 1), 1, CACHE_ACT_PARK);
    step_cache("two turns: a writer requests", &c, CACHE_OP_ACQ_RW,
               CACHE_MAKE(Q, Q, 1, 1), 1, CACHE_ACT_SEND_RW);
    step_cache("two turns: the second grant", &c, CACHE_OP_GRANT_RW,
               CACHE_MAKE(G, Q, 1, 0), 2, CACHE_ACT_DRAIN_RW);
    step_cache("two turns: the second turn ends", &c, CACHE_OP_REL_RW,
               CACHE_MAKE(I, Q, 0, 0), 2, CACHE_ACT_REL_RW);
    step_cache("two turns: the read grant finds both", &c, CACHE_OP_GRANT_RO,
               CACHE_MAKE(I, G, 0, 2), 0, CACHE_ACT_DRAIN_RO);
  }
  {
    /* The read grant arrives first, the write request out: it serves the
     * readers and no writer; the write grant follows their release. */
    struct cache_sim c = {CACHE_MAKE(Q, Q, 1, 2), 0u};
    step_cache("read first: the read grant", &c, CACHE_OP_GRANT_RO,
               CACHE_MAKE(Q, G, 1, 2), 0, CACHE_ACT_DRAIN_RO);
    step_cache("read first: a reader releases", &c, CACHE_OP_REL_RO,
               CACHE_MAKE(Q, G, 1, 1), 0, CACHE_ACT_NONE);
    step_cache("read first: the read grant goes back", &c, CACHE_OP_REL_RO,
               CACHE_MAKE(Q, I, 1, 0), 0, CACHE_ACT_REL_RO);
    step_cache("read first: the write grant", &c, CACHE_OP_GRANT_RW,
               CACHE_MAKE(G, I, 1, 0), 0, CACHE_ACT_DRAIN_RW);
  }
  {
    /* A holder of the write turn becomes a reader while the read request is
     * out: it joins as a holder, and the turn ends at its read release. */
    struct cache_sim c = {CACHE_MAKE(G, Q, 1, 0), 1u};
    step_cache("downgrade: the join", &c, CACHE_OP_ACQ_RO,
               CACHE_MAKE(G, Q, 1, 1), 1, CACHE_ACT_SELF_SERVE);
    step_cache("downgrade: the write release", &c, CACHE_OP_REL_RW,
               CACHE_MAKE(G, Q, 0, 1), 1, CACHE_ACT_NONE);
    step_cache("downgrade: the read release ends the turn", &c, CACHE_OP_REL_RO,
               CACHE_MAKE(I, Q, 0, 0), 1, CACHE_ACT_REL_RW);
    step_cache("downgrade: the read grant", &c, CACHE_OP_GRANT_RO,
               CACHE_MAKE(I, G, 0, 1), 0, CACHE_ACT_DRAIN_RO);
  }
  {
    /* A create claims a first touch with both requests out: the hold serves
     * the writer and closes the write request; the read request stays out,
     * takes its size round and is granted after the hold's release. */
    struct cache_sim c = {CACHE_MAKE_X(I, I, 1u, 0, 0), 0u};
    step_cache("claim: a reader requests", &c, CACHE_OP_ACQ_RO,
               CACHE_MAKE_X(I, Q, 1u, 0, 1), 0, CACHE_ACT_SEND_RO);
    step_cache("claim: a writer requests", &c, CACHE_OP_ACQ_RW,
               CACHE_MAKE_X(Q, Q, 1u, 1, 1), 0, CACHE_ACT_SEND_RW);
    step_cache("claim: the create takes the hold", &c, CACHE_OP_CREATE_HOLD,
               CACHE_MAKE(G, Q, 2, 0), 1, CACHE_ACT_DRAIN_RW);
    step_cache("claim: a grant under the hold is late", &c, CACHE_OP_GRANT_RW,
               CACHE_MAKE(G, Q, 2, 0), 1, CACHE_ACT_LATE);
    step_cache("claim: the size round re-sends the read request", &c,
               CACHE_OP_CTS_RO, CACHE_MAKE(G, Q, 2, 0), 1, CACHE_ACT_RESEND_RO);
    step_cache("claim: a holder releases", &c, CACHE_OP_REL_RW,
               CACHE_MAKE(G, Q, 1, 0), 1, CACHE_ACT_NONE);
    step_cache("claim: the hold's zero edge", &c, CACHE_OP_REL_RW,
               CACHE_MAKE(I, Q, 0, 0), 1, CACHE_ACT_REL_RW);
    step_cache("claim: the read grant", &c, CACHE_OP_GRANT_RO,
               CACHE_MAKE(I, G, 0, 1), 0, CACHE_ACT_DRAIN_RO);
    step_cache("claim: a hold is refused under the read grant", &c,
               CACHE_OP_CREATE_HOLD, CACHE_MAKE(I, G, 0, 1), 0, CACHE_ACT_NONE);
  }
  cache_walk();

  if (g_rc != 0) {
    return 1;
  }
  printf("PASS excl_compute_next: %d transitions (the lock word: RW/RO "
         "ACQ/REL, D6 rw->rw chain, D7 new-RO-while-RW-waiting, state_bit "
         "normalization, the teardown mark, the pre-claim write request and "
         "every reachable word; the cache word: one request per axis, write "
         "permission that sets the parked readers aside, the read grant that "
         "returns them, and every reachable word with its parked and holding "
         "populations) all correct\n",
         g_checks);
  return 0;
}
#endif /* ARTS_RETENTION_PURGE */

#ifdef ARTS_RETENTION_RETAIN
/* RETAIN's cache arbiter cache_owner_compute_next, its parked-population
 * reading, the readers a serving transition sets aside, and the home arbiter
 * lock_owner_compute_next. */
static uint64_t cache_owner_compute_next(uint64_t cur, int op, uint32_t taken,
                                         uint32_t *out_action);
static uint64_t lock_owner_compute_next(uint64_t cur, int op,
                                        unsigned int new_owner,
                                        uint32_t *out_action);
static inline bool cache_owner_serves_ro(uint64_t s);
static inline uint32_t cache_owner_parked_rw(uint64_t s);
static inline uint32_t cache_owner_parked_ro(uint64_t s);
static inline uint32_t cache_set_aside(uint64_t pre);

static int g_rc = 0;
static int g_checks = 0;

#define NT ARTS_EXCL_NO_TARGET
#define W(own, rws, ros, mt, wc, rc) CACHE_MAKE_FULL(own, rws, ros, mt, wc, rc)

/* What a transition owes besides its word, as its commit site computes it:
 * the writers and readers it drains and the readers it sets aside. */
static uint32_t owed_rw(uint64_t pre, uint32_t act) {
  return act == CACHE_ACT_DRAIN_RW ? cache_owner_parked_rw(pre) : 0u;
}
static uint32_t owed_ro(uint64_t pre, uint32_t taken, uint32_t act) {
  return act == CACHE_ACT_DRAIN_RO ? cache_owner_parked_ro(pre) + taken : 0u;
}
static uint32_t owed_aside(uint64_t pre, uint32_t act) {
  return act == CACHE_ACT_DRAIN_RW ? cache_set_aside(pre) : 0u;
}

/* One step of a cache-word sequence: apply `op` (with `taken` readers
 * returned from ro_aside), check the whole word (the stub, ro_granted and
 * spare bits included), the action and the readers it sets aside, carry
 * the word on. */
static void step_owner_x(const char *name, uint64_t *cur, int op,
                         uint32_t taken, uint64_t want, uint32_t ex_act,
                         uint32_t ex_aside) {
  uint32_t act = 0xffffffff;
  uint64_t next = cache_owner_compute_next(*cur, op, taken, &act);
  uint32_t aside = owed_aside(*cur, act);
  g_checks++;
  if (next != want || act != ex_act || aside != ex_aside) {
    (void)fprintf(stderr,
                  "FAIL [%s]: in %llx op=%d taken=%u => out %llx act=%u "
                  "aside=%u, expected %llx act=%u aside=%u\n",
                  name, (unsigned long long)*cur, op, taken,
                  (unsigned long long)next, act, aside,
                  (unsigned long long)want, ex_act, ex_aside);
    g_rc = 1;
  }
  *cur = next;
}

static void step_owner(const char *name, uint64_t *cur, int op, uint64_t want,
                       uint32_t ex_act) {
  step_owner_x(name, cur, op, 0u, want, ex_act, 0u);
}

static void chk_owner(const char *name, uint64_t cur, int op, uint64_t want,
                      uint32_t ex_act) {
  step_owner(name, &cur, op, want, ex_act);
}

static void chk_owner_x(const char *name, uint64_t cur, int op, uint32_t taken,
                        uint64_t want, uint32_t ex_act, uint32_t ex_aside) {
  step_owner_x(name, &cur, op, taken, want, ex_act, ex_aside);
}

/* A read answer (op), checked with the readers it resumes. */
static void chk_answer(const char *name, uint64_t cur, int op, uint32_t taken,
                       uint64_t want, uint32_t ex_act, uint32_t ex_resumed) {
  uint32_t act = 0xffffffff;
  uint64_t next = cache_owner_compute_next(cur, op, taken, &act);
  uint32_t resumed = owed_ro(cur, taken, act);
  g_checks++;
  if (next != want || act != ex_act || resumed != ex_resumed) {
    (void)fprintf(stderr,
                  "FAIL [%s]: in %llx op=%d taken=%u => out %llx act=%u "
                  "resumed=%u, expected %llx act=%u resumed=%u\n",
                  name, (unsigned long long)cur, op, taken,
                  (unsigned long long)next, act, resumed,
                  (unsigned long long)want, ex_act, ex_resumed);
    g_rc = 1;
  }
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

/* A cache as the commit sites of retain.c drive it: the word and ro_aside,
 * which the committer of a transition that makes the rank serve reads (a
 * migration's DELIVER, a create's hold) adds to and the committer of the
 * read request's answer (a read DELIVER, or the answer the owner gives its
 * own forwarded request) takes whole before its CAS.  The ship, the serve's
 * demote, the self-grant and the recall are the direct CASes of
 * owner_try_execute, owner_serve_readers and excl_recall_on, modelled here
 * on the word they commit. */
#define CW_MAX 3u
struct cw_node {
  uint64_t word;
  uint32_t aside;
  uint8_t parked_r, live_r, parked_w, live_w;
};

enum {
  M_SHIP = 100, /* a migration away: own 1 -> 0, the write grant leaves */
  M_DEMOTE,     /* the serve relinquishes the write grant, the bytes stay */
  M_SELF_GRANT, /* the home forwards this resident rank its own write turn */
  M_RECALL,     /* the home recalls a read grant (or overtakes its answer) */
};

static bool cw_serves(uint64_t w) { return cache_owner_serves_ro(w); }

/* Invariant J of the RETAIN word, the parked and live acquires the ghost
 * counts follow, and the facts the commit sites lean on. */
static bool cw_invariant(const struct cw_node *s) {
  uint64_t w = s->word;
  uint32_t own = CACHE_OWNER(w), rws = CACHE_RW_ST(w), ros = CACHE_RO_ST(w);
  uint32_t wc = CACHE_RW_CNT(w), rc = CACHE_RO_CNT(w);
  bool serves = cw_serves(w);
  bool read_grant = ros == CACHE_ST_GRANT || ros == CACHE_ST_GRANT_PURGE;
  return s->parked_r == (serves ? 0u : rc) + s->aside &&
         s->live_r == (serves ? rc : 0u) &&
         s->parked_w == (rws == CACHE_ST_REQ ? wc : 0u) &&
         s->live_w == (rws == CACHE_ST_GRANT ? wc : 0u) &&
         (s->aside == 0u || ros == CACHE_ST_REQ) &&
         (ros == CACHE_ST_REQ) == (s->parked_r != 0u) &&
         (rws == CACHE_ST_REQ) == (s->parked_w != 0u) &&
         (rws != CACHE_ST_IDLE || wc == 0u) &&
         /* at a resident rank the read count is live readers only */
         (own == 0u || s->parked_r == s->aside) &&
         (rws != CACHE_ST_GRANT || own == 1u) &&
         !(read_grant && (own == 1u || rws == CACHE_ST_GRANT)) &&
         !(read_grant && !CACHE_GRANTED(w)) &&
         !(ros == CACHE_ST_IDLE && CACHE_GRANTED(w)) &&
         (CACHE_STUB(w) == 0u ||
          (own == 0u && rws != CACHE_ST_GRANT && !read_grant)) &&
         CACHE_MIGRATE_TARGET(w) == NT &&
         (w & ((uint64_t)1 << 43)) == 0u && rws != 3u;
}

static void cache_walk(void) {
  enum { CAP = 16384 };
  static struct cw_node seen[CAP];
  int n = 0, head = 0;
  bool hit_with_readers_aside = false;
  bool sticky_edge_with_readers_aside = false;
  bool ship_with_readers_aside = false;
  bool own_answer_under_live_writer = false;
  bool aside_summed = false;
  bool read_grant_returns_aside = false;
  bool read_request_under_write_request = false;
  memset(seen, 0, sizeof(seen));
  /* an idle stub, the creator's sticky owner after its release, and a
   * resident rank without the write permission */
  seen[n++].word = W(0, CACHE_ST_IDLE, CACHE_ST_IDLE, NT, 0, 0);
  seen[n++].word = W(1, CACHE_ST_GRANT, CACHE_ST_IDLE, NT, 0, 0);
  seen[n++].word = W(1, CACHE_ST_IDLE, CACHE_ST_IDLE, NT, 0, 0);
  /* and a claimable first touch, which a create's hold alone may claim */
  seen[n++].word = W(0, CACHE_ST_IDLE, CACHE_ST_IDLE, NT, 0, 0) |
                   CACHE_OWNER_STUB_BIT;
  static const int ops[] = {
      CACHE_OP_ACQ_RW,       CACHE_OP_ACQ_RO,       CACHE_OP_GRANT_RW,
      CACHE_OP_GRANT_RO,     CACHE_OP_GRANT_RO_MARKED, CACHE_OP_OWN_RO,
      CACHE_OP_CREATE_HOLD,  CACHE_OP_CTS_RO,       CACHE_OP_REL_RW,
      CACHE_OP_REL_RO,
      M_SHIP,                M_DEMOTE,              M_SELF_GRANT,
      M_RECALL};
  while (head < n) {
    const struct cw_node cur = seen[head++];
    uint64_t w = cur.word;
    uint32_t own = CACHE_OWNER(w), rws = CACHE_RW_ST(w), ros = CACHE_RO_ST(w);
    uint32_t wc = CACHE_RW_CNT(w), rc = CACHE_RO_CNT(w);
    for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); k++) {
      int op = ops[k];
      if ((op == CACHE_OP_ACQ_RW && cur.parked_w + cur.live_w >= CW_MAX) ||
          (op == CACHE_OP_ACQ_RO && cur.parked_r + cur.live_r >= CW_MAX) ||
          (op == CACHE_OP_REL_RW && cur.live_w == 0u) ||
          (op == CACHE_OP_REL_RO && cur.live_r == 0u) ||
          (op == CACHE_OP_CREATE_HOLD && CACHE_STUB(w) == 0u)) {
        continue;
      }
      struct cw_node nx = cur;
      uint32_t act = CACHE_ACT_NONE;
      bool ok = true;
      bool moved = true;
      g_checks++;
      if (op == M_SHIP) {
        /* owner_try_execute's real migration: no live granted writer, no
         * reader counted. */
        if (own != 1u || (rws == CACHE_ST_GRANT && wc != 0u) || rc != 0u) {
          continue;
        }
        ship_with_readers_aside |= cur.aside != 0u;
        nx.word = W(0, rws == CACHE_ST_GRANT ? CACHE_ST_IDLE : rws, ros, NT,
                    wc, rc) |
                  (w & CACHE_OWNER_LEASED_MASK);
      } else if (op == M_DEMOTE) {
        /* owner_serve_readers: resident, the write grant with no writer. */
        if (own != 1u || rws != CACHE_ST_GRANT || wc != 0u) {
          continue;
        }
        nx.word = W(1, CACHE_ST_IDLE, ros, NT, wc, rc) |
                  (w & CACHE_OWNER_LEASED_MASK);
      } else if (op == M_SELF_GRANT) {
        /* owner_try_execute's self-migration: it drains rw_pending only. */
        if (own != 1u || rws != CACHE_ST_REQ ||
            (ros == CACHE_ST_GRANT && rc != 0u)) {
          continue;
        }
        nx.word = W(1, CACHE_ST_GRANT, ros, NT, wc, rc) |
                  (w & CACHE_OWNER_LEASED_MASK);
        nx.live_w += nx.parked_w;
        nx.parked_w = 0u;
      } else if (op == M_RECALL) {
        uint64_t lm = CACHE_OWNER_LEASED_MASK;
        if (ros == CACHE_ST_GRANT && rc == 0u) {
          nx.word = W(own, rws, CACHE_ST_IDLE, NT, wc, rc);
        } else if (ros == CACHE_ST_GRANT) {
          nx.word = W(own, rws, CACHE_ST_GRANT_PURGE, NT, wc, rc) | (w & lm);
        } else if (ros == CACHE_ST_REQ && !CACHE_GRANTED(w)) {
          nx.word = w | lm;
        } else {
          continue;
        }
      } else {
        uint32_t taken = (op == CACHE_OP_GRANT_RO ||
                          op == CACHE_OP_GRANT_RO_MARKED ||
                          op == CACHE_OP_OWN_RO)
                             ? cur.aside
                             : 0u;
        act = 0xffffffff;
        nx.word = cache_owner_compute_next(w, op, taken, &act);
        bool acquire = op == CACHE_OP_ACQ_RW || op == CACHE_OP_ACQ_RO;
        bool release = op == CACHE_OP_REL_RW || op == CACHE_OP_REL_RO;
        if (act == CACHE_ACT_LATE) {
          /* Refused, the word unchanged: never an acquire inside the bound
           * or a holder's release; a message the home never sends. */
          ok = nx.word == w && !acquire && !release;
          moved = false;
        } else if (act == CACHE_ACT_SELF_SERVE) {
          ok = acquire;
          if (op == CACHE_OP_ACQ_RW) {
            nx.live_w++;
          } else {
            nx.live_r++;
            hit_with_readers_aside |= own == 1u && cur.aside != 0u;
          }
        } else if (act == CACHE_ACT_SEND_RW || act == CACHE_ACT_SEND_RO ||
                   act == CACHE_ACT_PARK) {
          if (op == CACHE_OP_ACQ_RW) {
            ok = act ==
                 (rws == CACHE_ST_IDLE ? CACHE_ACT_SEND_RW : CACHE_ACT_PARK);
            nx.parked_w++;
          } else {
            /* A reader nothing here serves opens the read request exactly
             * when that axis is idle, whatever the write axis holds. */
            ok = op == CACHE_OP_ACQ_RO && !cw_serves(w) &&
                 act == (ros == CACHE_ST_IDLE ? CACHE_ACT_SEND_RO
                                              : CACHE_ACT_PARK);
            read_request_under_write_request |=
                act == CACHE_ACT_SEND_RO && rws == CACHE_ST_REQ;
            nx.parked_r++;
          }
        } else if (act == CACHE_ACT_DRAIN_RW) {
          ok = (op == CACHE_OP_GRANT_RW || op == CACHE_OP_CREATE_HOLD) &&
               owed_rw(w, act) == cur.parked_w &&
               owed_aside(w, act) == cur.parked_r - cur.aside;
          nx.aside += owed_aside(w, act);
          nx.live_w += nx.parked_w;
          nx.parked_w = 0u;
          if (op == CACHE_OP_CREATE_HOLD) {
            nx.live_w++;
          }
        } else if (act == CACHE_ACT_DRAIN_RO) {
          ok = (op == CACHE_OP_GRANT_RO || op == CACHE_OP_GRANT_RO_MARKED ||
                op == CACHE_OP_OWN_RO) &&
               owed_ro(w, taken, act) == cur.parked_r &&
               (op != CACHE_OP_OWN_RO || taken == cur.parked_r);
          if (op == CACHE_OP_OWN_RO) {
            own_answer_under_live_writer |= cur.live_w != 0u;
          } else {
            read_grant_returns_aside |= taken != 0u;
          }
          nx.aside = 0u;
          nx.live_r += nx.parked_r;
          nx.parked_r = 0u;
        } else if (act == CACHE_ACT_RESEND_RO) {
          ok = op == CACHE_OP_CTS_RO && ros == CACHE_ST_REQ;
        } else if (release) {
          ok = act == CACHE_ACT_NONE || act == CACHE_ACT_REL_RO;
          if (op == CACHE_OP_REL_RW) {
            nx.live_w--;
            sticky_edge_with_readers_aside |=
                nx.live_w == 0u && cur.aside != 0u && own == 1u;
          } else {
            nx.live_r--;
          }
        } else {
          ok = false;
        }
        if (!moved) {
          continue;
        }
        if (ok && op == CACHE_OP_GRANT_RW) {
          /* a second migration in adds to the readers set aside before */
          aside_summed |= cur.aside != 0u && cache_set_aside(w) != 0u;
        }
      }
      if (!ok || !cw_invariant(&nx)) {
        (void)fprintf(stderr,
                      "FAIL [cache walk]: %llx aside=%u op=%d => %llx "
                      "aside=%u act=%u (parked r=%u w=%u, live r=%u w=%u)\n",
                      (unsigned long long)w, cur.aside, op,
                      (unsigned long long)nx.word, nx.aside, act, nx.parked_r,
                      nx.parked_w, nx.live_r, nx.live_w);
        g_rc = 1;
        return;
      }
      int i = 0;
      while (i < n && !(seen[i].word == nx.word && seen[i].aside == nx.aside &&
                        seen[i].parked_r == nx.parked_r &&
                        seen[i].live_r == nx.live_r &&
                        seen[i].parked_w == nx.parked_w &&
                        seen[i].live_w == nx.live_w)) {
        i++;
      }
      if (i == n) {
        if (n == CAP) {
          (void)fprintf(stderr, "FAIL [cache walk]: too many states\n");
          g_rc = 1;
          return;
        }
        seen[n++] = nx;
      }
    }
  }
  if (!hit_with_readers_aside || !sticky_edge_with_readers_aside ||
      !ship_with_readers_aside || !own_answer_under_live_writer ||
      !aside_summed || !read_grant_returns_aside ||
      !read_request_under_write_request) {
    (void)fprintf(stderr,
                  "FAIL [cache walk]: not reached: a local hit at an owner "
                  "with readers set aside (%d), a sticky zero edge with "
                  "readers set aside (%d), a migration away with readers set "
                  "aside (%d), the owner's answer to its own read under a "
                  "live writer (%d), a second migration adding to the "
                  "readers set aside (%d), a read grant returning readers "
                  "set aside (%d), a read request opened while the write "
                  "request is out (%d)\n",
                  (int)hit_with_readers_aside,
                  (int)sticky_edge_with_readers_aside,
                  (int)ship_with_readers_aside,
                  (int)own_answer_under_live_writer, (int)aside_summed,
                  (int)read_grant_returns_aside,
                  (int)read_request_under_write_request);
    g_rc = 1;
  }
  printf("cache walk: %d states\n", n);
}

int main(void) {
  const uint32_t I = CACHE_ST_IDLE, Q = CACHE_ST_REQ, G = CACHE_ST_GRANT,
                 GP = CACHE_ST_GRANT_PURGE;
  const uint64_t STUB = CACHE_OWNER_STUB_BIT, LEASED = CACHE_OWNER_LEASED_MASK;
  const uint32_t D = 5u; /* a migrate target */
  const uint32_t RCMAX = (uint32_t)CACHE_OWNER_RC_MASK;

  /* ===== ACQ_RW: the count and the decision in one transition ===== */
  chk_owner("rwacq joins the held grant", W(1, G, I, NT, 0, 0), CACHE_OP_ACQ_RW,
            W(1, G, I, NT, 1, 0), CACHE_ACT_SELF_SERVE);
  chk_owner("rwacq parks on the request out", W(0, Q, I, NT, 1, 0),
            CACHE_OP_ACQ_RW, W(0, Q, I, NT, 2, 0), CACHE_ACT_PARK);
  chk_owner("rwacq: a resident rank without the permission requests",
            W(1, I, I, NT, 0, 0), CACHE_OP_ACQ_RW, W(1, Q, I, NT, 1, 0),
            CACHE_ACT_SEND_RW);
  chk_owner("rwacq: a non-owner requests, its read request out",
            W(0, I, Q, NT, 0, 1), CACHE_OP_ACQ_RW, W(0, Q, Q, NT, 1, 1),
            CACHE_ACT_SEND_RW);
  chk_owner("rwacq: a full count is refused",
            W(1, G, I, NT, CACHE_OWNER_WC_MASK, 0), CACHE_OP_ACQ_RW,
            W(1, G, I, NT, CACHE_OWNER_WC_MASK, 0), CACHE_ACT_LATE);

  /* ===== ACQ_RO ===== */
  chk_owner("roacq: the resident copy serves", W(1, I, I, NT, 0, 0),
            CACHE_OP_ACQ_RO, W(1, I, I, NT, 0, 1), CACHE_ACT_SELF_SERVE);
  chk_owner("roacq: resident with a write request out, ro_st untouched",
            W(1, Q, I, NT, 1, 0), CACHE_OP_ACQ_RO, W(1, Q, I, NT, 1, 1),
            CACHE_ACT_SELF_SERVE);
  chk_owner("roacq: resident with its read request out, a local hit",
            W(1, G, Q, NT, 1, 0), CACHE_OP_ACQ_RO, W(1, G, Q, NT, 1, 1),
            CACHE_ACT_SELF_SERVE);
  chk_owner("roacq: the write grant serves", W(1, G, I, NT, 1, 0),
            CACHE_OP_ACQ_RO, W(1, G, I, NT, 1, 1), CACHE_ACT_SELF_SERVE);
  chk_owner("roacq: a retained read grant serves",
            W(0, I, G, NT, 0, 0) | LEASED, CACHE_OP_ACQ_RO,
            W(0, I, G, NT, 0, 1) | LEASED, CACHE_ACT_SELF_SERVE);
  chk_owner("roacq: a grant marked for return serves",
            W(0, I, GP, NT, 0, 1) | LEASED, CACHE_OP_ACQ_RO,
            W(0, I, GP, NT, 0, 2) | LEASED, CACHE_ACT_SELF_SERVE);
  chk_owner("roacq: a non-owner opens a read request", W(0, I, I, NT, 0, 0),
            CACHE_OP_ACQ_RO, W(0, I, Q, NT, 0, 1), CACHE_ACT_SEND_RO);
  chk_owner("roacq: a non-owner opens its own read request beside its write "
            "request",
            W(0, Q, I, NT, 1, 0), CACHE_OP_ACQ_RO, W(0, Q, Q, NT, 1, 1),
            CACHE_ACT_SEND_RO);
  chk_owner("roacq: a first touch's read request keeps the stub",
            W(0, Q, I, NT, 1, 0) | STUB, CACHE_OP_ACQ_RO,
            W(0, Q, Q, NT, 1, 1) | STUB, CACHE_ACT_SEND_RO);
  chk_owner("roacq coalesces on the read request out", W(0, I, Q, NT, 0, 1),
            CACHE_OP_ACQ_RO, W(0, I, Q, NT, 0, 2), CACHE_ACT_PARK);
  chk_owner("roacq coalesces on the read request out, the write request out",
            W(0, Q, Q, NT, 1, 1), CACHE_OP_ACQ_RO, W(0, Q, Q, NT, 1, 2),
            CACHE_ACT_PARK);
  chk_owner("roacq: a full count is refused",
            W(1, I, I, NT, 0, CACHE_OWNER_RC_MASK), CACHE_OP_ACQ_RO,
            W(1, I, I, NT, 0, CACHE_OWNER_RC_MASK), CACHE_ACT_LATE);

  /* ===== the parked populations a serving transition owes ===== */
  chk_parked("parked: both requests out", W(0, Q, Q, NT, 2, 3), 2, 3);
  chk_parked("parked: resident, writers wait for the self-grant",
             W(1, Q, I, NT, 2, 1), 2, 0);
  chk_parked("parked: resident with its read request out, the count live",
             W(1, G, Q, NT, 1, 2), 0, 0);
  chk_parked("parked: under the write grant every count is live",
             W(1, G, I, NT, 2, 1), 0, 0);
  chk_parked("parked: under a read grant the readers are live",
             W(0, I, G, NT, 0, 2) | LEASED, 0, 0);
  chk_parked("parked: under a marked read grant too",
             W(0, Q, GP, NT, 1, 2) | LEASED, 1, 0);
  chk_parked("parked: idle", W(0, I, I, NT, 0, 0), 0, 0);

  /* ===== a migration's DELIVER (write permission and the bytes) ===== */
  chk_owner_x("grant rw: the writers, no reader request out",
              W(0, Q, I, NT, 2, 0) | STUB, CACHE_OP_GRANT_RW, 0u,
              W(1, G, I, NT, 2, 0), CACHE_ACT_DRAIN_RW, 0u);
  chk_owner_x("grant rw: the readers parked on the read request go aside",
              W(0, Q, Q, NT, 1, 3), CACHE_OP_GRANT_RW, 0u,
              W(1, G, Q, NT, 1, 0), CACHE_ACT_DRAIN_RW, 3u);
  chk_owner_x("grant rw: an early recall's token stays with the read request",
              W(0, Q, Q, NT, 1, 1) | LEASED, CACHE_OP_GRANT_RW, 0u,
              W(1, G, Q, NT, 1, 0) | LEASED, CACHE_ACT_DRAIN_RW, 1u);
  chk_owner("grant rw: no write request out is late", W(0, I, Q, NT, 0, 1),
            CACHE_OP_GRANT_RW, W(0, I, Q, NT, 0, 1), CACHE_ACT_LATE);
  chk_owner("grant rw: at an owner is late", W(1, Q, I, NT, 1, 0),
            CACHE_OP_GRANT_RW, W(1, Q, I, NT, 1, 0), CACHE_ACT_LATE);
  chk_owner("grant rw: under a read grant is late",
            W(0, Q, G, NT, 1, 0) | LEASED, CACHE_OP_GRANT_RW,
            W(0, Q, G, NT, 1, 0) | LEASED, CACHE_ACT_LATE);
  chk_owner("grant rw: under a marked read grant is late",
            W(0, Q, GP, NT, 1, 1) | LEASED, CACHE_OP_GRANT_RW,
            W(0, Q, GP, NT, 1, 1) | LEASED, CACHE_ACT_LATE);
  chk_owner("grant rw: readers counted on no read request is late",
            W(0, Q, I, NT, 1, 1), CACHE_OP_GRANT_RW, W(0, Q, I, NT, 1, 1),
            CACHE_ACT_LATE);

  /* ===== a create's hold ===== */
  {
    uint64_t w = W(0, Q, Q, NT, 1, 1) | STUB;
    chk_parked("hold: the claimed first touch's waiters", w, 1, 1);
    step_owner_x("hold: closes the write axis, sets the reader aside", &w,
                 CACHE_OP_CREATE_HOLD, 0u, W(1, G, Q, NT, 2, 0),
                 CACHE_ACT_DRAIN_RW, 1u);
    step_owner("hold: refused on a resident block", &w, CACHE_OP_CREATE_HOLD,
               W(1, G, Q, NT, 2, 0), CACHE_ACT_NONE);
  }
  chk_owner_x("hold: a read request alone", W(0, I, Q, NT, 0, 2) | STUB,
              CACHE_OP_CREATE_HOLD, 0u, W(1, G, Q, NT, 1, 0),
              CACHE_ACT_DRAIN_RW, 2u);
  chk_owner_x("hold: a write request alone", W(0, Q, I, NT, 1, 0) | STUB,
              CACHE_OP_CREATE_HOLD, 0u, W(1, G, I, NT, 2, 0),
              CACHE_ACT_DRAIN_RW, 0u);
  chk_owner("hold: refused under a read grant", W(0, I, G, NT, 0, 0) | LEASED,
            CACHE_OP_CREATE_HOLD, W(0, I, G, NT, 0, 0) | LEASED,
            CACHE_ACT_NONE);
  chk_owner("hold: refused with a migration pending", W(0, I, I, D, 0, 0),
            CACHE_OP_CREATE_HOLD, W(0, I, I, D, 0, 0), CACHE_ACT_NONE);
  chk_owner("hold: a full count is refused",
            W(0, Q, I, NT, CACHE_OWNER_WC_MASK, 0) | STUB, CACHE_OP_CREATE_HOLD,
            W(0, Q, I, NT, CACHE_OWNER_WC_MASK, 0) | STUB, CACHE_ACT_LATE);
  chk_owner("hold: readers counted on no read request is late",
            W(0, Q, I, NT, 1, 1) | STUB, CACHE_OP_CREATE_HOLD,
            W(0, Q, I, NT, 1, 1) | STUB, CACHE_ACT_LATE);

  /* ===== a read DELIVER: the readers parked since and the ones set aside
   * ===== */
  chk_answer("grant ro: the parked readers", W(0, I, Q, NT, 0, 2) | STUB,
             CACHE_OP_GRANT_RO, 0u, W(0, I, G, NT, 0, 2) | LEASED,
             CACHE_ACT_DRAIN_RO, 2u);
  chk_answer("grant ro: the readers set aside and the one parked since",
             W(0, I, Q, NT, 0, 1), CACHE_OP_GRANT_RO, 3u,
             W(0, I, G, NT, 0, 4) | LEASED, CACHE_ACT_DRAIN_RO, 4u);
  chk_answer("grant ro: the readers set aside alone", W(0, I, Q, NT, 0, 0),
             CACHE_OP_GRANT_RO, 2u, W(0, I, G, NT, 0, 2) | LEASED,
             CACHE_ACT_DRAIN_RO, 2u);
  chk_answer("grant ro: the write request out", W(0, Q, Q, NT, 1, 1),
             CACHE_OP_GRANT_RO, 0u, W(0, Q, G, NT, 1, 1) | LEASED,
             CACHE_ACT_DRAIN_RO, 1u);
  chk_answer("grant ro: an early recall marks the grant for return",
             W(0, I, Q, NT, 0, 1) | LEASED, CACHE_OP_GRANT_RO, 0u,
             W(0, I, GP, NT, 0, 1) | LEASED, CACHE_ACT_DRAIN_RO, 1u);
  chk_answer("grant ro: the home marked it for return", W(0, I, Q, NT, 0, 1),
             CACHE_OP_GRANT_RO_MARKED, 2u, W(0, I, GP, NT, 0, 3) | LEASED,
             CACHE_ACT_DRAIN_RO, 3u);
  chk_answer("grant ro: no read request out is late", W(0, I, I, NT, 0, 0),
             CACHE_OP_GRANT_RO, 0u, W(0, I, I, NT, 0, 0), CACHE_ACT_LATE, 0u);
  chk_answer("grant ro: at an owner is late", W(1, I, Q, NT, 0, 0),
             CACHE_OP_GRANT_RO, 1u, W(1, I, Q, NT, 0, 0), CACHE_ACT_LATE, 0u);
  chk_answer("grant ro: under the write grant is late", W(1, G, Q, NT, 1, 0),
             CACHE_OP_GRANT_RO, 1u, W(1, G, Q, NT, 1, 0), CACHE_ACT_LATE, 0u);
  chk_answer("grant ro: no reader to serve is late", W(0, I, Q, NT, 0, 0),
             CACHE_OP_GRANT_RO, 0u, W(0, I, Q, NT, 0, 0), CACHE_ACT_LATE, 0u);
  chk_answer("grant ro: a count past the field is late",
             W(0, I, Q, NT, 0, RCMAX), CACHE_OP_GRANT_RO, 1u,
             W(0, I, Q, NT, 0, RCMAX), CACHE_ACT_LATE, 0u);

  /* ===== the answer the owner gives its own forwarded read request ===== */
  chk_answer("own ro: under a live writer, the write grant untouched",
             W(1, G, Q, NT, 1, 0), CACHE_OP_OWN_RO, 2u, W(1, G, I, NT, 1, 2),
             CACHE_ACT_DRAIN_RO, 2u);
  chk_answer("own ro: beside a live reader, the early token cleared",
             W(1, G, Q, NT, 0, 1) | LEASED, CACHE_OP_OWN_RO, 1u,
             W(1, G, I, NT, 0, 2), CACHE_ACT_DRAIN_RO, 1u);
  chk_answer("own ro: at a demoted owner", W(1, I, Q, NT, 0, 0),
             CACHE_OP_OWN_RO, 3u, W(1, I, I, NT, 0, 3), CACHE_ACT_DRAIN_RO,
             3u);
  chk_answer("own ro: its write request out", W(1, Q, Q, NT, 1, 0),
             CACHE_OP_OWN_RO, 1u, W(1, Q, I, NT, 1, 1), CACHE_ACT_DRAIN_RO,
             1u);
  chk_answer("own ro: not the owner is late", W(0, I, Q, NT, 0, 1),
             CACHE_OP_OWN_RO, 1u, W(0, I, Q, NT, 0, 1), CACHE_ACT_LATE, 0u);
  chk_answer("own ro: no read request out is late", W(1, G, I, NT, 0, 0),
             CACHE_OP_OWN_RO, 0u, W(1, G, I, NT, 0, 0), CACHE_ACT_LATE, 0u);
  chk_answer("own ro: no reader set aside is late", W(1, G, Q, NT, 0, 0),
             CACHE_OP_OWN_RO, 0u, W(1, G, Q, NT, 0, 0), CACHE_ACT_LATE, 0u);
  chk_answer("own ro: a count past the field is late",
             W(1, G, Q, NT, 0, RCMAX), CACHE_OP_OWN_RO, 1u,
             W(1, G, Q, NT, 0, RCMAX), CACHE_ACT_LATE, 0u);

  /* ===== a size round ===== */
  chk_owner("cts re-sends, the stub cleared", W(0, I, Q, NT, 0, 1) | STUB,
            CACHE_OP_CTS_RO, W(0, I, Q, NT, 0, 1), CACHE_ACT_RESEND_RO);
  chk_owner("cts re-sends beside a write request", W(0, Q, Q, NT, 1, 1),
            CACHE_OP_CTS_RO, W(0, Q, Q, NT, 1, 1), CACHE_ACT_RESEND_RO);
  chk_owner("cts re-sends at an owner whose readers are set aside",
            W(1, G, Q, NT, 1, 0), CACHE_OP_CTS_RO, W(1, G, Q, NT, 1, 0),
            CACHE_ACT_RESEND_RO);
  chk_owner("cts keeps an early recall's token", W(0, I, Q, NT, 0, 1) | LEASED,
            CACHE_OP_CTS_RO, W(0, I, Q, NT, 0, 1) | LEASED,
            CACHE_ACT_RESEND_RO);
  chk_owner("cts with no read request out is late", W(0, I, I, NT, 0, 0),
            CACHE_OP_CTS_RO, W(0, I, I, NT, 0, 0), CACHE_ACT_LATE);

  /* ===== releases ===== */
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
  chk_owner("rorel: a marked grant returns at its zero edge",
            W(0, I, GP, NT, 0, 1) | LEASED, CACHE_OP_REL_RO,
            W(0, I, I, NT, 0, 0), CACHE_ACT_REL_RO);
  chk_owner("rorel: an unmarked grant is retained",
            W(0, I, G, NT, 0, 1) | LEASED, CACHE_OP_REL_RO,
            W(0, I, G, NT, 0, 0) | LEASED, CACHE_ACT_REL_RO);
  chk_owner("rorel: a local reader at an owner whose request is out",
            W(1, G, Q, NT, 0, 1) | LEASED, CACHE_OP_REL_RO,
            W(1, G, Q, NT, 0, 0) | LEASED, CACHE_ACT_REL_RO);

  /* ===== sequences, as retain.c's commit sites drive the word ===== */
  {
    /* A request held through the rank's own migration: the two readers
     * parked on it go aside at the DELIVER, a third reader is a local hit,
     * the writer's release keeps the sticky grant with them still aside, and
     * the home forwards the request to this rank, the owner it names: the
     * answer resumes exactly the two. */
    uint64_t w = W(0, I, I, NT, 0, 0);
    uint32_t aside = 0u;
    step_owner("held: R1 opens the read request", &w, CACHE_OP_ACQ_RO,
               W(0, I, Q, NT, 0, 1), CACHE_ACT_SEND_RO);
    step_owner("held: W opens the write request", &w, CACHE_OP_ACQ_RW,
               W(0, Q, Q, NT, 1, 1), CACHE_ACT_SEND_RW);
    step_owner("held: R2 joins the read request", &w, CACHE_OP_ACQ_RO,
               W(0, Q, Q, NT, 1, 2), CACHE_ACT_PARK);
    w |= LEASED; /* a recall overtook the request's answer */
    step_owner_x("held: the migration lands", &w, CACHE_OP_GRANT_RW, 0u,
                 W(1, G, Q, NT, 1, 0) | LEASED, CACHE_ACT_DRAIN_RW, 2u);
    aside += 2u;
    step_owner("held: R3 is a local hit", &w, CACHE_OP_ACQ_RO,
               W(1, G, Q, NT, 1, 1) | LEASED, CACHE_ACT_SELF_SERVE);
    step_owner("held: W releases, the grant sticks", &w, CACHE_OP_REL_RW,
               W(1, G, Q, NT, 0, 1) | LEASED, CACHE_ACT_NONE);
    step_owner_x("held: the owner answers its own request", &w,
                 CACHE_OP_OWN_RO, aside, W(1, G, I, NT, 0, 3),
                 CACHE_ACT_DRAIN_RO, 0u);
    aside = 0u;
    step_owner("held: a reader after the answer is a local hit", &w,
               CACHE_OP_ACQ_RO, W(1, G, I, NT, 0, 4), CACHE_ACT_SELF_SERVE);
  }
  {
    /* A claimable first touch with both requests out: the hold serves the
     * writer and sets the reader aside, the size round re-sends the read
     * request, and the home installed with this rank as owner forwards it
     * back: the answer resumes the reader. */
    uint64_t w = W(0, I, I, NT, 0, 0) | STUB;
    step_owner("claim: R opens the read request", &w, CACHE_OP_ACQ_RO,
               W(0, I, Q, NT, 0, 1) | STUB, CACHE_ACT_SEND_RO);
    step_owner("claim: W opens the write request", &w, CACHE_OP_ACQ_RW,
               W(0, Q, Q, NT, 1, 1) | STUB, CACHE_ACT_SEND_RW);
    step_owner_x("claim: the hold", &w, CACHE_OP_CREATE_HOLD, 0u,
                 W(1, G, Q, NT, 2, 0), CACHE_ACT_DRAIN_RW, 1u);
    step_owner("claim: the size round re-sends the read request", &w,
               CACHE_OP_CTS_RO, W(1, G, Q, NT, 2, 0), CACHE_ACT_RESEND_RO);
    step_owner("claim: the creator releases", &w, CACHE_OP_REL_RW,
               W(1, G, Q, NT, 1, 0), CACHE_ACT_NONE);
    step_owner_x("claim: the owner answers its own request", &w,
                 CACHE_OP_OWN_RO, 1u, W(1, G, I, NT, 1, 1), CACHE_ACT_DRAIN_RO,
                 0u);
  }
  {
    /* The bytes leave with the readers still aside (their request was not
     * yet counted at the home); readers that arrive after park on the
     * request, and the next owner's read DELIVER returns all of them. */
    uint64_t w = W(0, Q, Q, NT, 1, 2);
    step_owner_x("away: the migration lands", &w, CACHE_OP_GRANT_RW, 0u,
                 W(1, G, Q, NT, 1, 0), CACHE_ACT_DRAIN_RW, 2u);
    step_owner("away: the writer releases", &w, CACHE_OP_REL_RW,
               W(1, G, Q, NT, 0, 0), CACHE_ACT_NONE);
    w = W(0, I, Q, NT, 0, 0); /* owner_try_execute ships */
    step_owner("away: a reader parks again", &w, CACHE_OP_ACQ_RO,
               W(0, I, Q, NT, 0, 1), CACHE_ACT_PARK);
    step_owner_x("away: the read DELIVER returns three", &w, CACHE_OP_GRANT_RO,
                 2u, W(0, I, G, NT, 0, 3) | LEASED, CACHE_ACT_DRAIN_RO, 0u);
  }
  {
    /* Two migrations in while one read request stays out: each sets its
     * cohort aside and ro_aside sums them. */
    uint64_t w = W(0, Q, Q, NT, 1, 1);
    uint32_t aside = 0u;
    step_owner_x("twice: the first migration", &w, CACHE_OP_GRANT_RW, 0u,
                 W(1, G, Q, NT, 1, 0), CACHE_ACT_DRAIN_RW, 1u);
    aside += 1u;
    step_owner("twice: its writer releases", &w, CACHE_OP_REL_RW,
               W(1, G, Q, NT, 0, 0), CACHE_ACT_NONE);
    w = W(0, I, Q, NT, 0, 0); /* shipped away */
    step_owner("twice: a reader parks", &w, CACHE_OP_ACQ_RO,
               W(0, I, Q, NT, 0, 1), CACHE_ACT_PARK);
    step_owner("twice: a writer requests", &w, CACHE_OP_ACQ_RW,
               W(0, Q, Q, NT, 1, 1), CACHE_ACT_SEND_RW);
    step_owner_x("twice: the second migration", &w, CACHE_OP_GRANT_RW, 0u,
                 W(1, G, Q, NT, 1, 0), CACHE_ACT_DRAIN_RW, 1u);
    aside += 1u;
    chk_answer("twice: the answer resumes both", w, CACHE_OP_OWN_RO, aside,
               W(1, G, I, NT, 1, 2), CACHE_ACT_DRAIN_RO, 2u);
  }

  cache_walk();

  /* ===== the home directory ===== */
  chk_lock("home: a writer at idle migrates", LOCK_MAKE(EXCL_PHASE_IDLE, 0, 0, 0),
           EXCL_OP_RW_ACQ, 0, LOCK_MAKE(EXCL_PHASE_RW, 0, 1, 0),
           EXCL_ACTION_FORWARD_MIGRATE);
  chk_lock("home: a reader at idle is served", LOCK_MAKE(EXCL_PHASE_IDLE, 0, 0, 0),
           EXCL_OP_RO_ACQ, 0, LOCK_MAKE(EXCL_PHASE_RO, 0, 0, 1),
           EXCL_ACTION_FORWARD_SERVE_ONE);
  chk_lock("home: a reader in the read phase is served, a writer waiting",
           LOCK_MAKE(EXCL_PHASE_RO, 0, 1, 1), EXCL_OP_RO_ACQ, 0,
           LOCK_MAKE(EXCL_PHASE_RO, 0, 1, 2), EXCL_ACTION_FORWARD_SERVE_ONE);
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
         "acquire's count and decision in one transition, a reader's own "
         "request beside the write request, the readers a migration or a "
         "create's hold sets aside, the read answers that return them, the "
         "owner's answer to its own forwarded request, the parked "
         "populations, the sticky owner's migration, size rounds and "
         "releases, every reachable word; the home directory) all correct\n",
         g_checks);
  return 0;
}
#endif /* ARTS_RETENTION_RETAIN */

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
