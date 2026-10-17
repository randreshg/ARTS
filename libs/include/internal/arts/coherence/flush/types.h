/* SPDX-License-Identifier: Apache-2.0
 *
 * FLUSH arm cache/descriptor layout.
 *
 * @note Internal header.  User code should include @c arts.h.
 */

#ifndef ARTS_COHERENCE_FLUSH_TYPES_H
#define ARTS_COHERENCE_FLUSH_TYPES_H
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
#ifndef ARTS_PROTOCOL_FLUSH
#error                                                                         \
    "flush/types.h is the FLUSH layout; this translation unit's cell is another"
#endif

struct arts_db_cache_s {
  /* The home's line.  On a non-home stub the slot is empty except while the
   * rank is the block's creator: the creator's own copy sits here until its
   * release writes it back and clears the slot.  Orders: a slot's
   * (arts_shared_slot_t, shared.h). */
  _Atomic(arts_shared_slot_t) buffer;
  arts_guid_t db_guid;
  /* The block's exact size once learned, 0 until then; relaxed throughout
   * (see the VAL layout and arts_db_cache_learn_size). */
  _Atomic(uint64_t) db_size;
  /* 1 until a line is installed, 0 after; stored and loaded as in the VAL
   * layout, and as there only the destructor empties the home's slot.  A
   * non-home slot holds only the creator's copy: the create installs it, and
   * the creator's release empties the slot and then stores 1 with release.
   * No other acquisition on that rank reads the slot: each lands a private
   * copy. */
  _Atomic(uint8_t) payload_pending;
  /* What made this cache (arts_db_init_kind_t): a create's own descriptor,
   * the home's directory, or a dependence's first touch.  A create that
   * claims a first touch's cache turns it into its own
   * (arts_db_create_claims_stub); nothing else writes it after init.  Only
   * its own value: one CAS decides between a claim and an answer. */
  _Atomic(uint8_t) init_kind;
  /* The home line's wire address, learned from the first response that
   * names it (a fetch response or the create's return), constant for the
   * block's life.  flush_txid is the pairing id the home minted for this
   * rank's next PUT into the line: 0 = none, take the announce round.  A
   * release consumes it with one exchange; the ACK refills it.  Whoever
   * learns the line (a fetch response, the create's return) stores addr and
   * rkey relaxed and then txid with release; a release takes txid with an
   * acquire exchange, which orders the address and rkey it then loads
   * relaxed.  The ACK's refill of txid is a release store too.  The 0 the
   * exchange leaves publishes nothing: no reader acquires it, and the refill
   * follows the exchange through the round trip it answers. */
  _Atomic(uint64_t) home_line_addr;
  _Atomic(uint64_t) home_line_rkey;
  _Atomic(uint64_t) flush_txid;
  /* A remote creator's name for this descriptor, carried by its announce and
   * echoed by the home's CREATE_RETURN: the create's line credit is applied
   * only where the two match, never to a later descriptor of the same GUID.
   * 0 on every other cache.
   * Minted by the creator with a release store before its announce
   * leaves (the home's stub takes the announced value at init); the
   * credit handler's acquire compare is the only reader that decides
   * anything.  It carries nothing beyond its own value. */
  _Atomic(uint64_t) create_token;
};

struct arts_db_s {
  struct arts_db_cache_s cache; /**< FIRST — the cache aliases the descriptor */
  arts_db_types_t db_type;
  bool home_initialized;
  /* Ranks that hold a stub for this block (fetched it, or were told the
   * line's address at create): the destroy fan-out roster and nothing else.
   * Bits are set by relaxed fetch_or from any handler or worker and read
   * relaxed: only their own value is relied on. */
  _Atomic(unsigned long) *sharers;
  unsigned int sharer_words;
#ifdef ARTS_GPU_LAYOUT
  /* GPU staging state.  The lock exists only for an ARTS_DB_GPU block (NULL
   * otherwise) and is set before the create publishes the descriptor.  A CPU
   * EDT running with the block holds its read side for the whole body;
   * staging and the LC merges hold its write side.  The lock orders every
   * access to version and time_stamp; version is atomic only because
   * concurrent read-side holders bump it. */
  pthread_rwlock_t *gpu_lock;
  _Atomic(unsigned int) version; /* LC version */
  unsigned int time_stamp;       /* LC time stamp */
#endif
} ARTS_ALIGNED_MAX;

#ifdef __cplusplus
}
#endif
#endif /* ARTS_COHERENCE_FLUSH_TYPES_H */
