/* SPDX-License-Identifier: Apache-2.0
 *
 * FLUSH arm cache/descriptor layout.
 *
 * @note Internal header.  User code should include @c arts.h.
 */

#ifndef ARTS_MEMORY_COHERENCE_FLUSH_TYPES_H
#define ARTS_MEMORY_COHERENCE_FLUSH_TYPES_H
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file flush/types.h
 * @brief FLUSH arm cache/descriptor layout.
 *
 * The home holds the block's one payload line; every other rank holds only
 * what it needs to reach that line.  A non-home EDT never shares bytes with
 * another EDT of its rank: its acquire lands a private copy (a detached
 * buffer the EDT alone references) and its RW release writes that copy back
 * into the line.  The cache therefore carries no version, no count and no
 * queue — the only per-rank state is the line's wire address and the one
 * PUT credit the home last minted for this rank.
 */

#include "arts/coherence/types_common.h"

struct arts_db_cache_s {
  /* The home's line.  On a non-home stub the slot is empty except while the
   * rank is the block's creator: the creator's own copy sits here until its
   * release writes it back and clears the slot. */
  arts_atomic_shared_ptr_t buffer;
  /* The home line and a creator's copy are owned by this cache, so their
   * last drop recycles them here; only the detached per-EDT copies, which
   * no cache owns, go straight back to the pool. */
  arts_lockfree_pool_t buf_freelist;
  arts_guid_t db_guid;
  uint64_t db_size;
  uint8_t payload_pending; /* see the VAL layout: 1 until a line is installed */
  /* What made this cache (arts_db_init_kind_t): a create's own descriptor,
   * the home's directory, or a dependence's first touch.  A create that
   * claims a first touch's cache turns it into its own
   * (arts_db_create_claims_stub); nothing else writes it after init. */
  uint8_t init_kind;
  /* The home line's wire address, learned from the first response that
   * names it (a fetch response or the create's return), constant for the
   * block's life.  flush_txid is the pairing id the home minted for this
   * rank's next PUT into the line: 0 = none, take the announce round.  A
   * release consumes it with one exchange; the ACK refills it. */
  uint64_t home_line_addr;
  uint64_t home_line_rkey;
  volatile uint64_t flush_txid;
  /* A remote creator's name for this descriptor, carried by its announce and
   * echoed by the home's CREATE_RETURN: the create's line credit is applied
   * only where the two match, never to a later descriptor of the same GUID.
   * 0 on every other cache. */
  uint64_t create_token;
};

struct arts_db_s {
  struct arts_db_cache_s cache; /**< FIRST — the cache aliases the descriptor */
  arts_db_types_t db_type;
  bool home_initialized;
  /* Ranks that hold a stub for this block (fetched it, or were told the
   * line's address at create): the destroy fan-out roster and nothing else. */
  unsigned long *sharers;
  unsigned int sharer_words;
  volatile unsigned int reader;  /**< GPU staging reader lock. */
  volatile unsigned int writer;  /**< GPU staging writer lock. */
  volatile unsigned int version; /**< GPU LC version counter. */
  unsigned int time_stamp;       /**< GPU staging timestamp. */
} ARTS_ALIGNED_MAX;

#ifdef __cplusplus
}
#endif
#endif /* ARTS_MEMORY_COHERENCE_FLUSH_TYPES_H */
