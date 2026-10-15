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
 * This header carries ONLY the struct layout: it is included directly by
 * the header that lays out the embedding DB descriptor (which embeds
 * arts_rank_bitset_s by value) and therefore must not depend on any other
 * coherence header.  The rank-bitset FUNCTION declarations live in
 * coherence/directory.h. */

#ifndef ARTS_MEMORY_RANK_BITSET_H
#define ARTS_MEMORY_RANK_BITSET_H

#ifdef __cplusplus
extern "C" {
#endif

#include "arts/utils/stdatomic_compat.h"

#include <stdint.h>

/* `words` is allocated before the descriptor is published and freed after
 * its last user, so the pointer itself is plain.  Its elements are accessed
 * only atomically: by the rank-bitset functions in coherence/directory.h and
 * directly by the arms that set, drain or walk a roster themselves
 * (inv/engine.c, excl/retain.c, excl/directory.c, val/wt.c). */
struct arts_rank_bitset_s {
  _Atomic(uint64_t) *words;
  unsigned int nranks;
  unsigned int nwords;
};

#ifdef __cplusplus
}
#endif

#endif /* ARTS_MEMORY_RANK_BITSET_H */
