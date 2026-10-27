/* SPDX-License-Identifier: Apache-2.0
 *
 * Coherence protocol wire-message handlers: the per-message bodies the wire
 * dispatcher, the OoO engine and the self-send shortcuts call.
 *
 * Handlers run concurrently on every progress thread, and a self-send's
 * handler runs inline on the sending thread, a worker included, so no thread
 * is any field's sole writer by role.  A field a handler shares with a worker
 * or with another handler is accessed atomically, or written plainly before
 * a release read-modify-write on a count or state word that its reader
 * acquires first.
 *
 * Drop discipline — two categories:
 *   Cat-B (deferrable bodies, each an OOO_* kind in ooo.h, run at the home
 *     — e.g. GRANT_REQUEST / SNAPSHOT_REQUEST / PUBLISH / DESTROY): the wire
 *     dispatcher routes through arts_ooo_dispatch_or_defer_guid, which
 *     acquires this rank's db_s ref-pinned and hands the pure (item, args) body
 *     a live cache on HIT, or defers the args for replay once DB_CREATE
 *     installs.
 *   Cat-C (non-deferrable, no OOO_* kind — e.g. SNAPSHOT_RESPONSE /
 *     CACHE_DESTROY / PUBLISH_ACK / SNAPSHOT_REDIRECT / GRANT_INVALIDATE /
 *     GRANT_CONFIRM / GRANT_CONFIRM_ACK): the wire dispatcher (and the
 *     matching self-send shortcut) does a ref-pinned lookup via
 *     arts_route_table_lookup_db; on HIT it calls the pure (item, args)
 *     body, on MISS it applies the handler's exact miss-action (silent
 *     drop, a dismiss, CACHE_DESTROY reply, or sem-post — see each body).
 *   Either way an (item, args) body is handed its block already acquired
 *   and ref-pinned by the dispatcher or OoO engine and does not look it up
 *   to operate on it.  A body handed the whole packet (GRANT_RESPONSE,
 *   INV_DELIVER, EXCL_GRANT and the EXCL RETAIN messages) looks its block up
 *   itself. */

#ifndef ARTS_COHERENCE_HANDLERS_H
#define ARTS_COHERENCE_HANDLERS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "arts/transport/protocol.h"

/* Forward declaration for WB handler parameters. */
struct arts_db_cache_s;

/* ===== Cat-C handler args ============================================
 * Cat-C (silent-drop / response-ACK) handlers are NOT OoO-deferrable: they
 * carry no OOO_* kind and are never enqueued on a slot's ooo_list.  The wire
 * dispatcher (and the matching self-send shortcut) does the ref-pinned
 * lookup-acquire; on a HIT it calls the pure (item, args) body, and on a MISS
 * it applies that handler's exact miss-action (silent drop, CACHE_DESTROY
 * reply, or sem-post — see each handler's contract).  These args structs are
 * stack-built by the dispatcher from the decoded wire packet and passed by
 * pointer to the body; unlike the OoO Cat-B args they are never copied into a
 * heap payload. */

/* arts_handler_db_snapshot_response body args.  data_present mirrors the wire
 * packet's tri-state (0 = no data moved, 1 = data, 2 = size-only CTS).  A
 * data_present == 1 reply that moved bytes landed them one-sided
 * (rdzv_txid != 0; the landing is named by rdzv_cookie and install pairs by
 * txid); a same-rank serve moves none. */
struct arts_db_snapshot_response_args_s {
  uint32_t cover; /* the answered request's cover word */
  uint32_t data_present;
  uint64_t version;
  uint64_t db_size;     /* CTS: allocation size; data: landed byte count */
  uint64_t rdzv_txid;   /* != 0: payload PUT into our landing; pair by txid */
  uint64_t rdzv_cookie; /* our landing handle, echoed back */
};

/* arts_handler_db_cache_destroy body args. */
struct arts_db_cache_destroy_args_s {
  arts_guid_t db_guid;
};

/* arts_handler_db_publish_ack body args.  Two wake planes coexist: cv != 0 is
 * the pointer-identity sem-post of a grant return sent on its own (PURGE) —
 * cache-independent, posted on both HIT and MISS so a torn-down cache never
 * strands the blocked returner; cv == 0 is the flight-completion plane of
 * every publish flight, control-only ones included (the body drains the
 * releaser cache's version-covered waiter stack).  version is the published
 * version the ACK covers; the credit triple is the home's next in-place
 * publish credit (txid == 0 = none issued), which only a payload flight's
 * ACK carries. */
struct arts_db_publish_ack_args_s {
  uint64_t cv; /* a standalone grant return's heap rendezvous, or 0 */
  uint64_t version;
  uint64_t credit_addr;
  uint64_t credit_rkey;
  uint64_t credit_txid;
};

#ifdef ARTS_PROTOCOL_FLUSH
struct arts_db_fetch_response_args_s {
  uint64_t edt_cookie; /* the parked EDT's address on this rank */
#ifdef ARTS_DEBUG_WAKE_GUID
  arts_guid_t edt_guid;
#endif
  uint32_t slot;
  uint32_t kind;
  uint64_t db_size;
  uint64_t rdzv_txid;
  uint64_t rdzv_cookie;
  uint64_t line_addr;
  uint64_t line_rkey;
  uint64_t flush_txid;
};
struct arts_db_flush_commit_args_s {
  unsigned int releaser;
  arts_guid_t db_guid;
  uint64_t txid;
  uint64_t sem;
};
struct arts_db_flush_ack_args_s {
  uint64_t sem;
  uint64_t next_txid;
};
struct arts_db_flush_cts_args_s {
  uint64_t sem;
  uint64_t line_addr;
  uint64_t line_rkey;
  uint64_t txid;
};
/* Cat-B bodies (OoO): the engine pinned the home descriptor.  Both are
 * messages a rank can send before the home has installed the block — a first
 * touch ahead of the CREATE, and a creator's release ahead of its own CREATE's
 * return — which a route-table miss cannot tell from a destroy.  Their args
 * are the arts_ooo_args_db_* structs in ooo.h, like every other OoO kind. */
void arts_handler_db_fetch_request(void *item_v, void *args_v);
void arts_handler_db_flush_announce(void *item_v, void *args_v);
/* Cat-C bodies: the dispatcher looked the descriptor up.  item_v may be NULL
 * for flush_commit / flush_ack / flush_cts — the dispatcher passes what the
 * lookup gave it, unguarded, because a torn-down block must never strand a
 * blocked releaser: each of these completes the releaser's hand-shake on a
 * MISS as well as on a HIT (post the semaphore the message carries; a CTS with
 * no descriptor still hands its txid — zero or not — to the waiter, which is
 * what tells the waiter to skip the PUT).  Each of them follows a message the
 * home itself sent after installing, so a miss here means the block is gone,
 * never that it has not arrived.  Only fetch_response has a live descriptor by
 * construction: the dispatcher's own MISS branch discards the landing instead
 * of calling it. */
void arts_handler_db_fetch_response(void *item_v, void *args_v);
void arts_handler_db_flush_commit(void *item_v, void *args_v);
void arts_handler_db_flush_ack(void *item_v, void *args_v);
void arts_handler_db_flush_cts(void *item_v, void *args_v);
/* A fetch response whose descriptor is gone: free the landing the cookie
 * names once (or as soon as) the PUT of `len` bytes has landed. */
void arts_db_flush_discard_landing(uint64_t txid, uint64_t len,
                                   uint64_t cookie);
#endif /* ARTS_PROTOCOL_FLUSH */

#if defined(ARTS_PROTOCOL_VAL) && defined(ARTS_WRITE_POLICY_WB)
/* arts_handler_db_snapshot_redirect body args (owner side). */
struct arts_db_snapshot_redirect_args_s {
  arts_guid_t db_guid;
  unsigned int requester_rank;
  uint32_t cover;
  struct arts_rdzv_landing_s rdzv; /* requester's landing, forwarded by home */
};
#endif

/* ===== Home-side handlers ============================================ */

/* Cat-B pure body (OoO g_ooo_table[OOO_DB_GRANT_REQUEST]): item_v is the
 * home db_s the engine acquired (cache is its first member); args_v is an
 * arts_ooo_args_db_grant_request_s.  The wire dispatcher decodes
 * GRANT_REQUEST into those args and routes through
 * arts_ooo_dispatch_or_defer_guid.  Defined for the release-consistency family
 * (coherence/grant.c) where GRANT_REQUEST exists. */
void arts_handler_db_grant_request(void *item_v, void *args_v);
#ifdef ARTS_RELEASE_PURGE
/* Cat-B pure body (OoO g_ooo_table[OOO_DB_GRANT_RETURN]): item_v is the home
 * db_s the engine acquired; args_v is an arts_ooo_args_db_grant_return_s.
 * Accepts the returned write right, or parks it in the home's single-slot
 * latch for the thread already inside the serving tail.  Defined in
 * coherence/grant_purge.c. */
void arts_handler_db_grant_return(void *item_v, void *args_v);
#endif
/* Cat-B pure body (OoO g_ooo_table[OOO_DB_SNAPSHOT_REQUEST]): item_v is the
 * home db_s the engine acquired (cache is its first member); args_v is an
 * arts_ooo_args_db_snapshot_request_s.  The wire dispatcher decodes the packet
 * into those args and routes through arts_ooo_dispatch_or_defer_guid. */
void arts_handler_db_snapshot_request(void *item_v, void *args_v);
/* Cat-B pure body (OoO g_ooo_table[OOO_DB_PUBLISH]): item_v is the home db_s
 * the engine acquired (cache is its first member); args_v is an
 * arts_ooo_args_db_publish_s and nothing trails it: a payload lands one-sided
 * in the home's stable buffer (arts_send_db_publish).  The wire dispatcher
 * decodes the packet into those args and routes through
 * arts_ooo_dispatch_or_defer_guid. */
void arts_handler_db_publish(void *item_v, void *args_v);
/* Cat-B pure body (OoO g_ooo_table[OOO_DB_DESTROY]): item_v is the home db_s
 * the engine acquired (cache is its first member); args_v is an
 * arts_ooo_args_db_destroy_s.  The wire dispatcher decodes DESTROY_REQ into
 * those args and routes through arts_ooo_dispatch_or_defer_guid. */
void arts_handler_db_destroy(void *item_v, void *args_v);
/* OOO_DB_CREATE body (g_ooo_table): item_v is NULL (the engine runs a create
 * only on an empty slot); args_v is an arts_ooo_args_db_create_s (an announce)
 * or an arts_ooo_args_create_local_s naming a descriptor built on this rank.
 * The wire dispatcher and the self-send enter it through
 * arts_ooo_dispatch_or_defer_guid. */
void arts_handler_db_create(void *item_v, void *args_v);
/* An announce, received or self-sent, entering the engine as OOO_DB_CREATE. */
void arts_db_create_announce_enter(
    const struct arts_msg_db_create_coherent_packet_s *p);

/* ===== Sharer-side (response) handlers =============================== */

/* GRANT_RESPONSE at new owner C: payload = full contiguous wire buffer
 * (header + map + data); size is total bytes.  One signature for both write policies —
 * WT drains + runs immediately, WB defers the RW drain to CONFIRM_ACK. */
void arts_handler_db_grant_response(void *payload, size_t size);
/* Cat-C pure body (SNAPSHOT_RESPONSE): item_v is the db_s the dispatcher acquired
 * (cache is its first member); args_v is an
 * arts_db_snapshot_response_args_s.  NOT OoO-deferrable — the dispatcher
 * looks the cache up with a held ref and, on a MISS (DB destroyed / slot
 * NULL-stored), SILENTLY DROPS (this 1:1 response resumes a parked EDT; if the
 * cache is gone the EDT was already torn down). */
void arts_handler_db_snapshot_response(void *item_v, void *args_v);
/* Consume an in-flight rendezvous whose receiver-side target is gone (the
 * metadata packet reached a destroyed object): registers a discard
 * continuation so the landing frees and the txid pairing table stays
 * leak-free.  `len` is the payload the metadata names.  No-op for txid == 0.
 */
void arts_db_rdzv_discard_landing(uint64_t txid, uint64_t len,
                                  uint64_t cookie);
/* Pure (cache, args) body: item_v is the db_s (cache is its first member);
 * args_v is an arts_ooo_args_db_grant_invalidate_s.  The wire dispatcher /
 * self-send looks the cache up and calls this body DIRECTLY — no OoO defer.  In
 * both write policies the revoking home publishes the invalidate target
 * (rw_holder) only at the post-install CONFIRM owner-swap, so the cache is
 * provably installed when INVALIDATE arrives.  WT/WB define the real body
 * (sentinel withdrawal / transfer trigger); a home whose grants come back
 * unasked sends no INVALIDATE at all. */
void arts_handler_db_grant_invalidate(void *item_v, void *args_v);
/* Cat-C pure body (PUBLISH_ACK): item_v is the releaser-rank db_s the
 * dispatcher acquired; args_v is an arts_db_publish_ack_args_s.  NOT
 * OoO-deferrable.  cv != 0 wakes a standalone grant return's pointer-identity
 * sem (cache-independent — posted on BOTH HIT and MISS so a torn-down cache
 * never strands the blocked returner); cv == 0 runs flight completion on the
 * cache (credit record, covered-waiter drain, trailing-flight decision). */
void arts_handler_db_publish_ack(void *item_v, void *args_v);
/* Cat-C pure body (CACHE_DESTROY): item_v is the db_s the dispatcher acquired
 * (cache is its first member); args_v is an arts_db_cache_destroy_args_s.
 * NOT OoO-deferrable — the dispatcher looks the cache up with a held ref and,
 * on a MISS (already torn down on this rank), SILENTLY DROPS (idempotent). */
void arts_handler_db_cache_destroy(void *item_v, void *args_v);

/* ===== WB-only handlers ============================================== */

#if defined(ARTS_PROTOCOL_VAL) && defined(ARTS_WRITE_POLICY_WB)
/* Cat-C pure body (SNAPSHOT_REDIRECT, owner side): item_v is the db_s the dispatcher
 * acquired (cache is its first member); args_v is an
 * arts_db_snapshot_redirect_args_s.  NOT OoO-deferrable — the dispatcher
 * looks the cache up with a held ref and, on a MISS, sends CACHE_DESTROY to
 * the requester, retiring its cache: the parked RO waiter is freed with that
 * cache, never resumed.  A MISS means a destroy was issued while the read was
 * pending, which the destroy contract excludes. */
void arts_handler_db_snapshot_redirect(void *item_v, void *args_v);
#endif /* VAL x WB */

#if (defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)) &&              \
    !defined(ARTS_RELEASE_PURGE)
/* Cat-C pure body (CONFIRM, home side; both write policies of the revoking
 * release policy — a voluntarily returned grant names no holder, so it has no
 * CONFIRM): item_v is the db_s the dispatcher acquired (cache is its first
 * member); args_v is unused (the new owner is read from
 * db->pending_install_owner).  NOT OoO-deferrable — the
 * dispatcher looks the cache up with a held ref and, on a MISS (DB destroyed),
 * SILENTLY DROPS.  Records the new rw_holder, then starts the next transfer
 * round or releases the invalidate_in_flight baton.  WB additionally replies
 * with CONFIRM_ACK; WT does not (the new owner already drained at
 * GRANT_RESPONSE). */
void arts_handler_db_grant_confirm(void *item_v, void *args_v);

/* Send CONFIRM from new owner C back to home A once the ownership transfer is
 * complete (both write policies; home flips rw_holder + advances the round). */
void arts_send_db_grant_confirm(unsigned int home_rank, arts_guid_t db_guid);
#endif /* shared grant plane, revoking release policy */

/* ===== Sender helpers ================================================ */

/* Send GRANT_REQUEST to the DB's home (the home FIFO orders
 * rank-by-rank).  When the requester knows db_size a fresh transfer landing
 * is allocated and advertised in the request; a size-unknown first touch
 * sends landing-less (txid 0) and home answers GRANT_CTS, whose handler
 * re-issues through this sender with the landing attached. */
void arts_send_db_grant_request(struct arts_db_cache_s *cache, uint32_t cover);
/* Cat-C pure body (GRANT_CTS at the requester): item_v is the db_s the
 * dispatcher acquired; args_v is the GRANT_CTS packet.  Learns db_size
 * and re-issues the in-flight GRANT_REQUEST with a landing (the
 * coalescing flag stays held — this is the same round continuing).  MISS
 * (DB destroyed) silently drops. */
void arts_handler_db_grant_cts(void *item_v, void *args_v);
/* GRANT_CTS sender: home → remote first-touch requester (db_size reply). */
void arts_send_db_grant_cts(unsigned int requester_rank,
                                arts_guid_t db_guid, uint64_t db_size);
/* cv: nonzero only on an announce — the address of the releaser's heap
 * rendezvous (arts_db_pub_rendezvous_s; sem first) as uint64_t, forwarded
 * verbatim and echoed back in the PUBLISH_CTS for pointer-identity wakeup; a
 * commit and a data-less publish carry 0, and their ACK completes the
 * releaser's publish flight.  Every send is control-only: a same-rank send is
 * the ordering round of a releaser whose buffer is the canonical copy, and a
 * remote one is an announce (txid==0) or a commit (txid!=0) — the payload
 * travels one-sided between them, see arts_db_publish_sync. */
/* return_grant rides a PAYLOAD COMMIT leg only: it tells the home that the
 * releaser has given the write right back, and the home may act on that no
 * earlier than the moment these bytes have landed. */
void arts_send_db_publish(unsigned int home_rank, arts_guid_t db_guid,
                          uint64_t version, uint64_t cv, uint64_t data_size,
                          uint64_t rdzv_txid, uint64_t rdzv_cookie,
                          bool return_grant);
/* PUBLISH_CTS: home → releaser, advertising the home's stable buffer, in
 * place, as the landing for an announced dirty publish or a landing-less EXCL
 * PURGE release (cv echoed verbatim). */
void arts_send_db_publish_cts(unsigned int releaser_rank, arts_guid_t db_guid,
                                const struct arts_rdzv_landing_s *landing,
                                uint64_t cv);
/* home_cache non-NULL mints and attaches the next publish credit for
 * releaser_rank (stable-buffer {addr, rkey} + a fresh rdzv txid); NULL (or a
 * data-less / self-rank / single-rank ACK) sends a zero credit. */
void arts_send_db_publish_ack(unsigned int releaser_rank, arts_guid_t db_guid,
                                uint64_t cv, uint64_t version,
                                struct arts_db_cache_s *home_cache);
/* CREATE_RETURN — home -> remote creator after the home-side install: the
 * creator's FIRST publish credit, so create -> write -> release needs no
 * announce round.  Fire-and-forget hint; minting nothing (self-rank,
 * single-rank, data-less DB) sends nothing and leaves the creator on the
 * announce leg. */
void arts_send_db_create_return(unsigned int creator_rank,
                                struct arts_db_cache_s *home_cache);
/* new_owner_rank: rank home selected as next owner; new_owner_rdzv: that
 * rank's transfer landing (from its queued request), forwarded so the current
 * holder can PUT the transfer payload directly (NULL = zero landing —
 * sentinel/data-less round). */
void arts_send_db_grant_invalidate(
    unsigned int owner_rank, arts_guid_t db_guid, unsigned int new_owner_rank,
    const struct arts_rdzv_landing_s *new_owner_rdzv, uint64_t new_owner_have);
/* Send SNAPSHOT_REQUEST to the DB's home, advertising a fresh
 * snapshot landing when db_size is known (multi-rank runs); a size-unknown
 * first touch sends landing-less and the server answers a size-only CTS
 * response (data_present == 2), whose handler re-enters this sender. */
void arts_send_db_snapshot_request(struct arts_db_cache_s *cache,
                                   uint32_t cover);
/* Send SNAPSHOT_RESPONSE.  kind (-> wire data_present): 0 = no data moved (a
 * non-NULL landing's cookie is echoed so the requester recycles it), 1 =
 * payload (PUT from src_h's buffer into `landing`; src_h — a strong buffer
 * ref — is CONSUMED: transferred to the PUT's local completion, or released
 * after an inline self-serve), 2 = an answer without data: a size round
 * (db_size) or a dismiss, either returning the landing.  cover echoes the
 * answered request's cover word, whose parity the requester checks.  src_h
 * must be NULL for kinds 0/2.  db is the descriptor of db_guid the caller
 * holds pinned, or NULL when this rank has none: to a requester that is this
 * rank the reply is applied to db in place, and with no descriptor only the
 * landing goes back. */
void arts_send_db_snapshot_response(unsigned int requester_rank,
                                    struct arts_db_s *db, arts_guid_t db_guid,
                                    uint64_t version, uint32_t cover,
                                    uint32_t kind, uint64_t db_size,
                                    const struct arts_rdzv_landing_s *landing,
                                    arts_shared_ptr_t src_h);
/* owed: what the claimed first-touch cache had already asked the home for
 * (ARTS_CREATE_OWED_*), 0 for every other create. */
void arts_send_db_create_coherent(unsigned int home_rank, arts_guid_t db_guid,
                                  uint64_t db_size, uint16_t flags,
                                  uint16_t db_type, uint64_t cxl_addr,
                                  uint8_t owed);
void arts_send_db_destroy(unsigned int home_rank, arts_guid_t db_guid);
void arts_send_db_cache_destroy(unsigned int sharer_rank, arts_guid_t db_guid);

#if defined(ARTS_PROTOCOL_VAL) && defined(ARTS_WRITE_POLICY_WB)
/* Send SNAPSHOT_REDIRECT from home to the current owner, asking the owner to
 * serve SNAPSHOT_RESPONSE or a no-data response directly to requester_rank. */
void arts_send_db_snapshot_redirect(unsigned int owner_rank,
                                    arts_guid_t db_guid,
                                    unsigned int requester_rank, uint32_t cover,
                                    const struct arts_rdzv_landing_s *rdzv);
#endif

#if (defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)) &&              \
    defined(ARTS_WRITE_POLICY_WB)

/* Send CONFIRM_ACK from home to the new owner C after home has flipped
 * rw_holder to C. Self-send dispatches the handler inline.
 * piggyback_new_owner == ARTS_NO_PENDING_OWNER ⇒ plain CONFIRM_ACK (no
 * pending requester).  Otherwise the round advances and the ack carries the
 * next transfer target, so the new owner's handler applies the INVALIDATE
 * effect in the same message (no separate INVALIDATE, no reorder window). */
void arts_send_db_grant_confirm_ack(
    unsigned int new_owner_rank, arts_guid_t db_guid,
    unsigned int piggyback_new_owner,
    const struct arts_rdzv_landing_s *piggyback_rdzv, uint64_t piggyback_have);
/* Cat-C pure body (CONFIRM_ACK, new-owner side): item_v is the db_s the
 * dispatcher acquired (cache is its first member); args_v is the
 * CONFIRM_ACK packet (its new_owner_rank carries the piggybacked invalidate
 * target, or ARTS_NO_PENDING_OWNER for a plain ack). Drains the parked RW
 * waiters that the GRANT_RESPONSE handler deferred, clears the gate, applies the
 * piggybacked invalidate effect (publish incoming_new_owner + withdraw the
 * sentinel), and removes the drain guard (the relocated 0-edge ship-check). NOT
 * OoO-deferrable — the dispatcher looks the cache up with a held ref and, on a
 * MISS (DB destroyed), SILENTLY DROPS: the gated waiters are acquires the
 * destroy contract excludes, freed with the cache and never resumed. */
void arts_handler_db_grant_confirm_ack(void *item_v, void *args_v);

/* Kick a new GRANT_INVALIDATE round: read rw_holder, send notice to
 * holder carrying new_owner as the GRANT_RESPONSE target. */
void arts_db_owner_start_invalidate_round(
    struct arts_db_cache_s *cache, unsigned int new_owner,
    const struct arts_rdzv_landing_s *new_owner_rdzv, uint64_t new_owner_have);
#endif /* (VAL or INV) x WB */

#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)
/* Shared owner→owner transfer ship (defined in coherence/grant.c): ship the
 * current buffer (+ serialized owner-side map for WB, empty map for WT) to
 * new_owner via the GRANT_RESPONSE wire.  The target is an argument because
 * where it comes from is the release policy's: a revocation round publishes
 * it on the holder's cache ahead of the sentinel withdrawal, a home serving
 * its own queue names the requester it just popped. */
void arts_db_send_grant_response(struct arts_db_cache_s *cache,
                                 unsigned int new_owner,
                                 const struct arts_rdzv_landing_s *new_rdzv,
                                 bool data_less);
#ifndef ARTS_RELEASE_PURGE
/* Ship to the target published on this cache by a revocation round, re-arming
 * the field to the sentinel before the send.  Called on the 0-edge of
 * release_rw / the INVALIDATE handler (both write policies) and the WB
 * CONFIRM_ACK / WT GRANT_RESPONSE drain-guard removal. */
void arts_db_grant_ship_pending(struct arts_db_cache_s *cache);
#endif

#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)
/* The version a serve from the home may claim for the bytes it sends — the
 * arm's own canonical publication axis.  Under a write-through home that is
 * NOT the served buffer's own version: a home-resident writer mutates that
 * buffer in place, so its lane stands still while the data moves on, and a
 * serve stamped from it would hand two different write windows the same
 * number.  Read under the serve claim, which is what holds it still. */
uint64_t arts_db_grant_serve_version(struct arts_db_s *db);
#endif
#endif /* shared grant plane */

#ifdef ARTS_PROTOCOL_INV
/* ===== INV protocol handlers / senders ================================= */

struct arts_msg_inv_cts_packet_s;

/* Cat-B bodies @home.  Both write policies run the same set; what differs is what
 * a read request is answered WITH (the home's own bytes, or a redirect to the
 * current grant holder) and whether a publish carries payload. */
void arts_handler_db_inv_request(void *item_v, void *args_v);
void arts_handler_db_publish(void *item_v, void *args_v);
void arts_handler_db_inv_cts(struct arts_db_s *db,
                             struct arts_msg_inv_cts_packet_s *p);
void arts_handler_db_inv_deliver(void *payload, size_t size);
void arts_handler_db_inv_invalidate(struct arts_db_s *db);
void arts_handler_db_inv_invalidate_ack(struct arts_db_s *db,
                                        unsigned int sharer_rank);
#ifdef ARTS_WRITE_POLICY_WB
/* The home holds no bytes, so a read request is forwarded to the holder; a
 * holder that no longer has the bytes bounces it back rather than queueing. */
void arts_handler_db_inv_redirect(void *item_v, void *args_v);
void arts_send_db_inv_redirect(unsigned int holder_rank, arts_guid_t db_guid,
                               unsigned int requester, uint32_t cover,
                               const struct arts_rdzv_landing_s *rdzv);
void arts_send_db_inv_request_for(arts_guid_t db_guid, unsigned int requester,
                                  uint32_t cover,
                                  const struct arts_rdzv_landing_s *rdzv);
#endif

/* Senders. */
void arts_send_db_inv_request(struct arts_db_cache_s *cache,
                              arts_db_access_mode_t mode, uint32_t cover);
/* A size round or a dismiss to a read request: see the packet.  cover is
 * the answered request's cover word; cookie the landing it named (0: none).
 * db is the descriptor of db_guid the caller holds pinned, or NULL when this
 * rank has none: to a requester that is this rank the reply is applied to db
 * in place, and with no descriptor only the landing goes back. */
void arts_send_db_inv_cts(unsigned int requester_rank, struct arts_db_s *db,
                          arts_guid_t db_guid, uint64_t db_size,
                          uint32_t cover, uint64_t cookie);
/* Serve a reader from this rank's installed buffer.  The stamp the reply
 * carries is the install-lane arbitration version ONLY: it decides which of
 * two asynchronous installs wins the receiver's buffer slot, and no branch on
 * either side compares it to judge whether a copy is still valid. */

void arts_send_db_inv_deliver(unsigned int requester_rank,
                              struct arts_db_s *db,
                              const struct arts_rdzv_landing_s *rdzv);
void arts_send_db_inv_invalidate(unsigned int sharer_rank, arts_guid_t db_guid);
void arts_send_db_inv_invalidate_ack(unsigned int home_rank,
                                     arts_guid_t db_guid);
#endif /* ARTS_PROTOCOL_INV */

#ifdef ARTS_PROTOCOL_EXCL
/* ===== EXCL protocol handlers ======================================== */

/* Cat-B pure body (OoO g_ooo_table[OOO_DB_EXCL_REQUEST]) @home: item_v is the
 * home db_s the engine acquired (cache is its first member); args_v is an
 * arts_ooo_args_db_excl_request_s.  A write request queues on rw_waiters and
 * then counts with one lock_state CAS; a read request counts first and then
 * queues on ro_waiters.  Grants follow the transition. */
void arts_handler_db_excl_request(void *item_v, void *args_v);

/* Cat-B pure body (OoO g_ooo_table[OOO_DB_EXCL_RELEASE]) @home: same item_v
 * shape; args_v is an arts_ooo_args_db_excl_release_s.  No bytes trail it: a
 * remote RW release PUT them into the home's stable buffer, the home's own
 * release wrote them in place, and in a CXL build they are already in the
 * store.  The bytes are in place before it transitions lock_state, then
 * grant-sends the next holder(s). */
void arts_handler_db_excl_release(void *item_v, void *args_v);

/* Cat-C pure body @requester: payload = the wire packet alone (a payload
 * travels one-sided); size is its byte count.  cache_state CAS (REQ→GRANT,
 * or phantom RO release) + drain of the cache pending stacks, after any
 * one-sided payload has landed. */
void arts_handler_db_excl_grant(void *payload, size_t size);

/* NOTE: arts_handler_db_acquire is NOT re-declared here.  It is already
 * declared protocol-agnostically in coherence.h as void
 * arts_handler_db_acquire(void *item, void *args), the coherent body the
 * driver's acquire_one_dep calls (an OOO_DB_ACQUIRE replay reaches it through
 * arts_db_acquire_replay_dep).  EXCL only *defines* it
 * (coherence/excl/purge.c and coherence/excl/retain.c). */

/* ===== EXCL senders =================================================== */
/* Send EXCL_REQUEST to the home.  A request to another rank's home advertises
 * this rank's stable buffer as the landing of the grant/deliver that answers
 * it, for either mode (in-place install is EXCL's fixed-address contract); a
 * size-unknown first touch sends landing-less, the home answers EXCL_CTS and
 * the handler re-enters this sender.  A request to this rank's own home
 * advertises none: the home's own grant moves no payload, and a payload a
 * remote owner owes it lands in a landing the home draws when it forwards.
 * Where grants carry no payload (the CXL store), no request advertises one. */
/* cover: the request's EXCL_COVER_* word (its parity, or a write request's
 * declaration), read from the cache-word transition that opened it. */
void arts_send_db_excl_request(struct arts_db_cache_s *cache,
                               arts_db_access_mode_t mode, uint32_t cover);
/* Cat-C pure body (EXCL_CTS at the requester): item_v is the db_s; args_v is
 * the EXCL_CTS packet.  Learns db_size and re-issues the request (echoed
 * mode) with a landing. */
void arts_handler_db_excl_cts(void *item_v, void *args_v);
/* EXCL_CTS sender: a size round or a read request's dismiss (db_size, the
 * echoed mode and cover word).  db is the home's descriptor, held pinned by
 * the caller; to a requester that is this rank it is also the requester's
 * cache, and the reply is applied to it in place. */
void arts_send_db_excl_cts(struct arts_db_s *db, unsigned int requester_rank,
                           uint64_t db_size, uint32_t mode, uint32_t cover);
/* arts_send_db_excl_grant: version is always 0, and no grant handler reads
 * it.  The grant payload PUTs into
 * req_rdzv (the requester's advertised landing; src_h — a strong ref on the
 * home buffer — is CONSUMED); `pub`, for RW grants, is home's publish landing
 * for this grant's release (NULL/zero for RO). */
void arts_send_db_excl_grant(unsigned int requester_rank, arts_guid_t db_guid,
                             arts_db_access_mode_t mode, uint64_t version,
                             const struct arts_rdzv_landing_s *req_rdzv,
                             const struct arts_rdzv_landing_s *pub,
                             arts_shared_ptr_t src_h, uint64_t data_size);
/* arts_send_db_excl_release: cv is nonzero only on an RW release's
 * landing-less announce — the address of the releaser's heap rendezvous,
 * which the home echoes in the PUBLISH_CTS to unblock the releaser; nothing
 * acknowledges a release.  version is always 0, and no handler reads it.  A
 * remote RW release PUTs the dirty bytes into its landing (the grant's `pub`,
 * or the one the PUBLISH_CTS named) first and echoes {rdzv_txid,
 * rdzv_cookie} here; a release to this rank's own home carries no bytes. */
void arts_send_db_excl_release(unsigned int home_rank, arts_guid_t db_guid,
                               arts_db_access_mode_t mode, uint64_t version,
                               uint64_t cv, uint64_t data_size,
                               uint64_t rdzv_txid, uint64_t rdzv_cookie);

#ifdef ARTS_RELEASE_RETAIN
/* ===== EXCL RETAIN handlers (Cat-C direct dispatch) = */

/* arts_handler_db_excl_forward: Cat-C @owner.  item_v is the full wire packet
 * (arts_msg_excl_forward_packet_s *); the handler looks up the db_s internally
 * by ->db_guid.  Handles home→owner forward: either migrate RW to a new owner
 * or serve one RO reader directly from the owner's buffer. */
void arts_handler_db_excl_forward(void *item_v);

/* arts_handler_db_excl_deliver: Cat-C @target.  payload is the full wire
 * buffer (header + inline data); size is total bytes (data starts at
 * sizeof(arts_msg_excl_deliver_packet_s)).  Installs data + transitions cache
 * state (GRANT or RO-hold) and drains any pending local acquires. */
void arts_handler_db_excl_deliver(void *payload, size_t size);

/* arts_handler_db_excl_confirm: Cat-C @home.  item_v is the full wire packet
 * (arts_msg_excl_confirm_packet_s *); the handler looks up the db_s internally
 * by ->db_guid.  New owner → home: migration complete; home transitions
 * lock_state and serves the next waiter if any.  packet->rank = new owner. */
void arts_handler_db_excl_confirm(void *item_v);

/* arts_handler_db_excl_roret: Cat-C @home.  item_v is the full wire packet
 * (arts_msg_excl_confirm_packet_s *); the handler looks up the db_s internally
 * by ->db_guid.  Reader → home: RO release (data-less); home decrements the
 * reader count and serves the next waiter when the count reaches zero.
 * packet->rank = the returning reader. */
void arts_handler_db_excl_roret(void *item_v);

/* ===== EXCL RETAIN senders ========================= */

/* arts_send_db_excl_forward: home → current owner.  mode=DB_MODE_RW requests
 * migration to target rank; mode=DB_MODE_RO requests serving one RO reader. */
/* arts_send_db_excl_recall: home → a rank holding an unmarked RO grant.
 * Data-less, fire-and-forget, no ack: the completion signal is the ordinary
 * RO_RETURN it provokes.  The fan-out includes the sending rank itself. */
void arts_send_db_excl_recall(unsigned int target_rank, arts_guid_t db_guid);
void arts_handler_db_excl_recall(void *item_v);

void arts_send_db_excl_forward(unsigned int owner_rank, arts_guid_t db_guid,
                               uint32_t mode, uint32_t target, uint32_t tag,
                               const struct arts_rdzv_landing_s *target_rdzv);

/* arts_send_db_excl_deliver: owner → target.  Versionless (EXCL serialization
 * guarantees ordering).  The payload PUTs into the target's forwarded landing
 * (rdzv); src_h — a strong ref on the owner buffer — is CONSUMED. */
void arts_send_db_excl_deliver(unsigned int target_rank, arts_guid_t db_guid,
                               uint32_t mode, uint32_t tag,
                               const struct arts_rdzv_landing_s *rdzv,
                               arts_shared_ptr_t src_h, uint64_t data_size);

/* arts_send_db_excl_confirm: new owner → home: migration done. */
void arts_send_db_excl_confirm(unsigned int home_rank, arts_guid_t db_guid);

/* arts_send_db_excl_roret: reader → home: RO release (data-less). */
void arts_send_db_excl_roret(unsigned int home_rank, arts_guid_t db_guid);

#endif /* ARTS_RELEASE_RETAIN */

#endif /* ARTS_PROTOCOL_EXCL */

#ifdef __cplusplus
}
#endif

#endif /* ARTS_COHERENCE_HANDLERS_H */

/* Protocol seam invoked where a release policy has a point of its own between
 * an ex-holder's last release round and the next owner's first — WB's gated
 * CONFIRM, PURGE's hand-back: the PREVIOUS holder still has the bytes it
 * wrote.  A protocol whose readers re-validate at every acquire has nothing to
 * do; a write-invalidate protocol registers that rank as a sharer.  The WT
 * policy has no such point and never calls it: INV's round keeps every
 * releaser registered as it releases (inv/engine.c). */
void arts_db_grant_note_ex_holder(struct arts_db_s *db, unsigned int rank);
