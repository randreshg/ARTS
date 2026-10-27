/* SPDX-License-Identifier: Apache-2.0
 *
 * inv_round_keep_word / inv_round_targets_word — the PURE per-word decision
 * of an invalidation round's roster snapshot.
 *
 * A round exchanges each roster word for its keep and must decide, for every
 * rank it found, one of three things: retire it (it is a target), leave it
 * out (the write-through home, whose buffer is the canonical copy), or keep
 * it while not retiring it (a batch releaser: its bytes are what the round
 * publishes, and its copy is a sharer's copy from then on) — and a releaser
 * is put in even when the snapshot did not find it, because a creator's
 * seeded copy was never served and this round is the only thing that
 * registers it.  The releaser case is the one that has cost silent
 * staleness: a snapshot that dropped the releaser left the ex-holder's
 * durable copy uninvalidated whenever the next owner's first round beat that
 * owner's CONFIRM, which the WT policy never gates.  The keep is what the
 * exchange writes, so it must not depend on the word it replaces.
 *
 * Expected values are hand-derived.  Built standalone by #including
 * coherence/inv/arbiters.c.  Built and registered only in an INV tree.
 */
#include <stdio.h>
#include "arts/coherence/inv/types.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "core/coherence/inv/arbiters.c"

static int g_fail;

#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      g_fail = 1;                                                              \
      printf("FAIL inv_round_snapshot: " __VA_ARGS__);                         \
      printf("\n");                                                            \
    }                                                                          \
  } while (0)

int main(void) {
  uint64_t keep;
  uint64_t t;
  const unsigned int none = (unsigned int)-1;

  /* 1. A plain sharer is a target and does not stay; the batch's releaser
   *    is registered even though the snapshot did not find it. */
  {
    unsigned int writers[] = {5u};
    keep = inv_round_keep_word(0u, writers, 1u, none);
    t = inv_round_targets_word(1ull << 3, keep, 0u, none);
    CHECK(t == (1ull << 3), "sharer 3 must be the one target (t=%llx)",
          (unsigned long long)t);
    CHECK(keep == (1ull << 5), "releaser 5 is registered (keep=%llx)",
          (unsigned long long)keep);
  }

  /* 2. The batch's releaser is not a target but keeps its bit. */
  {
    unsigned int writers[] = {5u};
    keep = inv_round_keep_word(0u, writers, 1u, none);
    t = inv_round_targets_word((1ull << 3) | (1ull << 5), keep, 0u, none);
    CHECK(t == (1ull << 3), "releaser 5 must not be a target (t=%llx)",
          (unsigned long long)t);
    CHECK(keep == (1ull << 5), "releaser 5 must stay a sharer (keep=%llx)",
          (unsigned long long)keep);
  }

  /* 3. Several releasers in one batch all stay; every other rank goes. */
  {
    unsigned int writers[] = {1u, 9u};
    keep = inv_round_keep_word(0u, writers, 2u, none);
    t = inv_round_targets_word(
        (1ull << 1) | (1ull << 2) | (1ull << 9) | (1ull << 40), keep, 0u, none);
    CHECK(t == ((1ull << 2) | (1ull << 40)),
          "ranks 2 and 40 must be the targets (t=%llx)", (unsigned long long)t);
    CHECK(keep == ((1ull << 1) | (1ull << 9)),
          "releasers 1 and 9 must stay (keep=%llx)", (unsigned long long)keep);
  }

  /* 4. The exempt rank is neither a target nor kept, releaser or not. */
  {
    unsigned int writers[] = {0u};
    keep = inv_round_keep_word(0u, writers, 1u, 0u);
    t = inv_round_targets_word((1ull << 0) | (1ull << 6), keep, 0u, 0u);
    CHECK(t == (1ull << 6), "only rank 6 is a target (t=%llx)",
          (unsigned long long)t);
    CHECK(keep == 0u, "the exempt rank must not stay (keep=%llx)",
          (unsigned long long)keep);
    keep = inv_round_keep_word(0u, writers, 0u, 0u);
    t = inv_round_targets_word((1ull << 0) | (1ull << 6), keep, 0u, 0u);
    CHECK(t == (1ull << 6) && keep == 0u,
          "exempt without being a releaser: same answer");
    /* An exempt rank above the first word is dropped from its own word. */
    unsigned int hi[] = {130u};
    keep = inv_round_keep_word(128u, hi, 1u, 130u);
    t = inv_round_targets_word((1ull << 2) | (1ull << 3), keep, 128u, 130u);
    CHECK(keep == 0u && t == (1ull << 3),
          "exempt rank 130 is dropped from word 2 (keep=%llx t=%llx)",
          (unsigned long long)keep, (unsigned long long)t);
  }

  /* 5. A releaser the snapshot did not find is registered all the same: a
   *    creator's seeded copy was never served, so its first round is the
   *    only thing that ever puts it in. */
  {
    unsigned int writers[] = {7u};
    keep = inv_round_keep_word(0u, writers, 1u, none);
    t = inv_round_targets_word(1ull << 2, keep, 0u, none);
    CHECK(t == (1ull << 2), "rank 2 is still the target");
    CHECK(keep == (1ull << 7),
          "absent releaser 7 must be registered (keep=%llx)",
          (unsigned long long)keep);
    /* ...but only in its own word. */
    keep = inv_round_keep_word(64u, writers, 1u, none);
    CHECK(keep == 0u, "releaser 7 belongs to word 0, not word 1 (keep=%llx)",
          (unsigned long long)keep);
  }

  /* 6. Words above the first: base offsets the ranks, bits stay word-local. */
  {
    unsigned int writers[] = {64u + 4u};
    keep = inv_round_keep_word(64u, writers, 1u, none);
    t = inv_round_targets_word((1ull << 4) | (1ull << 5), keep, 64u, none);
    CHECK(t == (1ull << 5), "rank 69 is the target (t=%llx)",
          (unsigned long long)t);
    CHECK(keep == (1ull << 4), "releaser 68 keeps bit 4 of its word");
    unsigned int third[] = {128u + 63u};
    keep = inv_round_keep_word(128u, third, 1u, none);
    CHECK(keep == (1ull << 63),
          "releaser 191 keeps bit 63 of word 2 (keep=%llx)",
          (unsigned long long)keep);
    CHECK(inv_round_keep_word(64u, third, 1u, none) == 0u &&
              inv_round_keep_word(192u, third, 1u, none) == 0u,
          "releaser 191 belongs to word 2 alone");
  }

  /* 7. The keep is a function of the batch alone (it takes no snapshot), so
   *    it can be the value the exchange writes; the targets of an empty word
   *    and of a full one follow from it. */
  {
    unsigned int writers[] = {3u, 70u};
    uint64_t k0 = inv_round_keep_word(0u, writers, 2u, none);
    uint64_t k1 = inv_round_keep_word(64u, writers, 2u, none);
    CHECK(k0 == (1ull << 3) && k1 == (1ull << 6),
          "the keep names each releaser in its word (k0=%llx k1=%llx)",
          (unsigned long long)k0, (unsigned long long)k1);
    CHECK(inv_round_targets_word(0u, k0, 0u, none) == 0u,
          "an empty word names no target");
    CHECK(inv_round_targets_word(~0ull, k0, 0u, none) == ~(1ull << 3),
          "a full word names every rank but the releaser");
  }

  /* 8. A word with every bit set: all 64 ranks are targets, with no cap. */
  {
    keep = inv_round_keep_word(0u, NULL, 0u, none);
    t = inv_round_targets_word(~0ull, keep, 0u, none);
    CHECK(keep == 0u && t == ~0ull && __builtin_popcountll(t) == 64,
          "64 sharers in one word are 64 targets (t=%llx)",
          (unsigned long long)t);
  }

  if (g_fail) {
    return 1;
  }
  printf("PASS inv_round_snapshot: releasers stay sharers, targets retire, the "
         "exempt rank does neither\n");
  return 0;
}
