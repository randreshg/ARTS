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
** Reference herein to any specific commercial product, process, or service  **
** by trade name, trademark, manufacturer, or otherwise does not necessarily **
** constitute or imply its endorsement, recommendation, or favoring by the   **
** United States Government or any agency thereof, or Battelle Memorial      **
** Institute. The views and opinions of authors expressed herein do not      **
** necessarily state or reflect those of the United States Government or     **
** any agency thereof.                                                       **
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
** You may obtain a copy of the License at                                   **
**                                                                           **
**    https://www.apache.org/licenses/LICENSE-2.0                            **
**                                                                           **
** Unless required by applicable law or agreed to in writing, software       **
** distributed under the License is distributed on an "AS IS" BASIS, WITHOUT **
** WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the  **
** License for the specific language governing permissions and limitations   **
******************************************************************************/
#ifndef ARTS_OOO_H
#define ARTS_OOO_H
#ifdef __cplusplus
extern "C" {
#endif

#include "arts/defs.h"
#include "arts/gas/route_table.h" /* struct arts_route_item_s (ooo_list slot) */
#include "arts/runtime_types.h"   /* struct arts_edt_s / arts_db_s (OoO args) */
#include "arts/utils/lockfree_lifo.h" /* arts_lf_link_t (payload link) */

/* ===========================================================================
 * Out-of-order (OoO) deferred-op engine.
 *
 * The OoO engine owns no data structure of its own: it operates on the
 * arts_route_item_s.ooo_list Treiber stack declared in route_table.h.  A slot
 * accumulates deferred operations that arrived before their target object was
 * installed; the create handler drains them once the object is published.  The
 * engine depends one-way on the route-table slot API (it reaches a slot's
 * ooo_list via arts_route_table_reserve_or_lookup), so it lives in its own
 * module while the GUID directory it replays against stays in route_table.
 * ===========================================================================*/

/* OoO replay kind — dispatch tag for the handlers the engine gates on a slot's
 * occupancy.  Two polarities share the table:
 *   - a create kind (OOO_*_CREATE) runs while the slot is EMPTY: its body
 *     installs the object and drains the slot.  While the slot is occupied the
 *     create is parked, and the occupant's destroy dispatches it — the next
 *     generation of the GUID;
 *   - every other kind runs while the slot is OCCUPIED, against the occupant,
 *     and is parked until an install while it is empty.
 * Silent-drop (Cat C) and state-less (Cat E) handlers reach their handler only
 * through the wire dispatcher's MSG_* mapping and have NO kind here.
 *
 * Naming invariant: OOO_<NAME> == the handler arts_handler_<name> with the
 * arts_handler_ prefix stripped and upper-cased (kind ↔ handler 1:1), e.g.
 * arts_handler_event_add_dependence → OOO_EVENT_ADD_DEPENDENCE.
 *
 * The model-specific DB-coherence kinds are preprocessor-selected: VAL builds
 * define ARTS_PROTOCOL_VAL plus exactly one of ARTS_WRITE_POLICY_{WT,WB}.
 * FLUSH builds define ARTS_PROTOCOL_FLUSH alone — the write and release policy
 * axes are dead there — and carry OOO_DB_ACQUIRE, OOO_DB_FETCH_REQUEST and
 * OOO_DB_FLUSH_ANNOUNCE.
 * Each build's enum (and the mirroring g_ooo_table) carries only that model's OOO_DB_* kinds.
 * OOO_KIND_COUNT is therefore per-model — sound because every TU in one build
 * sees the same model define. */
enum arts_ooo_kind {
  /* ===== Model-agnostic creates — the inverted polarity ===== */
  OOO_EDT_CREATE,   /* → arts_handler_edt_create */
  OOO_EVENT_CREATE, /* → arts_handler_event_create */
  OOO_DB_CREATE,    /* → arts_handler_db_create */
  /* ===== Model-agnostic — always active (all Cat B) ===== */
  OOO_EVENT_SATISFY_SLOT, /* → arts_handler_event_satisfy_slot */
  OOO_EDT_SATISFY_SLOT,     /* → arts_handler_edt_satisfy_slot */
  OOO_EVENT_ADD_DEPENDENCE, /* → arts_handler_event_add_dependence */
  /* Lifecycle destroy replay kinds (before-create wire reorder): a DESTROY that
   * reaches home ahead of the object's CREATE defers here and replays once the
   * create handler installs+drains. */
  OOO_EDT_DESTROY,   /* → arts_handler_edt_destroy */
  OOO_EVENT_DESTROY, /* → arts_handler_event_destroy */
  OOO_DB_DESTROY, /* → arts_handler_db_destroy — no dep/stub_install gate, so a
                     create/destroy reorder can land DESTROY before home CREATE
                   */

/* ===== Model-specific DB coherence — exactly one model active =====
 * Re-issue the wire handler once the home db_s/cache is installed: a
 * remote-created DB's stub_install cache can fire a request/publish before
 * that DB's home CREATE arrives, so the message reaches home with db_s not yet
 * installed ⇒ OoO push, replayed on the CREATE handler's drain. */
#if defined(ARTS_PROTOCOL_EXCL) && defined(ARTS_RELEASE_PURGE)
  OOO_DB_ACQUIRE, /* → arts_db_acquire_replay_dep (re-attempts the one deferred
                     local dep; pushed by arts_db_acquire_all's per-dep 3-way)
                   */
  OOO_DB_EXCL_REQUEST, /* → arts_handler_db_excl_request @ home */
  OOO_DB_EXCL_RELEASE, /* → arts_handler_db_excl_release @ home */
#elif defined(ARTS_PROTOCOL_EXCL) && defined(ARTS_RELEASE_RETAIN)
  OOO_DB_ACQUIRE, /* → arts_db_acquire_replay_dep (re-attempts the one deferred
                     local dep; pushed by arts_db_acquire_all's per-dep 3-way)
                   */
  OOO_DB_EXCL_REQUEST, /* → arts_handler_db_excl_request @ home */
/* NO OOO_DB_EXCL_RELEASE: the RETAIN release policy has no synchronous publish release; the
 * DELIVER/CONFIRM/RORET messages are all direct-dispatched (Cat-C, target
 * provably installed by the time these messages arrive). */
#elif defined(ARTS_PROTOCOL_INV)
  OOO_DB_ACQUIRE, /* → arts_db_acquire_replay_dep (re-attempts the one deferred
                     local dep; pushed by arts_db_acquire_all's per-dep 3-way)
                   */
  /* The migrating grant, shared with every other grant-bearing arm. */
  OOO_DB_GRANT_REQUEST, /* → arts_handler_db_grant_request @ home */
  /* INV's own: the reader fetch, and (WT write policy only) the release that
   * carries payload to the home. */
  OOO_DB_INV_REQUEST, /* → arts_handler_db_inv_request @ home (RO fetch) */
  /* Every release asks the home for its invalidation round.  Under WT the
   * request carries the payload (the home installs it and serves readers from
   * it); under WB it is pure control.  That is the ONLY difference between
   * the two write policies' releases. */
  OOO_DB_PUBLISH, /* → arts_handler_db_publish @ home */
#ifdef ARTS_RELEASE_PURGE
  /* The voluntary return.  It can overtake the block's home CREATE, and the
   * home fields it touches do not exist on a cache-only stub, so it defers
   * and replays on the install's drain like any other home-side body. */
  OOO_DB_GRANT_RETURN, /* → arts_handler_db_grant_return @ home */
#endif
#ifdef ARTS_WRITE_POLICY_WB
  OOO_DB_INV_REDIRECT, /* → arts_handler_db_inv_redirect @ the grant holder */
#endif
/* GRANT_INVALIDATE / RESPONSE / CONFIRM, and DELIVER / INVALIDATE /
 * INV_ACK / CTS, are Cat-C: their targets are either provably installed (the
 * home names a holder only after that rank installed; a requester pinned its
 * cache when it sent the request; home for the acks) or the MISS action is
 * part of the protocol (INVALIDATE MISS → ACK; DELIVER MISS → landing
 * discard). */
#elif defined(ARTS_WRITE_POLICY_WT)
  OOO_DB_ACQUIRE, /* → arts_db_acquire_replay_dep (re-attempts the one deferred
                     local dep; pushed by arts_db_acquire_all's per-dep 3-way)
                   */
  OOO_DB_SNAPSHOT_REQUEST,  /* → arts_handler_db_snapshot_request @ home */
  OOO_DB_GRANT_REQUEST, /* → arts_handler_db_grant_request @ home */
  /* NO OOO_DB_GRANT_INVALIDATE — the WT write policy no longer defers
   * INVALIDATE: rw_holder is flipped only at the post-install CONFIRM (same as
   * WB), so the target is provably installed when INVALIDATE arrives and the
   * dispatcher/self-send call the body directly. */
  OOO_DB_PUBLISH, /* → arts_handler_db_publish @ home */
#ifdef ARTS_RELEASE_PURGE
  /* The voluntary return.  It can overtake the block's home CREATE, and the
   * home fields it touches do not exist on a cache-only stub, so it defers
   * and replays on the install's drain like any other home-side body. */
  OOO_DB_GRANT_RETURN, /* → arts_handler_db_grant_return @ home */
#endif
#elif defined(ARTS_WRITE_POLICY_WB)
  OOO_DB_ACQUIRE, /* → arts_db_acquire_replay_dep (re-attempts the one deferred
                     local dep; pushed by arts_db_acquire_all's per-dep 3-way)
                   */
  OOO_DB_SNAPSHOT_REQUEST,  /* → arts_handler_db_snapshot_request @ home */
  OOO_DB_GRANT_REQUEST, /* → arts_handler_db_grant_request @ home */
/* NO OOO_DB_GRANT_INVALIDATE — the WB write policy never defers INVALIDATE
 * (home publishes the target rw_holder only after that rank's cache install,
 * so the target is provably installed; the dispatcher/self-send call the body
 * directly).
 * NO OOO_DB_PUBLISH — the WB write policy has no synchronous publish (the
 * dispatcher fatals on the PUBLISH wire message). */
#elif defined(ARTS_PROTOCOL_FLUSH)
  OOO_DB_ACQUIRE,       /* → arts_db_acquire_replay_dep */
  OOO_DB_FETCH_REQUEST, /* → arts_handler_db_fetch_request @ home; a first
                           touch can overtake the home's CREATE, so it defers */
  OOO_DB_FLUSH_ANNOUNCE, /* → arts_handler_db_flush_announce @ home; a remote
                            creator's release can announce before the home's
                            CREATE lands, so it defers and replays on the
                            install's drain */
#else
#error                                                                         \
    "exactly one of ARTS_PROTOCOL_EXCL+ARTS_RELEASE_{PURGE,RETAIN}, ARTS_PROTOCOL_INV+ARTS_WRITE_POLICY_{WT,WB}, ARTS_WRITE_POLICY_{WT,WB} (VAL), or ARTS_PROTOCOL_FLUSH must be defined"
#endif

  OOO_KIND_COUNT /* sentinel — g_ooo_table size (per-model) */
};
typedef enum arts_ooo_kind ooo_kind_t;

/* Unified OoO payload.  The link is the FIRST member so a node address equals
 * its link address (Treiber stack contract).  A variable-size args blob trails
 * the header (heap-allocated as sizeof(payload) + args_size); each kind casts
 * the blob back to its own args struct. */
struct arts_ooo_payload_s {
  arts_lf_link_t link; /* MUST be first */
  ooo_kind_t kind;
  uint32_t args_size;
  /* The GUID this payload was deferred for.  A slot is returned to the table
   * on destroy, so the slot a thread resolved before that destroy may belong
   * to a different GUID by the time it pushes — the slot alone is no longer
   * proof of identity, and a dispatch against the wrong object would be
   * silent corruption rather than a miss.  Checked in dispatch: on a mismatch
   * the payload, of any kind, follows its GUID to whichever slot names it now,
   * because a GUID comes back when it is created again. */
  arts_guid_t guid;
  /* args blob follows here */
};

static inline void *arts_ooo_payload_args(struct arts_ooo_payload_s *p) {
  return (void *)(p + 1);
}

/* ===== per-kind args =====================================================
 * The deferring entry copies one of these into the payload blob; the
 * g_ooo_table[kind] handler casts the blob back and replays the operation
 * against the now-installed target (re-issuing the entry, so the install
 * race / fire-and-linger logic stays in one place). */

/* A create kind's args take one of two shapes, told apart by the leading
 * `size` word, which both shapes place first:
 *   - size != ARTS_OOO_CREATE_ADOPT: struct arts_ooo_args_create_blob_s — the
 *     object blob packet (struct arts_msg_object_blob_packet_s, header
 *     included, the serialized object trailing it) follows, `size` bytes of
 *     it.  This is the shape of a create received off the wire; the body
 *     copies the object out, so the bytes need no alignment.
 *   - size == ARTS_OOO_CREATE_ADOPT: struct arts_ooo_args_create_local_s — a
 *     create issued on the rank whose slot the object will occupy, which
 *     already holds the built object: the body installs that object itself,
 *     without a copy.  The payload owns the object until the install; a lost
 *     install re-parks the same object, and a payload freed without dispatch
 *     (teardown) frees it with the GUID kind's deleter.  A data block's
 *     descriptor is named by its control block (an arts_shared_ptr_t made
 *     before the install) instead, because its creator keeps a reference to
 *     it while the create is parked; the payload owns the reference the slot
 *     takes over at the install, and a payload freed without dispatch drops
 *     it.
 * A data block's announce to its home is the third shape, struct
 * arts_ooo_args_db_create_s, which leads with the same word.
 * One kind per object keeps the kind ↔ body naming 1:1; every create body
 * handles each shape its object has. */
#define ARTS_OOO_CREATE_ADOPT 0u

struct arts_ooo_args_create_blob_s {
  uint32_t size; /* != ARTS_OOO_CREATE_ADOPT */
  /* the object blob packet follows */
};

/* Create of a data block announced to its home: the shape tag, then the
 * fields of struct arts_msg_db_create_coherent_packet_s after its header. */
struct arts_ooo_args_db_create_s {
  uint32_t size;        /* sizeof(struct arts_ooo_args_db_create_s) */
  unsigned int creator; /* the announcing rank (the wire header's sender) */
  arts_guid_t db_guid;
  uint64_t db_size;
  uint64_t create_token; /* the creator descriptor's, echoed in the return */
#ifdef ARTS_FAM
  uint64_t fam_addr; /* the block's slot, 0 when the creator allocated none */
#endif
  uint16_t flags;
  uint16_t db_type;
};

struct arts_ooo_args_create_local_s {
  uint32_t size; /* == ARTS_OOO_CREATE_ADOPT */
  arts_guid_t guid;
  void *descriptor; /* the built object, installed as is */
};

/* EDT satisfy args (OOO_EDT_SATISFY_SLOT) — a GUID/value reference only,
 * fixed size. */
struct arts_ooo_args_edt_satisfy_s {
  arts_guid_t
      edt_guid; /* re-signal target (may differ from the deferred-on
                   slot, e.g. GPU LC invalidation defers on the wrapper) */
  arts_guid_t data_guid;
  uint32_t slot;
  arts_db_access_mode_t mode;
};

struct arts_ooo_args_event_satisfy_s {
  arts_guid_t event_guid;
  arts_guid_t data_guid;
  uint32_t slot;
};

struct arts_ooo_args_event_add_dep_s {
  arts_guid_t source;
  arts_guid_t destination;
  uint32_t slot;
  arts_db_access_mode_t mode;
};

struct arts_ooo_args_db_acquire_s {
  struct arts_edt_s *edt;
  arts_guid_t db_guid;
  uint32_t slot;
};

/* Coherence replay args — re-issue the wire handler once the home db_s/cache
 * is installed.  First-class fields are reconstructed into a stack packet by
 * the handler.  The home FIFO records only the requester rank (VAL
 * orders ownership rank-by-rank). */
struct arts_ooo_args_db_grant_request_s {
  unsigned int requester;
  arts_guid_t db_guid;
  struct arts_rdzv_landing_s rdzv; /* requester's transfer landing */
  uint64_t have_version;           /* version the requester already holds */
};

/* The voluntary return carries nothing but who is giving the right back. */
struct arts_ooo_args_db_grant_return_s {
  unsigned int returner;
  arts_guid_t db_guid;
  uint64_t cv;
};

struct arts_ooo_args_db_snapshot_request_s {
  unsigned int requester;
  arts_guid_t db_guid;
  arts_guid_t edt_guid;
  uint32_t slot;
  struct arts_rdzv_landing_s rdzv; /* requester's snapshot landing */
};

struct arts_ooo_args_db_fetch_request_s {
  unsigned int requester;
  arts_guid_t db_guid;
  arts_guid_t edt_guid;
  uint32_t slot;
  struct arts_rdzv_landing_s rdzv; /* the requester's private copy, advertised */
};

/* The credit-less release's announce: the releaser's identity and the heap
 * rendezvous the home's CTS must post. */
struct arts_ooo_args_db_flush_announce_s {
  unsigned int releaser;
  arts_guid_t db_guid;
  uint64_t sem;
};

struct arts_ooo_args_db_destroy_s {
  unsigned int requester;
  arts_guid_t db_guid;
};

/* DB publish.  Three shapes share this args struct (see the PUBLISH wire
 * doc in protocol.h):
 *   data_inline != 0                : same-rank publish — the payload
 *       (data_size bytes) trails this header in the args blob;
 *   data_inline == 0, rdzv_txid == 0: announce leg — home allocates a fresh
 *       landing and replies PUBLISH_CTS (nothing installs yet);
 *   data_inline == 0, rdzv_txid != 0: commit leg — the payload was PUT into
 *       home's landing (rdzv_cookie); install on {args, txid} pairing.
 *   data_size == 0                  : data-less round — install nothing, ACK. */
struct arts_ooo_args_db_publish_s {
  unsigned int releaser;
  arts_guid_t db_guid;
  uint64_t version;
  uint64_t cv; /* releaser's stack rendezvous address, echoed in CTS/ACK */
  uint64_t data_size;
  uint64_t rdzv_txid;
  uint64_t rdzv_cookie;
  uint32_t data_inline;
  /* The releaser gave the write right back with this publish; the home
   * owes it the same acceptance a standalone hand-back would ask for. */
  uint32_t returns_grant;
};

/* DB ownership invalidate: carries the fields the wire
 * arts_msg_grant_invalidate_packet_s delivers (db_guid + new_owner_rank).
 * The WT write policy ignores new_owner_rank; the WB write policy uses it as the
 * GRANT_RESPONSE target. */
struct arts_ooo_args_db_grant_invalidate_s {
  arts_guid_t db_guid;
  unsigned int new_owner_rank;
  struct arts_rdzv_landing_s new_owner_rdzv; /* transfer landing at new owner */
};

/* INV protocol OoO args (OOO_DB_INV_REQUEST / OOO_DB_PUBLISH). */
struct arts_ooo_args_db_inv_request_s {
  unsigned int requester; /* subject of the request — NOT the wire sender: a
                             holder may re-send a read request on the reader's
                             behalf */
  arts_guid_t db_guid;
  arts_db_access_mode_t mode;      /* DB_MODE_RO or DB_MODE_RW */
  struct arts_rdzv_landing_s rdzv; /* deliver/grant landing; txid==0 = first
                                      touch (home replies INV_CTS) */
};

#if defined(ARTS_PROTOCOL_INV) && defined(ARTS_WRITE_POLICY_WB)
/* The home forwarding a read it cannot answer to the current grant holder.
 * The requester is the subject, never the sender: the holder replies to it
 * directly, or bounces the request back to the home if the bytes have already
 * moved on. */
struct arts_ooo_args_db_inv_redirect_s {
  unsigned int requester;
  arts_guid_t db_guid;
  struct arts_rdzv_landing_s rdzv;
};
#endif /* ARTS_PROTOCOL_INV && ARTS_WRITE_POLICY_WB */

/* Event / EDT destroy replay (before-create reorder): the guid is enough to
 * re-issue the destroy once the object installs. */
struct arts_ooo_args_event_destroy_s {
  arts_guid_t guid;
};
struct arts_ooo_args_edt_destroy_s {
  arts_guid_t guid;
};

/* EXCL protocol OoO args (OOO_DB_EXCL_REQUEST / OOO_DB_EXCL_RELEASE). */
struct arts_ooo_args_db_excl_request_s {
  unsigned int requester; /* rank that sent MSG_DB_EXCL_REQUEST */
  arts_guid_t db_guid;
  arts_db_access_mode_t mode; /* DB_MODE_RO or DB_MODE_RW */
  struct arts_rdzv_landing_s rdzv; /* requester's grant/deliver landing */
};
/* EXCL_RELEASE: inline publish payload of data_size bytes trails this
 * header (data_size == 0 for RO releases).
 * cv: RW only — releaser's stack-local sem_t address; forwarded verbatim in
 * the PUBLISH_CTS so the releaser wakes by pointer identity.  0 for RO.
 * version: monotone round counter bumped by the releaser; home's buf_install
 * rejects stale overwrites when a reordered/duplicate RELEASE races a newer
 * one (same guard as the VAL publish path). */
struct arts_ooo_args_db_excl_release_s {
  unsigned int releaser; /* rank that sent MSG_DB_EXCL_RELEASE */
  arts_guid_t db_guid;
  arts_db_access_mode_t mode; /* DB_MODE_RO or DB_MODE_RW */
  uint64_t data_size;         /* 0 for RO; >0 for RW publish */
  uint64_t cv;                /* RW: releaser sem_t address; 0 for RO */
  uint64_t version; /* RW: monotone version for buf_install; 0 for RO */
  /* RW dirty payload delivery: data_inline != 0 = payload trails this header
   * (same-rank release); else the payload was PUT into the grant's pub landing
   * — {rdzv_txid, rdzv_cookie} echo it and install pairs by txid. */
  uint64_t rdzv_txid;
  uint64_t rdzv_cookie;
  uint32_t data_inline;
};

/* g_ooo_table handler, both polarities.  A non-create handler operates on an
 * already-acquired item with the decoded args and performs NO route-table
 * lookup / NULL-check / acquire / push — dispatch_or_defer guarantees `item`
 * is live and ref-pinned for the duration of the call.  A create body gets
 * `item == NULL`: the engine saw the GUID's slot empty, and the body installs
 * its object there with install_if_absent, re-entering the engine (which parks
 * it) when another install won the slot first. */
typedef void (*arts_ooo_handler_fn_t)(void *item, void *args);

/* Universal entry — wire RX dispatcher, API drivers, and the drain walk all
 * enter here, for every kind.
 *
 *   payload == NULL : fresh entry (wire RX / API).  On park a payload is
 *                     allocated (args copied) and pushed.
 *   payload != NULL : drain re-entry.  On park the SAME payload is re-pushed
 *                     (no alloc/free); on dispatch it is consumed.
 *
 * A non-create kind runs against the occupant and parks while the slot is
 * empty; a create kind runs its body (item == NULL) while the slot is empty and
 * parks while it is occupied.  The slot value is (re)loaded on every call, so a
 * destroy or an install earlier in the same drain walk is observed.
 */
void arts_ooo_dispatch_or_defer(struct arts_route_item_s *slot,
                                struct arts_ooo_payload_s *payload,
                                ooo_kind_t kind, arts_guid_t guid,
                                const void *args, uint32_t args_size);

/* The handle the engine pinned for the non-create handler running on this
 * thread, if that handler's item is `item`; NULL otherwise. */
arts_shared_ptr_t arts_ooo_dispatched_handle(const void *item);

/* Retire `item` under `key`: always that object, never whatever the slot
 * holds.  From a body the engine dispatched on `item` it uses the handle the
 * dispatch pinned (arts_route_table_set_destroyed_item); anywhere else — a
 * teardown owed by a release that runs on the object directly — it pins the
 * key's object and retires it only if it is `item`
 * (arts_route_table_set_destroyed_object).  Returns true if this call
 * detached the object. */
bool arts_ooo_retire_item(arts_guid_t key, const void *item);

/* Convenience fresh entry: reserve/lookup the slot for `guid`, then
 * dispatch_or_defer with payload == NULL. */
void arts_ooo_dispatch_or_defer_guid(arts_guid_t guid, ooo_kind_t kind,
                                     const void *args, uint32_t args_size);

/* Unconditional defer (force-push) keyed on `guid` — used by the GPU
 * GPU-LC-invalidation path which must hold a signal until the wrapper EDT's
 * outstanding invalidations drain, regardless of the destination's install
 * state. */
void arts_ooo_push_guid(arts_guid_t guid, ooo_kind_t kind, const void *args,
                        uint32_t args_size);

/* Create handler's last step: replay accumulated payloads against the
 * just-installed item.  Detaches one snapshot of the slot's OoO chain
 * (reverse_drain → FIFO) and runs each through dispatch_or_defer.  Lock-free:
 * concurrent drains detach disjoint snapshots. */
void arts_ooo_drain(struct arts_route_item_s *slot);

/* Convenience: look up the slot for `guid` and drain it.  Used by create
 * handlers that hold only the GUID (e.g. coherence DB_CREATE). */
void arts_ooo_drain_guid(arts_guid_t guid);

/* Free every payload still queued on a slot's chain without dispatching —
 * route-table teardown only (a retire returns the slot and re-drives its
 * chain; it never frees one). */
void arts_ooo_free_all(struct arts_route_item_s *slot);

/* Re-resolve every parked node against its own GUID and defer it again.  A
 * teardown calls this after returning the slot, so a node that raced the
 * return is moved to the slot its GUID now maps to instead of being stranded. */
void arts_ooo_redrive_all(struct arts_route_item_s *slot);

#ifdef __cplusplus
}
#endif

#endif
