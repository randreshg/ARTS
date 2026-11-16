/* SPDX-License-Identifier: Apache-2.0
 *
 * EXCL protocol cache/db layout.
 *
 * Selected by arts/coherence/types.h when ARTS_VALIDITY_EXCL is defined.
 * The EXCL protocol uses a single lock_state word on the home rank to
 * serialize all access-mode transitions: the home grants or queues each
 * requester.  What a turn's end sends is the retention's: PURGE hands
 * copy and permission back to the home, and RETAIN keeps both until another
 * node asks for the block.
 *
 * @note Internal header.  User code should include @c arts.h.
 */

#ifndef ARTS_COHERENCE_EXCL_TYPES_H
#define ARTS_COHERENCE_EXCL_TYPES_H
#ifdef __cplusplus
extern "C" {
#endif

#include "arts/coherence/types_common.h"
#ifndef ARTS_VALIDITY_EXCL
#error                                                                         \
    "excl/types.h is the EXCL layout; this translation unit's cell is another"
#endif
#include "arts/rank_bitset.h"
#include "arts/utils/stdatomic_compat.h"
#include <assert.h> /* static_assert, in C and in C++ alike */
#include <stddef.h>
#include <stdint.h>

/* ── PURGE cache_state ────────────────────────────────────────────────────
 * Per-rank single 64-bit coherence word (PURGE retention only).
 * Layout: [ rw_state:2 (63..62) | ro_state:2 (61..60) | spare:1 (59) |
 *           stub:1 (58) | rw_count:28 (57..30) | ro_count:30 (29..0) ]
 *
 *   rw_state/ro_state ∈ {IDLE, REQ (request sent to home), GRANT (grant held)}
 *   stub      = a dependence's first touch made this cache and nothing has
 *               shown the home installed yet: the create's claim
 *               (CACHE_OP_CREATE_HOLD) and every reply from the home clear
 *               it.  A write request sent while it is set carries PRECLAIM.
 *   rw_count  = #RW EDTs on this rank acquired-not-released: parked on the
 *               write request while rw_state is REQ, holders under GRANT.
 *   ro_count  = #RO EDTs on this rank counted in the word: the readers
 *               parked on the read request while ro_state is REQ and no
 *               write grant is held, holders otherwise.  The readers parked
 *               on the read request when write permission arrives leave this
 *               count for the cache's ro_aside until the read request's
 *               grant, so under a held grant the count is holders alone.
 *
 * Every transition — including the acquire's count++ — happens inside ONE
 * cache_compute_next CAS (see arbiters.c for why that atomicity is
 * load-bearing).  The two axes are independent: each request is opened by
 * its own axis's first unserved acquire and closed by its own grant.
 * (GRANT,GRANT) is unreachable: home never grants both to the same rank in
 * PURGE. */
#ifdef ARTS_RETENTION_PURGE
#define CACHE_ST_IDLE 0u
#define CACHE_ST_REQ 1u
#define CACHE_ST_GRANT 2u
#define CACHE_CNT_BITS 30
#define CACHE_CNT_MASK ((uint64_t)0x3fffffffULL)
#define CACHE_RW_CNT_MASK ((uint64_t)0x0fffffffULL)
#define CACHE_RO_CNT(s) ((uint32_t)((s) & CACHE_CNT_MASK))
#define CACHE_RW_CNT(s)                                                        \
  ((uint32_t)(((s) >> CACHE_CNT_BITS) & CACHE_RW_CNT_MASK))
#define CACHE_STUB_SHIFT 58
#define CACHE_STUB_BIT ((uint64_t)1ULL << CACHE_STUB_SHIFT)
#define CACHE_STUB(s) ((uint32_t)(((s) >> CACHE_STUB_SHIFT) & 0x1ULL))
#define CACHE_RO_ST(s) ((uint32_t)(((s) >> 60) & 0x3ULL))
#define CACHE_RW_ST(s) ((uint32_t)(((s) >> 62) & 0x3ULL))
#define CACHE_MAKE_X(rws, ros, stub, wc, rc)                                   \
  (((uint64_t)((rws) & 0x3ULL) << 62) | ((uint64_t)((ros) & 0x3ULL) << 60) |   \
   ((uint64_t)((stub) & 0x1ULL) << CACHE_STUB_SHIFT) |                         \
   (((uint64_t)(wc) & CACHE_RW_CNT_MASK) << CACHE_CNT_BITS) |                  \
   ((uint64_t)(rc) & CACHE_CNT_MASK))
#define CACHE_MAKE(rws, ros, wc, rc) CACHE_MAKE_X(rws, ros, 0u, wc, rc)
#endif /* ARTS_RETENTION_PURGE */

/* ── RETAIN cache_state ─────────────────────────────────────────────────────
 * Per-rank (owner) single 64-bit coherence word (RETAIN only).
 * Layout (MSB→LSB):
 *   [ ro_granted:1 (63) | owner:1 (62) | rw_st:2 (61..60) | ro_st:2 (59..58) |
 *     migrate_target:14 (57..44) | spare:1 (43) | stub:1 (42) |
 *     wc:20 (41..22) | rc:22 (21..0) ]
 *
 *   ro_granted = the read grant this rank holds came from the home (below);
 *                while the read request is still out, the early-recall token.
 *   owner-bit  = 1 when this node holds data ownership (fast-path RW/RO).
 *   rw_st/ro_st ∈ {IDLE=0, REQ=1, GRANT=2, and for ro_st GRANT_PURGE=3} —
 *                local RW/RO protocol state.
 *   migrate_target = 14-bit rank of the next RW owner (set by home FORWARD);
 *                    ARTS_EXCL_NO_TARGET (0x3FFF) = no pending migration.
 *   stub       = a dependence's first touch made this cache and nothing has
 *                shown the home installed yet: the create's claim
 *                (CACHE_OP_CREATE_HOLD) and every reply from the home clear
 *                it.  A write request sent while it is set carries PRECLAIM.
 *   wc         = #RW EDTs on this rank acquired-not-released: parked on the
 *                write request while rw_st is REQ, holders under GRANT.
 *   rc         = #RO EDTs on this rank counted in the word: the readers
 *                parked on the read request while nothing here serves a read
 *                (no write grant, no read grant, not resident), holders
 *                otherwise.  The readers parked on the read request when this
 *                rank starts serving reads with it still out (a migration
 *                makes it the owner, or a create's hold) leave this count for
 *                the cache's ro_aside until the request's answer, so at a
 *                resident rank the count is holders alone.  The two counts
 *                (20 and 22 bits) are far above any number of concurrent
 *                EDTs.
 *
 * Single-word CAS discipline: every transition — the acquire's count++
 * included — is one CAS (see arbiters.c for why the acquire's atomicity is
 * load-bearing), and owner-bit + migrate_target are cleared atomically with
 * the wc 0-edge in cache_owner_compute_next (REL_RW path), so migration needs
 * no split-decrement or separate flag.
 *
 * Note: under RETAIN the home does NOT hold canonical data — the current owner
 * (owner-bit set) does.  home.cache.buffer is unused by the EXCL·RETAIN
 * path. */
#ifdef ARTS_RETENTION_RETAIN
#define CACHE_ST_IDLE 0u
#define CACHE_ST_REQ 1u
#define CACHE_ST_GRANT 2u
/* GRANT_PURGE — a grant that relinquishes VOLUNTARILY at the count zero edge,
 * i.e. a PURGE-flavoured grant.  The retention is really a per-grant
 * property: under PURGE every grant carries it, under RETAIN none does until
 * the home asks (a recall) or the home issues the grant pre-marked because a
 * writer was already queued.  Only ro_st uses the value today. */
#define CACHE_ST_GRANT_PURGE 3u

/* Field widths and shifts. */
#define CACHE_OWNER_RC_BITS 22
#define CACHE_OWNER_WC_BITS 20
#define CACHE_OWNER_MT_BITS 14 /* migrate_target = ARTS_GUID_RANK_BITS */
#define CACHE_OWNER_ST_BITS 2  /* rw_st / ro_st */
#define CACHE_OWNER_OW_BITS 1  /* owner bit */

#define CACHE_OWNER_RC_SHIFT 0
#define CACHE_OWNER_WC_SHIFT                                                    \
  (CACHE_OWNER_RC_SHIFT + CACHE_OWNER_RC_BITS) /* 22                             \
                                              */
/* stub (42) rides every transition: a site that rebuilds the word from cur
 * carries CACHE_OWNER_STUB_BIT from it. */
#define CACHE_OWNER_STUB_SHIFT 42
#define CACHE_OWNER_STUB_BIT ((uint64_t)1ULL << CACHE_OWNER_STUB_SHIFT)
#define CACHE_STUB(s) ((uint32_t)(((s) >> CACHE_OWNER_STUB_SHIFT) & 0x1ULL))
#define CACHE_OWNER_MT_SHIFT 44
#define CACHE_OWNER_RO_ST_SHIFT                                                 \
  (CACHE_OWNER_MT_SHIFT + CACHE_OWNER_MT_BITS) /* 58 */
#define CACHE_OWNER_RW_ST_SHIFT                                                 \
  (CACHE_OWNER_RO_ST_SHIFT + CACHE_OWNER_ST_BITS) /* 60 */
#define CACHE_OWNER_OW_SHIFT                                                    \
  (CACHE_OWNER_RW_ST_SHIFT + CACHE_OWNER_ST_BITS) /* 62 */

#define CACHE_OWNER_RC_MASK ((uint64_t)((1ULL << CACHE_OWNER_RC_BITS) - 1))
#define CACHE_OWNER_WC_MASK ((uint64_t)((1ULL << CACHE_OWNER_WC_BITS) - 1))
#define CACHE_OWNER_MT_MASK ((uint64_t)((1ULL << CACHE_OWNER_MT_BITS) - 1))
#define CACHE_OWNER_ST_MASK ((uint64_t)0x3ULL)
#define CACHE_OWNER_OW_MASK ((uint64_t)0x1ULL)

/* Getters. */
#define CACHE_RO_CNT(s)                                                        \
  ((uint32_t)(((s) >> CACHE_OWNER_RC_SHIFT) & CACHE_OWNER_RC_MASK))
#define CACHE_RW_CNT(s)                                                        \
  ((uint32_t)(((s) >> CACHE_OWNER_WC_SHIFT) & CACHE_OWNER_WC_MASK))
#define CACHE_MIGRATE_TARGET(s)                                                \
  ((uint32_t)(((s) >> CACHE_OWNER_MT_SHIFT) & CACHE_OWNER_MT_MASK))
#define CACHE_RO_ST(s)                                                         \
  ((uint32_t)(((s) >> CACHE_OWNER_RO_ST_SHIFT) & CACHE_OWNER_ST_MASK))
#define CACHE_RW_ST(s)                                                         \
  ((uint32_t)(((s) >> CACHE_OWNER_RW_ST_SHIFT) & CACHE_OWNER_ST_MASK))
#define CACHE_OWNER(s)                                                         \
  ((uint32_t)(((s) >> CACHE_OWNER_OW_SHIFT) & CACHE_OWNER_OW_MASK))

/* ro_granted flag — bit 63.  The home counts each read request it takes in
 * until one RO_RETURN gives the count back.  While ro_st holds a read grant
 * (GRANT or GRANT_PURGE) the bit says the grant came by DELIVER(RO), so this
 * rank owes that one RO_RETURN, which the transition that takes ro_st back
 * to IDLE pays: the zero edge of a grant marked for return, or a recall that
 * retires a resting grant.  While ro_st is REQ it is a recall that overtook
 * the request's answer: a read DELIVER consumes it by committing the grant
 * marked for return, and the answer this rank gives its own request as the
 * owner, which sends the RO_RETURN itself, clears it.  A read served by the
 * write grant or the resident copy is counted nowhere but rc and touches
 * neither ro_st nor this bit.  A rank has one read request out at a time,
 * so a single bit suffices. */
#define CACHE_OWNER_GRANTED_SHIFT 63
#define CACHE_OWNER_LEASED_MASK ((uint64_t)1ULL << CACHE_OWNER_GRANTED_SHIFT)
#define CACHE_GRANTED(s) (((s) & CACHE_OWNER_LEASED_MASK) != 0u)

/* Constructor (all fields). */
#define CACHE_MAKE(rws, ros, wc, rc)                                           \
  CACHE_MAKE_FULL(0u, (rws), (ros), ARTS_EXCL_NO_TARGET, (wc), (rc))

/* CACHE_MAKE_FULL: full constructor including owner-bit and migrate_target. */
#define CACHE_MAKE_FULL(own, rws, ros, mt, wc, rc)                             \
  (((uint64_t)((own) & CACHE_OWNER_OW_MASK) << CACHE_OWNER_OW_SHIFT) |           \
   ((uint64_t)((rws) & CACHE_OWNER_ST_MASK) << CACHE_OWNER_RW_ST_SHIFT) |        \
   ((uint64_t)((ros) & CACHE_OWNER_ST_MASK) << CACHE_OWNER_RO_ST_SHIFT) |        \
   ((uint64_t)((mt) & CACHE_OWNER_MT_MASK) << CACHE_OWNER_MT_SHIFT) |            \
   ((uint64_t)((wc) & CACHE_OWNER_WC_MASK) << CACHE_OWNER_WC_SHIFT) |            \
   ((uint64_t)((rc) & CACHE_OWNER_RC_MASK) << CACHE_OWNER_RC_SHIFT))

/* Sentinel for "no pending migration target". */
#define ARTS_EXCL_NO_TARGET                                                    \
  ((uint32_t)((1u << CACHE_OWNER_MT_BITS) - 1)) /* 0x3FFF */
#endif                                         /* ARTS_RETENTION_RETAIN */

/* ── PURGE home lock_state ───────────────────────────────────────────────
 * (layout below)
 *
 * teardown records that a destroy arrived for a DB whose lock this home still
 * holds on someone's behalf.  It rides in the state word so that marking the
 * destroy and observing whether anyone still holds commit in ONE atom: the
 * destroy and the last release then race a single CAS instead of a flag and a
 * count that can each miss the other.  Whoever reaches the zero edge performs
 * the teardown, so the home outlives every release owed to it — which is what
 * lets the release itself be fire-and-forget.  The mark with both counts at
 * zero and no request awaited (rwo, below) is the torn-down word, which no
 * later transition leaves: it counts and grants no acquire.
 *
 * state_bit is meaningful only when w>0 && r>0:
 *   EXCL_PHASE_BIT_RW (0) = RW phase held / RO waiters queued
 *   EXCL_PHASE_BIT_RO (1) = RO phase held / RW waiters queued
 * When w==0 || r==0 the state_bit is normalized to 0. */
#ifdef ARTS_RETENTION_PURGE
/* Layout: [ teardown:1 (63) | state_bit:1 (62) | rwo:1 (61) |
 *           spare:17 (60..44) | w:22 (43..22) | r:22 (21..0) ]
 * rwo says the home still waits for the one request it was told is coming
 * and will not answer: the write request the claiming creator's first-touch
 * cache sent before the create claimed it, which the create's announce says
 * is coming.  It is discarded on arrival, and the teardown waits for it.  w
 * and r count the write and read requests the home has taken in whose turn
 * has not come back.  A rank has one request out per axis, and a count runs
 * ahead of the ranks only by releases still on their way, so 22 bits hold
 * the 14-bit rank space. */
#define EXCL_STATE_R_BITS 22
#define EXCL_STATE_W_BITS 22
#define EXCL_STATE_R_MASK ((uint64_t)0x3fffffULL)
#define EXCL_STATE_W_MASK ((uint64_t)0x3fffffULL)
#define EXCL_STATE_R(s) ((uint32_t)((s) & EXCL_STATE_R_MASK))
#define EXCL_STATE_W(s)                                                        \
  ((uint32_t)(((s) >> EXCL_STATE_R_BITS) & EXCL_STATE_W_MASK))
#define EXCL_STATE_RWO_SHIFT 61
#define EXCL_STATE_RWO(s) ((uint32_t)(((s) >> EXCL_STATE_RWO_SHIFT) & 0x1ULL))
#define EXCL_STATE_BIT_SHIFT 62
#define EXCL_STATE_BIT(s) ((uint32_t)(((s) >> EXCL_STATE_BIT_SHIFT) & 0x1ULL))
#define EXCL_STATE_TEARDOWN_SHIFT 63
#define EXCL_STATE_TEARDOWN(s)                                                 \
  ((uint32_t)(((s) >> EXCL_STATE_TEARDOWN_SHIFT) & 0x1ULL))
#define LOCK_MAKE_STATE_FULL(td, bit, rwo, w, r)                               \
  (((uint64_t)((td) & 0x1ULL) << EXCL_STATE_TEARDOWN_SHIFT) |                  \
   ((uint64_t)((bit) & 0x1ULL) << EXCL_STATE_BIT_SHIFT) |                      \
   ((uint64_t)((rwo) & 0x1ULL) << EXCL_STATE_RWO_SHIFT) |                      \
   (((uint64_t)(w) & EXCL_STATE_W_MASK) << EXCL_STATE_R_BITS) |                \
   ((uint64_t)(r) & EXCL_STATE_R_MASK))
#define LOCK_MAKE_STATE_TD(td, bit, w, r)                                      \
  LOCK_MAKE_STATE_FULL(td, bit, 0u, w, r)
#define LOCK_MAKE_STATE(bit, w, r) LOCK_MAKE_STATE_TD(0u, bit, w, r)
#define EXCL_PHASE_BIT_RW 0u
#define EXCL_PHASE_BIT_RO 1u
#endif /* ARTS_RETENTION_PURGE */

/* ── RETAIN home lock_state ────────────────────────────────────────────────
 * Layout (MSB→LSB): [ phase:2 (63..62) | owner:14 (61..48) |
 *                     w:24 (47..24) | r:24 (23..0) ]
 * (= 2+14+24+24 = 64 bits, no spare)
 *
 * While w != 0 the home holds one count on its own descriptor: every
 * migration it starts is answered by an EXCL_CONFIRM, which may legally
 * arrive after the block's teardown and acts on the descriptor the round's
 * home token names.  The write request whose CAS lifts w off zero takes the
 * count before that CAS (each request takes one and gives it back when its
 * CAS found w above zero), so the take happens before any CONFIRM's drop;
 * the CONFIRM whose CAS returns w to zero drops it, at the end of its
 * frame.
 *
 *   phase  ∈ {IDLE=0, RW=1, RO=2} — current arbitration phase.
 *   owner  = 14-bit rank of the current data-owning node (ARTS_GUID_RANK_BITS).
 *            Packing owner here lets CONFIRM set owner+w--+phase in ONE CAS.
 *   w      = count of RW requests whose migration the home has not yet
 *            confirmed (the one migrating plus the queued ones); CONFIRM
 *            drops it, so an owner whose turn is confirmed is not counted.
 *   r      = the read requests the home has taken in whose RO_RETURN has not
 *            come back (held through a write phase, forwarded, or answered by
 *            the owner itself); the RO→RW flip fires at r==0.  r may
 *            transiently exceed the number of distinct reader ranks under
 *            network reorder (commutative ±1 accounting tolerates this). */
#ifdef ARTS_RETENTION_RETAIN
#define EXCL_STATE_R_BITS 24
#define EXCL_STATE_W_BITS 24
#define EXCL_STATE_OWNER_BITS 14 /* = ARTS_GUID_RANK_BITS */
#define EXCL_STATE_PHASE_BITS 2

#define EXCL_STATE_R_SHIFT 0
#define EXCL_STATE_W_SHIFT (EXCL_STATE_R_SHIFT + EXCL_STATE_R_BITS)     /* 24 */
#define EXCL_STATE_OWNER_SHIFT (EXCL_STATE_W_SHIFT + EXCL_STATE_W_BITS) /* 48 */
#define EXCL_STATE_PHASE_SHIFT                                                  \
  (EXCL_STATE_OWNER_SHIFT + EXCL_STATE_OWNER_BITS) /* 62 */

#define EXCL_STATE_R_MASK ((uint64_t)((1ULL << EXCL_STATE_R_BITS) - 1))
#define EXCL_STATE_W_MASK ((uint64_t)((1ULL << EXCL_STATE_W_BITS) - 1))
#define EXCL_STATE_OWNER_MASK ((uint64_t)((1ULL << EXCL_STATE_OWNER_BITS) - 1))
#define EXCL_STATE_PHASE_MASK ((uint64_t)0x3ULL)

/* Phase constants. */
#define EXCL_PHASE_IDLE 0u
#define EXCL_PHASE_RW 1u
#define EXCL_PHASE_RO 2u

/* Getters. */
#define LOCK_R(s) ((uint32_t)(((s) >> EXCL_STATE_R_SHIFT) & EXCL_STATE_R_MASK))
#define LOCK_W(s) ((uint32_t)(((s) >> EXCL_STATE_W_SHIFT) & EXCL_STATE_W_MASK))
#define LOCK_OWNER(s)                                                          \
  ((uint32_t)(((s) >> EXCL_STATE_OWNER_SHIFT) & EXCL_STATE_OWNER_MASK))
#define LOCK_PHASE(s)                                                          \
  ((uint32_t)(((s) >> EXCL_STATE_PHASE_SHIFT) & EXCL_STATE_PHASE_MASK))

/* Constructor. */
#define LOCK_MAKE(phase, owner, w, r)                                          \
  (((uint64_t)((phase) & EXCL_STATE_PHASE_MASK) << EXCL_STATE_PHASE_SHIFT) |     \
   ((uint64_t)((owner) & EXCL_STATE_OWNER_MASK) << EXCL_STATE_OWNER_SHIFT) |     \
   ((uint64_t)((w) & EXCL_STATE_W_MASK) << EXCL_STATE_W_SHIFT) |                 \
   ((uint64_t)((r) & EXCL_STATE_R_MASK) << EXCL_STATE_R_SHIFT))
#endif /* ARTS_RETENTION_RETAIN */

/* ── home lock_state transition ops + grant codes (PURGE) ────────────────
 * excl_compute_next(cur, op, &grant) is the home arbiter, a pure function of
 * the single lock_state word run inside a CAS-retry loop (arbiters.c, which
 * the retention's unit includes).  The acquire and release handlers run
 * the SAME arbiter locally when home == self: a local hit runs the handler
 * logic directly, with no wire round.
 */
#ifdef ARTS_RETENTION_PURGE
#define EXCL_OP_RW_ACQ 0
#define EXCL_OP_RO_ACQ 1
#define EXCL_OP_RW_REL 2
#define EXCL_OP_RO_REL 3
#define EXCL_OP_TEARDOWN 4 /* a destroy arrived; tear down at the zero edge */
/* The claiming creator's pre-claim write request arrived (the caller
 * discards it): rwo clears, and a marked home it leaves with nothing counted
 * returns LOCK_GRANT_DESTROY. */
#define EXCL_OP_RWO_ARRIVE 5
#define LOCK_GRANT_NONE 0
#define LOCK_GRANT_ONE_RW 1
#define LOCK_GRANT_ALL_RO 2
/* Not a grant: the committer of this transition owns the DB's teardown.  At
 * most one transition per DB ever returns it. */
#define LOCK_GRANT_DESTROY 3
#endif /* ARTS_RETENTION_PURGE */

/* ── home lock_state transition ops + action codes (RETAIN) ────────────────
 * lock_owner_compute_next(cur, op, new_owner, &action) is the RETAIN home
 * arbiter (arbiters.c), with PURGE's CAS-retry pattern.  new_owner is read
 * only by EXCL_OP_CONFIRM (sets owner in the same word as w--/phase).  The
 * FORWARD recipient is LOCK_OWNER() of the returned word; the migrate target
 * and the served reader(s) are resolved by the caller from rw_waiters /
 * ro_waiters (a pure arbiter cannot peek the queues). */
#ifdef ARTS_RETENTION_RETAIN
#define EXCL_OP_RW_ACQ 0
#define EXCL_OP_RO_ACQ 1
#define EXCL_OP_CONFIRM 2 /* new owner confirms migration complete */
#define EXCL_OP_RO_RET 3  /* reader returns RO (r--) */

#define EXCL_ACTION_NONE 0
#define EXCL_ACTION_FORWARD_MIGRATE                                            \
  1 /* home→owner: FORWARD(migrate→target)                                 \
     */
#define EXCL_ACTION_FORWARD_SERVE_ONE                                          \
  2 /* home→owner: FORWARD(serve→1 reader) */
#define EXCL_ACTION_FORWARD_SERVE_ALL                                          \
  3 /* home→owner: FORWARD(serve→held RO)                                  \
     */
#endif /* ARTS_RETENTION_RETAIN */

/* ── cache_state transition ops + actions (PURGE) ────────────────────────
 * cache_compute_next(cur, op, taken, &action) (arbiters.c) is the cache-side
 * analogue of the home's excl_compute_next: a pure function of the current
 * word (and, for a read grant, of the count its committer took from
 * ro_aside), run inside a CAS-retry loop.  The ACQ_* ops carry the count++
 * AND the request/join decision inside the one CAS; the REL_* ops carry the
 * count-- inside the CAS (the decrement and the 0-edge state change must be
 * atomic together).  See arbiters.c for the invariants this single-atom shape
 * upholds. */
#ifdef ARTS_RETENTION_PURGE
#define CACHE_OP_ACQ_RW 0
#define CACHE_OP_ACQ_RO 1
#define CACHE_OP_GRANT_RW 2
#define CACHE_OP_GRANT_RO 3
#define CACHE_OP_REL_RW 4
#define CACHE_OP_REL_RO 5
/* A create recording its own hold in a cache this rank already made for the
 * block (see the arbiter for the word it admits). */
#define CACHE_OP_CREATE_HOLD 6
/* A size round does not transit the arbiter: its transition is
 * cache_size_round_next (arbiters.c), which clears the stub bit of a word
 * whose axis is at REQ and which its handler commits with a CAS only when the
 * bit is set. */

#define CACHE_ACT_NONE 0
#define CACHE_ACT_SELF_SERVE 1 /* a grant is held: serve the dep directly */
#define CACHE_ACT_SEND_RW 2    /* opened the RW request: park, then send */
#define CACHE_ACT_SEND_RO 3    /* opened the RO request: park, then send */
#define CACHE_ACT_PARK 4       /* its axis's request is out: park on it */
/* Write permission arrived (a grant, a create's hold): set the readers
 * parked on the read request aside, then serve rw_pending. */
#define CACHE_ACT_DRAIN_RW 5
#define CACHE_ACT_DRAIN_RO 6 /* RO grant: serve ro_pending */
/* Send RW RELEASE (with the payload, unless it rests in the CXL store). */
#define CACHE_ACT_REL_RW 7
#define CACHE_ACT_REL_RO 8 /* send RO RELEASE (notify) */
/* A grant that answers no request this rank has out, or a count that would
 * pass its field: the commit site raises it.  The arbiter leaves the word
 * unchanged. */
#define CACHE_ACT_LATE 10
#endif /* ARTS_RETENTION_PURGE */

/* ── cache_state transition ops + actions (RETAIN) ─────────────────────────
 * cache_owner_compute_next(cur, op, taken, &action) is the RETAIN owner-cache
 * arbiter (arbiters.c); `taken` is the count a read answer's committer took
 * from ro_aside before the CAS, and every other op passes 0. */
#ifdef ARTS_RETENTION_RETAIN
#define CACHE_OP_ACQ_RW 0
#define CACHE_OP_ACQ_RO 1
/* A migration's DELIVER answered this rank's write request: the bytes are
 * resident here with the write permission. */
#define CACHE_OP_GRANT_RW 2
/* A read DELIVER answered this rank's read request; MARKED when the home
 * issued the grant marked for return. */
#define CACHE_OP_GRANT_RO 3
#define CACHE_OP_REL_RW 4 /* local RW release: wc-- + 0-edge → migrate/noop */
#define CACHE_OP_REL_RO 5 /* local RO release: rc-- */
/* A create recording its own hold in a cache this rank already made for the
 * block (see the arbiter for the word it admits). */
#define CACHE_OP_CREATE_HOLD 6
#define CACHE_OP_GRANT_RO_MARKED 7
/* The home forwarded this rank, the owner, its own read request: the
 * request's answer, given here from the resident bytes. */
#define CACHE_OP_OWN_RO 8
/* The forwards that serve another rank or migrate the block do not transit
 * the arbiter: arts_handler_db_excl_forward installs migrate_target or pushes
 * ro_serve itself (the target is not a pure-function output).  Nor does a
 * size round: its transition is cache_owner_size_round_next (arbiters.c),
 * which clears the stub bit of a word whose axis is at REQ and which its
 * handler commits with a CAS only when the bit is set. */

/* Values are aligned with the PURGE block above so that a name shared by both
 * retentions also shares its number, and the acquire actions carry PURGE's
 * contract: a SELF_SERVE acquire never parks, and a SEND_* or PARK acquire
 * pushes its waiter after the CAS that counted it. */
#define CACHE_ACT_NONE 0
#define CACHE_ACT_SELF_SERVE 1 /* a grant or the resident copy serves the dep */
#define CACHE_ACT_SEND_RW 2    /* opened the RW request: park, then send */
#define CACHE_ACT_SEND_RO 3    /* opened the RO request: park, then send */
#define CACHE_ACT_PARK 4       /* its axis's request is out: park on it */
/* The transition makes this rank serve writes and reads (a migration's
 * DELIVER, a create's hold): set the readers parked on the read request
 * aside, then serve rw_pending. */
#define CACHE_ACT_DRAIN_RW 5
/* The read request's answer: serve the readers parked on it, the ones set
 * aside included. */
#define CACHE_ACT_DRAIN_RO 6
#define CACHE_ACT_REL_RO 8     /* the rc zero edge was reached */
#define CACHE_ACT_MIGRATE 9  /* wc 0-edge + migrate_target set: send DELIVER */
/* A transition for no request this rank has out, or a count that would pass
 * its field: the commit site raises it.  The arbiter leaves the word
 * unchanged. */
#define CACHE_ACT_LATE 10

/* CACHE_ACT_REL_RO marks the edge, NOT the wire send.  Whether a RO_RETURN goes
 * out is derived from the COMMITTED transition by the caller —
 * ro_st(cur) in {REQ, GRANT, GRANT_PURGE} && ro_st(next) == IDLE — because that
 * CAS is the exactly-once token and a concurrent recall must not be able to
 * make two sites send for one grant.  Under RETAIN the edge may relinquish
 * (GRANT_PURGE) or retain (GRANT); the engine must be driven either way. */
#endif /* ARTS_RETENTION_RETAIN */

/* Pending waiter (parked EDT dep slot) used in both ro_pending and rw_pending.
 * Same shape as VAL's RW waiter for consistency. */
struct arts_db_excl_waiter_s {
  arts_lf_link_t link; /* FIRST — required by arts_lf_stack_t */
  struct arts_edt_s *edt;
#ifdef ARTS_DEBUG_WAKE_GUID
  arts_guid_t edt_guid;
#endif
  unsigned int slot;
};
static_assert(offsetof(struct arts_db_excl_waiter_s, link) == 0,
              "a stack's link is its node's first member");
ARTS_DEP_WAITER_LAYOUT(struct arts_db_excl_waiter_s);

/* A read request queued on the home's ro_waiters.  Written before the push
 * that publishes it, read and freed by the frame whose drain took it (or by
 * the home's teardown). */
struct arts_lock_ro_node_s {
  arts_lf_link_t link; /* FIRST */
  unsigned int rank;
  struct arts_rdzv_landing_s rdzv; /* the requester's grant/serve landing */
  uint64_t token;                  /* the requester's answer token */
};

/* Per-rank DB cache for the EXCL protocol.
 * The layout is shared between PURGE and RETAIN; RETAIN adds ro_serve (the list
 * of reader ranks the owner must serve during the RO phase).  PURGE omits it
 * so sizeof(arts_db_cache_s) is unchanged in PURGE builds. */
struct arts_db_cache_s {
  /* This rank's coherence word (CACHE_* above).  After its init every write
   * is a CAS, and every RMW is acq_rel: it releases what its thread wrote
   * before it (installed or fetched bytes, home_pub_rdzv, migrate_rdzv, a
   * holder's accesses to the block) to the thread whose later RMW or acquire
   * load of the word reads it.  An acquirer that serves itself on the word
   * sees the bytes, and the zero-edge actor that ships, purges or returns
   * them is ordered after every holder. */
  _Atomic(uint64_t) cache_state;
  /* Single stable backing store: allocated once (first grant/create), written
   * in place by arts_db_buf_write_inplace on every later grant/publish, freed
   * only at destroy.  The address never moves, so DBs holding internal
   * self-pointers stay valid.  Under RETAIN, data stays with the owner; the
   * home-rank cache.buffer is unused by the coherence path.  Orders: a
   * slot's (arts_shared_slot_t, shared.h), whose release publishes the
   * buffer once; the bytes a grant or publish writes in place reach a holder
   * through cache_state's CAS that admits it (above), never through the
   * slot. */
  _Atomic(arts_shared_slot_t) buffer;
  arts_lf_stack_t ro_pending; /* parked RO EDT waiters on this rank */
  arts_lf_stack_t rw_pending; /* parked RW EDT waiters on this rank */
#ifdef ARTS_RETENTION_RETAIN
  /* Reader ranks the owner must serve during the RO phase.  Home pushes via
   * FORWARD; the owner drains (DELIVER copy to each reader) when wc reaches 0.
   * This list cannot be packed into the single cache_state word. */
  arts_lf_stack_t ro_serve;
  /* The pending migrate target's landing (its stable buffer) and the two
   * tokens its DELIVER carries (the target's request's and the home's),
   * written by the FORWARD(migrate) handler BEFORE the CAS that publishes
   * migrate_target (single in-flight migration per owner — home serializes by
   * CONFIRM), read by whichever actor ships the DELIVER on the wc 0-edge, or
   * grants the write turn here when the target is this rank. */
  struct arts_rdzv_landing_s migrate_rdzv;
  uint64_t migrate_token;
  uint64_t migrate_home_token;
#elif !defined(ARTS_USE_CXL)
  /* Home's publish landing for THIS grant's release (advertised in the
   * grant, 1:1 with the eventual RW release).  Written by the grant handler
   * before any local writer runs; consumed by the single RW releaser. */
  struct arts_rdzv_landing_s home_pub_rdzv;
#endif
  arts_guid_t db_guid;
  /* The block's exact size once learned, 0 until then; relaxed throughout
   * (see the VAL layout and arts_db_cache_learn_size). */
  _Atomic(uint64_t) db_size;
#ifdef ARTS_USE_CXL
  /* The block's slot in the CXL store between turns.  Recorded by a CAS from
   * 0, once per block per rank — by the create that made it, or by the first
   * grant that names it — read by both funnels, and cleared only on a cache
   * that was never published.  Only its own value: the slot's bytes move by
   * the store's flushes at the ownership edges, which the cache word
   * orders. */
  _Atomic(uint64_t) cxl_addr;
#endif
  /* The readers parked on this rank's read request whose count left
   * cache_state when the rank began serving reads with that request still
   * out: under PURGE a write grant, under RETAIN a migration that makes the
   * rank the owner, on both a create's hold.  That transition's committer
   * adds its CAS's pre-image read count after the CAS and before it resumes
   * any writer or sends anything; the committer of the request's answer (a
   * read grant, or under RETAIN the owner's answer to its own forwarded
   * request) takes the whole count, an exchange with 0, before its CAS,
   * which returns it to the word.  Non-zero only while the request is out.
   * The add happens-before the take through the home, and an earlier
   * answer's take happens-before the add:
   * PURGE: the home grants the read request only after the release that
   *   ended this rank's write turn, which a holder sent after the add (one
   *   the transition resumed, or the create that took the hold); an earlier
   *   read grant is returned before the home grants this rank's write
   *   request.
   * RETAIN: the home forwards the request, and starts a migration away from
   *   this rank, only after the EXCL_CONFIRM or the create's announce this
   *   rank sends after the add; an earlier answer's RO_RETURN reaches the
   *   home before it starts a migration here.
   * Each RMW is acq_rel and decides nothing against another word. */
  _Atomic(uint32_t) ro_aside;
  /* 1 until a buffer has been installed, 0 after: stored 0 with release
   * after the slot's install and read with acquire at the fast exit (see the
   * VAL layout). */
  _Atomic(uint8_t) payload_pending;
  /* What made this cache (arts_db_init_kind_t): a create's own descriptor,
   * the home's directory, or a dependence's first touch.  A create that
   * claims a first touch's cache turns it into its own
   * (arts_db_create_claims_stub); nothing else writes it after init.  Only
   * its own value: the claim is one CAS, so one create claims a cache. */
  _Atomic(uint8_t) init_kind;
};

#ifdef ARTS_USE_CXL
/* The arm's own observables: one read of the block's slot and one write back
 * per turn.  Statistics, relaxed throughout: only their own value.  Read only
 * by tests — the counter subsystem is compiled out by the tree's counters-off
 * default, and a counter-based oracle would change what the whole suite runs
 * under. */
extern _Atomic(uint64_t) arts_cxl_fetches;
extern _Atomic(uint64_t) arts_cxl_purges;
#endif

/** Internal DataBlock descriptor (EXCL protocol).
 *
 *  The per-rank coherence cache (struct arts_db_cache_s) is embedded by value
 *  as the FIRST member: the cb object the route_table wraps is the db_s, and
 *  cache-to-db_s recovery is a zero-cost cast.  Non-home ranks allocate a
 *  cache-only footprint (arts_db_cache_stub_size() = offsetof(lock_state))
 *  that includes home_initialized but omits the home-directory fields. */
struct arts_db_s {
  struct arts_db_cache_s
      cache;               /**< FIRST — coherence state (embedded by value). */
  arts_db_types_t db_type; /**< Storage subtype; in bounds on cache stubs. */
  /* Non-owning alias to the control block this descriptor is installed
   * with, armed right after the cb is made and before the install publishes
   * the descriptor; cleared on a lost install that abandons that cb.  Inside
   * the cache-only stub's span: every descriptor has one.  Ordered by the
   * install that publishes the descriptor.  It holds no count: the deleter
   * runs before the cb is freed, so it is valid while any count on the
   * descriptor is held, and a frame holding one upgrades with
   * arts_shared_copy or, once the descriptor is installed, retires the block
   * through it (arts_ooo_retire). */
  arts_shared_ptr_t self_cb;
  /* Home-directory fields — present only on the GUID home rank.  Cache-only
   * stubs (arts_db_cache_stub_size()) end at lock_state (the first home field),
   * so they include home_initialized but omit every home-arm field. */
  bool home_initialized; /**< One-shot init sentinel (arts_db_home_init). */
  /* The home's arbitration word (layouts above).  After its init every
   * write is an acq_rel CAS of the arbiter, so each transition reads the one
   * before it: the committer of a transition that pops, peeks or drains a
   * waiter queue is ordered after every requester whose count it read, and
   * sees what that requester queued before its own CAS. */
  _Atomic(uint64_t) lock_state;
  struct arts_home_grantreq_queue_s rw_waiters; /* Vyukov MPSC, pop-one */
  arts_lf_stack_t ro_waiters;                  /* Treiber, XCHG drain */
  /* destroy fan-out roster; the creator's bit is set at home init, every
   * other by arts_rank_bitset_note */
  struct arts_rank_bitset_s cached_ranks;
  /* The rank whose create claimed a first-touch cache, written before the
   * descriptor is published (ordered by the route slot's install). */
  unsigned int claim_rank;
#ifdef ARTS_RETENTION_RETAIN
  /* The destroy's gate (cover_gate.h): the destroy's mark and the claiming
   * creator's pre-claim write request the create's announce said is still
   * coming, one word so that marking and settling read each other.  Every
   * write after init is an acq_rel RMW. */
  _Atomic(uint32_t) cover_gate;
  /* Recall roster: ranks that have issued a RO request since the last drain.
   * Its OWN bitset — cached_ranks is the monotone destroy set and is written by
   * RW requesters too.  Set with fetch_or BEFORE the requester's lock_state CAS
   * (the ordering that makes "untagged implies recalled" hold) and drained by
   * an exchange after the CAS that takes w 0->1; the lock_state RMW chain
   * between the two CASes carries the bit to the drain.  The set must stay a
   * fetch_or: a test-then-store fast path could skip the store on a stale read
   * of a bit a drain has already taken. */
  struct arts_rank_bitset_s ro_retainers;
#endif
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

/* Shared by both retentions: perform the home teardown (notify every rank
 * that ever cached the DB, then return the route slot).  Called by the
 * transition that leaves the home marked with nothing owed: under PURGE the
 * destroy, the last release or the pre-claim write request's arrival on
 * lock_state; under RETAIN the destroy's mark or that arrival on
 * cover_gate. */
void arts_excl_home_teardown(struct arts_db_s *db, arts_guid_t db_guid);

/* Shared by both retentions, the two steps of ro_aside (above): the add of
 * the n readers a serving transition took out of the cache word, by its
 * committer before it resumes any writer, and the take by the committer of
 * the read request's answer before its CAS, which returns the count. */
void arts_excl_set_readers_aside(struct arts_db_cache_s *cache, uint32_t n);
uint32_t arts_excl_take_readers_aside(struct arts_db_cache_s *cache);

#ifdef __cplusplus
}
#endif

#endif /* ARTS_COHERENCE_EXCL_TYPES_H */
