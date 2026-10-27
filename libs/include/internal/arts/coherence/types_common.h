/******************************************************************************
** This material was prepared as an account of work sponsored by an agency   **
** of the United States Government.  Neither the United States Government    **
** nor the United States Department of Energy, nor Battelle, nor any of      **
** their employees, nor any jurisdiction or organization that has cooperated **
** in the development of these materials, makes any warranty, express or     **
** implied, or assumes any legal liability or responsibility for the accuracy,*
** completeness, or usefulness or any information, apparatus, product,       **
** software, or process disclosed, or represents that its use would not      **
** infringe privately owned rights.                                          **
**                                                                           **
**                      PACIFIC NORTHWEST NATIONAL LABORATORY                **
**                                  operated by                              **
**                                    BATTELLE                               **
**                                     for the                               **
**                      UNITED STATES DEPARTMENT OF ENERGY                   **
**                         under Contract DE-AC05-76RL01830                  **
**                                                                           **
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License");           **
** you may not use this file except in compliance with the License.          **
******************************************************************************/
#ifndef ARTS_COHERENCE_TYPES_COMMON_H
#define ARTS_COHERENCE_TYPES_COMMON_H
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file types_common.h
 * @brief Protocol-agnostic DB-coherence layout types shared by every
 *        ARTS_COHERENCE_PROTOCOL build.
 *
 * The protocol-specific cache/db ownership layout lives in the per-protocol
 * coherence/<proto>/types.h, selected by arts/coherence/types.h.  Anything
 * here is identical in every protocol that carries it: the buffer, the
 * parked-read nodes, the shared defines/typedefs, and container_of.
 *
 * @note Internal header.  User code should include @c arts.h.
 */

/* A translation unit that sees a coherence layout is compiled for exactly one
 * cell, the cell of the runtime it links: one protocol; under VAL, INV and
 * EXCL one write policy and one release policy (FLUSH has neither axis); and
 * one payload store, which is CXL memory only on the EXCL x PURGE cell, with
 * exactly one residency. */
#if defined(ARTS_PROTOCOL_VAL) + defined(ARTS_PROTOCOL_INV) +                  \
        defined(ARTS_PROTOCOL_EXCL) + defined(ARTS_PROTOCOL_FLUSH) !=          \
    1
#error                                                                         \
    "exactly one of ARTS_PROTOCOL_VAL, ARTS_PROTOCOL_INV, ARTS_PROTOCOL_EXCL and ARTS_PROTOCOL_FLUSH must be defined"
#endif
#if defined(ARTS_PROTOCOL_FLUSH)
#if defined(ARTS_WRITE_POLICY_WT) || defined(ARTS_WRITE_POLICY_WB) ||          \
    defined(ARTS_RELEASE_PURGE) || defined(ARTS_RELEASE_RETAIN)
#error                                                                         \
    "ARTS_PROTOCOL_FLUSH takes no ARTS_WRITE_POLICY_* or ARTS_RELEASE_* macro"
#endif
#elif defined(ARTS_WRITE_POLICY_WT) + defined(ARTS_WRITE_POLICY_WB) != 1
#error                                                                         \
    "exactly one of ARTS_WRITE_POLICY_WT and ARTS_WRITE_POLICY_WB must be defined"
#elif defined(ARTS_RELEASE_PURGE) + defined(ARTS_RELEASE_RETAIN) != 1
#error                                                                         \
    "exactly one of ARTS_RELEASE_PURGE and ARTS_RELEASE_RETAIN must be defined"
#endif
#if defined(ARTS_USE_CXL)
#if !defined(ARTS_PROTOCOL_EXCL) || !defined(ARTS_RELEASE_PURGE)
#error "ARTS_USE_CXL belongs to the EXCL x PURGE cell"
#endif
#if defined(ARTS_CXL_STAGED) + defined(ARTS_CXL_DIRECT) != 1
#error "ARTS_USE_CXL needs exactly one of ARTS_CXL_STAGED and ARTS_CXL_DIRECT"
#endif
#elif defined(ARTS_CXL_STAGED) || defined(ARTS_CXL_DIRECT)
#error "a CXL residency (ARTS_CXL_STAGED, ARTS_CXL_DIRECT) needs ARTS_USE_CXL"
#endif
/* The GPU library links the CPU-compiled object libraries beside the units
 * that compile the GPU code paths (ARTS_USE_GPU), so the descriptor's GPU
 * staging fields belong to every unit of a tree that builds that library
 * (ARTS_GPU_LAYOUT), never to the GPU units alone. */
#if defined(ARTS_USE_GPU) && !defined(ARTS_GPU_LAYOUT)
#error "ARTS_USE_GPU needs ARTS_GPU_LAYOUT"
#endif

#include "arts.h"

#include "arts/defs.h"
#include "arts/gas/guid.h" /* ARTS_GUID_DB_GET_SZHINT (ARTS_DB_STUB_CLAIMABLE) */
#include "arts/utils/lockfree_lifo.h"  /* arts_lf_stack_t / arts_lf_link_t */
#include "arts/utils/lockfree_pool.h"  /* arts_lockfree_pool_t (buf_freelist) */
#include "arts/utils/shared.h"         /* arts_shared_ptr_t (buffer cb) */
#include "arts/utils/stdatomic_compat.h"
#ifdef ARTS_GPU_LAYOUT
#include <pthread.h> /* pthread_rwlock_t (GPU staging lock) */
#endif
#include <assert.h> /* static_assert, in C and in C++ alike */
#include <stdbool.h>
#include <stddef.h> /* offsetof — container_of(cache, arts_db_s, cache) */
#include <stdint.h>
/* The home metadata embeds its destroy roster, a per-rank bit-set, by
 * value. */
#include "arts/rank_bitset.h"
#include "arts/coherence/census.h" /* ARTS_DB_CENSUS */

/* Sentinel for arts_db_cache_s.incoming_new_owner meaning "no ownership
 * transfer pending".  A real rank is always < rank_count, so UINT_MAX is a safe
 * out-of-band value (and rank 0 is a valid owner, so 0 cannot be the sentinel).
 */
#define ARTS_NO_PENDING_OWNER ((unsigned int)-1)

/* Sign bit of arts_db_cache_s.writer_count: this rank holds the grant, but the
 * home has not yet published the directory flip naming it.  The two facts a
 * write turn needs — held, and confirmed — share ONE word so that the
 * turn-taking CAS is itself the test; separate fields cannot be read atomically
 * with each other nor with the CAS, which leaves a hold that is lost and
 * re-granted between the reads indistinguishable from one that was never lost.
 * With the bit set the word reads back negative, which the fast path's
 * `(int)wc <= 0` rejection already refuses.
 *
 * Set and cleared by ADD/SUB of this value rather than by masking: it is its
 * own negation mod 2^32, so either direction toggles it exactly whatever the
 * low bits hold, and it commutes with the ±1 that the sentinel, the drain and
 * every local writer apply.  A word carrying it can never be exactly 0, so the
 * ownership-transfer zero edge cannot be crossed while a hold is unconfirmed.
 * Arms with no migrating grant (EXCL) never set it. */
#define ARTS_GRANT_UNCONFIRMED 0x80000000u

#ifndef ARTS_PROTOCOL_EXCL
/*--- The grant word ------------------------------------------------------
 * writer_count answers two questions — does this rank possess the write
 * right, and how many local writers are inside it — and the release policy
 * decides how the two share the word.
 *
 * RETAIN keeps the sentinel encoding: possession IS a +1 that outlives every
 * writer, so the word is (possession ? 1 : 0) + live writers and only an
 * INVALIDATE ever takes the sentinel back.
 *
 * PURGE gives possession its own bit beside a PURE activity count, because
 * the two are surrendered independently: a holder that reaches its own idle
 * edge hands the right back while its count is already zero, and a home that
 * is idle must still be distinguishable from a home that owns nothing.  Bit
 * 30 carries it — bit 31 is the unconfirmed marker's, and an owned word must
 * stay positive as a signed int so the acquire fast path, the ownership
 * predicate and the covering-read test all keep reading it unchanged.
 *
 * The load-bearing encoding invariant is `own == 0 ⟹ count == 0`: possession
 * is only ever cleared by a transition whose resulting count is zero.  It is
 * what makes `(int)word > 0` mean exactly `own == 1` under both policies, so
 * the fast path and the per-waiter drain are one shared body.
 *
 * An arm with no migrating write right carries the same word as a pure
 * reference count and reaches only the seeds below, whose values are the
 * counts it always used. */
#ifdef ARTS_RELEASE_PURGE
#define ARTS_GRANT_OWN 0x40000000u
#define ARTS_GRANT_COUNT_MASK 0x3fffffffu
#define ARTS_GRANT_OWN_OF(w) (((w) & ARTS_GRANT_OWN) != 0u)
#define ARTS_GRANT_COUNT_OF(w) ((w) & ARTS_GRANT_COUNT_MASK)
/* Seeds — every path that confers possession names it together with the
 * hold underneath; there is no path that seeds a count without possession,
 * and none that adds possession to a word that has lost it (a rank takes the
 * right from a grant or from the create that establishes the block, never
 * from itself). */
#define ARTS_GRANT_SEED_HOLDING (ARTS_GRANT_OWN | 1u)
#define ARTS_GRANT_SEED_IDLE (ARTS_GRANT_OWN)
#else
/* The accessors are diagnostics (tests, oracles); the release-entry guard
 * has its own per-policy predicate and must not be expressed through them —
 * this mapping would fire it on the sentinel-only word, which is exactly the
 * state whose release still owes a transfer. */
#define ARTS_GRANT_OWN_OF(w) ((int)(w) >= 1)
#define ARTS_GRANT_COUNT_OF(w) ((int)(w) >= 1 ? ((w) - 1u) : 0u)
#define ARTS_GRANT_SEED_HOLDING 2u
#define ARTS_GRANT_SEED_IDLE 1u
#endif /* ARTS_RELEASE_PURGE */

#ifdef ARTS_RELEASE_PURGE
/* The count-dropping edge, as a pure function of the word committed by one
 * CAS.  `purgeable` is the caller's own position — a rank that can hand the
 * right back at all (not the home, in a run with peers).  Rows:
 *
 *   own==0                     no-op       a double or spurious release
 *   own==1, c>1                [1, c-1]    other writers remain
 *   own==1, c==1, purgeable    [0, 0]      the idle edge: hand it back
 *   own==1, c==1, !purgeable   [1, 0]      the home goes idle: serve demand
 *   own==1, c==0               ILLEGAL     nothing to drop; a raw decrement
 *                                          here borrows the possession bit
 *                                          and manufactures a count of 2^30-1
 *
 * A next equal to cur means "commit nothing". */
#define ARTS_GRANT_ACT_NONE 0
#define ARTS_GRANT_ACT_RETURN 1
#define ARTS_GRANT_ACT_TAIL 2
#define ARTS_GRANT_ACT_ILLEGAL 3
unsigned int arts_grant_purge_release_next(unsigned int cur, bool purgeable,
                                           unsigned int *out_act);
#endif /* ARTS_RELEASE_PURGE */

/* Home-side single-slot return latch: no returner parked. */
#define ARTS_GRANT_NO_RETURNER ((unsigned int)-1)
#endif /* grant plane */

/* A create announce's owed byte: the requests the first-touch cache a create
 * claimed had already sent its home -- a read request (with its parity) and a
 * write request.  The home tears the block down only once each has arrived. */
#define ARTS_CREATE_OWED_RO 0x1u
#define ARTS_CREATE_OWED_RO_PAR 0x2u
#define ARTS_CREATE_OWED_RW 0x4u

/* Whether a cache a dependence's first touch made can still be claimed by a
 * create: only a GUID the program named before its create can be touched
 * first, and such a GUID (reserved or labeled) carries no size hint, while a
 * create that mints its GUID stamps one.  A cache that can never be claimed
 * keeps the stub bit clear, so its requests are never PRECLAIM. */
#define ARTS_DB_STUB_CLAIMABLE(guid)                                           \
  (ARTS_GUID_DB_GET_SZHINT(guid) == ARTS_GUID_DB_SZHINT_NONE)

/* A request's cover word: a read request carries its parity; a write request
 * declares the read request its rank has out (open + parity) and whether it
 * was opened by a first-touch cache no create had claimed yet. */
#define ARTS_COVER_PAR 0x1u
#define ARTS_COVER_RO_OPEN 0x2u
#define ARTS_COVER_RO_PAR 0x4u
#define ARTS_COVER_PRECLAIM 0x8u

/* In-memory rendezvous landing descriptor — the coherence-side mirror of the
 * packed wire struct arts_msg_rdzv_landing_s (protocol.h): where a payload
 * sender may fi_writedata, keyed for pairing.  txid == 0 means "no landing"
 * everywhere (data-less round / size not yet known).  cookie is the
 * advertiser-local landing-buffer pointer, only ever dereferenced back on the
 * advertiser rank. */
struct arts_rdzv_landing_s {
  uint64_t addr;
  uint64_t key;
  uint64_t txid;
  uint64_t cookie;
};

/* The home's ownership-request FIFO: one queued requester per node.  Producers
 * are request handlers on any thread; the single consumer is the holder of the
 * arm's serving turn.  The stub is the permanent sentinel, embedded and never
 * freed separately.  FLUSH's home keeps no such queue and embeds none.  Operations and orders: coherence/directory.h. */
struct arts_home_grantreq_node_s {
  _Atomic(struct arts_home_grantreq_node_s *) next;
  unsigned int rank;
  uint64_t have_version; /* version the requester already holds */
  struct arts_rdzv_landing_s rdzv; /* the requester's transfer landing */
};

struct arts_home_grantreq_queue_s {
  _Atomic(struct arts_home_grantreq_node_s *) tail; /* producer end */
  _Atomic(struct arts_home_grantreq_node_s *) head; /* consumer end */
  struct arts_home_grantreq_node_s stub;            /* permanent sentinel */
};

/* Arms that install a fresh buffer for every version a rank fetches (VAL's
 * snapshots, INV's reader copies) keep a per-DB free-list of buffers to draw
 * the next one from.  Every other arm draws too rarely for a list to pay: one
 * buffer per rank for the block's whole life, or a detached copy per
 * acquire.
 *
 * Where the free-list exists, a buffer drawn from a cache keeps that cache's
 * storage alive until it is parked back: the descriptor's destructor may run
 * while a transfer still holds the buffer, and the late last drop must still
 * have a list to park into.  The storage release that follows the
 * destructor drains whatever is parked when it runs, and the storage goes
 * with whichever comes last, that release or the last such buffer's park. */
#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)
#define ARTS_DB_BUF_POOL 1
#endif

/*--- Buffer ---------------------------------------------------------------
 * Holds version + user-visible data bytes.  Lifetime is managed by an
 * arts_shared_ptr_t control block (cache.buffer is the atomic slot; each
 * acquirer holds a strong ref).  No embedded refcount: the cb's strong count
 * IS the "cache-hold + per-acquirer" count, and the cb deleter ends the
 * buffer once the last holder releases — so a destroy concurrent with an
 * in-flight acquire can never free the bytes out from under a reader.
 *   version  monotonic per-buffer version stamp.  A release bumps it with an
 *            acq_rel fetch_add after writing the bytes, and the in-place
 *            publish commit stores it with release after the bytes landed;
 *            a load that names or serves those bytes is acquire, and a load
 *            whose bytes another edge already orders is relaxed.  A buffer
 *            not yet in a slot is stamped with atomic_init.
 *   cb       this buffer's own control block (== the slot's cb while
 *            installed).  A holder recovers it from its payload address, or,
 *            where the payload is external, from the descriptor its dep slot
 *            pins, to drop its acquire ref at release — safe because
 *            the holder's own ref keeps the buffer (hence buf->cb) alive
 *            until that release.
 *   data     this block's user-visible payload, db_size bytes, 64-byte
 *            aligned (cache-line atomicity).  Where the descriptor carries
 *            the bytes itself they follow the header at that offset; where
 *            the store is external the member points at it from that same
 *            offset, and the descriptor never owns, frees or zeroes them. */
struct arts_db_buffer_s {
#ifdef ARTS_DB_BUF_POOL
  arts_lf_link_t pool_link; /* FIRST — free-list node while parked */
#endif
  _Atomic(uint64_t) version; /* monotonic per buffer (stale while parked) */
  arts_shared_ptr_t cb; /* this buffer's control block */
#ifdef ARTS_DB_BUF_POOL
  /* The cache it was drawn from, whichever cache it is installed in, whose
   * storage it keeps alive until it is parked there; NULL for a buffer no
   * cache owns. */
  struct arts_db_cache_s *owner_cache;
  uint64_t capacity; /* payload bytes the allocation holds */
  /* Out or parked, written by a debug build only, which refuses to end a
   * drawn buffer that is not out; a re-draw marks it out again, so a second
   * end after one is caught, if at all, only at a later end.  Relaxed: only
   * its own value. */
  _Atomic(unsigned int) park_state;
  char _pad[20]; /* the payload member lands at offset 64 */
#else
  char _pad[48]; /* the payload member lands at offset 64 */
#endif
#ifdef ARTS_CXL_DIRECT
  char *data; /* storage this descriptor refers to and does not own */
#else
  char data[]; /* db_size bytes — user-visible */
#endif
};

struct arts_db_cache_s;
struct arts_edt_s;

/* A waiting entry names its parked EDT by address (`edt`), which stays valid
 * while the entry is linked: the EDT cannot run before the entry's resolver
 * accounts its slot.  Under ARTS_DEBUG_WAKE_GUID `edt_guid`, right after
 * `edt`, keeps the EDT's GUID for the wake's check (db.h). */
#ifdef ARTS_DEBUG_WAKE_GUID
#define ARTS_DEP_WAITER_LAYOUT(T)                                              \
  static_assert(offsetof(T, edt_guid) == offsetof(T, edt) + sizeof(void *),    \
                "the waiter GUID sits right after the EDT pointer")
#else
#define ARTS_DEP_WAITER_LAYOUT(T)                                              \
  static_assert(sizeof(((T *)0)->edt) == sizeof(void *), "an EDT pointer")
#endif

/* A parked read on a write-through home's publication waiters
 * (cache.publication_waiters): a home-local RO acquire, or a remote read whose
 * serve was deferred — `serve` is non-NULL — because the home's copy was not
 * yet published; drained whole once it is (arts_db_drain_publication_waiters).
 * A VAL or INV requester's parked readers are not these: they are the reader
 * pool's nodes below, chained in its request word. */
struct arts_db_read_waiter_s {
  arts_lf_link_t link;    /* FIRST — required by arts_lf_stack_t */
  struct arts_edt_s *edt; /* NULL on the deferred-serve arm */
#ifdef ARTS_DEBUG_WAKE_GUID
  arts_guid_t edt_guid;
#endif
  unsigned int slot;
  /* Deferred-serve arm: NULL = local waiter (the drain resolves the parked
   * slot on the installed buffer).  Non-NULL = deferred remote serve; the
   * drain invokes serve(cache, w) BEFORE freeing w, and requester/cover/rdzv
   * carry the original request. */
  uint32_t cover;
  void (*serve)(struct arts_db_cache_s *cache, struct arts_db_read_waiter_s *w);
  unsigned int requester;
  struct arts_rdzv_landing_s rdzv;
};
static_assert(offsetof(struct arts_db_read_waiter_s, link) == 0,
              "a stack's link is its node's first member");
ARTS_DEP_WAITER_LAYOUT(struct arts_db_read_waiter_s);
#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)
/*--- Parked readers ------------------------------------------------------
 * The readers a rank parks on its cache are nodes of a per-cache pool,
 * addressed by index (0 = none) so a chain head fits beside the arm's request
 * state in one word (VAL's req_state, INV's cache_state): parking is the
 * acquire's own decision CAS, and a serve takes a whole chain in the CAS that
 * decides it, which leaves the chain private to its server.  An index names
 * one node for the pool's whole life, and the storage goes with the cache
 * (arts_db_reader_pool_destroy).  Chunk k holds 2^(k+8) nodes, so the
 * directory stays small; the indices the chunks cover fit the head field. */
#define ARTS_DB_READER_IDX_BITS 29
#define ARTS_DB_READER_NCHUNKS 21u
#define ARTS_DB_READER_IDX_MAX                                                 \
  ((uint32_t)((1ULL << (ARTS_DB_READER_NCHUNKS + 8u)) - 256u))

/* A node's fields are written by its owner before the CAS that links it
 * (its park on the request word, or a push onto the free list) and read by
 * whoever a later acquiring CAS or load hands it to: the serve that took the
 * chain, or the pop that takes the node. */
struct arts_db_reader_node_s {
  /* Pool index of the next node, 0 = end.  A pop can also read it while the
   * node's next owner rewrites it; the pop's tagged CAS then fails and the
   * value is discarded: relaxed. */
  _Atomic(uint32_t) next;
  unsigned int slot;      /* dep slot of the parked acquire */
  struct arts_edt_s *edt; /* the parked EDT */
#ifdef ARTS_DEBUG_WAKE_GUID
  arts_guid_t edt_guid;
#endif
#if ARTS_DB_CENSUS
  bool census_held; /* its park found the right it waits for held here */
#endif
};
ARTS_DEP_WAITER_LAYOUT(struct arts_db_reader_node_s);

struct arts_db_reader_pool_s {
  /* The chunk directory, and each chunk in it, installed once by a CAS whose
   * release carries the zeroed storage to every acquire load of it. */
  _Atomic(uintptr_t) chunks;
  /* Bump allocator over the index space: only its own value. */
  _Atomic(uint32_t) next_fresh;
  /* The free list, [tag:32 | index:32], index 0 = empty.  Every change is an
   * acq_rel CAS that advances the tag: a push's release carries every link of
   * what it pushes (its own writes, and a served chain's links its server
   * acquired with the chain) to the pop that acquires the head naming them,
   * and a pop whose head was recycled inside its window fails on the tag. */
  _Atomic(uint64_t) free_head;
};
#endif

/*--- Pending RW waiters --------------------------------------------------
 * edt + slot together identify the parked EDT's dep slot to fill on
 * trigger.  The RW waiter chain is a Treiber stack (cache.pending_rw,
 * arts_lf_stack_t): the embedded link is owned by the stack (push prepends, a
 * take atomic-exchanges the whole chain out).  Producers are this rank's
 * acquiring threads; each consumer (a grant's drain, a create's claim) takes
 * the whole chain and wakes every waiter it took, so the consume is
 * order-free and a LIFO Treiber stack suffices; there is no FIFO or drain-one
 * requirement (that is the home grantreq queue, which stays Vyukov MPSC). */
struct arts_db_rw_waiter_s {
  arts_lf_link_t link; /* FIRST — required by arts_lf_stack_t */
  struct arts_edt_s *edt;
#ifdef ARTS_DEBUG_WAKE_GUID
  arts_guid_t edt_guid;
#endif
  unsigned int slot;
#if ARTS_DB_CENSUS
  bool census_held; /* its park found the write right held here */
#endif
};
static_assert(offsetof(struct arts_db_rw_waiter_s, link) == 0,
              "a stack's link is its node's first member");
ARTS_DEP_WAITER_LAYOUT(struct arts_db_rw_waiter_s);

/* Forward decl; sparse rank-keyed u64 map (owner-side dedup; protocol arms). */
struct arts_rank_to_u64_map_s;

/* Recover the wrapping struct arts_db_s from a coherence cache pointer.  cache
 * is the FIRST member of arts_db_s; container_of degenerates to the cache
 * address but is written as container_of for correctness-by-construction. */
#ifndef ARTS_CONTAINER_OF
#define ARTS_CONTAINER_OF(ptr, type, member)                                   \
  ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

#ifdef __cplusplus
}
#endif

#endif /* ARTS_COHERENCE_TYPES_COMMON_H */
