/* SPDX-License-Identifier: Apache-2.0
 *
 * Bit-packed atomic rank bit-set, sized to the cluster's rank count.
 * Used by the VAL, INV and EXCL homes (the destroy fan-out roster, INV's copy
 * roster, EXCL's read retainers); FLUSH keeps its own sharer bitmap.
 *
 * Each word covers 64 ranks.  At ARTS_GUID_RANK_BITS = 14 (max 16384
 * ranks), max size is (16384 + 63) / 64 = 256 words = 2 KiB per DB.
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
 * only atomically: by the functions below and directly by the arms that set,
 * drain or walk a roster themselves.  arts_rank_bitset_set is an acq_rel
 * fetch_or, and arts_rank_bitset_for_each loads each word with acquire.  In
 * a destroy roster and in EXCL's recall roster a bit carries only its own
 * value: the caller sends to the ranks named.  INV's copy roster needs the
 * set's acquire half: a read serve's set and the round's snapshot exchange
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
/* Call cb(rank, ctx) once for every bit set, reading each word once.  A bit
 * set during the walk may be missed, so a caller that needs every member
 * walks only after every set: the destroy fan-out walks once the block's
 * route-table slot is absent, so no later acquire can register a rank. */
void arts_rank_bitset_for_each(const struct arts_rank_bitset_s *r,
                               void (*cb)(unsigned int rank, void *ctx),
                               void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ARTS_RANK_BITSET_H */
