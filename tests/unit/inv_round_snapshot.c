/* SPDX-License-Identifier: Apache-2.0
 *
 * inv_round_snapshot_word — the PURE per-word decision of an invalidation
 * round's roster snapshot.
 *
 * A round takes the roster by XCHG and must decide, for every rank it found,
 * one of three things: retire it (it is a target), leave it out (the
 * write-through home, whose buffer is the canonical copy), or keep it while
 * not retiring it (a batch releaser: its bytes are what the round publishes,
 * and its copy is a sharer's copy from then on) — and a releaser is put in
 * even when the snapshot did not find it, because a creator's seeded copy
 * was never served and this round is the only thing that registers it.  The
 * releaser case is the one that has cost silent staleness: a snapshot that
 * dropped the releaser left the ex-holder's durable copy uninvalidated
 * whenever the next owner's first round beat that owner's CONFIRM, which the
 * WT policy never gates.
 *
 * Expected values are hand-derived.  Built standalone by #including
 * coherence/inv/arbiters.c.  INV-only; self-skips elsewhere.
 */
#include <stdio.h>
#if !defined(ARTS_PROTOCOL_INV)
int main(void) {
  printf("PASS inv_round_snapshot: skipped (INV only; the round's roster "
         "snapshot exists only in that build)\n");
  return 0;
}
#else
#include "arts/coherence/inv/types.h"

#include <limits.h>
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

static bool has_target(const unsigned int *t, unsigned int n, unsigned int r) {
  for (unsigned int i = 0; i < n; i++) {
    if (t[i] == r) {
      return true;
    }
  }
  return false;
}

int main(void) {
  unsigned int targets[8];
  unsigned int n;
  uint64_t keep;
  const unsigned int none = (unsigned int)-1;

  /* 1. A plain sharer is a target and does not stay; the batch's releaser
   *    is registered even though the snapshot did not find it. */
  {
    unsigned int writers[] = {5u};
    n = inv_round_snapshot_word(1ull << 3, 0u, writers, 1u, none, targets, 8u,
                                0u, &keep);
    CHECK(n == 1u && targets[0] == 3u, "sharer 3 must be the one target");
    CHECK((keep & (1ull << 3)) == 0u, "a retired sharer must not stay (keep=%llx)",
          (unsigned long long)keep);
    CHECK(keep == (1ull << 5), "releaser 5 is registered (keep=%llx)",
          (unsigned long long)keep);
  }

  /* 2. The batch's releaser is not a target but keeps its bit. */
  {
    unsigned int writers[] = {5u};
    n = inv_round_snapshot_word((1ull << 3) | (1ull << 5), 0u, writers, 1u,
                                none, targets, 8u, 0u, &keep);
    CHECK(n == 1u && !has_target(targets, n, 5u),
          "releaser 5 must not be a target (n=%u)", n);
    CHECK(keep == (1ull << 5), "releaser 5 must stay a sharer (keep=%llx)",
          (unsigned long long)keep);
  }

  /* 3. Several releasers in one batch all stay; every other rank goes. */
  {
    unsigned int writers[] = {1u, 9u};
    n = inv_round_snapshot_word((1ull << 1) | (1ull << 2) | (1ull << 9) |
                                    (1ull << 40),
                                0u, writers, 2u, none, targets, 8u, 0u, &keep);
    CHECK(n == 2u && has_target(targets, n, 2u) && has_target(targets, n, 40u),
          "ranks 2 and 40 must be the targets (n=%u)", n);
    CHECK(keep == ((1ull << 1) | (1ull << 9)),
          "releasers 1 and 9 must stay (keep=%llx)", (unsigned long long)keep);
  }

  /* 4. The exempt rank is neither a target nor kept, releaser or not. */
  {
    unsigned int writers[] = {0u};
    n = inv_round_snapshot_word((1ull << 0) | (1ull << 6), 0u, writers, 1u,
                                0u, targets, 8u, 0u, &keep);
    CHECK(n == 1u && targets[0] == 6u, "only rank 6 is a target (n=%u)", n);
    CHECK(keep == 0u, "the exempt rank must not stay (keep=%llx)",
          (unsigned long long)keep);
    n = inv_round_snapshot_word((1ull << 0) | (1ull << 6), 0u, writers, 0u,
                                0u, targets, 8u, 0u, &keep);
    CHECK(n == 1u && targets[0] == 6u && keep == 0u,
          "exempt without being a releaser: same answer");
  }

  /* 5. A releaser the snapshot did not find is registered all the same: a
   *    creator's seeded copy was never served, so its first round is the
   *    only thing that ever puts it in. */
  {
    unsigned int writers[] = {7u};
    n = inv_round_snapshot_word(1ull << 2, 0u, writers, 1u, none, targets, 8u,
                                0u, &keep);
    CHECK(n == 1u && targets[0] == 2u, "rank 2 is still the target");
    CHECK(keep == (1ull << 7), "absent releaser 7 must be registered (keep=%llx)",
          (unsigned long long)keep);
    /* ...but only in its own word. */
    n = inv_round_snapshot_word(1ull << 2, 64u, writers, 1u, none, targets, 8u,
                                0u, &keep);
    CHECK(keep == 0u, "releaser 7 belongs to word 0, not word 1 (keep=%llx)",
          (unsigned long long)keep);
  }

  /* 6. Words above the first: base offsets the ranks, bits stay word-local. */
  {
    unsigned int writers[] = {64u + 4u};
    n = inv_round_snapshot_word((1ull << 4) | (1ull << 5), 64u, writers, 1u,
                                none, targets, 8u, 0u, &keep);
    CHECK(n == 1u && targets[0] == 69u, "rank 69 is the target (got %u)",
          n ? targets[0] : 0u);
    CHECK(keep == (1ull << 4), "releaser 68 keeps bit 4 of its word");
  }

  /* 7. Appending continues an earlier word's count, and overflow is reported,
   *    not truncated. */
  {
    unsigned int writers[] = {none};
    n = inv_round_snapshot_word((1ull << 1) | (1ull << 2), 0u, writers, 0u,
                                none, targets, 8u, 6u, &keep);
    CHECK(n == 8u && targets[6] == 1u && targets[7] == 2u,
          "targets append after the running count (n=%u)", n);
    n = inv_round_snapshot_word((1ull << 1) | (1ull << 2) | (1ull << 3), 0u,
                                writers, 0u, none, targets, 8u, 6u, &keep);
    CHECK(n == UINT_MAX, "a third target past the cap must report overflow");
  }

  if (g_fail) {
    return 1;
  }
  printf("PASS inv_round_snapshot: releasers stay sharers, targets retire, the "
         "exempt rank does neither\n");
  return 0;
}
#endif
