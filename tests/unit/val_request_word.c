/* SPDX-License-Identifier: Apache-2.0
 *
 * Truth table of VAL's request word (arts_val_req_compute_next): both request
 * axes, the read request's parity, the first-touch stub bit and the two
 * reader chains in one word, so every decision that couples the axes is one
 * CAS.  Checked here: a reader opens a window or parks (on the write request,
 * or for the next window -- never on a request already sent); an answer serves
 * only its window's cohort and re-arms only when no write request is out; a
 * size round closes a covered window, re-sends for a waiting cohort, or hands
 * the cohort to the write request; a write permission takes every chain; the
 * write axis's commit and give-back act only under their OPEN's token; a size
 * round of the write request clears the stub bit; an answer, size round or
 * grant for no request out is LATE and leaves the word unchanged.
 */

#include <stdio.h>

#include "arts/coherence/val/types.h"

#include <stdint.h>

static int g_fail = 0;

static void expect(const char *what, uint64_t cur, int op, uint32_t idx,
                   uint64_t want, uint32_t want_act) {
  uint32_t act = 0xffffffffu;
  uint64_t next = arts_val_req_compute_next(cur, op, idx, &act);
  if (next != want || act != want_act) {
    (void)fprintf(stderr,
                  "FAIL val_request_word: %s -- got word %016llx act %u, want "
                  "word %016llx act %u\n",
                  what, (unsigned long long)next, act, (unsigned long long)want,
                  want_act);
    g_fail = 1;
  }
}

#define W(rw, ro, rp, par, stub, group, next)                                  \
  VREQ_MAKE(rw, ro, rp, par, stub, group, next)
#define I VREQ_RW_IDLE
#define O VREQ_RW_OPEN
#define S VREQ_RW_SENT

int main(void) {
  const uint32_t ME = 7u, G = 12u, N = 300000000u; /* N: a 29-bit index */

  /* readers */
  expect("a reader opens a window", W(I, 0, 0, 0, 0, 0, 0), VREQ_OP_ACQ_RO, ME,
         W(I, 1, 0, 1, 0, ME, 0), VREQ_ACT_SEND_RO);
  expect("a reader parks for the next window", W(I, 1, 0, 1, 0, G, 0),
         VREQ_OP_ACQ_RO, ME, W(I, 1, 0, 1, 0, G, ME), VREQ_ACT_PARK);
  expect("a reader rides the write request", W(S, 0, 0, 0, 0, 0, 0),
         VREQ_OP_ACQ_RO, ME, W(S, 0, 0, 0, 0, 0, ME), VREQ_ACT_PARK);
  expect("a reader rides an OPEN write axis", W(O, 0, 0, 0, 1, 0, 0),
         VREQ_OP_ACQ_RO, N, W(O, 0, 0, 0, 1, 0, N), VREQ_ACT_PARK);

  /* answers */
  expect("an answer serves its cohort", W(I, 1, 0, 1, 0, G, 0),
         VREQ_OP_ANSWER, 1, W(I, 0, 0, 1, 0, 0, 0), VREQ_ACT_SERVE);
  expect("an answer re-arms for the next window", W(I, 1, 0, 1, 0, G, N),
         VREQ_OP_ANSWER, 1, W(I, 1, 0, 0, 0, N, 0), VREQ_ACT_SERVE_SEND);
  expect("an answer leaves riders to the write request", W(S, 1, 0, 1, 0, G, N),
         VREQ_OP_ANSWER, 1, W(S, 0, 0, 1, 0, 0, N), VREQ_ACT_SERVE);
  expect("a covered window's late answer serves nobody", W(I, 1, 0, 1, 0, 0, 0),
         VREQ_OP_ANSWER, 1, W(I, 0, 0, 1, 0, 0, 0), VREQ_ACT_SERVE);
  expect("an answer declaims a non-creator's cache", W(I, 1, 0, 1, 1, G, 0),
         VREQ_OP_ANSWER, 1, W(I, 0, 0, 1, 0, 0, 0), VREQ_ACT_SERVE);
  expect("an answer under an OPEN keeps the stub (its token)",
         W(O, 1, 0, 1, 1, G, 0), VREQ_OP_ANSWER, 1, W(O, 0, 0, 1, 1, 0, 0),
         VREQ_ACT_SERVE);
  expect("an answer with the wrong parity is late", W(I, 1, 0, 1, 0, G, 0),
         VREQ_OP_ANSWER, 0, W(I, 1, 0, 1, 0, G, 0), VREQ_ACT_LATE);
  expect("an answer with no window is late", W(I, 0, 0, 1, 0, 0, ME),
         VREQ_OP_ANSWER, 1, W(I, 0, 0, 1, 0, 0, ME), VREQ_ACT_LATE);
  expect("an answer to a window that rides the write request is late",
         W(S, 1, 1, 1, 0, G, 0), VREQ_OP_ANSWER, 1, W(S, 1, 1, 1, 0, G, 0),
         VREQ_ACT_LATE);

  /* read size rounds and dismisses (the same answer without data) */
  expect("a covered window's dismiss closes it", W(I, 1, 0, 1, 0, 0, 0),
         VREQ_OP_CTS, 1, W(I, 0, 0, 1, 0, 0, 0), VREQ_ACT_NONE);
  expect("a waiting cohort's size round re-sends", W(I, 1, 0, 1, 0, G, 0),
         VREQ_OP_CTS, 1, W(I, 1, 0, 0, 0, G, 0), VREQ_ACT_RESEND);
  expect("readers parked after a cover go out on a fresh request",
         W(I, 1, 0, 1, 0, 0, N), VREQ_OP_CTS, 1, W(I, 1, 0, 0, 0, N, 0),
         VREQ_ACT_RESEND);
  expect("both chains: next waits one more window", W(I, 1, 0, 1, 0, G, N),
         VREQ_OP_CTS, 1, W(I, 1, 0, 0, 0, G, N), VREQ_ACT_RESEND);
  expect("a dismiss before the write grant: the cohort rides it",
         W(S, 1, 0, 1, 0, G, N), VREQ_OP_CTS, 1, W(S, 1, 1, 1, 0, G, N),
         VREQ_ACT_NONE);
  expect("a dismiss with the wrong parity is late", W(S, 1, 0, 1, 0, G, 0),
         VREQ_OP_CTS, 0, W(S, 1, 0, 1, 0, G, 0), VREQ_ACT_LATE);
  expect("a size round with no window is late", W(I, 0, 0, 1, 0, 0, 0),
         VREQ_OP_CTS, 1, W(I, 0, 0, 1, 0, 0, 0), VREQ_ACT_LATE);

  /* write permissions */
  expect("a grant's install takes every chain", W(S, 1, 0, 1, 1, G, N),
         VREQ_OP_COVER, 0, W(S, 1, 0, 1, 1, 0, 0), VREQ_ACT_SERVE);
  expect("a cover closes a window that rides the write request",
         W(S, 1, 1, 1, 0, G, 0), VREQ_OP_COVER, 0, W(S, 0, 0, 1, 0, 0, 0),
         VREQ_ACT_SERVE);
  expect("a grant's take closes the axis and clears the stub",
         W(S, 1, 0, 1, 1, G, N), VREQ_OP_RW_CLEAR, 0, W(I, 1, 0, 1, 0, 0, 0),
         VREQ_ACT_SERVE);
  expect("a grant with no write request out is late", W(I, 0, 0, 0, 0, 0, ME),
         VREQ_OP_RW_CLEAR, 0, W(I, 0, 0, 0, 0, 0, ME), VREQ_ACT_LATE);
  expect("the claim takes the chains, closes both axes, clears the stub",
         W(S, 1, 0, 1, 1, G, N), VREQ_OP_CLAIM, 0, W(I, 0, 0, 1, 0, 0, 0),
         VREQ_ACT_SERVE);
  expect("the claim resets an OPEN", W(O, 0, 0, 0, 1, 0, ME), VREQ_OP_CLAIM, 0,
         W(I, 0, 0, 0, 0, 0, 0), VREQ_ACT_SERVE);

  /* the write axis and its token */
  expect("axis claim", W(I, 1, 0, 1, 1, G, 0), VREQ_OP_RW_CLAIM, 0,
         W(O, 1, 0, 1, 1, G, 0), VREQ_ACT_WON);
  expect("axis claim while out", W(S, 0, 0, 0, 0, 0, 0), VREQ_OP_RW_CLAIM, 0,
         W(S, 0, 0, 0, 0, 0, 0), VREQ_ACT_NONE);
  expect("commit under its token", W(O, 1, 0, 1, 1, G, 0), VREQ_OP_RW_COMMIT, 1,
         W(S, 1, 0, 1, 1, G, 0), VREQ_ACT_WON);
  expect("a stale OPEN's commit loses", W(O, 0, 0, 0, 0, 0, 0),
         VREQ_OP_RW_COMMIT, 1, W(O, 0, 0, 0, 0, 0, 0), VREQ_ACT_LOST);
  expect("a commit with no OPEN loses", W(I, 0, 0, 0, 0, 0, 0),
         VREQ_OP_RW_COMMIT, 0, W(I, 0, 0, 0, 0, 0, 0), VREQ_ACT_LOST);
  expect("give-back", W(O, 0, 0, 0, 0, 0, 0), VREQ_OP_RW_GIVE, 0,
         W(I, 0, 0, 0, 0, 0, 0), VREQ_ACT_NONE);
  expect("a stale OPEN's give-back leaves it", W(O, 0, 0, 0, 0, 0, ME),
         VREQ_OP_RW_GIVE, 1, W(O, 0, 0, 0, 0, 0, ME), VREQ_ACT_NONE);
  expect("give-back re-sends a window that rode the axis",
         W(O, 1, 1, 1, 0, G, 0), VREQ_OP_RW_GIVE, 0, W(I, 1, 0, 0, 0, G, 0),
         VREQ_ACT_RESEND);
  expect("give-back opens a window for riders", W(O, 0, 0, 1, 0, 0, N),
         VREQ_OP_RW_GIVE, 0, W(I, 1, 0, 0, 0, N, 0), VREQ_ACT_SEND_RO);
  expect("give-back re-sends an empty-cohort window with the riders",
         W(O, 1, 1, 1, 0, 0, N), VREQ_OP_RW_GIVE, 0, W(I, 1, 0, 0, 0, N, 0),
         VREQ_ACT_RESEND);

  /* the write size round */
  expect("a write size round clears the stub", W(S, 1, 0, 1, 1, G, 0),
         VREQ_OP_DECLAIM, 0, W(S, 1, 0, 1, 0, G, 0), VREQ_ACT_NONE);
  expect("a write size round with no request out is late",
         W(O, 0, 0, 0, 1, 0, 0), VREQ_OP_DECLAIM, 0, W(O, 0, 0, 0, 1, 0, 0),
         VREQ_ACT_LATE);

  if (g_fail) {
    return 1;
  }
  printf("PASS val_request_word: request word truth table\n");
  return 0;
}

#include "core/coherence/val/arbiters.c"
