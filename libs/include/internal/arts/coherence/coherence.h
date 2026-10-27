/* SPDX-License-Identifier: Apache-2.0
 *
 * Coherence protocol data structures.
 *
 * Per-DB state under the home-directory + node-cache protocol.  This
 * header captures only the struct layouts the runtime needs to
 * manipulate.
 *
 * Atomic discipline: this header declares no shared word itself; the words
 * the runtime reads and writes concurrently live in the layouts it pulls in.
 * A word declared `_Atomic(T)` is accessed only through atomic_*_explicit
 * (arts/utils/stdatomic_compat.h).  Every field that is not `_Atomic` is
 * plain, ordered by the atomic word or the lock its declaration names
 * (`time_stamp` by the block's staging rwlock).
 *
 * Routing pointer convention: 48-bit virtual address space.  Tagged-
 * pointer encodings (lock-free stack `top`, marked-list `next`) live
 * at the primitive layer; this header just embeds those primitives.
 */

#ifndef ARTS_COHERENCE_COHERENCE_H
#define ARTS_COHERENCE_COHERENCE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "arts/utils/stdatomic_compat.h"

#include <semaphore.h> /* sem_t — arts_db_await_ack signature */
#include <stdbool.h>
#include <stdint.h>

/* The DB coherence layout structs (arts_db_buffer_s, arts_db_rw_waiter_s,
 * arts_db_read_waiter_s, arts_home_grantreq_queue_s + node,
 * arts_db_cache_s, arts_db_s) live in coherence/types.h (pulled in via
 * runtime_types.h), because struct arts_db_s embeds arts_db_cache_s by value
 * as its first member and inlines the home-directory fields, so it needs the
 * complete cache type.  This header keeps only the protocol function
 * declarations. */
#include "arts/runtime_types.h"
#include "arts/utils/shared.h" /* arts_shared_ptr_t — stub_install return type */

/*--- Pending RW Treiber-stack lifecycle helpers --------------------------
 * The per-cache RW waiter chain belongs to the shared grant plane, so it
 * exists in every arm whose write right migrates: it parks same-node RW EDTs
 * that piggyback on an in-flight ownership round. */
#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)
void arts_pending_rw_queue_init(arts_lf_stack_t *q);
/* Push a waiter (multi-producer).  Caller fills edt/slot before
 * calling.  Waiter must be heap-allocated; the stack takes ownership, and the
 * chain wake or destroy frees it. */
void arts_pending_rw_queue_push(arts_lf_stack_t *q,
                                struct arts_db_rw_waiter_s *w);
/* Destroy: free every queued waiter (single-threaded at teardown). */
void arts_pending_rw_queue_destroy(arts_lf_stack_t *q);
#endif /* shared grant plane */

/* Cache_s init kinds — selects how writer_count / home / buffer get
 * initialized. */
typedef enum {
  /* Creator side, home == self: install buffer, writer_count = the right
   * plus the creator EDT's hold, home struct with self as the initial
   * holder. */
  ARTS_DB_INIT_CREATOR_HOME = 0,
  /* Creator side, home != self: install buffer (creator local),
   * writer_count = the right plus the creator EDT's hold, no home struct. */
  ARTS_DB_INIT_CREATOR_REMOTE,
  /* Home side, creator != self (DB_CREATE handler): install
   * buffer (bytes unspecified), writer_count = 0, home struct with
   * creator_rank as the initial holder. */
  ARTS_DB_INIT_HOME_RECV,
  /* Stub install by a dependence's first touch on a rank that is not the
   * block's home: no buffer, writer_count = 0. */
  ARTS_DB_INIT_STUB,
} arts_db_init_kind_t;

/* Initialize a coherence cache_s in place for db_guid.  The cache is
 * embedded by value as the first member of struct arts_db_s; the caller
 * allocates (and zeroes) the db_s and passes &db->cache.  Used by
 * db_create_in_place (creator side), the DB_CREATE wire handler (home
 * side), and stub install on consumer ranks.  creator_rank: only consulted
 * when kind == ARTS_DB_INIT_HOME_RECV (the initial holder, which a revoking
 * home names in rw_holder).
 * Defined by each arm, around its own cache fields. */
void arts_db_cache_init(struct arts_db_cache_s *c, arts_guid_t db_guid,
                        uint64_t db_size, arts_db_init_kind_t kind,
                        unsigned int creator_rank);

/* Public destroy entry — sends DESTROY_REQ to home; home runs the
 * fan-out and finalize.  Defined in coherence.c. */
void arts_db_destroy_remote(arts_guid_t db_guid);

/* Cache_s destructor: drops the buffer slot's shared_ptr ref (the cb deleter
 * ends the buffer once the last in-flight acquirer releases) and tears down
 * home_s in place.
 * Called from arts_db_free.  Because the cache is embedded by value as the
 * first member of arts_db_s, the caller releases the wrapping db_s's storage
 * after this routine returns (arts_db_buf_storage_release) — it does NOT free
 * the cache separately.  Defined by each arm, around its own cache fields. */
void arts_db_cache_destructor(struct arts_db_cache_s *cache);

/*--- Acquire path --------------------------------------------------------
 *
 * Implements the 8-case dispatcher (WT × WB × {RO, RW}), the remote
 * acquire helpers (GRANT_REQUEST for RW, SNAPSHOT_REQUEST for RO), the
 * GRANT/SNAPSHOT_RESPONSE-side drain routines, and the WB first-touch cache_s
 * allocation for foreign ranks.
 *
 * Result enum, returned by the remote acquire helpers
 * (arts_db_acquire_remote_ro / arts_db_acquire_remote_rw): OK means data was
 * resolved synchronously into the slot; PARK means the EDT was parked on the
 * coherence protocol and a later wake (SNAPSHOT_RESPONSE / GRANT / TRANSFER) will
 * deliver it.  The acquire handler itself (arts_handler_db_acquire, below) is a
 * void self-accounting body: a synchronous resolve calls
 * arts_db_acquire_resolved (count the dep + advance the RW cursor); a remote
 * request parks. */
typedef enum {
  ARTS_DB_ACQUIRE_OK = 0,
  ARTS_DB_ACQUIRE_PARK,
} arts_db_acquire_result_t;

/* Stub first-touch: allocate cache_s on this rank if absent.  Used by the
 * acquire path when a lookup_db miss is observed on a remote-owned DB.
 * Returns a PINNED handle to the db_s whose cache it installed; the caller
 * MUST arts_shared_release it on every control-flow path.  A NULL handle =
 * the DB was destroyed before the install could be observed. */
arts_shared_ptr_t arts_db_cache_stub_install(arts_guid_t db_guid,
                                             uint64_t db_size);

/* The coherent acquire of one dep (per model).  item = the installed db_s;
 * args = arts_ooo_args_db_acquire_s {edt, db_guid, slot}.  Attempts the one
 * dep's acquire (mode x ownership dispatch): a local hit records its payload
 * and calls arts_db_acquire_resolved, an in-frame serve resolves the slot
 * through the arm's resolver, anything else parks.  Called only by the
 * driver's acquire_one_dep, which an OOO_DB_ACQUIRE replay reaches too: that
 * kind's table entry is arts_db_acquire_replay_dep, never this body.  The
 * slot's subtype and descriptor pin are in its acquire record before the call.
 *
 * A handler publishes the slot to its one resolver -- the RMW that links its
 * waiter, its push, or the send that names (edt, slot) -- or resolves it in
 * its own frame.  After that it touches neither the slot's acquire record nor
 * the EDT (its depv included): the resolver may already have accounted the
 * slot, and the EDT may have run.  It may keep using the descriptor it was
 * called with.  The frame that called it keeps the descriptor alive: the
 * acquire phase's own frame (its first pass and the walk, under the phase's
 * bias) or a resume drain, through the account it still owes (the EDT cannot
 * run, so the record's pin stays); an off-walk OoO replay through the OoO
 * dispatcher's pin on the installed object, held across every dispatch --
 * not through the record's pin, which the EDT's release drops.  So the rest of an off-walk replay's handler (a
 * read's request send, a write-through home's re-check and drain of its
 * publication waiters) may run after the slot's EDT ran, released and the
 * program legally destroyed the block: the caller's pin covers the free; the
 * destroy's synchronous half touches no state that rest touches; and a
 * request sent after its slot was served is a covered request the home
 * already counts (declared by a write request or owed by a create's
 * announce).  A serialized slot's replay runs under the account its worklist
 * entry owes, so its handler finishes before its EDT can run. */
void arts_handler_db_acquire(void *item, void *args);

/* Decrement acquire_remaining; schedule the EDT if it reaches 0. Caller must
 * not touch the EDT afterward (it may have been scheduled + run). */
void arts_db_acquire_account(struct arts_edt_s *edt);

/* A dep the firing frame resolved locally (its acquire record written): if it
 * is serialized, advance the cursor, which is at it (the acquire loop, not
 * this call, fires the next one); hand the payload to the slots that alias it;
 * then count the dep down, which may schedule the EDT. */
void arts_db_acquire_resolved(struct arts_edt_s *edt, unsigned int slot);

/* An EDT acquires each distinct block ONCE: the owner slot asks an arm for it,
 * every other slot naming it is an alias that requests nothing and skips the
 * arm's release.  This hands the block's payload to those aliases — the same
 * address, a per-slot buffer ref and descriptor pin, one account each — and is
 * the ONLY place an alias is resolved, on every arm.  Call it from the owner's
 * one resolver, where its pointer becomes final, BEFORE accounting the owner
 * (that account may schedule the EDT, after which nothing may read its acquire
 * state); a NULL payload resolves the aliases to NULL, the value a block with
 * no bytes reads as.  One pass over depv in the calling frame: no dispatch, no
 * recursion.
 *
 * The aliases copy the owner's record pin: an alias must never end with a
 * buffer ref and no descriptor pin (a release that cannot derive the buffer
 * from the payload address finds it through the descriptor), and a fresh
 * lookup can miss under a concurrent destroy while the payload is still alive
 * under the owner's own pin.  Only an owner with no pin (a non-coherent block)
 * has the block looked up here, and a miss resolves the aliases to NULL with
 * no ref at all. */
void arts_db_fill_aliases(struct arts_edt_s *edt, unsigned int owner_slot);

/* Per-protocol classification used by the arts_db_acquire_all driver: returns
 * true for the dep modes whose acquire can WAIT on another rank, which the
 * engine must therefore fire in one global GUID order so an EDT's own
 * acquisitions cannot deadlock against another EDT's.  An arm whose acquire
 * waits on nobody serializes nothing.  DB_MODE_NULL acquires nothing and is
 * never serialized.  Defined in each arm's own write-policy TU. */
bool arts_db_acquire_is_serialized(arts_db_access_mode_t mode);

/*--- Release path --------------------------------------------------------
 *
 * Each arm defines its own RW release.  Under VAL and INV it bumps the
 * buffer's version, publishes where the arm does and waits for that publish's
 * ACK — VAL WT ships the payload to the home from any other rank; INV asks
 * the home for the release's invalidation round, carrying the payload under
 * WT from a rank other than the home — and then takes the release policy's
 * count-dropping edge: the last writer of a revoked grant ships it to the
 * target the round named, and a voluntarily returned grant goes back to the
 * home, riding the publish where there is one. */

/* Release a RW acquire taken as a DEPENDENCE.  payload is the releasing EDT's
 * pointer into its hold; arms whose release does not move the EDT's own bytes
 * ignore it, and an arm that does has nothing to move when it is NULL. */
void arts_db_release_rw(struct arts_db_cache_s *cache, void *payload);

/* Release the hold a CREATE took.  A separate entry point because the two are
 * statically distinct callers, not two readings of one pointer: a creating
 * EDT's bytes are wherever the arm put them at create time, while a
 * dependence's are the ones the acquire resolved.  Defined once per protocol
 * TU beside arts_db_release_rw. */
void arts_db_release_created(struct arts_db_cache_s *cache);

/* Release a RO acquire — currently a no-op (no held ref to drop) but kept as
 * a separate symbol for symmetry and future cutover. */
void arts_db_release_ro(struct arts_db_cache_s *cache);

/* ===== Shared coherence services (defined in coherence.c) ============
 * Model TUs (and the release-family TU) call back into these model-agnostic
 * helpers.  These are the genuinely shared coherence internals the per-model
 * handler bodies reuse; they carry no model #ifdef. */

/* Releaser-side reply rendezvous: the wait state a blocked releaser parks on
 * until the home answers one message.  Its ADDRESS travels as the wire `cv`
 * and the answer posts it by pointer identity: the PUBLISH_CTS to a publish
 * announce (VAL and INV write-through, EXCL PURGE's landing-less release),
 * whose handler writes `landing` before posting (sem_post is the
 * release/acquire edge), or the PUBLISH_ACK to a grant return sent on its own
 * (VAL and INV PURGE), which posts it alone.  Always on the heap: a wait the
 * shutdown cuts short leaks it, so a late answer still lands in live memory.
 * `sem` MUST be the first member: the ACK path posts (sem_t *)cv directly, so
 * &wr == &wr.sem. */
struct arts_db_pub_rendezvous_s {
  sem_t sem; /* FIRST — cv posts resolve to this address */
  struct arts_rdzv_landing_s landing;
};

/* Synchronous publish to the DB's home: the write-through releases of VAL and
 * INV, which ship the payload, and INV WB's release, whose publish is
 * control-only (the invalidation-round request).  Registers the caller as a
 * version-covered waiter on the cache's publish flight: at most one publish is
 * in flight per (DB, rank), a payload flight ships the cache buffer's CURRENT
 * bytes under its current version, and releases that land mid-flight coalesce
 * into at most one trailing flight.  Returns once a publish covering `version`
 * is ACKed (the source-lifetime pin is the flight's own buffer ref, released at
 * the PUT's local completion).  Blocks the calling worker; returns early only
 * on shutdown. */
#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)
void arts_db_publish_sync(struct arts_db_cache_s *cache, uint64_t version);
#endif

/* Block on the caller's heap semaphore until the matching reply posts it;
 * returns early if teardown begins.  The one blocking wait of the
 * release path, in every arm that has one.  Returns whether the reply came:
 * false is a wait the shutdown cut short, which it records -- a caller that
 * then drops the rest of its round records the waits it never began. */
bool arts_db_await_ack(sem_t *cv);

/* Debug-only one-shot teardown-time invariant check over every live DB (see
 * the definition): once all runtime threads have joined, prints
 * QUIESCENCE-DEBUG markers for states the protocol can never leave behind
 * and, after a shutdown that abandoned no work, for residues a quiescent run
 * must not leave either.  Compiled out below ARTS_LOG_LEVEL DEBUG — the call
 * is always legal, the walk only exists where its messages do. */
void arts_db_debug_quiescence_check(void);

/* Take the EDT's strong buffer ref and return buf->data (NULL when no buffer is
 * installed).  Used by the per-protocol acquire bodies. */
void *arts_db_acquire_local(struct arts_db_cache_s *cache);

/* PARK the (edt, slot) read on the cache's combining window, which sends at
 * most one SNAPSHOT_REQUEST per cache at a time.  Used by the VAL acquire
 * bodies. */
#if defined(ARTS_PROTOCOL_VAL)
arts_db_acquire_result_t
arts_db_acquire_remote_ro(struct arts_db_cache_s *cache, struct arts_edt_s *edt,
                          unsigned int slot);
#endif

/* The one resolution of an owner slot the EDT's acquire phase handed to an
 * arm, by the frame that fired it or the one frame a plane's take hands its
 * waiter to.  Whether the slot also holds up the EDT's SERIALIZED acquire walk
 * is read off the slot itself: a resolution of a serialized owner slot lets
 * the walk past it in the same frame — after the payload is published, before
 * the account that may schedule the EDT — and any other slot leaves the walk
 * where it stands.  The slot's descriptor pin, stored before the arm's handler
 * ran, keeps the descriptor alive.  A Debug build aborts on a second
 * resolution of the slot or one after the EDT's acquire phase ended; a wake
 * through a waiting entry runs the entry's ARTS_DEP_WAITER_CHECK first.
 *
 * arts_db_resolve_slot resolves to buf_h, which it CONSUMES (it becomes the
 * slot's hold; NULL resolves a block with no bytes): an arm that gives every
 * EDT a private copy passes that copy. */
#ifdef ARTS_PROTOCOL_FLUSH
void arts_db_resolve_slot(struct arts_edt_s *edt, unsigned int slot,
                          arts_shared_ptr_t buf_h);
#endif

#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV) ||               \
    defined(ARTS_PROTOCOL_EXCL)
/* The cache-slot form of the resolution: the buffer is whatever this rank's
 * cache holds for the block, read through the descriptor the slot pins.  Used
 * by the in-frame serves, the drain paths and the response handlers of every
 * arm whose readers share the rank's own copy. */
void arts_db_resolve_slot_cached(struct arts_edt_s *edt, unsigned int slot);
#endif

#if (defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)) &&              \
    defined(ARTS_WRITE_POLICY_WT)
/* Wake every read parked on a write-through home before the block's first
 * publication (see the definition).  Called at the publication edges and at a
 * grant's install. */
void arts_db_drain_publication_waiters(struct arts_db_cache_s *cache);
#endif

#ifdef ARTS_PROTOCOL_VAL
/* Response-terminal hook of the remote-read combining window: resume the
 * in-flight batch against the now-current buffer, then re-arm the window with
 * whatever accumulated meanwhile or release it.  Call at every point the
 * response path resumes the on-wire requester. */
void arts_db_ro_combine_on_terminal(struct arts_db_cache_s *cache,
                                    uint32_t cover);

/* Ownership-arrival hook of the combining window: resume every read waiter
 * still accumulating (not the in-flight group) against the just-transferred
 * buffer.  Must be called from the transfer-commit body while its
 * sentinel/guard pins writer_count — the bracket that makes the local serve
 * race-free against a concurrent ownership departure. */
void arts_db_ro_combine_grant_drain(struct arts_db_cache_s *cache);
#endif

/* Shared cache_s construct/destruct sub-helpers.  The per-protocol
 * arts_db_cache_init / arts_db_cache_destructor wrap these, preserving the
 * exact order: protocol field-init runs BEFORE arts_db_cache_common_init on
 * construct; on destruct the wrapper runs arts_db_cache_common_destroy_pre
 * (buffer-NULL) → protocol field-destroy → arts_db_cache_common_destroy_post
 * (snapshot drain → home teardown).  Splitting the destruct into pre/post lets
 * the protocol field-destroy land between the buffer-NULL and the snapshot/home
 * teardown. */
#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV) ||                \
    defined(ARTS_PROTOCOL_EXCL)
void arts_db_cache_common_init(struct arts_db_cache_s *c, arts_guid_t db_guid,
                               uint64_t db_size, arts_db_init_kind_t kind,
                               unsigned int creator_rank);
void arts_db_cache_common_destroy_pre(struct arts_db_cache_s *cache);
void arts_db_cache_common_destroy_post(struct arts_db_cache_s *cache);
#endif

#ifdef ARTS_USE_CXL
/* Record this block's slot in a cache that has none; a cache that already
 * names a slot keeps it.  Every slot has one origin: the block's creator mints
 * it from the arena before the create is sent, and every other rank learns
 * it from the create or a grant.  True when this call installed addr; false
 * when the cache already named a slot or addr is 0. */
bool arts_db_cxl_slot_record(struct arts_db_cache_s *cache, uint64_t addr);
/* Allocate a slot of db_size bytes out of the arena for a create that
 * builds no descriptor here; 0 for a sentinel-sized block, which has none. */
uint64_t arts_db_cxl_slot_mint(uint64_t db_size);
/* Allocate out of the arena, for the size the cache DECLARES, and
 * record it.  False when the block is sentinel-sized (db_size == 0) or
 * already carries a slot — in which case nothing was allocated.  The caller
 * must have recorded cache->db_size first: a first-touch stub carries none,
 * and a create that finds such a cache is the block's declaration of its
 * size.  Only a create that MADE the block may call it, and only for
 * db_type == ARTS_DB — a kind that keeps no coherence state has no funnel to
 * read the slot.
 *
 * The slot is not written: a fresh slot's contents are unspecified until a
 * holder writes them. */
bool arts_db_cxl_slot_create(struct arts_db_cache_s *cache);
/* Drop a slot this rank minted for a create that turned out to create
 * nothing.  The cache is left naming neither the slot nor a payload in it, so
 * nothing behind it could hand the slot out again; the bytes stay in the
 * arena.
 *
 * The cache must be one NOTHING ELSE CAN NAME — a create whose route install
 * failed, whose object is private and about to be freed.  A published cache
 * may have a holder, a fetch in flight or a first user about to adopt the
 * slot; so a create that published its object keeps the slot for the block's
 * whole life. */
void arts_db_cxl_slot_discard(struct arts_db_cache_s *cache);
/* The slot this rank knows for the block, 0 when it knows none. */
static inline uint64_t
arts_db_cxl_slot_addr(const struct arts_db_cache_s *cache) {
  return atomic_load_explicit(&cache->cxl_addr, memory_order_acquire);
}
#else
/* Every create announces the address it knows, and a build with no CXL
 * store knows none — so the call site is the same one in both builds. */
static inline uint64_t
arts_db_cxl_slot_addr(const struct arts_db_cache_s *cache) {
  (void)cache;
  return 0u;
}
static inline uint64_t arts_db_cxl_slot_mint(uint64_t db_size) {
  (void)db_size;
  return 0u;
}
#endif /* ARTS_USE_CXL */

/* Case-D (arts_handler_db_create) per-protocol leaf function.
 * install_home_buffer: WT/WB defer the install to the creator's first
 * PUBLISH (no-op here).  Defined once per protocol TU. */
void arts_db_create_install_home_buffer(struct arts_db_cache_s *cache,
                                        uint64_t db_size);

/* Record what made a cache while it is still private to its builder: a
 * create's own descriptor, the home's directory, or a dependence's first touch
 * (ARTS_DB_INIT_STUB).  Called from each arm's arts_db_cache_init. */
void arts_db_cache_kind_seed(struct arts_db_cache_s *c,
                             arts_db_init_kind_t kind);

/* A create on a rank other than the block's home that finds this rank's slot
 * holding a cache a dependence made claims it rather than parking: that cache
 * is the rank's view of the block the create is making, and the create makes
 * it its own (a cache of an earlier lifetime that kept a right to the block
 * then refuses the create's hold, and the create parks behind it).  Turns the
 * kind from ARTS_DB_INIT_STUB into the creator's (ARTS_DB_INIT_CREATOR_REMOTE)
 * in one step, so exactly one create claims a given cache and every later
 * create of the label on this rank finds a creator's cache and parks.  False
 * when the cache was not a dependence's. */
bool arts_db_create_claims_stub(struct arts_db_cache_s *cache);

/* Record the create's own hold in this rank's cache, each arm naming it in
 * its own state: the migrating-grant arms take possession plus one writer on
 * the grant word, the exclusion arm takes its cache word's RW grant, and an
 * arm that keeps no permission word takes nothing.  Runs only for a create
 * that claimed the cache; it reports whether it committed, because possession
 * is never seeded over a word that already carries a hold (a cache of an
 * earlier lifetime that kept a right to the block refuses it).  Defined once
 * per protocol TU. */
bool arts_db_create_take_hold(struct arts_db_cache_s *cache, uint8_t *owed);

#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)
/* The write-request axis of the arm's request word, for the shared grant
 * plane.  claim: IDLE -> OPEN (true = won), handing back the stub bit it read
 * as the OPEN's token.  commit: OPEN -> SENT only while the stub bit still
 * equals the token, returning the request's ARTS_COVER_* word read in that
 * same atom; false = a create's claim reset this OPEN (the claim clears the
 * stub bit exactly once, so an OPEN claimed after it carries another token
 * and a stale holder can never commit it), and the claim serves the waiter.
 * giveback: OPEN -> IDLE with nothing to ask for, under the same token.
 * clear: SENT -> IDLE where a grant lands, serving the readers that rode it,
 * and clearing the stub bit.  declaim: a size round answered the write
 * request (rw must be SENT): clear the stub bit -- the home is installed and
 * was created elsewhere, so this cache can never be claimed -- and return the
 * cover word the re-send carries (never PRECLAIM). */
bool arts_db_rw_req_claim(struct arts_db_cache_s *cache, uint32_t *tok);
bool arts_db_rw_req_commit(struct arts_db_cache_s *cache, uint32_t tok,
                           uint32_t *cover);
void arts_db_rw_req_giveback(struct arts_db_cache_s *cache, uint32_t tok);
void arts_db_rw_req_clear(struct arts_db_cache_s *cache);
uint32_t arts_db_rw_req_declaim(struct arts_db_cache_s *cache);
/* The grant plane's half of a create's claim (grant.c): claim_writers serves
 * every parked writer under the create's hold, each with a hold of its own,
 * BEFORE the arm's CLAIM transition reopens the write axis; rearm, after it,
 * opens a request for any writer that pushed past the take. */
void arts_db_grant_claim_writers(struct arts_db_cache_s *cache);
void arts_db_grant_rearm(struct arts_db_cache_s *cache);

/* The declared-arrival rule at the home (coherence.c).  cover_request says
 * what becomes of an arriving request (ARTS_COVER_*); cover_destroy marks
 * the gate and completes the teardown now when nothing is owed; the arm's
 * destroy_now is the teardown itself (fan-out and slot return), run by
 * whichever of the destroy and the last owed arrival reaches the edge. */
#define ARTS_COVER_PROCESS 0
#define ARTS_COVER_DROP 1    /* discarded unanswered: a pre-claim request */
#define ARTS_COVER_DISMISS 2 /* declared: answer with a dismiss (no data; the
                                landing comes back) and process nothing */
/* *teardown: this arrival completed the gate; the caller runs
 * arts_db_home_destroy_now after any reply. */
int arts_db_cover_request(struct arts_db_s *db, unsigned int requester,
                          bool write, uint32_t cover, bool *teardown);
void arts_db_cover_destroy(struct arts_db_s *db);
/* Whether the block's destroy has marked the home's gate (an acquire load). */
bool arts_db_cover_marked(struct arts_db_s *db);
void arts_db_home_destroy_now(struct arts_db_s *db);
void arts_db_cover_home_init(struct arts_db_s *db, unsigned int nranks);
void arts_db_cover_home_teardown(struct arts_db_s *db);

/* The parked-reader pool (struct arts_db_reader_pool_s, reader_pool.c).
 * alloc hands out an index the caller owns until it links the node; free
 * returns one owned node; free_chain returns a chain the caller owns, linked
 * head to tail through next, with one push; serve_chain wakes every node of a
 * chain a CAS took and then returns it.  destroy frees the storage, with no
 * thread left to touch the pool; a node still linked is freed, not woken. */
void arts_db_reader_pool_init(struct arts_db_reader_pool_s *p);
uint32_t arts_db_reader_alloc(struct arts_db_reader_pool_s *p);
struct arts_db_reader_node_s *
arts_db_reader_node(struct arts_db_reader_pool_s *p, uint32_t idx);
void arts_db_reader_free(struct arts_db_reader_pool_s *p, uint32_t idx);
void arts_db_reader_free_chain(struct arts_db_reader_pool_s *p, uint32_t head,
                               uint32_t tail);
void arts_db_reader_serve_chain(struct arts_db_reader_pool_s *p,
                                uint32_t head);
void arts_db_reader_pool_destroy(struct arts_db_reader_pool_s *p);
#endif
#ifdef ARTS_PROTOCOL_VAL
void arts_db_ro_combine_on_cts(struct arts_db_cache_s *cache, uint32_t cover);
uint8_t arts_db_val_claim_cover(struct arts_db_cache_s *cache);
#endif

/* The counterpart, for a create that gives the block its first image in a
 * cache this rank had already made: on an arm that tracks a durable reader
 * copy, that image is this rank's.  Committed ONLY from a reader word that
 * is exactly idle — no fetch open, no chain parked — since a fetch already
 * in flight brings the copy with it when it lands, and stamping a claim over
 * it would hand that reader an invented image.  A no-op on the arms whose
 * readers re-validate at every acquire.  Defined once per protocol TU. */
void arts_db_create_claim_creator_copy(struct arts_db_s *db);

/* Retract whatever the create-time seed claimed about this rank holding a
 * READER copy of the block.  Called only where the create takes no hold
 * (ARTS_DB_PROP_NO_ACQUIRE), which is exactly where the claim is false: an
 * arm that seeds "the creator's copy is valid — it holds the bytes it is
 * about to write" is describing a creator that was given storage, and a
 * create that acquires nothing is given none.  Left standing, the claim
 * answers this rank's own reads from a copy it never had.  A no-op on every
 * arm whose reader plane keeps no durable copy claim. */
void arts_db_create_retract_creator_copy(struct arts_db_s *db);


#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)
/* Single-owner ownership machinery shared by every grant-bearing arm (VAL,
 * INV) (defined in coherence/grant.c).  Called by the WT/WB
 * arts_handler_db_acquire bodies; the RO-path predicate is the only divergence
 * between WT and WB, so it stays inline in each protocol's handler.
 *
 * arts_db_acquire_remote_rw: the remote-RW acquire path.  Pushes a
 * pending_rw waiter and kicks a GRANT_REQUEST if none is in flight —
 * returns PARK. */
arts_db_acquire_result_t
arts_db_acquire_remote_rw(struct arts_db_cache_s *cache, struct arts_edt_s *edt,
                          unsigned int slot);

/* ===== Release-policy seams (coherence/grant_purge.c / grant_retain.c) =====
 * WHO initiates the grant's return is the release policy's whole question, and
 * these points are where the two answers differ.  Under RETAIN the home
 * drives every hand-over (it revokes the current holder and the holder ships
 * onward), so the request arrival starts a round, the new owner's CONFIRM
 * names it as the holder the next revocation targets, the baton holder
 * advances or puts the baton down, and the count-dropping edges only ship when
 * a round has already named a target.  Under PURGE the holder gives the right
 * back at its own idle edge and the home hands it out from there, so the
 * request arrival only queues unless the home holds the right, the home's
 * own possession is its whole directory (no holder is named, no CONFIRM is
 * sent, and no round outlives the serve that grants it), and the
 * count-dropping edges are the return itself.
 *
 * arrived:      home-side GRANT_REQUEST tail, with the requester queued.
 * advance:      the baton holder's next step (RETAIN only): revoke the
 *               current holder for the next linked requester, or put the
 *               baton down.  `seen` is the claim count the caller looked
 *               behind: 1 for the claim that won it, and for a CONFIRM, which
 *               inherits the hold through the round's messages, a value it
 *               loaded with acquire before its own look; `looked` says the
 *               caller has just found no linked requester.
 * release_skip: release_rw's defensive entry guard (true = nothing to drop).
 * release_commit: the count-dropping edge of a writer's own release; the
 *               install's own hold is closed by commit_drain / commit_finish
 *               below.
 * Every one of them is called with the arm's own state already settled: the
 * release's publish has been acknowledged. */
void arts_db_grant_request_arrived(struct arts_db_cache_s *cache,
                                   struct arts_db_s *db,
                                   unsigned int requester);
#ifndef ARTS_RELEASE_PURGE
void arts_db_grant_advance(struct arts_db_cache_s *cache, struct arts_db_s *db,
                           bool looked, uint64_t seen);
#endif
bool arts_db_grant_release_skip(struct arts_db_cache_s *cache);
void arts_db_grant_release_commit(struct arts_db_cache_s *cache);
/* The install's drain, and what closes it.  Under a demand-revoked policy the
 * commit keeps a hold of its own across the walk and drops it at the end;
 * under a voluntary-return policy the commit's hold BECOMES the waiters' holds
 * in one step before any of them is woken, so the last of them to release sees
 * a word carrying exactly its own hold and can hand the right back on its own
 * publish.  commit_drain returns how many waiters it woke and commit_finish is
 * given that count: where the hold became the waiters', at least one waiter
 * must have taken it. */
unsigned int arts_db_grant_commit_drain(struct arts_db_cache_s *cache);
void arts_db_grant_commit_finish(struct arts_db_cache_s *cache,
                                 unsigned int drained);
/* Chain primitives for a commit that counts before it wakes (grant_queue.c). */
arts_lf_link_t *arts_pending_rw_queue_take(arts_lf_stack_t *q);
/* Whether any waiter is parked (a snapshot: a push may land right after). */
bool arts_pending_rw_queue_pending(arts_lf_stack_t *q);
unsigned int arts_pending_rw_chain_count(arts_lf_link_t *chain);
/* A grant's arrival: take every waiter it serves and release the coalescing
 * flag, opening a request for any waiter pushed after the take.  The caller
 * counts and wakes the returned chain. */
arts_lf_link_t *arts_db_grant_take_waiters(struct arts_db_cache_s *cache);
/* Hand every waiter of a taken chain to cb, then free it.  cb gets the waiter
 * itself, live for the call, and reads its EDT; this walk reads none. */
void arts_pending_rw_chain_wake(arts_lf_link_t *chain,
                                void (*cb)(const struct arts_db_rw_waiter_s *w,
                                           void *ctx),
                                void *ctx);
/* ===== The hand-back as a rider on the release's own publish =============
 * A write-through release already sends the home its payload, and the home is
 * exactly who the write right goes back to — so the two travel together and
 * the hand-back costs no message of its own.  What that buys has to be paid
 * for in discipline: the hand-back becomes an OBLIGATION on the cache, and an
 * obligation needs exactly one discharger.
 *
 * claim:  before the publish, and only where a hand-back is owed at all, take
 *         the whole write right in ONE attempt.  A single attempt on purpose:
 *         any other word means other writers are still live, and that release
 *         belongs to the ordinary count-dropping edge instead.  A won claim
 *         arms the obligation and its caller must NOT run that edge.
 * take_leg: the publish sender, immediately before a payload COMMIT leg,
 *         asks whether to carry the obligation.  Whoever wins the word sends
 *         it; there is no second winner.
 * settle: after the publish returns, the armer re-checks ITS OWN token.
 *         Still armed means the leg it meant to ride had already gone out,
 *         so it converts to the standalone message.  The token names the
 *         grant generation it was armed under: by the time the armer wakes,
 *         its leg may have carried the right home and the home may have
 *         granted it back here, and a settle that could take that later
 *         token would send the new round's hand-back before the new round's
 *         publish had stamped the home.  Two CAS sites on one word, each
 *         naming what it discharges: exactly one hand-back, never none,
 *         never two.
 * arrived: the home side of a hand-back, however it travelled.  `reply` is
 *         what the returner waits on — the standalone form's rendezvous, or
 *         the carried form's publish acknowledgement — and is sent by
 *         whoever APPLIES the hand-back, never on its arrival alone. */
uint64_t arts_db_grant_release_claim(struct arts_db_cache_s *cache,
                                     bool will_publish);
bool arts_db_grant_return_claim_leg(struct arts_db_cache_s *cache);
void arts_db_grant_release_settle(struct arts_db_cache_s *cache,
                                  uint64_t token);
struct arts_db_grant_return_reply_s {
  uint64_t cv;      /* echoed verbatim */
  uint64_t version; /* the covering version of a carried publish, else 0 */
  bool ack;         /* the returner waits on a reply */
  bool credit;      /* the reply refills the returner's in-place credit */
};
void arts_db_grant_return_arrived(
    struct arts_db_s *db, unsigned int returner,
    const struct arts_db_grant_return_reply_s *reply);
/* The install's possession-setting transition (sentinel + drain guard under
 * RETAIN; the possession bit and that same guard under PURGE).  Possession is
 * never set by an add: an add cannot state the precondition it depends on. */
void arts_db_grant_install(struct arts_db_cache_s *cache);
#endif /* shared grant plane */

#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)
/* arts_db_acquire_rw_local_fast: the case-2/6 RW local fast path.
 * CAS-increments writer_count "if positive"; on success resolves slot `slot`
 * of `edt` (acquire_local) and returns true.  The CAS is the whole admission
 * test, not a commit behind one: writer_count carries both "this rank holds
 * the grant" (positive) and "the home has published the directory flip"
 * (ARTS_GRANT_UNCONFIRMED clear), so callers must NOT pre-test either fact —
 * a separate load cannot be atomic with the take, and a grant lost and
 * re-granted in between would read like one never lost.  Returns false for a
 * count of 0 (no grant) and for an unconfirmed hold alike, so the caller falls
 * through to arts_db_acquire_remote_rw, which parks. */
bool arts_db_acquire_rw_local_fast(struct arts_db_cache_s *cache,
                                   struct arts_edt_s *edt, unsigned int slot);

#endif /* shared grant plane */

#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)
/* GRANT drain: install the granted waiters — pops every pending_rw waiter
 * (bump writer_count per waiter).  Called where a write grant commits on this
 * rank: the WB CONFIRM_ACK handler, RETAIN's commit drain, and a PURGE home
 * that serves its own request from the head of its grant queue. */
void arts_db_drain_pending_rw_after_grant(struct arts_db_cache_s *cache);
#endif /* shared grant plane */

#ifdef __cplusplus
}
#endif

#endif /* ARTS_COHERENCE_COHERENCE_H */
