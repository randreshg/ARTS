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
 * Drop discipline — two categories, and the answers:
 *   Cat-B (deferrable bodies, each an OOO_* kind in ooo.h, run at the home
 *     — e.g. GRANT_REQUEST / SNAPSHOT_REQUEST / PUBLISH / DESTROY): the wire
 *     dispatcher routes through arts_ooo_dispatch_or_defer_guid, which
 *     acquires this rank's db_s ref-pinned and hands the pure (item, args) body
 *     a live cache on HIT, or defers the args for replay once DB_CREATE
 *     installs.
 *   Cat-C (non-deferrable, no OOO_* kind — e.g. CACHE_DESTROY /
 *     SNAPSHOT_REDIRECT / GRANT_INVALIDATE / CREATE_RETURN / FLUSH_COMMIT):
 *     the wire dispatcher does a ref-pinned lookup via
 *     arts_route_table_lookup_db; on HIT it calls the pure (item, args)
 *     body, on MISS it applies the handler's exact miss-action (a silent
 *     drop, a dismiss, or the reply its round still needs, such as an ACK
 *     — see each body).
 *   Either way an (item, args) body is handed its block already acquired
 *   and ref-pinned and does not look it up to operate on it.  A message
 *   handed over as the whole packet (the EXCL RETAIN FORWARD, RO_RETURN and
 *   RECALL) has a wire entry that looks its block up once and a body on the
 *   object.  A self-send acts on the descriptor its caller holds (a
 *   Cat-B kind through arts_ooo_dispatch_on, a Cat-C body through
 *   arts_route_table_borrow_or_lookup, or under a copy of its own,
 *   arts_route_table_copy_live, where the body can drop the count its caller
 *   runs under), resolving the GUID only when that descriptor is not live.
 *   Answers (VAL, INV: SNAPSHOT_RESPONSE, GRANT_CTS, GRANT_RESPONSE,
 *     GRANT_CONFIRM_ACK, PUBLISH_ACK, INV_CTS, INV_DELIVER; EXCL: EXCL_CTS,
 *     EXCL_GRANT, EXCL_DELIVER): no lookup.  The entry takes the requester's
 *     descriptor from the token the answer echoes, under the count its open
 *     request holds, and runs the body on it unless its tag shows it
 *     retired, when it commits nothing, ends the landing and drops that count
 *     (coherence.h; on EXCL, whose landing is the descriptor's own stable
 *     buffer, once any bytes in flight have landed).  The answers to a round
 *     the home started (INV_INVALIDATE_ACK, GRANT_CONFIRM, and EXCL_CONFIRM,
 *     which ends a migration) take the home's descriptor from the home token
 *     the same way, under the round's count (on EXCL the home's count, held
 *     while w is above zero), and on a retired one take only the round's
 *     counting step (an EXCL_CONFIRM also dismisses a held read request its
 *     rank's write turn covered).  The wire and a self-send call the same
 *     entry, except that a self-transfer runs the GRANT_RESPONSE body
 *     (arts_db_grant_response_on) directly on the descriptor it holds, an INV
 *     self-serve checks the token's descriptor for a retire itself and runs the
 *     INV_DELIVER body (inv_deliver_on), and a GRANT_CTS is never self-sent; an
 *     EXCL_CONFIRM self-send calls excl_confirm_answer, the body its wire entry
 *     unpacks the packet into.  FLUSH's answers resolve nothing either: a fetch
 *     response reaches the block through the parked EDT it names, and an ACK or
 *     a CTS wakes the releaser, which holds its own pin. */

#ifndef ARTS_COHERENCE_HANDLERS_H
#define ARTS_COHERENCE_HANDLERS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "arts/transport/protocol.h"

/* Forward declaration for WB handler parameters. */
struct arts_db_cache_s;

/* ===== Cat-C and answer handler args ==================================
 * Neither kind is OoO-deferrable: they carry no OOO_* kind and are never
 * enqueued on a slot's ooo_list.  For a Cat-C body the wire dispatcher does
 * the ref-pinned lookup-acquire (a self-send borrows its caller's descriptor
 * while it is live, or takes a copy of its own where the body can drop the
 * count its caller runs under); on a HIT it calls the pure (item, args) body,
 * and on a MISS it applies that handler's exact miss-action (a silent drop, a
 * dismiss, or the reply its round still needs — see each handler's contract).
 * An answer's args carry the token and GUID its entry acts on.  These args
 * structs are stack-built by the dispatcher (or a self-send) from the decoded
 * wire packet and passed by pointer to the body; unlike the OoO Cat-B args they
 * are never copied into a heap payload. */

/* arts_handler_db_snapshot_response body args.  data_present mirrors the wire
 * packet's tri-state (0 = no data moved, 1 = data, 2 = size-only CTS).  A
 * data_present == 1 reply that moved bytes landed them one-sided
 * (rdzv_txid != 0; the landing is named by rdzv_cookie and install pairs by
 * txid); a same-rank serve moves none. */
struct arts_db_snapshot_response_args_s {
  arts_guid_t db_guid;
  uint64_t token; /* the request's, echoed */
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

/* arts_handler_db_publish_ack args.  Two wake planes coexist: cv != 0 is
 * the pointer-identity sem-post of a grant return sent on its own (PURGE) —
 * cache-independent, so a torn-down cache never strands the blocked
 * returner; cv == 0 is the flight-completion plane of every publish flight,
 * control-only ones included (the body drains the version-covered waiter
 * stack of the releaser descriptor the token names).  version is the
 * published version the ACK covers; the credit triple is the home's next
 * in-place publish credit (txid == 0 = none issued), which only a payload
 * flight's ACK carries. */
struct arts_db_publish_ack_args_s {
  arts_guid_t db_guid;
  uint64_t token; /* the flight's, echoed (cv == 0 only) */
  uint64_t cv; /* a standalone grant return's rendezvous, or 0 */
  uint64_t version;
  uint64_t credit_addr;
  uint64_t credit_rkey;
  uint64_t credit_txid;
};

#ifdef ARTS_VALIDITY_FLUSH
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
/* A release at the home (Cat-C): the dispatcher looked the descriptor up, and
 * item_v may be NULL — a torn-down block must never strand a blocked
 * releaser, so the commit is acknowledged on a MISS as well as on a HIT. */
void arts_handler_db_flush_commit(void *item_v, void *args_v);
/* Answers to this rank's own rounds, which resolve nothing.  A fetch response
 * reaches the block through the parked EDT its cookie names (that slot's
 * descriptor pin).  An ACK or a CTS writes into the releaser's wait and posts
 * it; the woken releaser, which holds its pin, carries the credit and the
 * line into the cache.  A CTS's txid, zero or not, is what tells the releaser
 * whether to PUT. */
void arts_handler_db_fetch_response(
    const struct arts_db_fetch_response_args_s *a);
void arts_handler_db_flush_ack(const struct arts_db_flush_ack_args_s *a);
void arts_handler_db_flush_cts(const struct arts_db_flush_cts_args_s *a);
#endif /* ARTS_VALIDITY_FLUSH */

#if defined(ARTS_VALIDITY_VAL) && defined(ARTS_READ_SOURCE_OWNER)
/* arts_handler_db_snapshot_redirect body args (owner side). */
struct arts_db_snapshot_redirect_args_s {
  arts_guid_t db_guid;
  unsigned int requester_rank;
  uint32_t cover;
  struct arts_rdzv_landing_s rdzv; /* requester's landing, forwarded by home */
  uint64_t token;                  /* requester's, forwarded by home */
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
#ifdef ARTS_RETENTION_PURGE
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
/* Cat-B pure body (OoO g_ooo_table[OOO_DB_DESTROY]): item_v is the home's
 * descriptor of the block, held by the engine or by the holder that
 * dispatched on it (arts_send_db_destroy); args_v is an
 * arts_ooo_args_db_destroy_s.  The wire dispatcher decodes DESTROY_REQ into
 * those args and routes through arts_ooo_dispatch_or_defer_guid.  A GPU kind
 * is retired here; a coherent block goes to arts_db_destroy_arrived. */
void arts_handler_db_destroy(void *item_v, void *args_v);
/* A coherent block's destroy at its home (each arm's own): its roster
 * fan-out and the retire of `db`, at once or at the edge the arm waits
 * for.  The caller holds `db`. */
void arts_db_destroy_arrived(struct arts_db_s *db);
/* OOO_DB_CREATE body (g_ooo_table): item_v is the GUID's route slot, which
 * the engine found empty and the body installs into; args_v is an
 * arts_ooo_args_db_create_s (an announce) or an arts_ooo_args_create_local_s
 * naming a descriptor built on this rank.  The wire dispatcher enters it
 * through arts_ooo_dispatch_or_defer_guid, a local create through the engine
 * on its fresh slot or by GUID. */
void arts_handler_db_create(void *item_v, void *args_v);
/* An announce received from a creator on another rank, entering the engine as
 * OOO_DB_CREATE (a creator on the home rank installs its descriptor there
 * itself and announces nothing). */
void arts_db_create_announce_enter(
    const struct arts_msg_db_create_coherent_packet_s *p);

/* ===== Sharer-side (response) handlers =============================== */

/* GRANT_RESPONSE at new owner C (an answer): payload = the wire packet
 * (header + the owner-side version map, nothing after it); size is its total
 * bytes.  One signature for both write policies — WT drains + runs
 * immediately, WB defers the RW drain to CONFIRM_ACK.  The wire entry takes
 * the descriptor from the token, loads the map (WB) and runs
 * arts_db_grant_response_on, or, on a retired descriptor, ends the landing
 * and drops the write request's count. */
void arts_handler_db_grant_response(void *payload, size_t size);
/* What the GRANT_RESPONSE body acts on.  No bytes ride with it: a payload
 * from another rank lands one-sided (rdzv_txid != 0), and a rank granting the
 * right to itself already holds its bytes in place. */
struct arts_db_grant_response_args_s {
  unsigned int sender;  /* the rank that shipped the grant */
  uint64_t version;     /* the sender's buffer version; 0 = it held none */
  uint64_t data_size;   /* the block's size, payload or not */
  uint64_t rdzv_txid;   /* != 0: payload PUT into our landing; pair by txid */
  uint64_t rdzv_cookie; /* our landing handle, echoed back; 0 = none */
  uint64_t home_token;  /* the transfer's, for the CONFIRM; 0 = none owed */
};
/* The GRANT_RESPONSE body on this rank's descriptor of the block, under the
 * count of the write request it answers (a one-sided payload's pairing
 * borrows it).  Under WT the commit closes the write axis and drops that
 * count; under WB the CONFIRM_ACK does, on the home rank inside the commit
 * (in this call, or in a one-sided payload's pairing callback), so no caller
 * runs on that count after the commit. */
void arts_db_grant_response_on(struct arts_db_s *db,
                               const struct arts_db_grant_response_args_s *a);
/* SNAPSHOT_RESPONSE (an answer; the wire and a self-serve): the read window's
 * answer on the descriptor a->token names.  A retired descriptor only ends
 * the landing and drops the read request's count (this 1:1 response resumes
 * readers a teardown already freed). */
void arts_handler_db_snapshot_response(
    struct arts_db_snapshot_response_args_s *a);
/* Consume an in-flight rendezvous whose receiver-side target is gone (the
 * metadata packet reached a destroyed object): registers a discard
 * continuation so the landing frees and the txid pairing table stays
 * leak-free.  `len` is the payload the metadata names.  No-op for txid == 0.
 */
void arts_db_rdzv_discard_landing(uint64_t txid, uint64_t len,
                                  uint64_t cookie);
/* Pure (cache, args) body: item_v is the db_s (cache is its first member);
 * args_v is an arts_ooo_args_db_grant_invalidate_s.  The wire dispatcher (a
 * lookup) and the self-send (the home's descriptor, or a lookup when it is
 * not live) call this body DIRECTLY — no OoO defer.  In
 * both write policies the revoking home publishes the invalidate target
 * (rw_holder) only at the post-install CONFIRM owner-swap, so the cache is
 * provably installed when INVALIDATE arrives.  WT/WB define the real body
 * (sentinel withdrawal / transfer trigger); a home whose grants come back
 * unasked sends no INVALIDATE at all. */
void arts_handler_db_grant_invalidate(void *item_v, void *args_v);
/* PUBLISH_ACK (an answer when cv == 0).  cv != 0 wakes a standalone grant
 * return's pointer-identity sem, naming no descriptor; cv == 0 runs flight
 * completion on the releaser descriptor the token names (credit record,
 * covered-waiter drain, trailing-flight decision), or drops the flight's
 * count on a retired one. */
void arts_handler_db_publish_ack(const struct arts_db_publish_ack_args_s *a);
/* Cat-C pure body (CACHE_DESTROY): item_v is the db_s the dispatcher acquired
 * (cache is its first member); args_v is an arts_db_cache_destroy_args_s.
 * NOT OoO-deferrable — the dispatcher looks the cache up with a held ref and,
 * on a MISS (already torn down on this rank), SILENTLY DROPS (idempotent). */
void arts_handler_db_cache_destroy(void *item_v, void *args_v);

/* ===== WB-only handlers ============================================== */

#if defined(ARTS_VALIDITY_VAL) && defined(ARTS_READ_SOURCE_OWNER)
/* Cat-C pure body (SNAPSHOT_REDIRECT, owner side): item_v is the db_s the
 * dispatcher acquired (cache is its first member); args_v is an
 * arts_db_snapshot_redirect_args_s.  NOT OoO-deferrable — a redirect continues
 * a request the home already took in, so the dispatcher (and the self-send)
 * answers a MISS (a block torn down since) with a dismiss that gives the
 * reader its landing back, never a park. */
void arts_handler_db_snapshot_redirect(void *item_v, void *args_v);
#endif /* VAL x WB */

#if (defined(ARTS_VALIDITY_VAL) || defined(ARTS_VALIDITY_INV)) &&              \
    !defined(ARTS_RETENTION_PURGE)
/* CONFIRM at the home (both write policies of the revoking release policy —
 * a voluntarily returned grant names no holder, so it has no CONFIRM): the
 * answer to the home's transfer, on the home descriptor the packet's
 * home_token names, under the baton tenure's count.  Records the new
 * rw_holder (read from db->pending_install_owner), then starts the next
 * transfer round or puts the baton down.  WB additionally replies with
 * CONFIRM_ACK, echoing the new owner's token; WT does not (the new owner
 * already drained at GRANT_RESPONSE).  On a retired home it only ends the
 * tenure (arts_db_grant_end_tenure). */
void arts_handler_db_grant_confirm(
    const struct arts_msg_grant_confirm_packet_s *p);

/* Send CONFIRM from new owner C back to home A once the ownership transfer is
 * complete (both write policies; home flips rw_holder + advances the round).
 * db is the new owner's descriptor, which the caller holds; home_token the
 * transfer's, from its GRANT_RESPONSE. */
void arts_send_db_grant_confirm(struct arts_db_s *db, uint64_t home_token);
#endif /* shared grant plane, revoking release policy */

/* ===== Sender helpers ================================================ */

/* Send GRANT_REQUEST to the DB's home (the home FIFO orders
 * rank-by-rank).  When the requester knows db_size a fresh transfer landing
 * is allocated and advertised in the request; a size-unknown first touch
 * sends landing-less (txid 0) and home answers GRANT_CTS, whose handler
 * re-issues through this sender with the landing attached. */
void arts_send_db_grant_request(struct arts_db_cache_s *cache, uint32_t cover);
/* GRANT_CTS at the requester (an answer): on the descriptor the packet's
 * token names, learns db_size and re-issues the in-flight GRANT_REQUEST with
 * a landing (the write axis stays open — this is the same round
 * continuing).  A retired descriptor only drops the write request's count. */
void arts_handler_db_grant_cts(struct arts_msg_grant_cts_packet_s *p);
/* GRANT_CTS sender: home → remote first-touch requester (db_size reply),
 * echoing the request's token. */
void arts_send_db_grant_cts(unsigned int requester_rank, arts_guid_t db_guid,
                            uint64_t db_size, uint64_t token);
/* cv: nonzero only on an announce — the address of the releaser's
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
/* db is the releaser's descriptor, which the caller holds; the home is its
 * GUID's rank.  The packet names db as the flight's token. */
void arts_send_db_publish(struct arts_db_s *db, uint64_t version, uint64_t cv,
                          uint64_t data_size, uint64_t rdzv_txid,
                          uint64_t rdzv_cookie, bool return_grant);
/* PUBLISH_CTS: home → releaser, advertising the home's stable buffer, in
 * place, as the landing for an announced dirty publish or a landing-less EXCL
 * PURGE release (cv echoed verbatim). */
void arts_send_db_publish_cts(unsigned int releaser_rank, arts_guid_t db_guid,
                                const struct arts_rdzv_landing_s *landing,
                                uint64_t cv);
/* db is the home's descriptor, which the caller holds (the releaser's own
 * when releaser_rank is this rank).  credit mints and attaches the next
 * publish credit for releaser_rank (stable-buffer {addr, rkey} + a fresh rdzv
 * txid); without it (or for a data-less / self-rank / single-rank ACK) the
 * credit is zero.  token is the flight's, echoed (0 with a nonzero cv). */
void arts_send_db_publish_ack(unsigned int releaser_rank, struct arts_db_s *db,
                              uint64_t cv, uint64_t version, bool credit,
                              uint64_t token);
/* CREATE_RETURN — home -> remote creator after the home-side install: the
 * creator's FIRST publish credit, so create -> write -> release needs no
 * announce round.  Fire-and-forget hint; minting nothing (self-rank,
 * single-rank, data-less DB) sends nothing and leaves the creator on the
 * announce leg. */
void arts_send_db_create_return(unsigned int creator_rank,
                                struct arts_db_cache_s *home_cache);
/* db: the home's descriptor, which the caller holds, named by the message's
 * home token; new_owner_rank: rank home selected as next owner;
 * new_owner_rdzv: that rank's transfer landing (from its queued request),
 * forwarded so the current holder can PUT the transfer payload directly
 * (NULL = zero landing — sentinel/data-less round); new_owner_token: that
 * request's token, which the grant echoes. */
void arts_send_db_grant_invalidate(
    struct arts_db_s *db, unsigned int owner_rank, unsigned int new_owner_rank,
    const struct arts_rdzv_landing_s *new_owner_rdzv, uint64_t new_owner_have,
    uint64_t new_owner_token);
/* Send SNAPSHOT_REQUEST to the DB's home, advertising a fresh
 * snapshot landing when db_size is known (multi-rank runs); a size-unknown
 * first touch sends landing-less and the server answers a size-only CTS
 * response (data_present == 2), whose handler re-enters this sender. */
void arts_send_db_snapshot_request(struct arts_db_cache_s *cache,
                                   uint32_t cover);
/* Send SNAPSHOT_RESPONSE.  kind (-> wire data_present): 0 = no data moved (a
 * non-NULL landing's cookie is echoed so the requester returns it), 1 =
 * payload (PUT from src_h's buffer into `landing`; src_h — a strong buffer
 * ref — is CONSUMED: transferred to the PUT's local completion, or released
 * after an inline self-serve), 2 = an answer without data: a size round
 * (db_size) or a dismiss, either returning the landing.  cover echoes the
 * answered request's cover word, whose parity the requester checks.  src_h
 * must be NULL for kinds 0/2.  token is the request's, echoed: to a
 * requester that is this rank the reply runs on the descriptor it names. */
void arts_send_db_snapshot_response(unsigned int requester_rank, uint64_t token,
                                    arts_guid_t db_guid, uint64_t version,
                                    uint32_t cover, uint32_t kind,
                                    uint64_t db_size,
                                    const struct arts_rdzv_landing_s *landing,
                                    arts_shared_ptr_t src_h);
/* owed: what the claimed first-touch cache had already asked the home for
 * (ARTS_CREATE_OWED_*), 0 for every other create. */
void arts_send_db_create_coherent(unsigned int home_rank, arts_guid_t db_guid,
                                  uint64_t db_size, uint16_t flags,
                                  uint16_t db_type, uint64_t cxl_addr,
                                  uint8_t owed);
/* The destroy of db_guid, to its home.  held is a handle the caller holds on
 * this rank's descriptor of the block, or NULL; a home on this rank runs the
 * destroy on it while it is live (arts_ooo_dispatch_on).  held is borrowed. */
void arts_send_db_destroy(arts_guid_t db_guid, arts_shared_ptr_t held);
/* A home's notice to a caching rank other than its own. */
void arts_send_db_cache_destroy(unsigned int sharer_rank, arts_guid_t db_guid);

#if defined(ARTS_VALIDITY_VAL) && defined(ARTS_READ_SOURCE_OWNER)
/* Send SNAPSHOT_REDIRECT from home to the current owner, asking the owner to
 * serve SNAPSHOT_RESPONSE or a no-data response directly to requester_rank.
 * db is the home's descriptor, which the caller holds; token is the
 * requester's, forwarded for the answer. */
void arts_send_db_snapshot_redirect(struct arts_db_s *db,
                                    unsigned int owner_rank,
                                    unsigned int requester_rank, uint32_t cover,
                                    const struct arts_rdzv_landing_s *rdzv,
                                    uint64_t token);
#endif

#if (defined(ARTS_VALIDITY_VAL) || defined(ARTS_VALIDITY_INV)) &&              \
    defined(ARTS_READ_SOURCE_OWNER)

/* Send CONFIRM_ACK from home to the new owner C after home has flipped
 * rw_holder to C; db is the home's descriptor, which the caller holds, and
 * new_owner_token C's request's token, from its CONFIRM.  Self-send runs the
 * handler inline.  piggyback_new_owner == ARTS_NO_PENDING_OWNER ⇒ plain
 * CONFIRM_ACK (no pending requester).  Otherwise the round advances and the
 * ack carries the next transfer target, its token and the home's (db's), so
 * the new owner's handler applies the INVALIDATE effect in the same message
 * (no separate INVALIDATE, no reorder window). */
void arts_send_db_grant_confirm_ack(
    struct arts_db_s *db, unsigned int new_owner_rank, uint64_t new_owner_token,
    unsigned int piggyback_new_owner,
    const struct arts_rdzv_landing_s *piggyback_rdzv, uint64_t piggyback_have,
    uint64_t piggyback_token);
/* CONFIRM_ACK at the new owner (an answer: it closes the write request the
 * packet's token names).  Opens the gate, drains the parked RW waiters that
 * the GRANT_RESPONSE handler deferred, applies the piggybacked invalidate
 * effect (publish incoming_new_owner + withdraw the sentinel), removes the
 * drain guard (the relocated 0-edge ship-check) and drops the write
 * request's count.  On a retired descriptor it only drops that count: the
 * gated waiters are acquires the destroy contract excludes, freed with the
 * cache and never resumed. */
void arts_handler_db_grant_confirm_ack(
    struct arts_msg_grant_confirm_ack_packet_s *p);
#endif /* (VAL or INV) x WB */

#if defined(ARTS_VALIDITY_VAL) || defined(ARTS_VALIDITY_INV)
/* Shared owner→owner transfer ship (defined in coherence/grant.c): ship the
 * current buffer (+ serialized owner-side map for WB, empty map for WT) to
 * new_owner via the GRANT_RESPONSE wire.  The target is an argument because
 * where it comes from is the release policy's: a revocation round publishes
 * it on the holder's cache ahead of the sentinel withdrawal, a home serving
 * its own queue names the requester it just popped. */
void arts_db_send_grant_response(struct arts_db_cache_s *cache,
                                 unsigned int new_owner,
                                 const struct arts_rdzv_landing_s *new_rdzv,
                                 bool data_less, uint64_t token,
                                 uint64_t home_token);
#ifndef ARTS_RETENTION_PURGE
/* Ship to the target published on this cache by a revocation round, re-arming
 * the field to the sentinel before the send.  Called on the 0-edge of
 * release_rw / the INVALIDATE handler (both write policies) and the WB
 * CONFIRM_ACK / WT GRANT_RESPONSE drain-guard removal. */
void arts_db_grant_ship_pending(struct arts_db_cache_s *cache);
#endif

#if defined(ARTS_VALIDITY_VAL) || defined(ARTS_VALIDITY_INV)
/* The version a serve from the home may claim for the bytes it sends — the
 * arm's own canonical publication axis.  Under a write-through home that is
 * NOT the served buffer's own version: a home-resident writer mutates that
 * buffer in place, so its lane stands still while the data moves on, and a
 * serve stamped from it would hand two different write windows the same
 * number.  Read under the serve claim, which is what holds it still. */
uint64_t arts_db_grant_serve_version(struct arts_db_s *db);
#endif
#endif /* shared grant plane */

#ifdef ARTS_VALIDITY_INV
/* ===== INV protocol handlers / senders ================================= */

struct arts_msg_inv_cts_packet_s;
struct arts_msg_inv_invalidate_ack_packet_s;

/* Cat-B bodies @home.  Both write policies run the same set; what differs is
 * what a read request is answered WITH (the home's own bytes, or a redirect to
 * the current grant holder) and whether a publish carries payload. */
void arts_handler_db_inv_request(void *item_v, void *args_v);
void arts_handler_db_publish(void *item_v, void *args_v);
/* INV_CTS (an answer) on the descriptor the packet's token names; a retired
 * one only gets its landing back and drops the read request's count. */
void arts_handler_db_inv_cts(struct arts_msg_inv_cts_packet_s *p);
void arts_handler_db_inv_deliver(void *payload, size_t size);
/* INVALIDATE at a sharer (resolved by GUID, or the home's descriptor on a
 * self-send); home_token is the round's, which the ack echoes or, when the
 * ack is owed to a kill-marked fetch, the cache keeps until it is sent. */
void arts_handler_db_inv_invalidate(struct arts_db_s *db, uint64_t home_token);
/* INVALIDATE_ACK at the home (an answer to its round) on the descriptor the
 * home token names, under the round's count: the decrement that ends the
 * round finishes it, unless the home was retired meanwhile, and drops the
 * count. */
void arts_handler_db_inv_invalidate_ack(
    const struct arts_msg_inv_invalidate_ack_packet_s *p);
#ifdef ARTS_READ_SOURCE_OWNER
/* The home holds no bytes, so a read request is forwarded to the holder; a
 * holder that no longer has the bytes bounces it back rather than queueing. */
void arts_handler_db_inv_redirect(void *item_v, void *args_v);
/* Never a self-send: the home serves a reader itself when it holds the grant,
 * and a holder that bounces is never the home. */
void arts_send_db_inv_redirect(unsigned int holder_rank, arts_guid_t db_guid,
                               unsigned int requester, uint32_t cover,
                               const struct arts_rdzv_landing_s *rdzv,
                               uint64_t token);
void arts_send_db_inv_request_for(arts_guid_t db_guid, unsigned int requester,
                                  uint32_t cover,
                                  const struct arts_rdzv_landing_s *rdzv,
                                  uint64_t token);
#endif

/* Senders. */
void arts_send_db_inv_request(struct arts_db_cache_s *cache,
                              arts_db_access_mode_t mode, uint32_t cover);
/* A size round or a dismiss to a read request: see the packet.  cover is
 * the answered request's cover word; cookie the landing it named (0: none);
 * token the request's, echoed: to a requester that is this rank the reply
 * runs on the descriptor it names. */
void arts_send_db_inv_cts(unsigned int requester_rank, uint64_t token,
                          arts_guid_t db_guid, uint64_t db_size,
                          uint32_t cover, uint64_t cookie);
/* Serve a reader from this rank's installed buffer.  The stamp the reply
 * carries is the install-lane arbitration version ONLY: it decides which of
 * two asynchronous installs wins the receiver's buffer slot, and no branch on
 * either side compares it to judge whether a copy is still valid. */

/* db is the serving descriptor, which the caller holds; token the request's,
 * echoed. */
void arts_send_db_inv_deliver(unsigned int requester_rank,
                              struct arts_db_s *db,
                              const struct arts_rdzv_landing_s *rdzv,
                              uint64_t token);
/* db: the home's descriptor, which the caller holds. */
void arts_send_db_inv_invalidate(struct arts_db_s *db,
                                 unsigned int sharer_rank);
/* home_token: the round's, from its INVALIDATE; a home on this rank
 * accounts the ack inline on the descriptor it names. */
void arts_send_db_inv_invalidate_ack(unsigned int home_rank,
                                     arts_guid_t db_guid, uint64_t home_token);
#endif /* ARTS_VALIDITY_INV */

#ifdef ARTS_VALIDITY_EXCL
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

/* An answer @requester (the wire and the home's self-grant alike): payload =
 * the wire packet alone (a payload travels one-sided); size is its byte
 * count.  Acts on the descriptor the token names; on a retired one commits
 * nothing and drops the request's count once any bytes in flight have
 * landed.  Otherwise cache_state CAS (REQ→GRANT) + drain of the cache pending
 * stacks, after any one-sided payload has landed, and the request's count
 * last. */
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
/* EXCL_CTS at the requester, an answer (the wire and a self-send alike): acts
 * on the descriptor the token names, and on a retired one only drops the
 * request's count.  A size round learns db_size and re-issues the request
 * (echoed mode) with a landing, the axis open; a read request's dismiss
 * re-sends for readers that ride no write request, or closes the axis and
 * drops its count. */
void arts_handler_db_excl_cts(const struct arts_msg_excl_cts_packet_s *p);
/* EXCL_CTS sender: a size round or a read request's dismiss (db_size, the
 * echoed mode, cover word and token), from the home or, under RETAIN, from
 * an owner a read FORWARD found without the block; to a requester that is
 * this rank the answer runs inline. */
void arts_send_db_excl_cts(arts_guid_t db_guid, unsigned int requester_rank,
                           uint64_t db_size, uint32_t mode, uint32_t cover,
                           uint64_t token);
/* arts_send_db_excl_grant: db is the home's descriptor, which the caller
 * holds.  version is always 0, and no grant handler reads it.  The grant
 * payload PUTs into
 * req_rdzv (the requester's advertised landing; src_h — a strong ref on the
 * home buffer — is CONSUMED); `pub`, for RW grants, is home's publish landing
 * for this grant's release (NULL/zero for RO); token is the request's, which
 * the grant echoes. */
void arts_send_db_excl_grant(struct arts_db_s *db, unsigned int requester_rank,
                             arts_db_access_mode_t mode, uint64_t version,
                             const struct arts_rdzv_landing_s *req_rdzv,
                             const struct arts_rdzv_landing_s *pub,
                             arts_shared_ptr_t src_h, uint64_t data_size,
                             uint64_t token);
/* arts_send_db_excl_release: cv is nonzero only on an RW release's
 * landing-less announce — the address of the releaser's rendezvous,
 * which the home echoes in the PUBLISH_CTS to unblock the releaser; nothing
 * acknowledges a release.  version is always 0, and no handler reads it.  A
 * remote RW release PUTs the dirty bytes into its landing (the grant's `pub`,
 * or the one the PUBLISH_CTS named) first and echoes {rdzv_txid,
 * rdzv_cookie} here.  Never sent to this rank's own home, whose releases
 * commit in place. */
void arts_send_db_excl_release(unsigned int home_rank, arts_guid_t db_guid,
                               arts_db_access_mode_t mode, uint64_t version,
                               uint64_t cv, uint64_t data_size,
                               uint64_t rdzv_txid, uint64_t rdzv_cookie);

#ifdef ARTS_RETENTION_RETAIN
/* ===== EXCL RETAIN handlers (Cat-C direct dispatch) = */

/* arts_handler_db_excl_forward: Cat-C @owner.  item_v is the full wire packet
 * (arts_msg_excl_forward_packet_s *); the wire entry resolves the db_s by
 * ->db_guid once.  Handles home→owner forward: either migrate RW to a new
 * owner or serve one RO reader directly from the owner's buffer.  A read
 * FORWARD that finds no block is answered with a dismiss; a write one is
 * dropped. */
void arts_handler_db_excl_forward(void *item_v);

/* arts_handler_db_excl_deliver: an answer @target.  payload is the full wire
 * buffer (header + inline data); size is total bytes (data starts at
 * sizeof(arts_msg_excl_deliver_packet_s)).  On the descriptor the token
 * names: installs data + transitions cache state (GRANT or RO-hold), drains
 * any pending local acquires and drops the request's count; on a retired one
 * it commits nothing and drops the count once any bytes in flight have
 * landed. */
void arts_handler_db_excl_deliver(void *payload, size_t size);

/* arts_handler_db_excl_confirm: an answer @home.  item_v is the full wire
 * packet (arts_msg_excl_confirm_packet_s *).  New owner → home: migration
 * complete; on the descriptor the home token names, the home transitions
 * lock_state and serves the next waiter if any (a retired home takes the
 * w-- and dismisses the covered reads held behind the ended write phase,
 * serving nothing).  packet->rank = new owner. */
void arts_handler_db_excl_confirm(void *item_v);

/* arts_handler_db_excl_roret: Cat-C @home.  item_v is the full wire packet
 * (arts_msg_excl_notice_packet_s *); the wire entry resolves the db_s by
 * ->db_guid once.  Reader → home: RO release (data-less); home decrements the
 * reader count and serves the next waiter when the count reaches zero.
 * packet->rank = the returning reader. */
void arts_handler_db_excl_roret(void *item_v);

/* ===== EXCL RETAIN senders ========================= */

/* recall, forward and roret take the descriptor of the block the caller
 * holds (the home's, or the sender's own cache), and a self-send runs the
 * body on it while it is live, and on what a lookup finds otherwise; confirm
 * takes the new owner's descriptor and the home token its migration carried,
 * and a self-send runs the answer on the descriptor the token names; deliver
 * never self-sends and takes the target's rank and the GUID. */
/* arts_send_db_excl_forward: home → current owner.  mode=DB_MODE_RW requests
 * migration to target rank; mode=DB_MODE_RO requests serving one RO reader. */
/* arts_send_db_excl_recall: home → a rank holding an unmarked RO grant.
 * Data-less, fire-and-forget, no ack: the completion signal is the ordinary
 * RO_RETURN it provokes.  The fan-out includes the sending rank itself. */
void arts_send_db_excl_recall(struct arts_db_s *db, unsigned int target_rank);
void arts_handler_db_excl_recall(void *item_v);

/* cover: a serve's read request cover word (0 for a migrate); token: the
 * target's request's; home_token: a migrate's home descriptor (0 for a
 * serve). */
void arts_send_db_excl_forward(struct arts_db_s *db, unsigned int owner_rank,
                               uint32_t mode, uint32_t target, uint32_t tag,
                               uint32_t cover,
                               const struct arts_rdzv_landing_s *target_rdzv,
                               uint64_t token, uint64_t home_token);

/* arts_send_db_excl_deliver: owner → target.  Versionless (EXCL serialization
 * guarantees ordering).  The payload PUTs into the target's forwarded landing
 * (rdzv); src_h — a strong ref on the owner buffer — is CONSUMED.  token is
 * the target's request's, home_token a migration's home (0 for a serve). */
void arts_send_db_excl_deliver(unsigned int target_rank, arts_guid_t db_guid,
                               uint32_t mode, uint32_t tag,
                               const struct arts_rdzv_landing_s *rdzv,
                               arts_shared_ptr_t src_h, uint64_t data_size,
                               uint64_t token, uint64_t home_token);

/* arts_send_db_excl_confirm: new owner → home: migration done, naming the
 * home by the token the migration carried. */
void arts_send_db_excl_confirm(struct arts_db_s *db, uint64_t home_token);

/* arts_send_db_excl_roret: reader → home: RO release (data-less). */
void arts_send_db_excl_roret(struct arts_db_s *db);

#endif /* ARTS_RETENTION_RETAIN */

#endif /* ARTS_VALIDITY_EXCL */

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
