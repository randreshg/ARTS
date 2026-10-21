/* SPDX-License-Identifier: Apache-2.0
 *
 * The rank bit-set (arts/rank_bitset.h): one definition for every arm whose
 * rank sets are arts_rank_bitset_s (all but FLUSH, which keeps its own
 * sharers words).
 */

#include "arts/rank_bitset.h"

#include "arts/utils/malloc.h"

void arts_rank_bitset_init(struct arts_rank_bitset_s *r, unsigned int nranks) {
  r->nranks = nranks;
  r->nwords = (nranks + 63) / 64;
  r->words =
      (_Atomic(uint64_t) *)arts_calloc(r->nwords, sizeof(_Atomic(uint64_t)));
}

void arts_rank_bitset_destroy(struct arts_rank_bitset_s *r) {
  arts_free(r->words);
  r->words = NULL;
  r->nwords = 0;
}

bool arts_rank_bitset_set(struct arts_rank_bitset_s *r, unsigned int rank) {
  if (rank >= r->nranks) {
    return false;
  }
  unsigned int word_idx = rank / 64;
  uint64_t bit = (uint64_t)1 << (rank % 64);
  uint64_t prev =
      atomic_fetch_or_explicit(&r->words[word_idx], bit, memory_order_acq_rel);
  return (prev & bit) == 0;
}

bool arts_rank_bitset_put(struct arts_rank_bitset_s *r, unsigned int rank,
                          bool value) {
  if (rank >= r->nranks) {
    return false;
  }
  unsigned int word_idx = rank / 64;
  uint64_t bit = (uint64_t)1 << (rank % 64);
  uint64_t prev =
      value ? atomic_fetch_or_explicit(&r->words[word_idx], bit,
                                       memory_order_acq_rel)
            : atomic_fetch_and_explicit(&r->words[word_idx], ~bit,
                                        memory_order_acq_rel);
  return (prev & bit) != 0;
}

void arts_rank_bitset_for_each(const struct arts_rank_bitset_s *r,
                               void (*cb)(unsigned int rank, void *ctx),
                               void *ctx) {
  for (unsigned int w = 0; w < r->nwords; w++) {
    uint64_t snap = atomic_load_explicit(&r->words[w], memory_order_acquire);
    while (snap) {
      unsigned int b = (unsigned int)__builtin_ctzll(snap);
      cb(w * 64 + b, ctx);
      snap &= snap - 1;
    }
  }
}
