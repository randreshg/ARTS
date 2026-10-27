/* SPDX-License-Identifier: Apache-2.0
 *
 * Bit-packed atomic rank bit-set, sized to the cluster's rank count.
 * Used by the VAL, INV and EXCL homes (the destroy fan-out roster, INV's copy
 * roster, EXCL's read retainers); FLUSH keeps its own sharer bitmap.
 *
 * Each word covers 64 ranks.  At ARTS_GUID_RANK_BITS = 14 (at most 16382
 * ranks), max size is (16382 + 63) / 64 = 256 words = 2 KiB per DB.
 * At experiment scale (<=32 ranks), 1 word = 8 B.
 *
 * The header that lays out the embedding DB descriptor (which embeds
 * arts_rank_bitset_s by value) includes this one directly, so it depends on
 * no coherence header. */

#ifndef ARTS_RANK_BITSET_H
#define ARTS_RANK_BITSET_H

#ifdef __cplusplus
extern "C" {
#endif

#include "arts/utils/stdatomic_compat.h"

#include <stdbool.h>
#include <stdint.h>

/* `words` is allocated before the descriptor is published and freed after
 * its last user, so the pointer itself is plain.  Its elements are accessed
 * only atomically: by the functions below and directly by the arms that
 * drain a roster themselves.  arts_rank_bitset_set is an acq_rel fetch_or,
 * arts_rank_bitset_note a relaxed load and, only when the bit is clear, a
 * relaxed fetch_or, and a cursor (arts_rank_bitset_next) loads each word with
 * acquire.  In a destroy roster and in EXCL's recall roster a bit carries only
 * its own value: the caller sends to the ranks named.  INV's copy roster needs
 * the set's acquire half: a read serve's set and the round's snapshot exchange
 * RMW the same word, so either the round names the reader, or the set reads
 * the exchange and the serve is ordered after the round's stamp (see that
 * roster's declaration). */
struct arts_rank_bitset_s {
  _Atomic(uint64_t) *words;
  unsigned int nranks;
  unsigned int nwords;
};

/* Size the set for `nranks` ranks, every bit clear, before it is published. */
void arts_rank_bitset_init(struct arts_rank_bitset_s *r, unsigned int nranks);
/* Free the words once the set has no other user. */
void arts_rank_bitset_destroy(struct arts_rank_bitset_s *r);
/* Set `rank`'s bit with an acq_rel fetch_or.  True when this call set it,
 * false when it was already set or `rank` is out of range: of concurrent
 * callers on one bit, exactly one sees true. */
bool arts_rank_bitset_set(struct arts_rank_bitset_s *r, unsigned int rank);
/* Set `rank`'s bit unless a relaxed load already finds it; nothing when
 * `rank` is out of range.  For a monotone roster whose bit carries only its
 * own value and is read by a walk that every set it must see happens before
 * (a destroy roster): a skipped set read some set of the bit, which the walk
 * then reads or reads past, and every write of the word is a fetch_or. */
static inline void arts_rank_bitset_note(struct arts_rank_bitset_s *r,
                                         unsigned int rank) {
  if (rank >= r->nranks) {
    return;
  }
  uint64_t bit = (uint64_t)1 << (rank % 64u);
  if ((atomic_load_explicit(&r->words[rank / 64u], memory_order_relaxed) &
       bit) == 0u) {
    (void)atomic_fetch_or_explicit(&r->words[rank / 64u], bit,
                                   memory_order_relaxed);
  }
}
/* Put `value` in `rank`'s bit with one acq_rel RMW of its word (a fetch_or
 * for 1, a fetch_and for 0) and return the bit's previous value; false, with
 * nothing written, when `rank` is out of range.  Of two puts of one value on
 * one bit, exactly one sees the other's. */
bool arts_rank_bitset_put(struct arts_rank_bitset_s *r, unsigned int rank,
                          bool value);
/* A walk over the members of a set, each word read once:
 *
 *   struct arts_rank_bitset_cursor_s c = arts_rank_bitset_cursor(set);
 *   unsigned int rank;
 *   while (arts_rank_bitset_next(&c, &rank)) { ... }
 *
 * yields every rank whose bit was set when its word was read, in ascending
 * order, each once.  A bit set during the walk may be missed, so a caller that
 * needs every member walks only after every set it must see: the destroy
 * fan-out walks at the home's teardown edge, which every request the home
 * processed happens before. */
struct arts_rank_bitset_cursor_s {
  const struct arts_rank_bitset_s *set;
  unsigned int word; /* the next word to read */
  uint64_t bits;     /* the last word read's members not yet yielded */
};

static inline struct arts_rank_bitset_cursor_s
arts_rank_bitset_cursor(const struct arts_rank_bitset_s *r) {
  struct arts_rank_bitset_cursor_s c = {r, 0u, 0u};
  return c;
}

static inline bool arts_rank_bitset_next(struct arts_rank_bitset_cursor_s *c,
                                         unsigned int *rank) {
  while (c->bits == 0u) {
    if (c->word >= c->set->nwords) {
      return false;
    }
    c->bits =
        atomic_load_explicit(&c->set->words[c->word], memory_order_acquire);
    c->word++;
  }
  *rank = (c->word - 1u) * 64u + (unsigned int)__builtin_ctzll(c->bits);
  c->bits &= c->bits - 1u;
  return true;
}

#ifdef __cplusplus
}
#endif

#endif /* ARTS_RANK_BITSET_H */
