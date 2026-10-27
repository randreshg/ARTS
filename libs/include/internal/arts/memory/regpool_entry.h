/* SPDX-License-Identifier: Apache-2.0
 *
 * A registered-pool table entry as a lock-free reader takes it: the entry's
 * state and its mapping's range in one 64-bit word,
 *
 *   [base >> 21 : 36 | len >> 21 : 26 | state : 2],
 *
 * so one single-copy-atomic load cannot pair one rewrite's base with another
 * rewrite's length.  Every mapping the pool makes starts and ends on a 2 MiB
 * boundary and a user address lies below 2^57, so the word names any base and
 * any length below 128 TiB; a range it cannot name is refused before anything
 * is published.  State 0 is the free state, so a word never written reads as
 * a free entry.
 */
#ifndef ARTS_MEMORY_REGPOOL_ENTRY_H
#define ARTS_MEMORY_REGPOOL_ENTRY_H

#include <stddef.h>
#include <stdint.h>

#include "arts/system/print.h"

#define ARTS_REGPOOL_ENTRY_GRANULE_SHIFT 21
#define ARTS_REGPOOL_ENTRY_BASE_BITS 36
#define ARTS_REGPOOL_ENTRY_LEN_BITS 26
#define ARTS_REGPOOL_ENTRY_STATE_BITS 2
_Static_assert(ARTS_REGPOOL_ENTRY_BASE_BITS + ARTS_REGPOOL_ENTRY_LEN_BITS +
                       ARTS_REGPOOL_ENTRY_STATE_BITS ==
                   64,
               "a table entry's fields fill exactly one 64-bit word");

/* Where the base field starts in the word. */
#define ARTS_REGPOOL_ENTRY_BASE_SHIFT                                          \
  (ARTS_REGPOOL_ENTRY_LEN_BITS + ARTS_REGPOOL_ENTRY_STATE_BITS)
#define ARTS_REGPOOL_ENTRY_STATE_MASK                                          \
  (((uint64_t)1 << ARTS_REGPOOL_ENTRY_STATE_BITS) - 1)
#define ARTS_REGPOOL_ENTRY_LEN_MASK                                            \
  (((uint64_t)1 << ARTS_REGPOOL_ENTRY_LEN_BITS) - 1)

static inline unsigned arts_regpool_entry_state(uint64_t w) {
  return (unsigned)(w & ARTS_REGPOOL_ENTRY_STATE_MASK);
}

static inline uintptr_t arts_regpool_entry_base(uint64_t w) {
  return (uintptr_t)((w >> ARTS_REGPOOL_ENTRY_BASE_SHIFT)
                     << ARTS_REGPOOL_ENTRY_GRANULE_SHIFT);
}

static inline size_t arts_regpool_entry_len(uint64_t w) {
  return (size_t)(((w >> ARTS_REGPOOL_ENTRY_STATE_BITS) &
                   ARTS_REGPOOL_ENTRY_LEN_MASK)
                  << ARTS_REGPOOL_ENTRY_GRANULE_SHIFT);
}

/* The same range in another state. */
static inline uint64_t arts_regpool_entry_with_state(uint64_t w,
                                                     unsigned state) {
  return (w & ~ARTS_REGPOOL_ENTRY_STATE_MASK) |
         ((uint64_t)state & ARTS_REGPOOL_ENTRY_STATE_MASK);
}

/* The word of an entry in `state` for [base, base + len); a range the word
 * cannot name exactly is a fatal error. */
static inline uint64_t arts_regpool_entry_word(unsigned state, const void *base,
                                               size_t len) {
  const unsigned sh = ARTS_REGPOOL_ENTRY_GRANULE_SHIFT;
  const uint64_t b = (uint64_t)(uintptr_t)base;
  const uint64_t l = (uint64_t)len;
  if (((b | l) & (((uint64_t)1 << sh) - 1)) != 0 ||
      (b >> (sh + ARTS_REGPOOL_ENTRY_BASE_BITS)) != 0 ||
      (l >> (sh + ARTS_REGPOOL_ENTRY_LEN_BITS)) != 0) {
    ARTS_ERROR("regpool: the mapping at %p of %zu bytes cannot be named by "
               "one table word (it takes 2 MiB multiples, a base below 2^57 "
               "and a length below 2^47)",
               base, len);
  }
  return (b >> sh) << ARTS_REGPOOL_ENTRY_BASE_SHIFT |
         (l >> sh) << ARTS_REGPOOL_ENTRY_STATE_BITS |
         ((uint64_t)state & ARTS_REGPOOL_ENTRY_STATE_MASK);
}

#endif /* ARTS_MEMORY_REGPOOL_ENTRY_H */
