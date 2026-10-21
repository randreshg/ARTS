/* SPDX-License-Identifier: Apache-2.0
 *
 * EXCL protocol cache/db layout.
 *
 * Selected by arts/coherence/types.h when ARTS_PROTOCOL_EXCL is defined.
 * The EXCL protocol uses a single lock_state word on the home rank to
 * serialize all access-mode transitions: the home grants or queues each
 * requester.  What a turn's end sends is the release policy's: PURGE hands
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
#ifndef ARTS_PROTOCOL_EXCL
#error                                                                         \
    "excl/types.h is the EXCL layout; this translation unit's cell is another"
#endif
#include "arts/rank_bitset.h"
#include "arts/utils/stdatomic_compat.h"
#include <stdint.h>

/* ── PURGE cache_state ────────────────────────────────────────────────────
 * Per-rank single 64-bit coherence word (PURGE release policy only).
 * Layout: [ rw_state:2 (63..62) | ro_state:2 (61..60) | ro_parity:1 (59) |
 *           stub:1 (58) | rw_count:28 (57..30) | ro_count:30 (29..0) ]
 *
 *   rw_state/ro_state ∈ {IDLE, REQ (request sent to home), GRANT (grant held)}
 *   ro_parity = the parity of the last read request this rank sent, flipped by
 *               the CAS that opens one; a write request declares it.
 *   stub      = a dependence's first touch made this cache and nothing has
 *               shown the home installed yet: the create's claim
 *               (CACHE_OP_CREATE_HOLD) and every reply from the home clear
 *               it.  A request sent while it is set carries PRECLAIM.
 *   rw_count/ro_count = #RW/#RO EDTs on this rank acquired-not-released.
 *
 * Every transition — including the acquire's count++ — happens inside ONE
 * cache_compute_next CAS (see arbiters.c for why that atomicity is
 * load-bearing).  (GRANT,GRANT) is unreachable: home never grants both to the
 * same rank in PURGE. */
#ifdef ARTS_RELEASE_PURGE
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
#define CACHE_RO_PAR_SHIFT 59
#define CACHE_STUB(s) ((uint32_t)(((s) >> CACHE_STUB_SHIFT) & 0x1ULL))
#define CACHE_RO_PAR(s) ((uint32_t)(((s) >> CACHE_RO_PAR_SHIFT) & 0x1ULL))
#define CACHE_RO_ST(s) ((uint32_t)(((s) >> 60) & 0x3ULL))
#define CACHE_RW_ST(s) ((uint32_t)(((s) >> 62) & 0x3ULL))
#define CACHE_MAKE_X(rws, ros, par, stub, wc, rc)                              \
  (((uint64_t)((rws) & 0x3ULL) << 62) | ((uint64_t)((ros) & 0x3ULL) << 60) |   \
   ((uint64_t)((par) & 0x1ULL) << CACHE_RO_PAR_SHIFT) |                        \
   ((uint64_t)((stub) & 0x1ULL) << CACHE_STUB_SHIFT) |                         \
   (((uint64_t)(wc) & CACHE_RW_CNT_MASK) << CACHE_CNT_BITS) |                  \
   ((uint64_t)(rc) & CACHE_CNT_MASK))
#define CACHE_MAKE(rws, ros, wc, rc) CACHE_MAKE_X(rws, ros, 0u, 0u, wc, rc)
#endif /* ARTS_RELEASE_PURGE */

/* ── RETAIN cache_state ─────────────────────────────────────────────────────
 * Per-rank (owner) single 64-bit coherence word (RETAIN release policy only).
 * Layout (MSB→LSB):
 *   [ spare:1 (63) | owner:1 (62) | rw_st:2 (61..60) | ro_st:2 (59..58) |
 *     migrate_target:14 (57..44) | wc:22 (43..22) | rc:22 (21..0) ]
 *
 *   owner-bit  = 1 when this node holds data ownership (fast-path RW/RO).
 *   rw_st/ro_st ∈ {IDLE=0, REQ=1, GRANT=2} — local RW/RO protocol state.
 *   migrate_target = 14-bit rank of the next RW owner (set by home FORWARD);
 *                    ARTS_EXCL_NO_TARGET (0x3FFF) = no pending migration.
 *   wc/rc      = #RW/#RO EDTs on this rank acquired-not-released (local EDT
 *                count; 22 bits → ≥4 M concurrent EDTs, well above any limit).
 *
 * Single-word CAS discipline: every transition — the acquire's count++
 * included — is one CAS (see arbiters.c for why the acquire's atomicity is
 * load-bearing), and owner-bit + migrate_target are cleared atomically with
 * the wc 0-edge in cache_owner_compute_next (REL_RW path), so migration needs
 * no split-decrement or separate flag.
 *
 * Note: under RETAIN the home does NOT hold canonical data — the current owner
 * (owner-bit set) does.  home.cache.buffer is unused by the EXCL RETAIN
 * path. */
#ifdef ARTS_RELEASE_RETAIN
#define CACHE_ST_IDLE 0u
#define CACHE_ST_REQ 1u
#define CACHE_ST_GRANT 2u
/* GRANT_PURGE — a grant that relinquishes VOLUNTARILY at the count zero edge,
 * i.e. a PURGE-flavoured grant.  The release policy is really a per-grant
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
/* ro_parity (43) and stub (42): as under PURGE.  They ride every
 * transition: a site that rebuilds the word from cur carries them with
 * CACHE_OWNER_STICKY_MASK. */
#define CACHE_OWNER_STUB_SHIFT 42
#define CACHE_OWNER_RO_PAR_SHIFT 43
#define CACHE_OWNER_STUB_BIT ((uint64_t)1ULL << CACHE_OWNER_STUB_SHIFT)
#define CACHE_OWNER_RO_PAR_BIT ((uint64_t)1ULL << CACHE_OWNER_RO_PAR_SHIFT)
#define CACHE_OWNER_STICKY_MASK (CACHE_OWNER_STUB_BIT | CACHE_OWNER_RO_PAR_BIT)
#define CACHE_STUB(s) ((uint32_t)(((s) >> CACHE_OWNER_STUB_SHIFT) & 0x1ULL))
#define CACHE_RO_PAR(s) ((uint32_t)(((s) >> CACHE_OWNER_RO_PAR_SHIFT) & 0x1ULL))
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

/* ro_granted flag — the single spare bit (63).  Set when this rank's current RO
 * grant arrived from the home via DELIVER(RO) (a home-COUNTED grant), as
 * opposed to an owner-fast-path local RO (never counted at home).  The
 * owner-local RO release path takes owner_try_execute and sends NO RO_RETURN;
 * without this flag a rank that requested an RO and THEN became the owner (a
 * held reader promoted to owner at the RW→RO flip) would self-serve and
 * self-release its granted RO without ever returning the home's r count → the
 * RO→RW flip deadlocks.  With the flag, the RO release sends a RO_RETURN
 * whenever the released RO was granted, regardless of owner-bit, keeping the
 * home's r balanced.  RO grants on a rank are homogeneous per generation (the
 * first RO acquire fixes ro_st to GRANT via the owner fast-path, or to REQ via
 * a home request, and later RO acquires join that state), so a single flag
 * suffices. */
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
#endif                                         /* ARTS_RELEASE_RETAIN */

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
 * zero is the torn-down word, which no later transition leaves: it counts and
 * grants no acquire.
 *
 * state_bit is meaningful only when w>0 && r>0:
 *   EXCL_PHASE_BIT_RW (0) = RW phase held / RO waiters queued
 *   EXCL_PHASE_BIT_RO (1) = RO phase held / RW waiters queued
 * When w==0 || r==0 the state_bit is normalized to 0.
 * w = #RW participant ranks; r = #RO participant ranks. */
#ifdef ARTS_RELEASE_PURGE
/* Layout: [ teardown:1 (63) | state_bit:1 (62) | rwo:1 (61) | roo:1 (60) |
 *           owed:16 (59..44) | w:22 (43..22) | r:22 (21..0) ]
 * owed counts the requests the home has been told are still coming (a create
 * announce's, and a write request's declaration of its rank's read request);
 * rwo and roo are the claiming creator's pre-claim write and read requests,
 * which are discarded unanswered.  The teardown waits for owed to reach zero.
 * owed holds one declared read request per rank plus the two pre-claim ones
 * (16 bits for the 14-bit rank space); w and r count participant ranks. */
#define EXCL_STATE_R_BITS 22
#define EXCL_STATE_W_BITS 22
#define EXCL_STATE_R_MASK ((uint64_t)0x3fffffULL)
#define EXCL_STATE_W_MASK ((uint64_t)0x3fffffULL)
#define EXCL_STATE_R(s) ((uint32_t)((s) & EXCL_STATE_R_MASK))
#define EXCL_STATE_W(s)                                                        \
  ((uint32_t)(((s) >> EXCL_STATE_R_BITS) & EXCL_STATE_W_MASK))
#define EXCL_STATE_OWED_SHIFT 44
#define EXCL_STATE_OWED_MASK ((uint64_t)0xffffULL)
#define EXCL_STATE_OWED(s)                                                     \
  ((uint32_t)(((s) >> EXCL_STATE_OWED_SHIFT) & EXCL_STATE_OWED_MASK))
#define EXCL_STATE_ROO_SHIFT 60
#define EXCL_STATE_ROO(s) ((uint32_t)(((s) >> EXCL_STATE_ROO_SHIFT) & 0x1ULL))
#define EXCL_STATE_RWO_SHIFT 61
#define EXCL_STATE_RWO(s) ((uint32_t)(((s) >> EXCL_STATE_RWO_SHIFT) & 0x1ULL))
#define EXCL_STATE_BIT_SHIFT 62
#define EXCL_STATE_BIT(s) ((uint32_t)(((s) >> EXCL_STATE_BIT_SHIFT) & 0x1ULL))
#define EXCL_STATE_TEARDOWN_SHIFT 63
#define EXCL_STATE_TEARDOWN(s)                                                 \
  ((uint32_t)(((s) >> EXCL_STATE_TEARDOWN_SHIFT) & 0x1ULL))
#define LOCK_MAKE_STATE_FULL(td, bit, rwo, roo, owed, w, r)                    \
  (((uint64_t)((td) & 0x1ULL) << EXCL_STATE_TEARDOWN_SHIFT) |                  \
   ((uint64_t)((bit) & 0x1ULL) << EXCL_STATE_BIT_SHIFT) |                      \
   ((uint64_t)((rwo) & 0x1ULL) << EXCL_STATE_RWO_SHIFT) |                      \
   ((uint64_t)((roo) & 0x1ULL) << EXCL_STATE_ROO_SHIFT) |                      \
   (((uint64_t)(owed) & EXCL_STATE_OWED_MASK) << EXCL_STATE_OWED_SHIFT) |      \
   (((uint64_t)(w) & EXCL_STATE_W_MASK) << EXCL_STATE_R_BITS) |                \
   ((uint64_t)(r) & EXCL_STATE_R_MASK))
#define LOCK_MAKE_STATE_TD(td, bit, w, r)                                      \
  LOCK_MAKE_STATE_FULL(td, bit, 0u, 0u, 0u, w, r)
#define LOCK_MAKE_STATE(bit, w, r) LOCK_MAKE_STATE_TD(0u, bit, w, r)
#define EXCL_PHASE_BIT_RW 0u
#define EXCL_PHASE_BIT_RO 1u
#endif /* ARTS_RELEASE_PURGE */

/* ── RETAIN home lock_state ────────────────────────────────────────────────
 * Layout (MSB→LSB): [ phase:2 (63..62) | owner:14 (61..48) |
 *                     w:24 (47..24) | r:24 (23..0) ]
 * (= 2+14+24+24 = 64 bits, no spare)
 *
 *   phase  ∈ {IDLE=0, RW=1, RO=2} — current arbitration phase.
 *   owner  = 14-bit rank of the current data-owning node (ARTS_GUID_RANK_BITS).
 *            Packing owner here lets CONFIRM set owner+w--+phase in ONE CAS.
 *   w      = count of RW requests whose migration the home has not yet
 *            confirmed (the one migrating plus the queued ones); CONFIRM
 *            drops it, so an owner whose turn is confirmed is not counted.
 *   r      = count of outstanding RO grants (issued but not yet returned);
 *            the RO→RW flip fires at r==0. r may transiently exceed the number
 *            of distinct reader ranks under network reorder (commutative ±1
 *            accounting tolerates this). */
#ifdef ARTS_RELEASE_RETAIN
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
#endif /* ARTS_RELEASE_RETAIN */

/* ── home lock_state transition ops + grant codes (PURGE) ────────────────
 * excl_compute_next(cur, op, &grant) is the home arbiter, a pure function of
 * the single lock_state word run inside a CAS-retry loop.  Exposed here (not
 * just in purge.c) so the acquire/release handlers can run the SAME arbiter
 * LOCALLY when home == self — a local hit runs the handler logic directly, with
 * no wire round (HPC: a local op must not touch the network layer).
 */
#ifdef ARTS_RELEASE_PURGE
#define EXCL_OP_RW_ACQ 0
#define EXCL_OP_RO_ACQ 1
#define EXCL_OP_RW_REL 2
#define EXCL_OP_RO_REL 3
#define EXCL_OP_TEARDOWN 4 /* a destroy arrived; tear down at the zero edge */
/* A request the home was told is coming: OWE counts one more, ARRIVE settles
 * a declared read request that came (the caller dismisses it), RWO_ARRIVE and
 * ROO_ARRIVE the claiming creator's pre-claim write and read requests (the
 * caller discards them).  Any of them that leaves a marked home with nothing
 * counted and nothing owed returns LOCK_GRANT_DESTROY. */
#define EXCL_OP_OWE 5
#define EXCL_OP_ARRIVE 6
#define EXCL_OP_RWO_ARRIVE 7
#define EXCL_OP_ROO_ARRIVE 8
#define LOCK_GRANT_NONE 0
#define LOCK_GRANT_ONE_RW 1
#define LOCK_GRANT_ALL_RO 2
/* Not a grant: the committer of this transition owns the DB's teardown.  At
 * most one transition per DB ever returns it. */
#define LOCK_GRANT_DESTROY 3
/* Not a grant: the transition would overflow a count field; the word is left
 * unchanged and the caller raises it. */
#define LOCK_GRANT_FULL 4
uint64_t excl_compute_next(uint64_t cur, int op, uint32_t *out_grant);
#endif /* ARTS_RELEASE_PURGE */

/* ── home lock_state transition ops + action codes (RETAIN) ────────────────
 * lock_owner_compute_next(cur, op, &action) is the RETAIN home arbiter.
 * Same CAS-retry pattern as PURGE's excl_compute_next. */
#ifdef ARTS_RELEASE_RETAIN
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
/* Home arbiter — pure function of the single lock_state word, run in a
 * CAS-retry loop.  new_owner is read only by EXCL_OP_CONFIRM (sets owner in the
 * same word as w--/phase); ignored by the other ops.  The FORWARD recipient is
 * LOCK_OWNER() of the returned word; the migrate target / served reader(s) are
 * resolved by the caller from rw_waiters/ro_waiters (a pure arbiter cannot peek
 * the queues). */
uint64_t lock_owner_compute_next(uint64_t cur, int op, unsigned int new_owner,
                                uint32_t *out_action);
#endif /* ARTS_RELEASE_RETAIN */

/* ── cache_state transition ops + actions (PURGE) ────────────────────────
 * cache_compute_next(cur, op, &action) is the cache-side analogue of the home's
 * excl_compute_next: a pure function of the current word, run inside a
 * CAS-retry loop.  The ACQ_* ops carry the count++ AND the request/join
 * decision inside the one CAS; the REL_* ops carry the count-- inside the CAS
 * (the decrement and the 0-edge state change must be atomic together).  See
 * arbiters.c for the invariants this single-atom shape upholds. */
#ifdef ARTS_RELEASE_PURGE
#define CACHE_OP_ACQ_RW 0
#define CACHE_OP_ACQ_RO 1
#define CACHE_OP_GRANT_RW 2
#define CACHE_OP_GRANT_RO 3
#define CACHE_OP_REL_RW 4
#define CACHE_OP_REL_RO 5
/* A create recording its own hold in a cache this rank already made for the
 * block (see the arbiter for the word it admits). */
#define CACHE_OP_CREATE_HOLD 6
/* A size round or a dismiss answered this rank's read request: re-send for a
 * cohort that rides no write request, or close the axis. */
#define CACHE_OP_CTS_RO 7
/* A size round answered this rank's write request: the home is installed and
 * the claim is elsewhere, so the stub bit clears before the re-send. */
#define CACHE_OP_DECLAIM 8

#define CACHE_ACT_NONE 0
#define CACHE_ACT_SELF_SERVE 1 /* covering grant held: serve the dep directly */
#define CACHE_ACT_SEND_RW 2    /* opened the RW request: park, then send */
#define CACHE_ACT_SEND_RO 3    /* opened the RO request: park, then send */
#define CACHE_ACT_PARK 4       /* covered by the in-flight request: park */
#define CACHE_ACT_DRAIN_BOTH 5 /* RW grant: serve rw_pending + ro_pending */
#define CACHE_ACT_DRAIN_RO 6   /* RO grant: serve ro_pending */
#define CACHE_ACT_REL_RW 7     /* send RW RELEASE (with the payload, unless it
                                * rests in the CXL store) */
#define CACHE_ACT_REL_RO 8     /* send RO RELEASE (notify) */
/* A grant or size round that answers no request this rank has out: the commit
 * site raises it.  The arbiter leaves the word unchanged. */
#define CACHE_ACT_LATE 10
#define CACHE_ACT_RESEND_RO 11

/* Defined in arbiters.c (included by purge.c); exposed non-static for the
 * cache-state model test (mirrors excl_compute_next's exposure). */
uint64_t cache_compute_next(uint64_t cur, int op, uint32_t *out_action);
#endif /* ARTS_RELEASE_PURGE */

/* ── cache_state transition ops + actions (RETAIN) ─────────────────────────
 * cache_owner_compute_next(cur, op, &action) is the RETAIN owner-cache
 * arbiter. */
#ifdef ARTS_RELEASE_RETAIN
#define CACHE_OP_ACQ_RW 0
#define CACHE_OP_ACQ_RO 1
/* No FORWARD ops: FORWARD-migrate / FORWARD-serve do not transit
 * cache_owner_compute_next — arts_handler_db_excl_forward CASes
 * migrate_target / pushes ro_serve directly (the target/reader is not a
 * pure-function output). */
#define CACHE_OP_REL_RW 4 /* local RW release: wc-- + 0-edge → migrate/noop */
#define CACHE_OP_REL_RO 5 /* local RO release: rc-- */
/* A create recording its own hold in a cache this rank already made for the
 * block (see the arbiter for the word it admits). */
#define CACHE_OP_CREATE_HOLD 6
#define CACHE_OP_CTS_RO 7

/* Values are aligned with the PURGE block above so that a name shared by both
 * policies also shares its number, and the acquire actions carry PURGE's
 * contract: a SELF_SERVE acquire never parks, and a SEND_* or PARK acquire
 * pushes its waiter after the CAS that counted it. */
#define CACHE_ACT_NONE 0
#define CACHE_ACT_SELF_SERVE 1 /* a grant or the resident copy serves the dep */
#define CACHE_ACT_SEND_RW 2    /* opened the RW request: park, then send */
#define CACHE_ACT_SEND_RO 3    /* opened the RO request: park, then send */
#define CACHE_ACT_PARK 4       /* covered by the in-flight request: park */
#define CACHE_ACT_DRAIN_BOTH 5 /* serve rw_pending + ro_pending */
#define CACHE_ACT_REL_RO 8     /* the rc zero edge was reached */
#define CACHE_ACT_MIGRATE 9  /* wc 0-edge + migrate_target set: send DELIVER */
#define CACHE_ACT_LATE 10
#define CACHE_ACT_RESEND_RO 11

/* CACHE_ACT_REL_RO marks the edge, NOT the wire send.  Whether a RO_RETURN goes
 * out is derived from the COMMITTED transition by the caller —
 * ro_st(cur) in {REQ, GRANT, GRANT_PURGE} && ro_st(next) == IDLE — because that
 * CAS is the exactly-once token and a concurrent recall must not be able to
 * make two sites send for one grant.  Under RETAIN the edge may relinquish
 * (GRANT_PURGE) or retain (GRANT); the engine must be driven either way. */

uint64_t cache_owner_compute_next(uint64_t cur, int op, uint32_t *out_action);
#endif /* ARTS_RELEASE_RETAIN */

/* The request packet's cover word: a read request carries its parity; a write
 * request declares the read request its rank has out (open + parity) and
 * whether it was opened before a create claimed the cache. */
#define EXCL_COVER_PAR 0x1u
#define EXCL_COVER_RO_OPEN 0x2u
#define EXCL_COVER_RO_PAR 0x4u
#define EXCL_COVER_PRECLAIM 0x8u

/* Pending waiter (parked EDT dep slot) used in both ro_pending and rw_pending.
 * Same shape as VAL's RW waiter for consistency. */
struct arts_db_excl_waiter_s {
  arts_lf_link_t link; /* FIRST — required by arts_lf_stack_t */
  arts_guid_t edt_guid;
  unsigned int slot;
};

/* A read request queued on the home's ro_waiters.  Written before the push
 * that publishes it, read and freed by the frame whose drain took it (or by
 * the home's teardown). */
struct arts_lock_ro_node_s {
  arts_lf_link_t link; /* FIRST */
  unsigned int rank;
  uint32_t cover;                  /* the request's cover word (its parity) */
  struct arts_rdzv_landing_s rdzv; /* the requester's grant/serve landing */
};

/* Each arm that runs a home request queue defines its own
 * arts_home_grantreq_node_s and arts_home_grantreq_queue_s; this is EXCL's,
 * in the same Vyukov MPSC shape, so the same directory.h declarations apply.
 * Producers are the request handlers; the single consumer is the holder of
 * the queue's turn: the committer of the lock_state transition whose action
 * pops or peeks it, or, under RETAIN, the CONFIRM that ends a forwarded
 * migrate.  Orders: the queue's declaration in coherence/directory.h. */
struct arts_home_grantreq_node_s {
  _Atomic(struct arts_home_grantreq_node_s *) next;
  unsigned int rank;
  uint64_t have_version; /* version the requester already holds */
  struct arts_rdzv_landing_s rdzv; /* requester's grant/deliver landing */
};

struct arts_home_grantreq_queue_s {
  _Atomic(struct arts_home_grantreq_node_s *) tail; /* producer end */
  _Atomic(struct arts_home_grantreq_node_s *) head; /* consumer end */
  struct arts_home_grantreq_node_s stub;            /* permanent sentinel */
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
   * slot's (arts_shared_slot_t, shared.h). */
  _Atomic(arts_shared_slot_t) buffer;
  arts_lf_stack_t ro_pending; /* parked RO EDT waiters on this rank */
  arts_lf_stack_t rw_pending; /* parked RW EDT waiters on this rank */
#ifdef ARTS_RELEASE_RETAIN
  /* Reader ranks the owner must serve during the RO phase.  Home pushes via
   * FORWARD; the owner drains (DELIVER copy to each reader) when wc reaches 0.
   * This list cannot be packed into the single cache_state word. */
  arts_lf_stack_t ro_serve;
  /* The pending migrate target's landing (its stable buffer), written by the
   * FORWARD(migrate) handler BEFORE the CAS that publishes migrate_target
   * (single in-flight migration per owner — home serializes by CONFIRM), read
   * by whichever actor ships the DELIVER on the wc 0-edge. */
  struct arts_rdzv_landing_s migrate_rdzv;
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
  struct arts_rank_bitset_s cached_ranks;      /* destroy fan-out roster */
  /* Per requester rank, the parity of its last read request this home has
   * accounted for, by its arrival or by a declaration of it, whichever came
   * first: the one that finds the bit already at the request's parity is the
   * second of the pair.  Each update is one acq_rel RMW of the rank's word. */
  struct arts_rank_bitset_s ro_parity;
  /* Per requester rank, whether its read request waits here to be served as
   * a read.  Set by the request before its parity step; taken (one acq_rel
   * RMW returning the old bit) by whichever comes first of the drain that
   * pops its node and the same rank's write request whose parity step found
   * the read request already arrived.  The drain serves the node only if it
   * took the bit; otherwise that write turn covers the readers and the node
   * is dismissed, its read count released here. */
  struct arts_rank_bitset_s ro_queued;
  /* The rank whose create claimed a first-touch cache, written before the
   * descriptor is published (ordered by the route slot's install). */
  unsigned int claim_rank;
#ifdef ARTS_RELEASE_RETAIN
  /* [td:1 (31) | rwo:1 (30) | roo:1 (29) | owed:29]: the destroy's mark and
   * what the home was told is still coming, one word so that marking and
   * settling read each other.  Every write after init is an acq_rel RMW. */
  _Atomic(uint32_t) cover_gate;
#endif
#ifdef ARTS_RELEASE_RETAIN
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

#ifdef __cplusplus
}
#endif

/* Shared by both release policies: perform the home teardown (notify every
 * rank that ever cached the DB, then return the route slot).  PURGE reaches it
 * from whichever of the destroy and the last release hits the zero edge under
 * the teardown mark; RETAIN calls it directly on destroy. */
struct arts_db_s;
void arts_excl_home_teardown(struct arts_db_s *db, arts_guid_t db_guid);

#endif /* ARTS_COHERENCE_EXCL_TYPES_H */
