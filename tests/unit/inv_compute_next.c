/* SPDX-License-Identifier: Apache-2.0
 *
 * INV arts_inv_cache_compute_next / inv_round_submit_next — the PURE
 * decisions over the packed reader word [ro|inflight|head_ro] and the round
 * word [entries|OPEN].
 *
 * Both deciders are identical under either write policy (the write policy
 * decides where bytes come FROM, never what a word does), so this one truth
 * table covers the whole protocol.  Expected values are hand-derived, not
 * produced by an oracle that re-implements the functions.  Covered:
 *
 *   cache (reader plane):
 *   - a covering copy makes a read SELF_SERVE with the word UNCHANGED — the
 *     property that makes a read on a valid copy wait-free;
 *   - opening a fetch and joining one are the SAME decision CAS that links
 *     self_idx as the chain head, so a parked node cannot miss what it joined;
 *   - a landed reply grabs the whole chain in that same atom (head -> 0);
 *   - the kill mark (REQ -> REQ_KILL) still serves its cohort once, then owes
 *     the round an ack;
 *   - a reply with no fetch open answers no request: LATE, which the caller
 *     raises;
 *   - a write request's give-back opens a fetch for its riders, leaves them to
 *     a fetch already out, and is raised when it finds them on a valid copy;
 *   - the chain head carries the reader pool's full index width, and a head
 *     with every bit set leaves the request bits beside it untouched;
 *
 *   round word (invalidation round):
 *   - a release that reads an idle word claims it (OPEN, and it runs its own
 *     entry);
 *   - one that reads a held word, with or without entries waiting, pushes
 *     (e|OPEN), so a push never lands on an idle word and only a claim
 *     enters one;
 *   - the hold is bit 0 (an entry's address leaves it clear: arts_malloc's
 *     alignment, asserted at run time where a release submits its entry).
 *
 * There is deliberately NO writer coverage: write ownership is the migrating
 * sentinel grant, which lives in plain counters (coherence/grant.c), not in
 * either word.
 *
 * Built standalone by #including coherence/inv/arbiters.c.  Built and
 * registered only in an INV tree.
 */

#include <stdio.h>

#include "arts/coherence/inv/types.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── word-packing arithmetic (compile-time pins) ───────────────────────── */
_Static_assert(INV_CACHE_RO_SHIFT + INV_CACHE_ST_BITS == 64,
               "cache word must pack to exactly 64 bits");
_Static_assert(INV_CACHE_HEAD_RO_SHIFT == 0 && INV_CACHE_INFLIGHT_SHIFT == 61 &&
                   INV_CACHE_RO_SHIFT == 62,
               "cache word field shifts drifted");
_Static_assert(INV_ROUND_OPEN == 1u, "the round's hold is bit 0");
_Static_assert(INV_CACHE_HEAD_BITS == 29 &&
                   INV_CACHE_HEAD_BITS == ARTS_DB_READER_IDX_BITS &&
                   ARTS_DB_READER_IDX_MAX <= INV_CACHE_HEAD_MASK,
               "the chain head must hold every reader pool index");

#include "core/coherence/inv/arbiters.c"

static int g_fail;

static void expect_cache(const char *what, uint64_t cur, int op, uint32_t idx,
                         uint64_t want_next, uint32_t want_act) {
  uint32_t act = 0xFFFFFFFFu;
  uint64_t next = arts_inv_cache_compute_next(cur, op, idx, &act);
  /* The read plane's checks ignore the request bits (the parity is the
   * sender's); expect_req below checks them whole. */
  if ((next & ~INV_CACHE_REQ_MASK) != want_next || act != want_act) {
    (void)fprintf(stderr,
                  "FAIL inv_compute_next: %s — got word %016llx act %u, "
                  "want word %016llx act %u\n",
                  what, (unsigned long long)next, act,
                  (unsigned long long)want_next, want_act);
    g_fail = 1;
  }
}

static void expect_req(const char *what, uint64_t cur, int op, uint32_t idx,
                       uint64_t want_next, uint32_t want_act) {
  uint32_t act = 0xFFFFFFFFu;
  uint64_t next = arts_inv_cache_compute_next(cur, op, idx, &act);
  if (next != want_next || act != want_act) {
    (void)fprintf(stderr,
                  "FAIL inv_compute_next: %s — got word %016llx act %u, "
                  "want word %016llx act %u\n",
                  what, (unsigned long long)next, act,
                  (unsigned long long)want_next, want_act);
    g_fail = 1;
  }
}

static void expect_submit(const char *what, uintptr_t cur, uintptr_t e,
                          uintptr_t want_next, bool want_claim) {
  bool claim = !want_claim;
  uintptr_t next = inv_round_submit_next(cur, e, &claim);
  if (next != want_next || claim != want_claim) {
    (void)fprintf(stderr,
                  "FAIL inv_compute_next: %s — got word %llx claim %d, "
                  "want word %llx claim %d\n",
                  what, (unsigned long long)next, (int)claim,
                  (unsigned long long)want_next, (int)want_claim);
    g_fail = 1;
  }
}

int main(void) {
  const uint32_t ME = 7u;   /* this acquire's pool node */
  const uint32_t OTH = 12u; /* a node already on the chain */

  /* ---- reader plane ---------------------------------------------------- */

  /* A covering copy is served with NO word change: this is the property that
   * makes a read on a valid copy wait-free. */
  {
    uint64_t w = INV_CACHE_MAKE(INV_RO_VALID, 0u, 0u);
    expect_cache("valid copy read is a no-op self-serve", w,
                 INV_CACHE_OP_ACQ_RO, ME, w, INV_CACHE_ACT_SELF_SERVE);
  }

  /* No copy, no fetch: open one AND park in the same transition. */
  expect_cache("first read opens the fetch and parks",
               INV_CACHE_MAKE(INV_RO_IDLE, 0u, 0u), INV_CACHE_OP_ACQ_RO, ME,
               INV_CACHE_MAKE(INV_RO_REQ, 1u, ME), INV_CACHE_ACT_SEND_RO);

  /* A fetch is open: join it — same CAS, head replaced, no second message. */
  expect_cache("second read joins the open fetch",
               INV_CACHE_MAKE(INV_RO_REQ, 1u, OTH), INV_CACHE_OP_ACQ_RO, ME,
               INV_CACHE_MAKE(INV_RO_REQ, 1u, ME), INV_CACHE_ACT_PARK);

  /* A kill-marked fetch is still joinable: its reply serves the cohort once
   * before the reserved purge, so nobody is stranded. */
  expect_cache("read joins a kill-marked fetch",
               INV_CACHE_MAKE(INV_RO_REQ_KILL, 1u, OTH), INV_CACHE_OP_ACQ_RO,
               ME, INV_CACHE_MAKE(INV_RO_REQ_KILL, 1u, ME),
               INV_CACHE_ACT_PARK);

  /* The reply publishes and grabs the whole chain in one atom. */
  expect_cache("reply publishes and grabs the chain",
               INV_CACHE_MAKE(INV_RO_REQ, 1u, ME), INV_CACHE_OP_DELIVER, 0u,
               INV_CACHE_MAKE(INV_RO_VALID, 0u, 0u), INV_CACHE_ACT_PUBLISH);

  /* A doomed reply serves its cohort, then owes the purge + ack. */
  expect_cache("kill-marked reply serves once, then purges",
               INV_CACHE_MAKE(INV_RO_REQ_KILL, 1u, ME), INV_CACHE_OP_DELIVER,
               0u, INV_CACHE_MAKE(INV_RO_VALID, 0u, 0u),
               INV_CACHE_ACT_PUBLISH_KILL);

  /* A reply with no fetch open answers no request: the caller raises it. */
  expect_cache("orphan reply is late", INV_CACHE_MAKE(INV_RO_IDLE, 0u, 0u),
               INV_CACHE_OP_DELIVER, 0u, INV_CACHE_MAKE(INV_RO_IDLE, 0u, 0u),
               INV_CACHE_ACT_LATE);

  /* Invalidate: retire a copy, mark a fetch, or be idempotent. */
  expect_cache("invalidate retires a valid copy",
               INV_CACHE_MAKE(INV_RO_VALID, 0u, 0u), INV_CACHE_OP_INVALIDATE,
               0u, INV_CACHE_MAKE(INV_RO_IDLE, 0u, 0u),
               INV_CACHE_ACT_PURGE_ACK);
  expect_cache("invalidate marks an open fetch",
               INV_CACHE_MAKE(INV_RO_REQ, 1u, ME), INV_CACHE_OP_INVALIDATE, 0u,
               INV_CACHE_MAKE(INV_RO_REQ_KILL, 1u, ME),
               INV_CACHE_ACT_KILL_MARKED);
  expect_cache("invalidate on nothing is idempotent",
               INV_CACHE_MAKE(INV_RO_IDLE, 0u, 0u), INV_CACHE_OP_INVALIDATE,
               0u, INV_CACHE_MAKE(INV_RO_IDLE, 0u, 0u),
               INV_CACHE_ACT_NOOP_ACK);
  expect_cache("the reserved purge retires the served copy",
               INV_CACHE_MAKE(INV_RO_VALID, 0u, 0u), INV_CACHE_OP_KILL_PURGE,
               0u, INV_CACHE_MAKE(INV_RO_IDLE, 0u, 0u), INV_CACHE_ACT_NONE);

  /* ---- invalidation round ---------------------------------------------- */

  {
    /* Entry addresses as arts_malloc returns them: bit 0 clear. */
    const uintptr_t e1 = (uintptr_t)0x7f0012345670u;
    const uintptr_t e2 = (uintptr_t)0x7f00abcdef10u;
    expect_submit("an idle word is claimed", (uintptr_t)0, e1, INV_ROUND_OPEN,
                  true);
    expect_submit("a held word with nothing waiting takes a push",
                  INV_ROUND_OPEN, e1, e1 | INV_ROUND_OPEN, false);
    expect_submit("a held word with entries waiting takes a push",
                  e1 | INV_ROUND_OPEN, e2, e2 | INV_ROUND_OPEN, false);
  }

  /* The request axes. */
  {
#define W(ro, inf, hro, rw, par, stub)                                         \
  INV_CACHE_WITH_REQ(INV_CACHE_MAKE(ro, inf, hro), rw, par, stub)
    expect_req("a fetch flips the parity", W(INV_RO_IDLE, 0u, 0u, 0u, 0u, 1u),
               INV_CACHE_OP_ACQ_RO, ME, W(INV_RO_REQ, 1u, ME, 0u, 1u, 1u),
               INV_CACHE_ACT_SEND_RO);
    expect_req("a reader rides the write request",
               W(INV_RO_IDLE, 0u, 0u, INV_RW_SENT, 0u, 0u), INV_CACHE_OP_ACQ_RO,
               ME, W(INV_RO_IDLE, 0u, ME, INV_RW_SENT, 0u, 0u),
               INV_CACHE_ACT_PARK);
    expect_req("rw claim", W(INV_RO_REQ, 1u, OTH, 0u, 1u, 1u),
               INV_CACHE_OP_RW_CLAIM, 0u,
               W(INV_RO_REQ, 1u, OTH, INV_RW_OPEN, 1u, 1u), INV_CACHE_ACT_WON);
    expect_req("rw commit under its token",
               W(INV_RO_REQ, 1u, OTH, INV_RW_OPEN, 1u, 1u),
               INV_CACHE_OP_RW_COMMIT, 1u,
               W(INV_RO_REQ, 1u, OTH, INV_RW_SENT, 1u, 1u), INV_CACHE_ACT_WON);
    expect_req("a stale OPEN's commit loses",
               W(INV_RO_IDLE, 0u, 0u, INV_RW_OPEN, 0u, 0u),
               INV_CACHE_OP_RW_COMMIT, 1u,
               W(INV_RO_IDLE, 0u, 0u, INV_RW_OPEN, 0u, 0u), INV_CACHE_ACT_LOST);
    expect_req("a stale OPEN's give-back leaves it",
               W(INV_RO_IDLE, 0u, 0u, INV_RW_OPEN, 0u, 0u), INV_CACHE_OP_RW_GIVE,
               1u, W(INV_RO_IDLE, 0u, 0u, INV_RW_OPEN, 0u, 0u),
               INV_CACHE_ACT_NONE);
    expect_req("the claim takes the chain and closes both axes and the stub",
               W(INV_RO_REQ, 1u, ME, INV_RW_SENT, 1u, 1u), INV_CACHE_OP_CLAIM,
               0u, W(INV_RO_IDLE, 0u, 0u, 0u, 1u, 0u), INV_CACHE_ACT_SERVE);
    expect_req("a covered fetch's size round or dismiss closes it",
               W(INV_RO_REQ, 1u, 0u, 0u, 1u, 0u), INV_CACHE_OP_CTS, 1u,
               W(INV_RO_IDLE, 0u, 0u, 0u, 1u, 0u), INV_CACHE_ACT_NONE);
    expect_req("a dismiss under the write request closes; riders stay",
               W(INV_RO_REQ, 1u, ME, INV_RW_SENT, 1u, 0u), INV_CACHE_OP_CTS, 1u,
               W(INV_RO_IDLE, 0u, ME, INV_RW_SENT, 1u, 0u), INV_CACHE_ACT_NONE);
    expect_req("a dismiss with the wrong parity is late",
               W(INV_RO_REQ, 1u, ME, 0u, 1u, 0u), INV_CACHE_OP_CTS, 0u,
               W(INV_RO_REQ, 1u, ME, 0u, 1u, 0u), INV_CACHE_ACT_LATE);
    expect_req("a kill-marked fetch's size round or dismiss owes the ack",
               W(INV_RO_REQ_KILL, 1u, 0u, 0u, 1u, 0u), INV_CACHE_OP_CTS, 1u,
               W(INV_RO_IDLE, 0u, 0u, 0u, 1u, 0u), INV_CACHE_ACT_CLOSE_ACK);
    expect_req("a kill-marked fetch with riders under a write request closes "
               "and acks",
               W(INV_RO_REQ_KILL, 1u, ME, INV_RW_SENT, 1u, 0u), INV_CACHE_OP_CTS,
               1u, W(INV_RO_IDLE, 0u, ME, INV_RW_SENT, 1u, 0u),
               INV_CACHE_ACT_CLOSE_ACK);
    expect_req("a kill-marked fetch re-sent for waiting readers keeps the mark",
               W(INV_RO_REQ_KILL, 1u, ME, 0u, 1u, 0u), INV_CACHE_OP_CTS, 1u,
               W(INV_RO_REQ_KILL, 1u, ME, 0u, 0u, 0u), INV_CACHE_ACT_RESEND);
    expect_req("a waiting reader's size round re-sends and declaims",
               W(INV_RO_REQ, 1u, ME, 0u, 1u, 1u), INV_CACHE_OP_CTS, 1u,
               W(INV_RO_REQ, 1u, ME, 0u, 0u, 0u), INV_CACHE_ACT_RESEND);
    expect_req("an answer under an OPEN keeps the stub (its token)",
               W(INV_RO_REQ, 1u, ME, INV_RW_OPEN, 1u, 1u), INV_CACHE_OP_CTS,
               1u, W(INV_RO_IDLE, 0u, ME, INV_RW_OPEN, 1u, 1u),
               INV_CACHE_ACT_NONE);
    expect_req("a size round with no fetch out is late",
               W(INV_RO_IDLE, 0u, 0u, 0u, 1u, 0u), INV_CACHE_OP_CTS, 1u,
               W(INV_RO_IDLE, 0u, 0u, 0u, 1u, 0u), INV_CACHE_ACT_LATE);
    expect_req("a grant with no write request out is late",
               W(INV_RO_IDLE, 0u, ME, 0u, 0u, 0u), INV_CACHE_OP_RW_CLEAR, 0u,
               W(INV_RO_IDLE, 0u, ME, 0u, 0u, 0u), INV_CACHE_ACT_LATE);

    /* The write request's give-back: riders with no fetch out get one; a
     * fetch already out keeps them; riders on a valid copy are a state no
     * transition makes (a reader never parks on one, and every way into a
     * valid copy takes the chain), so the caller raises it. */
    expect_req(
        "a give-back opens a fetch for its riders",
        W(INV_RO_IDLE, 0u, ME, INV_RW_OPEN, 0u, 0u), INV_CACHE_OP_RW_GIVE, 0u,
        W(INV_RO_REQ, 1u, ME, INV_RW_IDLE, 1u, 0u), INV_CACHE_ACT_SEND_RO);
    expect_req("a give-back with no riders only closes the OPEN",
               W(INV_RO_IDLE, 0u, 0u, INV_RW_OPEN, 0u, 0u),
               INV_CACHE_OP_RW_GIVE, 0u,
               W(INV_RO_IDLE, 0u, 0u, INV_RW_IDLE, 0u, 0u), INV_CACHE_ACT_NONE);
    expect_req("a give-back leaves its riders to the fetch already out",
               W(INV_RO_REQ, 1u, ME, INV_RW_OPEN, 1u, 0u), INV_CACHE_OP_RW_GIVE,
               0u, W(INV_RO_REQ, 1u, ME, INV_RW_IDLE, 1u, 0u),
               INV_CACHE_ACT_NONE);
    expect_req(
        "a give-back that finds riders on a valid copy is raised",
        W(INV_RO_VALID, 0u, ME, INV_RW_OPEN, 0u, 0u), INV_CACHE_OP_RW_GIVE, 0u,
        W(INV_RO_VALID, 0u, ME, INV_RW_OPEN, 0u, 0u), INV_CACHE_ACT_LATE);

    /* A head with every bit of the field set, beside set request bits. */
    const uint32_t WIDE = (uint32_t)INV_CACHE_HEAD_MASK;
    if (INV_CACHE_HEAD_RO(W(INV_RO_REQ, 1u, WIDE, INV_RW_SENT, 1u, 1u)) !=
            WIDE ||
        INV_CACHE_STUB(W(INV_RO_REQ, 1u, WIDE, INV_RW_IDLE, 0u, 0u)) != 0u ||
        INV_CACHE_PAR(W(INV_RO_REQ, 1u, WIDE, INV_RW_IDLE, 0u, 0u)) != 0u ||
        INV_CACHE_RW(W(INV_RO_REQ, 1u, WIDE, INV_RW_IDLE, 0u, 0u)) != 0u) {
      (void)fprintf(stderr, "FAIL inv_compute_next: a full-width head does "
                            "not round-trip beside the request bits\n");
      g_fail = 1;
    }
    expect_req("a full-width index opens the fetch",
               W(INV_RO_IDLE, 0u, 0u, 0u, 0u, 1u), INV_CACHE_OP_ACQ_RO, WIDE,
               W(INV_RO_REQ, 1u, WIDE, 0u, 1u, 1u), INV_CACHE_ACT_SEND_RO);
    expect_req("a full-width index rides the write request",
               W(INV_RO_IDLE, 0u, OTH, INV_RW_SENT, 1u, 1u),
               INV_CACHE_OP_ACQ_RO, WIDE,
               W(INV_RO_IDLE, 0u, WIDE, INV_RW_SENT, 1u, 1u),
               INV_CACHE_ACT_PARK);
    expect_req("the reply takes a full-width chain; the OPEN keeps the stub",
               W(INV_RO_REQ_KILL, 1u, WIDE, INV_RW_OPEN, 1u, 1u),
               INV_CACHE_OP_DELIVER, 0u,
               W(INV_RO_VALID, 0u, 0u, INV_RW_OPEN, 1u, 1u),
               INV_CACHE_ACT_PUBLISH_KILL);
    expect_req("the claim takes a full-width chain",
               W(INV_RO_REQ, 1u, WIDE, INV_RW_SENT, 0u, 1u),
               INV_CACHE_OP_CLAIM, 0u, W(INV_RO_IDLE, 0u, 0u, 0u, 0u, 0u),
               INV_CACHE_ACT_SERVE);
#undef W
  }

  if (g_fail) {
    return 1;
  }
  printf("PASS inv_compute_next: reader plane + invalidation round truth "
         "table\n");
  return 0;
}
