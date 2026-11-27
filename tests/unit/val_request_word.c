/* SPDX-License-Identifier: Apache-2.0
 *
 * Truth table of VAL's request word (val_req_compute_next): both request
 * axes, the first-touch stub bit and the two reader chains in one word.  The
 * axes are independent: a reader opens a window or joins the next one
 * whatever the write axis holds, an answer serves only its window's cohort
 * and opens the next window for the readers parked since, and a write
 * permission (a grant's clear, a create's claim) touches the write axis and
 * the stub bit only.  The write axis's commit and give-back act only under
 * their OPEN's token; an answer or grant for no request out is LATE and
 * leaves the word unchanged.  An exhaustive walk from the seed words over
 * every operation checks that no reachable word has a reader parked with no
 * window out (next != 0, ro clear) or a window out with nobody in it (ro
 * set, group == 0), and that the two spare bits stay clear.
 */

#include <stdio.h>

#include "arts/coherence/val/types.h"

#include <stdint.h>

/* The arbiter is static to the unit that includes it; this file includes it
 * at its end. */
static uint64_t val_req_compute_next(uint64_t cur, int op, uint32_t idx,
                                     uint32_t *out_act);

static int g_fail = 0;

/* The layout: [rw:2 (63..62) | ro:1 (61) | spare:2 (60..59) | stub:1 (58) |
 * group:29 (57..29) | next:29 (28..0)]. */
#define W(rw, ro, stub, group, next)                                           \
  ((((uint64_t)(rw)) << 62) | (((uint64_t)(ro)) << 61) |                       \
   (((uint64_t)(stub)) << 58) | (((uint64_t)(group)) << 29) |                  \
   ((uint64_t)(next)))
#define SPARE_BITS ((uint64_t)3u << 59)
#define I VREQ_RW_IDLE
#define O VREQ_RW_OPEN
#define S VREQ_RW_SENT

static void expect(const char *what, uint64_t cur, int op, uint32_t idx,
                   uint64_t want, uint32_t want_act) {
  uint32_t act = 0xffffffffu;
  uint64_t next = val_req_compute_next(cur, op, idx, &act);
  if (next != want || act != want_act) {
    (void)fprintf(stderr,
                  "FAIL val_request_word: %s -- got word %016llx act %u, want "
                  "word %016llx act %u\n",
                  what, (unsigned long long)next, act, (unsigned long long)want,
                  want_act);
    g_fail = 1;
  }
}

/* The header's field accessors read the layout above. */
static void check_layout(void) {
  const uint64_t w = W(S, 1, 1, 300000000u, 7u);
  if (VREQ_RW(w) != S || VREQ_RO(w) != 1u || VREQ_STUB(w) != 1u ||
      VREQ_GROUP(w) != 300000000u || VREQ_NEXT(w) != 7u ||
      VREQ_HEAD_BITS != 29) {
    (void)fprintf(stderr, "FAIL val_request_word: the header's layout is not "
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
      "a reader is parked with no window out",
      "a window is out with nobody in it",
      "a spare bit is set",
      "the write axis left its three states",
  };
  static unsigned int reported;
  int kind = -1;
  if (act == VREQ_ACT_LATE && post != pre) {
    kind = 0;
  } else if (VREQ_NEXT(post) != 0u && !VREQ_RO(post)) {
    kind = 1;
  } else if (VREQ_RO(post) && VREQ_GROUP(post) == 0u) {
    kind = 2;
  } else if ((post & SPARE_BITS) != 0u) {
    kind = 3;
  } else if (VREQ_RW(post) > VREQ_RW_SENT) {
    kind = 4;
  }
  if (kind < 0) {
    return;
  }
  g_fail = 1;
  if ((reported & (1u << kind)) == 0u) {
    reported |= 1u << kind;
    (void)fprintf(stderr,
                  "FAIL val_request_word: %s (word %016llx, op %d, idx %u -> "
                  "word %016llx act %u)\n",
                  kinds[kind], (unsigned long long)pre, op, idx,
                  (unsigned long long)post, act);
  }
}

/* Every operation with every argument it is called with: a reader's pool
 * index (two distinct ones, so a chain head can be replaced), and the OPEN's
 * token for the commit and the give-back. */
static void walk(void) {
  static const struct {
    int op;
    uint32_t idx;
  } steps[] = {
      {VREQ_OP_ACQ_RO, 1u},    {VREQ_OP_ACQ_RO, 2u},    {VREQ_OP_ANSWER, 0u},
      {VREQ_OP_RW_CLEAR, 0u},  {VREQ_OP_CLAIM, 0u},     {VREQ_OP_RW_CLAIM, 0u},
      {VREQ_OP_RW_COMMIT, 0u}, {VREQ_OP_RW_COMMIT, 1u}, {VREQ_OP_RW_GIVE, 0u},
      {VREQ_OP_RW_GIVE, 1u},
  };
  g_nseen = 0;
  g_seen[g_nseen++] = W(I, 0, 0, 0, 0);
  g_seen[g_nseen++] = W(I, 0, 1, 0, 0); /* a claimable first touch */
  for (int at = 0; at < g_nseen; at++) {
    uint64_t pre = g_seen[at];
    for (size_t s = 0; s < sizeof(steps) / sizeof(steps[0]); s++) {
      uint32_t act = 0xffffffffu;
      uint64_t post =
          val_req_compute_next(pre, steps[s].op, steps[s].idx, &act);
      judge(pre, steps[s].op, steps[s].idx, post, act);
      if (!seen(post)) {
        if (g_nseen == MAX_WORDS) {
          (void)fprintf(stderr, "FAIL val_request_word: the walk outgrew "
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
  const uint32_t ME = 7u, G = 12u, N = 300000000u; /* N: a 29-bit index */
  static const uint32_t rws[3] = {I, O, S};

  check_layout();

  /* readers: a window opens whatever the write axis holds */
  for (int k = 0; k < 3; k++) {
    uint32_t rw = rws[k];
    expect("a reader opens a window", W(rw, 0, 0, 0, 0), VREQ_OP_ACQ_RO, ME,
           W(rw, 1, 0, ME, 0), VREQ_ACT_SEND_RO);
    expect("a first touch's reader opens a window", W(rw, 0, 1, 0, 0),
           VREQ_OP_ACQ_RO, N, W(rw, 1, 1, N, 0), VREQ_ACT_SEND_RO);
    expect("a reader parks for the next window", W(rw, 1, 0, G, 0),
           VREQ_OP_ACQ_RO, ME, W(rw, 1, 0, G, ME), VREQ_ACT_PARK);
    expect("a reader joins the readers parked for the next window",
           W(rw, 1, 0, G, N), VREQ_OP_ACQ_RO, ME, W(rw, 1, 0, G, ME),
           VREQ_ACT_PARK);
  }

  /* answers: the window's cohort, then the next window, whatever the write
   * axis holds; the stub bit clears unless a writer holds an OPEN (its
   * token) */
  expect("an answer serves its cohort", W(I, 1, 0, G, 0), VREQ_OP_ANSWER, 0,
         W(I, 0, 0, 0, 0), VREQ_ACT_SERVE);
  expect("an answer under a write request serves its cohort",
         W(S, 1, 0, G, 0), VREQ_OP_ANSWER, 0, W(S, 0, 0, 0, 0),
         VREQ_ACT_SERVE);
  for (int k = 0; k < 3; k++) {
    uint32_t rw = rws[k];
    expect("an answer opens the next window", W(rw, 1, 0, G, N),
           VREQ_OP_ANSWER, 0, W(rw, 1, 0, N, 0), VREQ_ACT_SERVE_SEND);
  }
  expect("an answer declaims a non-creator's cache", W(I, 1, 1, G, 0),
         VREQ_OP_ANSWER, 0, W(I, 0, 0, 0, 0), VREQ_ACT_SERVE);
  expect("an answer under a write request declaims the cache",
         W(S, 1, 1, G, N), VREQ_OP_ANSWER, 0, W(S, 1, 0, N, 0),
         VREQ_ACT_SERVE_SEND);
  expect("an answer under an OPEN keeps the stub (its token)",
         W(O, 1, 1, G, 0), VREQ_OP_ANSWER, 0, W(O, 0, 1, 0, 0),
         VREQ_ACT_SERVE);
  expect("an answer under an OPEN re-arms and keeps the stub",
         W(O, 1, 1, G, N), VREQ_OP_ANSWER, 0, W(O, 1, 1, N, 0),
         VREQ_ACT_SERVE_SEND);
  expect("an answer with no window is late", W(I, 0, 0, 0, 0), VREQ_OP_ANSWER,
         0, W(I, 0, 0, 0, 0), VREQ_ACT_LATE);
  expect("an answer with no window under a write request is late",
         W(S, 0, 1, 0, 0), VREQ_OP_ANSWER, 0, W(S, 0, 1, 0, 0),
         VREQ_ACT_LATE);

  /* write permissions: the write axis and the stub bit, never the readers */
  expect("a grant's take closes the write axis and clears the stub",
         W(S, 1, 1, G, N), VREQ_OP_RW_CLEAR, 0, W(I, 1, 0, G, N),
         VREQ_ACT_NONE);
  expect("a grant's take with no reader", W(S, 0, 0, 0, 0), VREQ_OP_RW_CLEAR,
         0, W(I, 0, 0, 0, 0), VREQ_ACT_NONE);
  expect("a grant with no write request out is late", W(I, 1, 0, G, ME),
         VREQ_OP_RW_CLEAR, 0, W(I, 1, 0, G, ME), VREQ_ACT_LATE);
  expect("a grant under an OPEN is late", W(O, 0, 1, 0, 0), VREQ_OP_RW_CLEAR,
         0, W(O, 0, 1, 0, 0), VREQ_ACT_LATE);
  expect("the claim closes the write axis and clears the stub",
         W(S, 1, 1, G, N), VREQ_OP_CLAIM, 0, W(I, 1, 0, G, N), VREQ_ACT_NONE);
  expect("the claim resets an OPEN", W(O, 1, 1, G, 0), VREQ_OP_CLAIM, 0,
         W(I, 1, 0, G, 0), VREQ_ACT_NONE);
  expect("the claim with no request out", W(I, 0, 1, 0, 0), VREQ_OP_CLAIM, 0,
         W(I, 0, 0, 0, 0), VREQ_ACT_NONE);

  /* the write axis and its token */
  expect("axis claim", W(I, 1, 1, G, 0), VREQ_OP_RW_CLAIM, 0,
         W(O, 1, 1, G, 0), VREQ_ACT_WON);
  expect("axis claim while out", W(S, 0, 0, 0, 0), VREQ_OP_RW_CLAIM, 0,
         W(S, 0, 0, 0, 0), VREQ_ACT_NONE);
  expect("axis claim while OPEN", W(O, 1, 0, G, N), VREQ_OP_RW_CLAIM, 0,
         W(O, 1, 0, G, N), VREQ_ACT_NONE);
  expect("commit under its token", W(O, 1, 1, G, 0), VREQ_OP_RW_COMMIT, 1,
         W(S, 1, 1, G, 0), VREQ_ACT_WON);
  expect("a stale OPEN's commit loses", W(O, 0, 0, 0, 0), VREQ_OP_RW_COMMIT, 1,
         W(O, 0, 0, 0, 0), VREQ_ACT_LOST);
  expect("a commit with no OPEN loses", W(I, 0, 0, 0, 0), VREQ_OP_RW_COMMIT, 0,
         W(I, 0, 0, 0, 0), VREQ_ACT_LOST);
  expect("give-back", W(O, 0, 0, 0, 0), VREQ_OP_RW_GIVE, 0, W(I, 0, 0, 0, 0),
         VREQ_ACT_NONE);
  expect("give-back leaves the readers to their window", W(O, 1, 1, G, N),
         VREQ_OP_RW_GIVE, 1, W(I, 1, 1, G, N), VREQ_ACT_NONE);
  expect("a stale OPEN's give-back leaves it", W(O, 1, 0, G, ME),
         VREQ_OP_RW_GIVE, 1, W(O, 1, 0, G, ME), VREQ_ACT_NONE);
  expect("a give-back with no OPEN leaves the word", W(S, 1, 0, G, 0),
         VREQ_OP_RW_GIVE, 0, W(S, 1, 0, G, 0), VREQ_ACT_NONE);

  walk();

  if (g_fail) {
    return 1;
  }
  printf("PASS val_request_word: request word truth table, %d words walked\n",
         g_nseen);
  return 0;
}

#include "core/coherence/val/arbiters.c"
