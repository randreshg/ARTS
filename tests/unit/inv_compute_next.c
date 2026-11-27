/* SPDX-License-Identifier: Apache-2.0
 *
 * INV inv_cache_compute_next / inv_round_submit_next — the PURE
 * decisions over the packed reader word and the round word [entries|OPEN].
 *
 * Both deciders are identical under either read source (the read source
 * decides where bytes come FROM, never what a word does), so this one truth
 * table covers the whole protocol.  Expected values are hand-derived, not
 * produced by an oracle that re-implements the functions.  Covered:
 *
 *   cache (reader plane and the write request's axis):
 *   - a covering copy makes a read SELF_SERVE with the word UNCHANGED — the
 *     property that makes a read on a valid copy wait-free;
 *   - a read with no covering copy opens a fetch, or joins the one out,
 *     whatever the write axis holds, and that is the SAME decision CAS that
 *     links self_idx as the chain head, so a parked node cannot miss what it
 *     joined;
 *   - a landed reply grabs the whole chain in that same atom (head -> 0),
 *     whatever the write axis holds;
 *   - the kill mark (REQ -> REQ_KILL) still serves its cohort once, then owes
 *     the round an ack;
 *   - a write permission (a grant's clear, a create's claim) and a writer's
 *     give-back touch the write axis and the stub bit only;
 *   - a reply or a grant for no request out is LATE, leaving the word;
 *   - the chain head carries the reader pool's full index width beside set
 *     request bits;
 *   - an exhaustive walk from the seed words over every operation: no
 *     reachable word has readers parked with no fetch out and no valid copy,
 *     or a fetch out with nobody parked on it, and the spare bits stay clear;
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
 * There is deliberately NO writer-count coverage: write ownership is the
 * migrating sentinel grant, which lives in plain counters
 * (coherence/grant.c), not in either word.
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

/* The layout: [ro:2 (63..62) | inflight:1 (61) | rw:2 (60..59) |
 * spare:1 (58) | stub:1 (57) | spare:28 (56..29) | head_ro:29 (28..0)]. */
#define W(ro, inf, hro, rw, stub)                                              \
  ((((uint64_t)(ro)) << 62) | (((uint64_t)(inf)) << 61) |                      \
   (((uint64_t)(rw)) << 59) | (((uint64_t)(stub)) << 57) | ((uint64_t)(hro)))
#define SPARE_BITS (((uint64_t)1u << 58) | ((((uint64_t)1u << 28) - 1u) << 29))
#define I INV_RW_IDLE
#define O INV_RW_OPEN
#define S INV_RW_SENT
#define R_IDLE INV_RO_IDLE
#define R_REQ INV_RO_REQ
#define R_KILL INV_RO_REQ_KILL
#define R_VALID INV_RO_VALID

static void expect(const char *what, uint64_t cur, int op, uint32_t idx,
                   uint64_t want, uint32_t want_act) {
  uint32_t act = 0xFFFFFFFFu;
  uint64_t next = inv_cache_compute_next(cur, op, idx, &act);
  if (next != want || act != want_act) {
    (void)fprintf(stderr,
                  "FAIL inv_compute_next: %s -- got word %016llx act %u, "
                  "want word %016llx act %u\n",
                  what, (unsigned long long)next, act, (unsigned long long)want,
                  want_act);
    g_fail = 1;
  }
}

static void expect_submit(const char *what, uintptr_t cur, uintptr_t e,
                          uintptr_t want_next, bool want_claim) {
  bool claim = !want_claim;
  uintptr_t next = inv_round_submit_next(cur, e, &claim);
  if (next != want_next || claim != want_claim) {
    (void)fprintf(stderr,
                  "FAIL inv_compute_next: %s -- got word %llx claim %d, "
                  "want word %llx claim %d\n",
                  what, (unsigned long long)next, (int)claim,
                  (unsigned long long)want_next, (int)want_claim);
    g_fail = 1;
  }
}

/* The header's field accessors read the layout above. */
static void check_layout(void) {
  const uint64_t w = W(R_KILL, 1u, 300000000u, S, 1u);
  if (INV_CACHE_RO(w) != R_KILL || INV_CACHE_INFLIGHT(w) != 1u ||
      INV_CACHE_RW(w) != S || INV_CACHE_STUB(w) != 1u ||
      INV_CACHE_HEAD_RO(w) != 300000000u ||
      INV_CACHE_HEAD_RO(W(R_VALID, 0u, 0u, O, 1u)) != 0u) {
    (void)fprintf(stderr, "FAIL inv_compute_next: the header's layout is not "
                          "the word's\n");
    g_fail = 1;
  }
}

/* ---- the exhaustive walk ---------------------------------------------- */

#define MAX_WORDS 4096
static uint64_t g_seen[MAX_WORDS];
static int g_nseen;

static int seen(uint64_t w) {
  for (int i = 0; i < g_nseen; i++) {
    if (g_seen[i] == w) {
      return 1;
    }
  }
  return 0;
}

/* Each kind of bad word is reported once, at its first occurrence; the walk
 * goes on, so every kind a broken arbiter reaches is named. */
static void judge(uint64_t pre, int op, uint32_t idx, uint64_t post,
                  uint32_t act) {
  static const char *const kinds[] = {
      "a LATE step changed the word",
      "readers are parked with no fetch out and no valid copy",
      "a fetch is out with nobody parked on it",
      "a spare bit is set",
      "the write axis left its three states",
      "a fetch is open while ro is neither REQ nor REQ_KILL",
      "a valid copy has readers parked on it",
  };
  static unsigned int reported;
  uint32_t ro = INV_CACHE_RO(post);
  uint32_t inflight = INV_CACHE_INFLIGHT(post);
  uint32_t hro = INV_CACHE_HEAD_RO(post);
  int kind = -1;
  if (act == INV_CACHE_ACT_LATE && post != pre) {
    kind = 0;
  } else if (hro != 0u && inflight == 0u && ro != R_VALID) {
    kind = 1;
  } else if (inflight != 0u && hro == 0u) {
    kind = 2;
  } else if ((post & SPARE_BITS) != 0u) {
    kind = 3;
  } else if (INV_CACHE_RW(post) > S) {
    kind = 4;
  } else if ((inflight != 0u) != (ro == R_REQ || ro == R_KILL)) {
    kind = 5;
  } else if (ro == R_VALID && hro != 0u) {
    kind = 6;
  }
  if (kind < 0) {
    return;
  }
  g_fail = 1;
  if ((reported & (1u << kind)) == 0u) {
    reported |= 1u << kind;
    (void)fprintf(stderr,
                  "FAIL inv_compute_next: %s (word %016llx, op %d, idx %u -> "
                  "word %016llx act %u)\n",
                  kinds[kind], (unsigned long long)pre, op, idx,
                  (unsigned long long)post, act);
  }
}

/* Every operation with every argument it is called with: a reader's pool
 * index (two distinct ones, so a chain head can be replaced), and the OPEN's
 * token for the commit and the give-back.  The reserved purge runs only on
 * the valid copy its kill-marked reply just made (its caller checks). */
static void walk(void) {
  static const struct {
    int op;
    uint32_t idx;
  } steps[] = {
      {INV_CACHE_OP_ACQ_RO, 1u},     {INV_CACHE_OP_ACQ_RO, 2u},
      {INV_CACHE_OP_DELIVER, 0u},    {INV_CACHE_OP_INVALIDATE, 0u},
      {INV_CACHE_OP_KILL_PURGE, 0u}, {INV_CACHE_OP_RW_CLEAR, 0u},
      {INV_CACHE_OP_CLAIM, 0u},      {INV_CACHE_OP_RW_CLAIM, 0u},
      {INV_CACHE_OP_RW_COMMIT, 0u},  {INV_CACHE_OP_RW_COMMIT, 1u},
      {INV_CACHE_OP_RW_GIVE, 0u},    {INV_CACHE_OP_RW_GIVE, 1u},
  };
  g_nseen = 0;
  g_seen[g_nseen++] = W(R_IDLE, 0u, 0u, I, 0u);
  g_seen[g_nseen++] = W(R_IDLE, 0u, 0u, I, 1u);  /* a claimable first touch */
  g_seen[g_nseen++] = W(R_VALID, 0u, 0u, I, 0u); /* a creator's copy */
  for (int at = 0; at < g_nseen; at++) {
    uint64_t pre = g_seen[at];
    for (size_t s = 0; s < sizeof(steps) / sizeof(steps[0]); s++) {
      if (steps[s].op == INV_CACHE_OP_KILL_PURGE &&
          INV_CACHE_RO(pre) != R_VALID) {
        continue;
      }
      uint32_t act = 0xFFFFFFFFu;
      uint64_t post =
          inv_cache_compute_next(pre, steps[s].op, steps[s].idx, &act);
      judge(pre, steps[s].op, steps[s].idx, post, act);
      if (!seen(post)) {
        if (g_nseen == MAX_WORDS) {
          (void)fprintf(stderr, "FAIL inv_compute_next: the walk outgrew "
                                "its table\n");
          g_fail = 1;
          return;
        }
        g_seen[g_nseen++] = post;
      }
    }
  }
}

int main(void) {
  const uint32_t ME = 7u;   /* this acquire's pool node */
  const uint32_t OTH = 12u; /* a node already on the chain */
  const uint32_t WIDE = (uint32_t)INV_CACHE_HEAD_MASK;
  static const uint32_t rws[3] = {I, O, S};

  check_layout();

  /* ---- reads: the read axis alone decides ------------------------------ */
  for (int k = 0; k < 3; k++) {
    uint32_t rw = rws[k];
    /* A covering copy is served with NO word change: the property that
     * makes a read on a valid copy wait-free. */
    expect("a valid copy's read is a no-op self-serve",
           W(R_VALID, 0u, 0u, rw, 0u), INV_CACHE_OP_ACQ_RO, ME,
           W(R_VALID, 0u, 0u, rw, 0u), INV_CACHE_ACT_SELF_SERVE);
    expect("a reader opens a fetch", W(R_IDLE, 0u, 0u, rw, 0u),
           INV_CACHE_OP_ACQ_RO, ME, W(R_REQ, 1u, ME, rw, 0u),
           INV_CACHE_ACT_SEND_RO);
    expect("a first touch's reader opens a fetch", W(R_IDLE, 0u, 0u, rw, 1u),
           INV_CACHE_OP_ACQ_RO, ME, W(R_REQ, 1u, ME, rw, 1u),
           INV_CACHE_ACT_SEND_RO);
    expect("a reader joins the open fetch", W(R_REQ, 1u, OTH, rw, 0u),
           INV_CACHE_OP_ACQ_RO, ME, W(R_REQ, 1u, ME, rw, 0u),
           INV_CACHE_ACT_PARK);
    /* Its reply serves the cohort once before the reserved purge, so nobody
     * is stranded. */
    expect("a reader joins a kill-marked fetch", W(R_KILL, 1u, OTH, rw, 1u),
           INV_CACHE_OP_ACQ_RO, ME, W(R_KILL, 1u, ME, rw, 1u),
           INV_CACHE_ACT_PARK);
  }
  expect("a full-width index opens the fetch", W(R_IDLE, 0u, 0u, I, 1u),
         INV_CACHE_OP_ACQ_RO, WIDE, W(R_REQ, 1u, WIDE, I, 1u),
         INV_CACHE_ACT_SEND_RO);
  expect("a full-width index joins under a write request",
         W(R_REQ, 1u, OTH, S, 1u), INV_CACHE_OP_ACQ_RO, WIDE,
         W(R_REQ, 1u, WIDE, S, 1u), INV_CACHE_ACT_PARK);

  /* ---- replies: the cohort, whatever the write axis holds; the stub bit
   * clears unless a writer holds an OPEN (its token) ---------------------- */
  for (int k = 0; k < 3; k++) {
    uint32_t rw = rws[k];
    uint32_t stub_after = rw == O ? 1u : 0u;
    expect("a reply publishes and grabs the chain", W(R_REQ, 1u, ME, rw, 1u),
           INV_CACHE_OP_DELIVER, 0u, W(R_VALID, 0u, 0u, rw, stub_after),
           INV_CACHE_ACT_PUBLISH);
    expect("a kill-marked reply serves once, then purges",
           W(R_KILL, 1u, ME, rw, 1u), INV_CACHE_OP_DELIVER, 0u,
           W(R_VALID, 0u, 0u, rw, stub_after), INV_CACHE_ACT_PUBLISH_KILL);
    expect("a reply with no fetch out is late", W(R_IDLE, 0u, 0u, rw, 0u),
           INV_CACHE_OP_DELIVER, 0u, W(R_IDLE, 0u, 0u, rw, 0u),
           INV_CACHE_ACT_LATE);
    expect("a reply on a valid copy is late", W(R_VALID, 0u, 0u, rw, 0u),
           INV_CACHE_OP_DELIVER, 0u, W(R_VALID, 0u, 0u, rw, 0u),
           INV_CACHE_ACT_LATE);
  }
  expect("a reply takes a full-width chain; the OPEN keeps the stub",
         W(R_KILL, 1u, WIDE, O, 1u), INV_CACHE_OP_DELIVER, 0u,
         W(R_VALID, 0u, 0u, O, 1u), INV_CACHE_ACT_PUBLISH_KILL);

  /* ---- invalidations: retire a copy, mark a fetch, or be idempotent ---- */
  expect("an invalidate retires a valid copy", W(R_VALID, 0u, 0u, I, 0u),
         INV_CACHE_OP_INVALIDATE, 0u, W(R_IDLE, 0u, 0u, I, 0u),
         INV_CACHE_ACT_PURGE_ACK);
  for (int k = 0; k < 3; k++) {
    uint32_t rw = rws[k];
    expect("an invalidate marks an open fetch", W(R_REQ, 1u, ME, rw, 0u),
           INV_CACHE_OP_INVALIDATE, 0u, W(R_KILL, 1u, ME, rw, 0u),
           INV_CACHE_ACT_KILL_MARKED);
  }
  expect("an invalidate on nothing is idempotent", W(R_IDLE, 0u, 0u, I, 0u),
         INV_CACHE_OP_INVALIDATE, 0u, W(R_IDLE, 0u, 0u, I, 0u),
         INV_CACHE_ACT_NOOP_ACK);
  expect("the reserved purge retires the served copy",
         W(R_VALID, 0u, 0u, S, 0u), INV_CACHE_OP_KILL_PURGE, 0u,
         W(R_IDLE, 0u, 0u, S, 0u), INV_CACHE_ACT_NONE);

  /* ---- write permissions: the write axis and the stub bit, never the
   * readers --------------------------------------------------------------- */
  expect("a grant's take closes the write axis; the fetch keeps its cohort",
         W(R_REQ, 1u, ME, S, 1u), INV_CACHE_OP_RW_CLEAR, 0u,
         W(R_REQ, 1u, ME, I, 0u), INV_CACHE_ACT_NONE);
  expect("a grant's take leaves a kill-marked fetch", W(R_KILL, 1u, ME, S, 0u),
         INV_CACHE_OP_RW_CLEAR, 0u, W(R_KILL, 1u, ME, I, 0u),
         INV_CACHE_ACT_NONE);
  expect("a grant's take leaves a valid copy", W(R_VALID, 0u, 0u, S, 0u),
         INV_CACHE_OP_RW_CLEAR, 0u, W(R_VALID, 0u, 0u, I, 0u),
         INV_CACHE_ACT_NONE);
  expect("a grant's take with no reader", W(R_IDLE, 0u, 0u, S, 0u),
         INV_CACHE_OP_RW_CLEAR, 0u, W(R_IDLE, 0u, 0u, I, 0u),
         INV_CACHE_ACT_NONE);
  expect("a grant with no write request out is late", W(R_REQ, 1u, ME, I, 0u),
         INV_CACHE_OP_RW_CLEAR, 0u, W(R_REQ, 1u, ME, I, 0u),
         INV_CACHE_ACT_LATE);
  expect("a grant under an OPEN is late", W(R_IDLE, 0u, 0u, O, 1u),
         INV_CACHE_OP_RW_CLEAR, 0u, W(R_IDLE, 0u, 0u, O, 1u),
         INV_CACHE_ACT_LATE);
  expect("the claim closes the write axis; the fetch keeps its cohort",
         W(R_REQ, 1u, ME, S, 1u), INV_CACHE_OP_CLAIM, 0u,
         W(R_REQ, 1u, ME, I, 0u), INV_CACHE_ACT_NONE);
  expect("the claim takes a write axis beside a full-width chain",
         W(R_REQ, 1u, WIDE, S, 1u), INV_CACHE_OP_CLAIM, 0u,
         W(R_REQ, 1u, WIDE, I, 0u), INV_CACHE_ACT_NONE);
  expect("the claim resets an OPEN", W(R_REQ, 1u, ME, O, 1u),
         INV_CACHE_OP_CLAIM, 0u, W(R_REQ, 1u, ME, I, 0u), INV_CACHE_ACT_NONE);
  expect("the claim with no request out", W(R_IDLE, 0u, 0u, I, 1u),
         INV_CACHE_OP_CLAIM, 0u, W(R_IDLE, 0u, 0u, I, 0u), INV_CACHE_ACT_NONE);

  /* ---- the write axis and its token ------------------------------------ */
  expect("axis claim", W(R_REQ, 1u, OTH, I, 1u), INV_CACHE_OP_RW_CLAIM, 0u,
         W(R_REQ, 1u, OTH, O, 1u), INV_CACHE_ACT_WON);
  expect("axis claim while out", W(R_IDLE, 0u, 0u, S, 0u),
         INV_CACHE_OP_RW_CLAIM, 0u, W(R_IDLE, 0u, 0u, S, 0u),
         INV_CACHE_ACT_NONE);
  expect("commit under its token", W(R_REQ, 1u, OTH, O, 1u),
         INV_CACHE_OP_RW_COMMIT, 1u, W(R_REQ, 1u, OTH, S, 1u),
         INV_CACHE_ACT_WON);
  expect("a stale OPEN's commit loses", W(R_IDLE, 0u, 0u, O, 0u),
         INV_CACHE_OP_RW_COMMIT, 1u, W(R_IDLE, 0u, 0u, O, 0u),
         INV_CACHE_ACT_LOST);
  expect("a commit with no OPEN loses", W(R_IDLE, 0u, 0u, I, 0u),
         INV_CACHE_OP_RW_COMMIT, 0u, W(R_IDLE, 0u, 0u, I, 0u),
         INV_CACHE_ACT_LOST);
  expect("give-back", W(R_IDLE, 0u, 0u, O, 0u), INV_CACHE_OP_RW_GIVE, 0u,
         W(R_IDLE, 0u, 0u, I, 0u), INV_CACHE_ACT_NONE);
  expect("give-back leaves the readers to their fetch", W(R_REQ, 1u, ME, O, 1u),
         INV_CACHE_OP_RW_GIVE, 1u, W(R_REQ, 1u, ME, I, 1u), INV_CACHE_ACT_NONE);
  expect("give-back leaves a valid copy", W(R_VALID, 0u, 0u, O, 0u),
         INV_CACHE_OP_RW_GIVE, 0u, W(R_VALID, 0u, 0u, I, 0u),
         INV_CACHE_ACT_NONE);
  expect("a stale OPEN's give-back leaves it", W(R_REQ, 1u, ME, O, 0u),
         INV_CACHE_OP_RW_GIVE, 1u, W(R_REQ, 1u, ME, O, 0u), INV_CACHE_ACT_NONE);
  expect("a give-back with no OPEN leaves the word", W(R_REQ, 1u, ME, S, 0u),
         INV_CACHE_OP_RW_GIVE, 0u, W(R_REQ, 1u, ME, S, 0u), INV_CACHE_ACT_NONE);

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

  walk();

  if (g_fail) {
    return 1;
  }
  printf("PASS inv_compute_next: reader word truth table, %d words walked, "
         "invalidation round\n",
         g_nseen);
  return 0;
}
