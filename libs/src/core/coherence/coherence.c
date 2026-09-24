/* SPDX-License-Identifier: Apache-2.0
 *
 * Coherence cache lifecycle + acquire + release + destroy.
 *
 * This translation unit consolidates four phases of the per-rank
 * coherence cache_s:
 *
 *   1. Cache construction / destruction
 *      - arts_db_cache_init: in-place cache_s initializer (creator-home,
 *        creator-remote, home-recv, OWNER).
 *      - arts_db_cache_destructor: chained from arts_db_free.
 *
 *   2. Acquire path (8-case dispatcher + supporting routines).  See the
 *      design plan for the full algorithm; inline comments highlight the
 *      trickier race resolutions.  Wake mechanism: parked EDTs are tracked
 *      via arts_edt_s.depc_needed; a triggered waiter looks up the EDT,
 *      writes the buffer data pointer into depv[slot].ptr, and
 *      atomic_sub(depc_needed); on reaching 0 the EDT is handed to the
 *      scheduler.
 *
 *   3. Release path (the four release cases R1-R4 and the PUBLISH_ACK
 *      rendezvous).  Wait/wake mechanism: a stack-local binary semaphore;
 *      release_rw sem_init's a sem_t on its stack, embeds its address in the
 *      PUBLISH packet, and sem_waits on it.  The home echoes that address
 *      verbatim in PUBLISH_ACK; arts_handler_db_publish_ack sem_posts it.
 *      Matching is by pointer identity (the address is valid only on the
 *      releaser rank, where the post runs) — no per-cache seq state, no
 *      busy-wait.
 *
 *   4. Destroy lifecycle (arts_db_destroy_remote public entry).  Final teardown
 *      is driven by the cb (shared-ptr) deferred-free model: destroy fans out,
 *      then
 *      arts_route_table_set_destroyed frees the cache_s via
 *      arts_db_cache_destructor once all refs drain.
 */

#include <errno.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "arts/coherence/buffer.h"
#include "arts/coherence/coherence.h"
#include "arts/coherence/handlers.h"
#include "arts/coherence/directory.h"
#include "arts/db.h"
#include "arts/edt.h"
#include "arts/fam/pool.h" /* arts_fam_alloc / arts_fam_free (ARTS_FAM only) */
#include "arts/gas/guid.h" /* GUID kind extraction (quiescence debug check) */
#include "arts/gas/route_table.h"
#include "arts/memory/regpool.h"
#include "arts/ooo.h"
#include "arts/runtime_state.h"
#include "arts/runtime_types.h"
#include "arts/system/print.h"
#include "arts/system/schedfuzz.h"
#include "arts/system/threads.h"
#include "arts/transport/net.h" /* arts_net_put_payload */
#include "arts/utils/atomics.h"
#include "arts/utils/lockfree_lifo.h" /* publish flight waiter stack */
#include "arts/utils/malloc.h"
#include "arts/utils/shared.h"
#include "arts/counter/Preamble.h"

/* ================================================================== */
/* ===== Cache lifecycle ============================================ */
/* ================================================================== */

/* The create mark's seed.  A create that installs a fresh descriptor here is
 * the block's creator on this rank, so its cache starts marked; every other
 * cache — a first touch's stub, the home's own — starts unmarked.  The cache
 * is still private to the caller at this point, so a plain store is the whole
 * of it. */
void arts_db_create_hold_seed(struct arts_db_cache_s *c,
                              arts_db_init_kind_t kind) {
  __atomic_store_n(&c->creator_hold,
                   (uint8_t)(kind == ARTS_DB_INIT_CREATOR_REMOTE ? 1 : 0),
                   __ATOMIC_RELAXED);
}

/* One rank's create makes one block once.  The mark outlives the hold it
 * records, because a rank whose create released the block holds no image of
 * it that a later create could be handed; so exactly one create per rank
 * wins the mark, and every later one creates nothing. */
bool arts_db_create_hold_once(struct arts_db_cache_s *cache) {
  uint8_t expected = 0;
  return __atomic_compare_exchange_n(&cache->creator_hold, &expected,
                                     (uint8_t)1, /*weak=*/false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* Protocol-agnostic cache_s field init.  The per-protocol arts_db_cache_init
 * wrapper (coherence/<protocol>.c) runs its protocol-specific field-init
 * (WT/WB pending_rw queue + WB dedup-map/sentinel) BEFORE
 * calling this, so the Vyukov MPSC stub is wired before any push could land.
 *
 * The arms that share this shape are the ones whose cache carries a snapshot
 * reorder buffer and a home directory; an arm with neither builds its cache
 * itself. */
#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV) ||               \
    defined(ARTS_PROTOCOL_EXCL)
void arts_db_cache_common_init(struct arts_db_cache_s *c, arts_guid_t db_guid,
                               uint64_t db_size, arts_db_init_kind_t kind,
                               unsigned int creator_rank) {
  /* Caller provides a zeroed cache (embedded in a zeroed/calloc'd db_s, or
   * memset by the stub path).  We do not zero it here — the embedding db_s
   * owns the storage. */
  c->db_guid = db_guid;
  c->db_size = db_size;
  /* Every cache starts without a buffer; the install path clears this the
   * moment one is in the slot, whoever put it there. */
  __atomic_store_n(&c->payload_pending, (uint8_t)1, __ATOMIC_RELAXED);
#ifdef ARTS_FAM
  c->fam_addr = 0;
#endif
  arts_db_create_hold_seed(c, kind);
  /* Snapshot reorder-buffer: a Treiber stack (zero-initializable, but init
   * explicitly for clarity).  Nodes are heap-allocated on the case-3 push path
   * and freed when drained by the next install. */
  arts_lf_stack_init(&c->pending_snapshot);
#ifdef ARTS_RO_COMBINING_LIVE
  arts_lf_stack_init(&c->ro_combine);
  c->ro_combine_group = NULL;
  c->snapshot_req_in_flight = 0;
#endif
  arts_lf_pool_init(&c->buf_freelist, 0);
  /* Initialize the inlined home-directory fields only on the rank that owns
   * this DB's GUID home; non-home ranks leave db_self->home_initialized false
   * (and allocate only the cache-only stub, so the home fields don't exist).
   * init_kind selects the initial rw_holder. */
  unsigned int self = arts_global_rank_id;
  unsigned int n = arts_global_rank_count;
  if (n == 0) {
    n = 1;
  }
  struct arts_db_s *db_self = arts_db_of_cache(c);
  if (kind == ARTS_DB_INIT_HOME_RECV) {
    arts_db_home_init(db_self, creator_rank, n);
    db_self->home_initialized = true;
  } else if (kind == ARTS_DB_INIT_CREATOR_HOME) {
    arts_db_home_init(db_self, self, n);
    db_self->home_initialized = true;
#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)
    /* The creator boots holding the write right, with its own create-time
     * hold under it: possession, and one live writer. */
    c->writer_count = ARTS_GRANT_SEED_HOLDING;
#endif
  } else if (kind == ARTS_DB_INIT_CREATOR_REMOTE) {
#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)
    c->writer_count = ARTS_GRANT_SEED_HOLDING;
#endif
  }
  /* WT PUBLISH ACK rendezvous is a stack-local sem_t per
   * release_rw (pointer-identity match) — no per-cache seq fields to
   * initialize.  WB owner-side fields (dedup map + transfer sentinel) are
   * armed by the protocol init hook above. */
}
#endif /* arms sharing the common cache shape */

#ifdef ARTS_FAM
bool arts_db_fam_slot_record(struct arts_db_cache_s *cache, uint64_t addr) {
  if (cache == NULL || addr == 0) {
    return false;
  }
  uint64_t expect = 0;
  if (__atomic_compare_exchange_n(&cache->fam_addr, &expect, addr, false,
                                  __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
    return true;
  }
  if (expect != addr) {
    /* One block, one slot.  Two different addresses for one GUID means two
     * creators each minted a store for it — outside the contract (a create
     * of a label another rank is still creating), and keeping both would
     * silently split the block.  This is the slot rule's only loud
     * failure, and the home reaches it on the announce it coalesces. */
    ARTS_ERROR("fam: guid %lu was handed two different slots — two "
               "creators of one label",
               (unsigned long)cache->db_guid);
  }
  return false;
}

bool arts_db_fam_slot_create(struct arts_db_cache_s *cache) {
  if (cache == NULL || cache->db_size == 0 ||
      __atomic_load_n(&cache->fam_addr, __ATOMIC_ACQUIRE) != 0) {
    return false;
  }
  void *slot = arts_fam_alloc((size_t)cache->db_size);
  if (!arts_db_fam_slot_record(cache, (uint64_t)(uintptr_t)slot)) {
    arts_fam_free(slot);
    return false;
  }
  return true;
}

/* A store that goes back leaves nothing naming it.  The cache stops naming it
 * first, so nothing can adopt it after this point; then the descriptor that
 * named it is withdrawn, because a descriptor outliving its storage would
 * hand a later, legitimate first user a pointer into a granule that belongs
 * to some other block by then.
 *
 * Reached only on a cache nothing else can name: a create whose route install
 * FAILED, whose object is private and about to be freed.  That is what makes
 * both steps safe — no other party can be holding the descriptor, fetching
 * into the store, or about to adopt it — and it is a property of the call
 * sites, not of this function, so a new caller has to establish it.  A create
 * whose object IS published never hands its store back: the store belongs to
 * the block from the moment the cache names it. */
void arts_db_fam_slot_discard(struct arts_db_cache_s *cache) {
  uint64_t addr = __atomic_exchange_n(&cache->fam_addr, (uint64_t)0,
                                      __ATOMIC_ACQ_REL);
  if (addr == 0) {
    return;
  }
  (void)arts_db_buf_withdraw(cache, (const void *)(uintptr_t)addr);
  arts_fam_free((void *)(uintptr_t)addr);
}

/* A slot is freed at exactly one of three points: a create that allocated
 * one and then made nothing (discard, above), the block's teardown at the
 * home (here), and the free-to-owner handler on the rank whose slice it
 * came from.  The cache destructor never frees a slot — a creator's cache
 * can die while the home still serves the block from the slot that creator
 * allocated.  The address alone is meaningful: its owner is derived from
 * it, never stored beside it, so there is no second field a free could
 * leave inconsistent. */
void arts_db_fam_slot_release(struct arts_db_cache_s *cache) {
  uint64_t addr = __atomic_exchange_n(&cache->fam_addr, (uint64_t)0,
                                      __ATOMIC_ACQ_REL);
  if (addr == 0) {
    return;
  }
  arts_send_db_fam_free(addr);
}

#endif /* ARTS_FAM */

/* ================================================================== */
/* ===== Acquire path =============================================== */
/* ================================================================== */

/* ===== EDT wake helper ============================================== */

/* Resume `edt`'s parked `slot` on a buffer the caller supplies, and — when the
 * slot is one the EDT's serialized acquire walk is parked at — advance that
 * walk past it, in this one frame.  The caller holds the EDT for the whole
 * call, so the wake reads its acquire state and accounts under a single ref.
 *
 * The ref in buf_h is what the EDT's dep slot will hold for the rest of its
 * life: on a won claim it becomes the EDT's hold (release_one_dep drops it
 * through the block's descriptor) and the descriptor is pinned for that same
 * span.  A lost claim leaves the ref with the caller, which is the only way
 * the two delivery paths that can wake one slot both stay leak-free.
 *
 * Whose buffer it is, is the arm's business: an arm whose payload lives in
 * the rank's cache passes the cache slot's buffer (the cache-slot wrappers
 * below), an arm that gives every EDT a private copy passes that copy.  A
 * NULL handle resolves a zero-size block, whose defined value is NULL.
 *
 * db_h is a pin on the block's descriptor and is CONSUMED: a won claim moves
 * it into the dep slot, so the descriptor cannot be freed under the buffer
 * ref the same claim installed.  The two must be ONE pin taken with the
 * buffer, never a fresh lookup here — a lookup between them could miss a
 * destroy's slot withdrawal and leave the dep holding bytes with nothing
 * keeping their cache alive.
 *
 * Whether this wake also discharges the slot's place in the EDT's serialized
 * walk is read off the slot itself — a serialized owner slot, never an alias —
 * and never off the caller, so whichever frame wins the claim is the one that
 * lets the walk past it.  The tail then runs claim, aliases, cursor, account,
 * drain, and both of its orderings are load-bearing:
 *
 *   claim BEFORE cursor — the cursor passing a slot is what entitles a frame
 *   to fire the next one, and such a frame reads the passed slot's payload as
 *   final.  Publishing first is what makes "the cursor is never ahead of an
 *   unpublished payload" an invariant instead of a race.
 *
 *   cursor BEFORE account — the account may be the decrement that schedules
 *   the EDT, and a scheduled EDT's acquire state belongs to whoever runs it;
 *   nothing may read or write it afterwards.
 *
 * A wake that LOSES the claim owes neither step: the winner owns the
 * continuation, and a second frame advancing the same cursor would put two
 * threads in one walk.  A winning wake whose payload is NULL — a block with no
 * bytes, or one destroyed under a pending acquire — still owes the cursor step,
 * because NULL is that slot's resolved value and a walk never advanced past it
 * never finishes.
 *
 * Stack depth is bounded on every path: the cursor step does nothing but raise
 * a thread-local flag or append to the resume worklist, and the walk it hands
 * off to runs in the single top-level loop of the drain, never nested here.
 *
 * Returns false when the ref was NOT taken — the slot was already resolved by
 * another wake — and the caller still owns buf_h. */
static bool resume_parked(struct arts_edt_s *edt, unsigned int slot,
                          arts_shared_ptr_t buf_h, arts_shared_ptr_t db_h) {
  arts_edt_dep_t *depv = (arts_edt_dep_t *)arts_get_depv(edt);
  bool secured = !depv[slot].alias && arts_dep_is_serialized(depv, slot);
  struct arts_db_buffer_s *buf =
      (struct arts_db_buffer_s *)arts_shared_get(buf_h);
  /* A resolved acquire owes the EDT storage of the DB's declared size.
   * data == NULL therefore means db_size == 0 (a sentinel block, whose
   * defined value is NULL) or the DB was destroyed under a pending
   * acquire, which the programming model leaves undefined. */
  void *data = buf ? buf->data : NULL;
  /* The descriptor this resolution belongs to, borrowed for the alias fill
   * below: the aliases pin THIS handle rather than looking the block up again,
   * because a lookup can miss under a concurrent destroy while the payload is
   * still alive under the pin installed here.  Taken before db_h is moved into
   * the slot and dropped on every path out. */
  arts_shared_ptr_t alias_pin = arts_shared_copy(db_h);
  if (data != NULL) {
    /* Idempotent slot claim.  Two delivery paths can wake the SAME
     * (edt, slot) — e.g. a snapshot_response case-2 drain racing a direct
     * response.  The slot resolves exactly once: CAS depv[slot].ptr
     * NULL->data so only the first wake keeps its buffer ref (the EDT's hold)
     * and accounts; a loser leaves the ref with its caller and returns
     * WITHOUT accounting — no double-decrement of acquire_remaining, no
     * buffer-ref leak. */
    void *expected = NULL;
    if (!atomic_compare_exchange_strong((_Atomic(void *) *)&depv[slot].ptr,
                                        &expected, data)) {
      arts_shared_release(&alias_pin);
      arts_shared_release(&db_h);
      return false;
    }
    /* Won: MOVE the pin into the dep slot (released last in release_one_dep)
     * rather than dropping it here.  The embedded cache is the descriptor's
     * FIRST member, so this pin is what keeps the cache (buffer slot +
     * recycle pool) alive for the whole acquire->release span. */
    if (db_h != NULL &&
        __atomic_load_n(&depv[slot].db_pin, __ATOMIC_ACQUIRE) == NULL) {
      /* Publish with release so the run/release thread (reached via the
       * work-stealing deque handoff) observes db_pin like the sibling ptr
       * field's atomic CAS — keeps TSan clean and the ARM ordering explicit
       * rather than relying on the deque's incidental HW fence. */
      __atomic_store_n(&depv[slot].db_pin, (void *)db_h, __ATOMIC_RELEASE);
      db_h = NULL;
    }
  } else if (db_h != NULL) {
    /* A block with no bytes resolves to NULL, which no CAS on the pointer can
     * arbitrate — the resolved value and the unresolved one are the same.  So
     * the descriptor pin is the claim instead: exactly one wake installs it,
     * and only that wake accounts.  (With no handle at all the block is gone
     * and there is nothing to arbitrate on; a destroyed block woken twice is
     * outside the programming model.) */
    void *pin_expected = NULL;
    if (!__atomic_compare_exchange_n(&depv[slot].db_pin, &pin_expected,
                                     (void *)db_h, false, __ATOMIC_RELEASE,
                                     __ATOMIC_RELAXED)) {
      arts_shared_release(&alias_pin);
      arts_shared_release(&db_h);
      return false;
    }
    db_h = NULL; /* the slot owns it now */
  }
  arts_shared_release(&db_h); /* no-op when moved into db_pin above */
  /* The block's payload is final for this EDT now: every other slot naming it
   * shares these bytes.  Run after the claim published them, and before this
   * slot's own account, which may be the one that schedules the EDT. */
  arts_db_fill_aliases(edt, slot, alias_pin);
  arts_shared_release(&alias_pin);
  if (secured) {
    /* Every byte this slot resolves to is published, so the walk may pass it. */
    arts_db_rw_secure(edt, slot);
  }
  /* Data resolved for this dep — count it down; the actor that reaches 0
   * schedules. The caller's ref on the EDT keeps it alive even if the schedule
   * lets another worker run (and free) it; do NOT touch edt after
   * arts_db_acquire_account returns. */
  arts_db_acquire_account(edt);
  if (secured) {
    /* Free of this EDT's acquire state now, so the walk the cursor step handed
     * off runs here, as one top-level loop over the worklist. */
    arts_db_drain_resume_list();
  }
  return true;
}

/* Resolve the parked EDT and hold it for the whole wake: every read of its
 * acquire state, and the account that may schedule it, happen under this one
 * ref, so the wake needs no second lookup and can meet no intervening
 * teardown.  NULL means the EDT is no longer there, and the caller has nothing
 * left to wake. */
static struct arts_edt_s *parked_edt_pin(arts_guid_t edt_guid,
                                         arts_shared_ptr_t *edt_h) {
  *edt_h = arts_route_table_lookup_edt(edt_guid);
  struct arts_edt_s *edt = (struct arts_edt_s *)arts_shared_get(*edt_h);
  if (edt == NULL) {
    ARTS_INFO("coherence: edt_guid %lu not found at trigger time", edt_guid);
    arts_shared_release(edt_h);
  }
  return edt;
}

bool arts_db_resume_parked(arts_guid_t edt_guid, unsigned int slot,
                           arts_shared_ptr_t buf_h, arts_shared_ptr_t db_h) {
  arts_shared_ptr_t edt_h = NULL;
  struct arts_edt_s *edt =
      (edt_guid == NULL_GUID) ? NULL : parked_edt_pin(edt_guid, &edt_h);
  if (edt == NULL) {
    arts_shared_release(&db_h);
    return false;
  }
  bool claimed = resume_parked(edt, slot, buf_h, db_h);
  arts_shared_release(&edt_h);
  return claimed;
}

#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV) ||               \
    defined(ARTS_PROTOCOL_EXCL)
/* The cache-slot form of the resume: the buffer is whatever this rank's cache
 * currently holds for the DB the dep names.  This wake reaches any rank for
 * any parked slot and knows nothing about whether this one may hold the
 * block, so it never materializes a buffer: the arm that decided to wake the
 * slot did that at the point it established the hold.
 *
 * Cross-TU: the response handlers and the drain paths resume a parked EDT by
 * (edt_guid, slot) through the wrapper below. */
static void resume_cache_slot(arts_guid_t edt_guid, unsigned int slot) {
  if (edt_guid == NULL_GUID) {
    return;
  }
  arts_shared_ptr_t edt_h = NULL;
  struct arts_edt_s *edt = parked_edt_pin(edt_guid, &edt_h);
  if (edt == NULL) {
    return;
  }
  /* The dep slot names the DB whose cache holds the bytes. */
  arts_guid_t db_guid = ((arts_edt_dep_t *)arts_get_depv(edt))[slot].guid;
  arts_shared_ptr_t buf_h = NULL;
  arts_shared_ptr_t db_h = NULL;
  if (db_guid != NULL_GUID) {
    db_h = arts_route_table_lookup_db(db_guid);
    struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(db_h);
    if (db != NULL && db->db_type == ARTS_DB) {
      buf_h = arts_db_buf_acquire(&db->cache);
    }
  }
  /* One lookup yields both the buffer and the pin the dep will carry; the
   * resume CONSUMES the pin, so there is no second lookup and no window in
   * which the two could disagree about whether the block still exists. */
  if (!resume_parked(edt, slot, buf_h, db_h)) {
    arts_db_buf_release(&buf_h); /* lost the claim: drop the extra ref */
  }
  arts_shared_release(&edt_h);
}

/* One wake, one EDT lookup, one block lookup.  A slot the EDT's serialized
 * walk is parked at is let past in the same frame, once its payload is there
 * to be read; any other slot is resolved and accounted with the walk left
 * where it stands. */
void mark_edt_ready_by_guid(arts_guid_t edt_guid, unsigned int slot) {
  resume_cache_slot(edt_guid, slot);
}
#endif /* arms whose readers resume on the rank's own cache */

/* ===== Stub first-touch =========================================== */

/* Allocate a stub arts_db_s + cache_s for a DIST DB that this rank
 * has not yet touched, register it in the route_table, and return a
 * PINNED handle to the db_s whose cache it installed.  The stub has no
 * user data payload — install_buffer allocates on demand cache->user_data on
 * first wire arrival.
 *
 * Race-safe: route_table_install_if_absent rejects if another thread
 * (e.g. a concurrent wire-receive) raced us; in that case we free our
 * stub and return a pinned handle to the established db_s.
 *
 * Returns a pinned handle; the caller MUST arts_shared_release it once the
 * cache is no longer needed (on every control-flow path).  A NULL handle
 * means the DB was destroyed before the install could be observed (the
 * lost-race lookup found no live entry). */
arts_shared_ptr_t arts_db_cache_stub_install(arts_guid_t db_guid,
                                             uint64_t db_size) {
  /* First check if it already exists (someone else stub-installed or
   * a wire-receive fired). */
  arts_shared_ptr_t existing = arts_route_table_lookup_db(db_guid);
  if (arts_shared_get(existing) != NULL) {
    return existing;
  }
  arts_shared_release(&existing);

  /* Stub install (non-home consumer first acquire): cache-only stub — no home
   * directory (this rank is not the GUID home).  arts_db_cache_stub_size()
   * spans cache + db_type, stopping before the home fields. */
  uint64_t stub_sz = arts_db_cache_stub_size();
  struct arts_db_s *stub =
      (struct arts_db_s *)arts_malloc_aligned(stub_sz, ARTS_CACHE_LINE_SIZE);
  memset(stub, 0, stub_sz);
  stub->db_type = ARTS_DB;

  /* db_size==0 ⇒ stub install: buffer alloc deferred until first wire
   * arrival (install_buffer with the actual db_size).  Cache-only: this path
   * runs on a rank that is NOT the block's home (the caller gates it on the
   * owner not being this rank), so there are no home fields to initialize and
   * the stub's allocation deliberately stops before them. */
  arts_db_cache_init(&stub->cache, db_guid, /*db_size=*/db_size,
                     ARTS_DB_INIT_STUB,
                     /*creator_rank=*/0);

  if (arts_route_table_install_if_absent(stub, db_guid, arts_global_rank_id,
                                         /*used=*/true)) {
    arts_ooo_drain_guid(db_guid);
    /* Pin the just-installed db_s (one cb ref) for the caller. */
    return arts_route_table_lookup_db(db_guid);
  }

  /* Lost the race — another thread already installed.  Tear down our
   * stub and return a pinned handle to the established db_s. */
  arts_db_free(stub);
  return arts_route_table_lookup_db(db_guid);
}

/* ===== Case 1/3/5: local-buffer acquire ============================= */

void *arts_db_acquire_local(struct arts_db_cache_s *cache) {
  /* Take the EDT's strong ref on the buffer and return buf->data.  The handle
   * is intentionally NOT released here — the ref is the EDT's hold for its
   * whole lifetime; release_one_dep drops it through the block's descriptor.
   * The ref keeps the buffer alive against a concurrent destroy.
   *
   * No materialization here either: this helper is reached from every arm's
   * acquire body and cannot tell whether this rank may hold the block.  Each
   * arm calls arts_db_buf_ensure in the branch where it has already decided
   * that it may. */
  arts_shared_ptr_t h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *buf = (struct arts_db_buffer_s *)arts_shared_get(h);
  if (buf == NULL) {
    return NULL; /* h is NULL — nothing installed, nothing held */
  }
  INCREMENT_NUM_DB_ACQUIRE_LOCAL_HIT_BY(1);
  return buf->data;
}

/* Case 2/6 (RW local fast path) and Case 4/8 (remote-RW path) live in
 * coherence/grant.c — they touch the OCR-model home-directory cache fields
 * (pending_rw, grant_req_in_flight). */

/* ===== Case 7: remote-RO / remote-snapshot path =================== */

#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)
#ifdef ARTS_RO_COMBINING_LIVE
/* ===== Remote-read request combining ===============================
 *
 * One snapshot request per cache may be in flight ("the window").  The 0->1
 * CAS winner on snapshot_req_in_flight owns it: it isolates the accumulated
 * waiter stack in one exchange, sends ONE wire request naming an arbitrary
 * member (the leader — the response resumes it through the unchanged 1:1
 * path), and parks the rest as ro_combine_group.  The response terminal
 * resumes the whole group against the buffer the response made current, then
 * re-arms: isolate whatever accumulated meanwhile and send again, or release
 * the window.
 *
 * Correctness boundary: a waiter may only ride a request sent AFTER its
 * park.  Any release the waiter is event-ordered after published before its
 * park, hence before the send, hence is contained in the version the server
 * serves at receive time — so one response satisfies the whole batch.
 * Waiters that arrive while a request is in flight must NOT join it (its
 * response may predate their ordering obligations); they form the next
 * window.  Pull semantics stay intact: currency is established per-request
 * at serve time, the server tracks no readers, writers pay nothing.
 *
 * Single-rank runs bypass combining: the self-send serve chain is fully
 * synchronous, so window chaining would recurse through the inline response
 * handler — and there is no wire cost to amortize.  On multi-rank runs the
 * re-arm can inline-recurse only while a request resolves back onto this
 * rank (owner-is-self serve); that recursion is finite and small, because
 * once ownership is local, new read acquires resolve locally and stop
 * feeding the stack. */

/* Isolate the accumulated stack and launch one request.  Caller must own the
 * window.  Returns false when there was nothing to send (caller releases). */
static bool ro_combine_launch_owned(struct arts_db_cache_s *cache) {
  arts_lf_link_t *batch = arts_lf_stack_drain(&cache->ro_combine);
  if (batch == NULL) {
    return false;
  }
  struct arts_db_snapshot_waiter_s *leader =
      (struct arts_db_snapshot_waiter_s *)batch;
  cache->ro_combine_group =
      atomic_load_explicit(&batch->next, memory_order_relaxed);
  arts_guid_t leader_guid = leader->edt_guid;
  unsigned int leader_slot = leader->slot;
  arts_free(leader);
#ifdef ARTS_PROTOCOL_INV
  arts_send_db_inv_request(cache, DB_MODE_RO);
#else
  arts_send_db_snapshot_request(cache, leader_guid, leader_slot);
#endif
  return true;
}

/* Claim the window if free and launch.  Push-then-claim on the acquire side
 * plus release-then-recheck here close the missed-wakeup race: a pusher that
 * loses the claim is guaranteed its node is seen either by the owner's next
 * isolation or by this loop after the owner releases. */
static void ro_combine_pump(struct arts_db_cache_s *cache) {
  while (!arts_lf_stack_empty(&cache->ro_combine)) {
    if (arts_atomic_cswap(&cache->snapshot_req_in_flight, 0, 1) != 0) {
      /* A reader that found a window already open: one request combining
       * kept off the wire. */
      INCREMENT_NUM_RO_COMBINE_JOINED_BY(1);
      return; /* someone owns the window; their terminal rechecks */
    }
    if (ro_combine_launch_owned(cache)) {
      INCREMENT_NUM_RO_COMBINE_WINDOW_BY(1);
      return; /* window stays owned until the response terminal */
    }
    (void)arts_atomic_swap(&cache->snapshot_req_in_flight, 0);
  }
}

void arts_db_ro_combine_on_terminal(struct arts_db_cache_s *cache,
                                    bool buffer_live, uint64_t version) {
  arts_lf_link_t *node = cache->ro_combine_group;
  cache->ro_combine_group = NULL;
  while (node != NULL) {
    struct arts_db_snapshot_waiter_s *w =
        (struct arts_db_snapshot_waiter_s *)node;
    arts_lf_link_t *next =
        atomic_load_explicit(&node->next, memory_order_relaxed);
    if (buffer_live) {
      /* The buffer the response made current is at least as new as anything
       * a batch member is ordered after — resume against it (the resume
       * re-derives dep->ptr from the installed buffer). */
      arts_guid_t edt_local = w->edt_guid;
      unsigned int slot_local = w->slot;
      arts_free(w);
      mark_edt_ready_by_guid(edt_local, slot_local);
    } else {
      /* Reorder case: the publish satisfying this response is still in
       * flight.  Park the member on the reorder buffer (its need is <=
       * `version`); the coming install drains it. */
      w->target_version = version;
      w->serve = NULL;
      arts_lf_stack_push(&cache->pending_snapshot, &w->link);
    }
    node = next;
  }
  /* Re-arm: whatever accumulated during this window rides the next request;
   * otherwise release the window and recheck (a push may race the release). */
  if (ro_combine_launch_owned(cache)) {
    return;
  }
  (void)arts_atomic_swap(&cache->snapshot_req_in_flight, 0);
  ro_combine_pump(cache);
}
void arts_db_ro_combine_grant_drain(struct arts_db_cache_s *cache) {
  /* Ownership-arrival drain: called from the transfer-commit body inside the
   * install's drain guard, which holds a count on the word across this walk.
   * That is what stops ownership leaving under it, and it holds however the
   * word is encoded: the guard is one hold, every waiter this commit wakes
   * adds another, so no release reaching this word can be the one that gives
   * ownership up — the count cannot fall to the edge while the guard is
   * there.  It also means a releasing writer cannot take the whole-right
   * claim, whose precondition is the word carrying exactly one hold.
   *
   * Every waiter here parked before this drain, and the buffer this rank now
   * holds contains every release completed before the transfer (the ownership
   * chain linearizes all writers), so resuming against it is correct for any
   * park time — unlike a snapshot install, which is only a specific version.
   * That still reads correctly when the grant arrived carrying NO bytes: a
   * server sends the permission alone only when the copy already here is at
   * the canonical version, and everything completed before the transfer is at
   * or below that version by the same argument.
   *
   * The in-flight window group (ro_combine_group) is NOT touched: its
   * response terminal owns it.  Stragglers that push after this exchange are
   * picked up by their own pump (a fresh request round trip, correct via the
   * response path). */
  arts_lf_link_t *node = arts_lf_stack_drain(&cache->ro_combine);
  while (node != NULL) {
    struct arts_db_snapshot_waiter_s *w =
        (struct arts_db_snapshot_waiter_s *)node;
    arts_lf_link_t *next =
        atomic_load_explicit(&node->next, memory_order_relaxed);
    arts_guid_t edt_local = w->edt_guid;
    unsigned int slot_local = w->slot;
    arts_free(w);
    mark_edt_ready_by_guid(edt_local, slot_local);
    node = next;
  }
}
#endif /* ARTS_RO_COMBINING_LIVE */

arts_db_acquire_result_t
arts_db_acquire_remote_ro(struct arts_db_cache_s *cache, arts_guid_t edt_guid,
                          unsigned int slot) {
  /* No list registration.  Fire SNAPSHOT_REQUEST carrying edt_guid + slot and
   * PARK; the matching SNAPSHOT_RESPONSE at this rank resumes the EDT directly
   * (case 1/2), or — only under transport reorder — case 3 pushes a
   * reorder-buffer node onto pending_snapshot.  A concurrent destroy is handled
   * by the caller's lookup-miss + OoO defer. */
  INCREMENT_NUM_DB_ACQUIRE_REMOTE_BY(1);
#ifdef ARTS_RO_COMBINING_LIVE
  if (arts_global_rank_count > 1) {
    struct arts_db_snapshot_waiter_s *w =
        (struct arts_db_snapshot_waiter_s *)arts_malloc(sizeof(*w));
    w->edt_guid = edt_guid;
    w->slot = slot;
    w->target_version = 0;
    w->serve = NULL;
    arts_lf_stack_push(&cache->ro_combine, &w->link);
    ro_combine_pump(cache);
    return ARTS_DB_ACQUIRE_PARK;
  }
#endif
#ifdef ARTS_PROTOCOL_INV
  arts_send_db_inv_request(cache, DB_MODE_RO);
#else
  arts_send_db_snapshot_request(cache, edt_guid, slot);
#endif
  return ARTS_DB_ACQUIRE_PARK;
}
#endif /* arms with a pulled reader copy */

/* The 8-case acquire dispatcher arts_handler_db_acquire is protocol-specific:
 * WT and WB define it in coherence/grant.c-backed
 * each arm's own write-policy TU (single-owner GRANT_REQUEST / GRANT path,
 * differing only on the RO-has-local-data predicate).  The shared remote-RO
 * path (arts_db_acquire_remote_ro) and the local-buffer fast read
 * (arts_db_acquire_local) above are reused by both.
 */

/* Drain the snapshot reorder buffer in one atomic_exchange.  Monotonic version
 * guarantees every parked node's target_version <= the buffer version that
 * triggers the drain, so a full drain (no partial pop) is always correct
 * (plan: "install 시 전체 drain").  Called from the case-2 install path, the
 * GRANT install, the WB GRANT_RESPONSE install, and destroy fan-out. */
#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)
void arts_db_drain_pending_snapshot(struct arts_db_cache_s *cache) {
  arts_lf_link_t *node = arts_lf_stack_drain(&cache->pending_snapshot);
  while (node != NULL) {
    struct arts_db_snapshot_waiter_s *w =
        (struct arts_db_snapshot_waiter_s *)node;
    arts_lf_link_t *next =
        atomic_load_explicit(&node->next, memory_order_relaxed);
    if (w->serve != NULL) {
      /* Deferred remote serve (home parked a SNAPSHOT_REQUEST while it had no buffer
       * yet): re-issue against the now-installed buffer.  The callback must
       * not retain w past its return. */
      w->serve(cache, w);
      arts_free(w);
    } else {
      arts_guid_t edt_local = w->edt_guid;
      unsigned int slot_local = w->slot;
      arts_free(w);
      /* Wake after free: mark_edt_ready_by_guid re-derives dep->ptr from the
       * (now-installed) cache buffer and resumes the acquire walk. */
      mark_edt_ready_by_guid(edt_local, slot_local);
    }
    node = next;
  }
}
#endif /* arms with a snapshot reorder buffer */

/* ================================================================== */
/* ===== Release path =============================================== */
/* ================================================================== */

/* ===== release-reply wait (shared coherence service) ===============
 *
 * A release that must not return before the home has seen its bytes blocks
 * on a stack-local (or heap) semaphore matched by pointer identity: the
 * address rides the request and every reply posts it.  Compiled for every
 * arm; declared in coherence/coherence.h so the arm TUs can invoke it. */
void arts_db_await_ack(sem_t *cv) {
  /* Block until the arm's ACK/CTS handler posts this semaphore by pointer
   * identity.  The wait state is the caller's — a stack frame on one arm, a
   * heap object on another — so nothing here may assume either.  No busy-wait:
   * sem_timedwait sleeps the worker.  We re-arm on a coarse cadence only to
   * re-check the shutdown flag — once teardown starts the network receiver
   * stops draining and the reply never arrives, so the EDT epilogue must not
   * block forever (returning lets the worker exit). */
  for (;;) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1; /* shutdown re-check cadence, not a timeout on the ACK */
    if (sem_timedwait(cv, &ts) == 0) {
      return; /* ACK arrived (pointer-identity post) */
    }
    if (errno == ETIMEDOUT &&
        arts_atomic_read(&arts_node_info.shutdown_state) != 0) {
      return; /* teardown: ACK will never come */
    }
    /* ETIMEDOUT (not shutting down) or EINTR: re-arm the blocking wait. */
  }
}

/* arts_db_release_rw is protocol-specific (the version bump is shared, but the
 * pre-decrement buffer-ref drop and the post-decrement transfer/publish
 * decision differ per protocol), so its whole body lives in each arm's own
 * placement TU.  VAL+WT and INV under either write policy call
 * arts_db_publish_sync below for the synchronous-PUBLISH rendezvous (INV's
 * publish doubles as its invalidation-round request). */

/* ===== publish flight machine (write-combining publish) ============
 *
 * Compiled by every arm that publishes at a release.  Under WT that is the
 * payload write-through; under WB only INV publishes at all, and its
 * publish is control-only — the round request.
 *
 * At most ONE publish is in flight per (DB, rank).  Every releaser registers
 * as a version-covered waiter, then either claims the flight (CAS 0->FLYING)
 * or joins it (CAS ->|DIRTY).  A flight ships the cache buffer's CURRENT
 * bytes stamped with the buffer's current version, so the covering ACK wakes
 * every waiter at or below that version and releases that landed mid-flight
 * coalesce into at most one trailing flight — the write-side twin of the RO
 * request-combining window.  The ACK doubles as the credit teacher: it
 * carries the home's next in-place credit {stable-buffer addr, rkey, txid},
 * consumed 1:1 by the next payload flight, so the steady state is one PUT +
 * one commit + one blocked wait, announce-free. */
#if (defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)) &&             \
    (!defined(ARTS_WRITE_POLICY_WB) || defined(ARTS_PROTOCOL_INV))

#define ARTS_PUB_FLYING 1u
#define ARTS_PUB_DIRTY 2u

struct arts_db_pub_waiter_s {
  arts_lf_link_t link; /* FIRST — Treiber membership */
  uint64_t version;    /* wake once a publish >= this version is ACKed */
  sem_t sem;
};

/* Does this rank's publish carry payload?  Write-through owners ship bytes; a
 * home-resident releaser's buffer IS the canonical copy (its publish is the
 * ordering round alone), and the write-back invalidation arm publishes
 * control only. */
/* Does a payload commit leg leaving now owe the home a hand-back of the write
 * right?  Only an arm whose write right migrates can owe one. */
static inline bool pub_leg_takes_grant_return(struct arts_db_cache_s *cache) {
#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)
  return arts_db_grant_return_claim_leg(cache);
#else
  (void)cache;
  return false;
#endif
}

static bool pub_flight_carries_payload(struct arts_db_cache_s *cache) {
#if defined(ARTS_PROTOCOL_INV) && defined(ARTS_WRITE_POLICY_WB)
  (void)cache;
  return false;
#else
  return arts_guid_get_rank(cache->db_guid) != arts_global_rank_id;
#endif
}

/* Drive ONE publish flight from the cache's current buffer state.  The caller
 * holds the FLYING claim.  may_block separates the two calling contexts: a
 * claiming releaser (worker thread — may run the credit-less announce/CTS
 * leg, which blocks) vs a completing ACK (progress thread — the ACK that
 * launched it just refilled the credit, so needing to block there means the
 * refill chain is broken and fails loudly). */
static struct arts_db_pub_waiter_s *
pub_flight_drive(struct arts_db_cache_s *cache, bool may_block,
                 uint64_t fallback_version) {
  INCREMENT_NUM_PUB_FLIGHT_BY(1);
  arts_shared_ptr_t buf_h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *buf =
      (struct arts_db_buffer_s *)arts_shared_get(buf_h);
  /* The stamp is the buffer's CURRENT version (covers every registered
   * waiter); a buffer-less cache (a data-less DB, or a control-only release
   * before any payload existed) flies under the caller's version instead —
   * its waiters all registered at that same version source. */
  uint64_t version =
      (buf != NULL) ? arts_atomic_read_u64(&buf->version) : fallback_version;
  unsigned int home_rank = arts_guid_get_rank(cache->db_guid);
  /* A claiming releaser gates on ITS OWN flight, not merely on a covering
   * ACK: a claim won after the claimer's registered waiter was already
   * covered (the claim-after-covered race) would otherwise launch a flight
   * nobody waits for, and the release's 0-edge — the ownership ship — could
   * overtake the still-flying PUT, letting the next owner's publish
   * interleave with this one.  The gate node is a waiter at the flight's own
   * stamp, so exactly this flight's ACK (or an abandon) releases it.  A
   * trailing relaunch needs no gate: its uncovered waiters are blocked
   * releasers, so the 0-edge cannot fire until its ACK lands. */
  struct arts_db_pub_waiter_s *gate = NULL;
  if (may_block) {
    gate = (struct arts_db_pub_waiter_s *)arts_malloc(sizeof(*gate));
    gate->version = version;
    sem_init(&gate->sem, 0, 0);
    arts_lf_stack_push(&cache->pub_waiters, &gate->link);
  }
  if (!pub_flight_carries_payload(cache) || buf == NULL) {
    if (buf == NULL && pub_flight_carries_payload(cache)) {
      /* Unreachable: a flight only ever leaves under a hold this rank took,
       * and a hold on a sized block implies its storage. */
      ARTS_ERROR("coherence: payload publish flight with no local buffer");
    }
    arts_db_buf_release(&buf_h);
    arts_send_db_publish(home_rank, cache->db_guid, version, /*cv=*/0,
                           /*data=*/NULL, /*data_size=*/0, /*rdzv_txid=*/0,
                           /*rdzv_cookie=*/0, /*return_grant=*/false);
    return gate;
  }
  /* Payload flight: consume the credit.  The acquire exchange pairs with the
   * refill's release-store; addr/rkey are stable across refills (one DB has
   * one stable buffer), so reading them after the exchange is safe.  The
   * strong buffer ref transfers to the PUT's local completion — the source-
   * lifetime gate outlives even a shutdown-escaped waiter. */
  uint64_t txid =
      __atomic_exchange_n(&cache->home_pub_txid, 0, __ATOMIC_ACQUIRE);
  if (txid != 0) {
    arts_net_put_payload((int)home_rank,
                         __atomic_load_n(&cache->home_pub_addr,
                                         __ATOMIC_RELAXED),
                         __atomic_load_n(&cache->home_pub_rkey,
                                         __ATOMIC_RELAXED),
                         txid, buf->data, cache->db_size,
                         arts_db_buf_ref_release_cb, (void *)buf_h);
    arts_send_db_publish(home_rank, cache->db_guid, version, /*cv=*/0,
                           /*data=*/NULL, cache->db_size, txid,
                           /*rdzv_cookie=*/0,
                           pub_leg_takes_grant_return(cache));
    return gate;
  }
  if (!may_block) {
    /* No credit on a progress-thread relaunch: the refill chain breaks only
     * when the home died mid-flight (its MISS ACK carries no credit).
     * Abandon instead of aborting — the waiters wake unpublished, and the
     * destroy fan-out (or each waiter's own recheck) retires the cache. */
    arts_db_buf_release(&buf_h);
    arts_db_pub_flight_abandon(cache);
    __atomic_store_n(&cache->pub_flight, 0u, __ATOMIC_RELEASE);
    return NULL;
  }
  INCREMENT_NUM_PUB_CTS_FALLBACK_BY(1);
  /* Credit-less first flight: announce -> blocked CTS wait -> PUT -> commit.
   * The rendezvous lives on the HEAP: the shutdown escape can abandon the
   * wait while the CTS reply is still in flight, and that reply writes the
   * landing fields through the echoed cv before posting.  A heap block
   * deliberately LEAKED on the escape keeps that late write inside live
   * memory (a bounded, teardown-only leak); a popped stack frame would be
   * corrupted. */
  struct arts_db_pub_rendezvous_s *wr =
      (struct arts_db_pub_rendezvous_s *)arts_malloc(sizeof(*wr));
  sem_init(&wr->sem, 0, 0);
  wr->landing = (struct arts_rdzv_landing_s){0, 0, 0, 0};
  arts_send_db_publish(home_rank, cache->db_guid, version,
                         (uint64_t)(uintptr_t)wr, /*data=*/NULL,
                         cache->db_size, /*rdzv_txid=*/0, /*rdzv_cookie=*/0,
                         /*return_grant=*/false);
  arts_db_await_ack(&wr->sem); /* CTS wake — or the shutdown escape */
  if (wr->landing.txid == 0) {
    /* Shutdown escape before the CTS landed: the flight is abandoned with
     * the runtime (a live run's CTS wake always carries a landing).
     *
     * This read of wr->landing.txid is UNSYNCHRONIZED on the escape path —
     * sem_timedwait returned via the shutdown timeout, not a real post, so
     * there is no happens-before edge against a CTS reply racing in.  That
     * is precisely why wr must be leaked rather than freed: freeing it and
     * letting the racing write land afterward would be a use-after-free.
     * The buffer ref is ours to drop — nothing read the bytes. */
    arts_db_buf_release(&buf_h);
    return gate;
  }
  arts_net_put_payload((int)home_rank, wr->landing.addr, wr->landing.key,
                       wr->landing.txid, buf->data, cache->db_size,
                       arts_db_buf_ref_release_cb, (void *)buf_h);
  arts_send_db_publish(home_rank, cache->db_guid, version, /*cv=*/0,
                         /*data=*/NULL, cache->db_size, wr->landing.txid,
                         wr->landing.cookie,
                         pub_leg_takes_grant_return(cache));
  /* The commit carries cv 0 — the final ACK completes the FLIGHT, not this
   * rendezvous.  The CTS was the only writer through wr, so it dies here. */
  sem_destroy(&wr->sem);
  arts_free(wr);
  return gate;
}

/* Abandon the cache's publish flight: wake EVERY parked waiter without a
 * covering publish.  Called on the destroy paths (fan-out receiver and home
 * destroy body, AFTER the route slot is withdrawn) and as a teardown
 * backstop: the ACK completion is cache-KEYED, so once the route slot is
 * NULL a still-flying ACK MISSes and can never reach this stack again — and
 * a parked waiter holds a buffer ref that keeps the descriptor (and this
 * stack) alive, so waiting for the destructor would deadlock.  Racing an ACK
 * drain is safe: the Treiber drain hands each node to exactly one drainer.
 * Nodes are freed by their woken owners. */
void arts_db_pub_flight_abandon(struct arts_db_cache_s *cache) {
  INCREMENT_NUM_PUB_FLIGHT_ABANDON_BY(1);
#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)
  /* No further leg will leave this cache, so a hand-back still riding on one
   * would never arrive.  Convert whatever is armed to its own message. */
  arts_db_grant_return_flight_abandoned(cache);
#endif
  arts_lf_link_t *n =
      (arts_lf_link_t *)__atomic_exchange_n(&cache->pub_parked, NULL,
                                            __ATOMIC_ACQ_REL);
  arts_lf_link_t *fresh = arts_lf_stack_drain(&cache->pub_waiters);
  if (n == NULL) {
    n = fresh;
  } else if (fresh != NULL) {
    arts_lf_link_t *tail = n;
    for (arts_lf_link_t *t; (t = (arts_lf_link_t *)atomic_load_explicit(
                                 &tail->next, memory_order_relaxed)) != NULL;
         tail = t) {
    }
    atomic_store_explicit(&tail->next, fresh, memory_order_relaxed);
  }
  while (n != NULL) {
    arts_lf_link_t *next = atomic_load_explicit(&n->next, memory_order_relaxed);
    sem_post(&((struct arts_db_pub_waiter_s *)n)->sem);
    n = next;
  }
}

void arts_db_publish_sync(struct arts_db_cache_s *cache, uint64_t version) {
  /* The waiter node lives on the HEAP for the same reason the CTS rendezvous
   * does: a shutdown-escaped waiter leaks its node, and a late completion
   * drain may still post into it — posting into leaked live memory is safe,
   * freeing under the poster is not. */
  struct arts_db_pub_waiter_s *w =
      (struct arts_db_pub_waiter_s *)arts_malloc(sizeof(*w));
  w->version = version;
  sem_init(&w->sem, 0, 0);
  arts_lf_stack_push(&cache->pub_waiters, &w->link);
  arts_sched_fuzz_point(); /* widen the push<->flight-claim window */
  struct arts_db_pub_waiter_s *gate = NULL;
  for (;;) {
    unsigned int f = __atomic_load_n(&cache->pub_flight, __ATOMIC_RELAXED);
    if (f == 0u) {
      unsigned int expected = 0u;
      if (__atomic_compare_exchange_n(&cache->pub_flight, &expected,
                                      ARTS_PUB_FLYING, false, __ATOMIC_ACQ_REL,
                                      __ATOMIC_RELAXED)) {
        gate = pub_flight_drive(cache, /*may_block=*/true, version);
        break;
      }
    } else if (__atomic_compare_exchange_n(&cache->pub_flight, &f,
                                           f | ARTS_PUB_DIRTY, false,
                                           __ATOMIC_ACQ_REL,
                                           __ATOMIC_RELAXED)) {
      INCREMENT_NUM_PUB_FLIGHT_JOIN_BY(1);
      break;
    }
  }
  /* Push-then-recheck against a concurrent destroy: once the route slot is
   * withdrawn the cache-keyed ACK completion can no longer find this cache,
   * so a waiter registered around that instant must self-abandon. */
  {
    arts_shared_ptr_t dh = arts_route_table_lookup_db(cache->db_guid);
    bool destroyed = (arts_shared_get(dh) == NULL);
    arts_shared_release(&dh);
    if (destroyed) {
      arts_db_pub_flight_abandon(cache);
    }
  }
  arts_db_await_ack(&w->sem);
  if (arts_atomic_read(&arts_node_info.shutdown_state) != 0) {
    return; /* possible shutdown escape: a late drain may still post — leak
             * w (and the gate, which stays parked) */
  }
  sem_destroy(&w->sem);
  arts_free(w);
  if (gate != NULL) {
    arts_db_await_ack(&gate->sem);
    if (arts_atomic_read(&arts_node_info.shutdown_state) != 0) {
      return; /* shutdown escape — leak the gate */
    }
    sem_destroy(&gate->sem);
    arts_free(gate);
  }
}

/* Cat-C pure body (PUBLISH_ACK), shared by every publishing arm.  cv != 0 is
 * the pointer-identity sem-post plane (the CTS-leg announce; cache-
 * independent, posted on both HIT and MISS).  cv == 0 is flight completion:
 * record the refilled credit, wake every waiter the ACKed version covers,
 * and either relaunch (uncovered waiters remain — their releases landed
 * after the flight's version stamp) or land the flight.  A DIRTY bit set
 * after the drain forces a re-examination before landing, so a waiter
 * registered mid-completion is never stranded. */
void arts_handler_db_publish_ack(void *item_v, void *args_v) {
  struct arts_db_publish_ack_args_s *a =
      (struct arts_db_publish_ack_args_s *)args_v;
  if (a->cv != 0) {
    sem_post((sem_t *)(uintptr_t)a->cv);
    return;
  }
  struct arts_db_s *db = (struct arts_db_s *)item_v;
  if (db == NULL) {
    /* Destroyed while a flight was outstanding: the program failed to order
     * the destroy after its releases (contract violation).  The waiters
     * unblock through the shutdown escape. */
    return;
  }
  struct arts_db_cache_s *cache = &db->cache;
  if (a->credit_txid != 0) {
    /* Relaxed stores: two teachers (ACK refill, CREATE_RETURN) may overlap,
     * but every teacher writes the same {addr, rkey} — one DB has ONE stable
     * buffer — so the only ordering that matters is the txid release-store
     * publishing the triple to the consuming exchange. */
    __atomic_store_n(&cache->home_pub_addr, a->credit_addr, __ATOMIC_RELAXED);
    __atomic_store_n(&cache->home_pub_rkey, a->credit_rkey, __ATOMIC_RELAXED);
    __atomic_store_n(&cache->home_pub_txid, a->credit_txid, __ATOMIC_RELEASE);
  }
  for (;;) {
    /* Waiters parked by the previous completion first, then everything that
     * queued since.  Parked nodes are re-examined only here — under the
     * FLYING claim exactly one completion runs at a time, so the parked
     * field needs no synchronization. */
    arts_lf_link_t *chain =
        (arts_lf_link_t *)__atomic_exchange_n(&cache->pub_parked, NULL,
                                              __ATOMIC_ACQ_REL);
    arts_lf_link_t *fresh = arts_lf_stack_drain(&cache->pub_waiters);
    if (chain == NULL) {
      chain = fresh;
    } else if (fresh != NULL) {
      arts_lf_link_t *tail = chain;
      for (arts_lf_link_t *n; (n = (arts_lf_link_t *)atomic_load_explicit(
                                   &tail->next, memory_order_relaxed)) != NULL;
           tail = n) {
      }
      atomic_store_explicit(&tail->next, fresh, memory_order_relaxed);
    }
    arts_lf_link_t *uncovered = NULL;
    while (chain != NULL) {
      arts_lf_link_t *next =
          (arts_lf_link_t *)atomic_load_explicit(&chain->next,
                                                 memory_order_relaxed);
      struct arts_db_pub_waiter_s *w = (struct arts_db_pub_waiter_s *)chain;
      if (w->version <= a->version) {
        sem_post(&w->sem);
      } else {
        atomic_store_explicit(&chain->next, uncovered, memory_order_relaxed);
        uncovered = chain;
      }
      chain = next;
    }
    if (uncovered != NULL) {
      /* Releases landed after the flight's stamp: keep the claim (dropping
       * any DIRTY — this drain subsumes it) and fly again; the buffer's
       * current version now covers every parked waiter. */
      __atomic_store_n(&cache->pub_parked, (void *)uncovered,
                       __ATOMIC_RELEASE);
      __atomic_store_n(&cache->pub_flight, ARTS_PUB_FLYING, __ATOMIC_RELEASE);
      INCREMENT_NUM_PUB_FLIGHT_TRAILING_BY(1);
      (void)pub_flight_drive(cache, /*may_block=*/false, a->version);
      /* Park-then-recheck against a concurrent destroy: once the route slot
       * is withdrawn no future ACK can find this cache, so nodes parked
       * around that instant must be self-abandoned (the drain hands each
       * node to exactly one drainer). */
      {
        arts_shared_ptr_t dh = arts_route_table_lookup_db(cache->db_guid);
        bool destroyed = (arts_shared_get(dh) == NULL);
        arts_shared_release(&dh);
        if (destroyed) {
          arts_db_pub_flight_abandon(cache);
        }
      }
      return;
    }
    unsigned int expected = ARTS_PUB_FLYING;
    if (__atomic_compare_exchange_n(&cache->pub_flight, &expected, 0u, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      return; /* flight landed */
    }
    /* DIRTY raced in after the drain: consume it and re-examine.  The word
     * stays in {FLYING, FLYING|DIRTY} for the whole completion (only a
     * completion clears FLYING, and only one runs), so a plain store is
     * enough — a DIRTY overwritten here is re-covered by the loop's next
     * drain. */
    __atomic_store_n(&cache->pub_flight, ARTS_PUB_FLYING, __ATOMIC_RELEASE);
  }
}
#endif /* publishing arms */

#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)
void arts_db_release_ro(struct arts_db_cache_s *cache) {
  /* RO release is a no-op for VAL: the EDT's buf ref is dropped
   * by release_one_dep's DIST branch via release_buf (matching the
   * acquire_buf in mark_edt_ready_by_guid / acquire_local).
   * EXCL defines its own arts_db_release_ro in coherence/excl/purge.c and
   * coherence/excl/retain.c. */
  (void)cache;
}
#endif /* arms whose RO release drops nothing but the EDT's own ref */

/* ================================================================== */
/* ===== Destroy lifecycle ========================================== */
/* ================================================================== */

/* ===== arts_db_destroy_remote public API ============================= */

/* Public destroy: forward DESTROY_REQ to home (uniform path; home ==
 * self gets the message via self-loop).  Caller is responsible for
 * the OCR-spec contract: no concurrent acquires/uses in flight. */
void arts_db_destroy_remote(arts_guid_t db_guid) {
  unsigned int home_rank = arts_guid_get_rank(db_guid);
  arts_send_db_destroy(home_rank, db_guid);
}

/* ===== cache_s destructor (chained from arts_db_free) =============
 *
 * The full destructor arts_db_cache_destructor is protocol-specific — it
 * sequences the protocol's own field teardown between these two shared steps —
 * and lives with each arm's placement TU.  The agnostic steps split into pre
 * (the buffer-NULL that must run first) and post (snapshot drain + home
 * teardown), so the per-arm wrapper runs pre, its own teardown, then post.
 * cache_s itself is
 * freed by the route_table after the wrapper returns; buffers (payload storage) are
 * recycled / freed by the cb deleter chain once outstanding refs drain. */

#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV) ||               \
    defined(ARTS_PROTOCOL_EXCL)
/* Step 1: release the cache-hold on the buffer (store NULL into the shared
 * slot).  If no acquirer holds a ref the cb deleter frees the buffer now;
 * otherwise it survives until the last in-flight acquirer releases.  Runs
 * FIRST so it covers the rare race where a wire handler installed a buffer past
 * try_finalize_destroy's NULL-swap. */
void arts_db_cache_common_destroy_pre(struct arts_db_cache_s *cache) {
  if (cache == NULL) {
    return;
  }
  arts_atomic_shared_store(&cache->buffer, NULL);
  /* The slot is empty again, so the "has this block storage" byte goes back
   * with it: the two are one fact, and leaving a stale 0 would tell a use of
   * a resurrected cache that its payload is already there. */
  __atomic_store_n(&cache->payload_pending, (uint8_t)1, __ATOMIC_RELEASE);
  /* Drain the per-DB recycled-buffer pool, returning leftovers to the
   * registered pool.  The buffer slot is already NULL'd above, and B1 keeps
   * the descriptor (hence this pool) alive until the last buffer ref drops,
   * so no late deleter can push in after this point; the cache is torn down
   * single-threaded here.  Every node was allocated by arts_db_buf_alloc via
   * arts_regpool_alloc_aligned (pool_link is at offset 0, so the node ptr is
   * the buffer base), so the matching free is arts_regpool_free, not the
   * pool's default arts_free. */
  arts_lf_pool_destroy_with(&cache->buf_freelist, arts_regpool_free);
}
#endif /* arms sharing the common cache shape */

/* Steps 3b+4: drain+free the snapshot reorder buffer (a Treiber stack), then
 * tear down the inlined home-directory sub-resources.  Runs AFTER the protocol
 * field-destroy (pending_rw in HOME and OWNER builds). */
/* Debug-only terminal-quiescence check.  Runs once, after every runtime
 * thread has joined and before teardown frees the caches: at that point
 * every coherence wait-structure must be empty — a survivor is a lost wake
 * that the run's own success criteria may have masked (a reader that never
 * ran, a release that never completed).  Violations print a QUIESCENCE-DEBUG
 * marker; the stress suites turn that marker into a test failure.  Signal-
 * driven shutdowns legitimately strand waiters mid-flight, which is why
 * this reports rather than aborts.
 *
 * The walk exists only to produce diagnostic messages, so it is gated on
 * the log level that compiles those messages: below DEBUG the whole check
 * is compiled out — its cost (a full route-table scan at teardown) never
 * lands in a measurement build. */
void arts_db_debug_quiescence_check(void) {
#if ARTS_LOG_LEVEL >= 3
  extern uint64_t num_tables;
  unsigned int viol = 0;
  arts_route_table_t *tables[ARTS_REMOTE_ROUTE_SHARDS + 64];
  unsigned int nt = 0;
  for (uint64_t i = 0; i < num_tables && nt < 64; i++) {
    tables[nt++] = arts_node_info.route_table[i];
  }
  for (unsigned int i = 0; i < ARTS_REMOTE_ROUTE_SHARDS; i++) {
    tables[nt++] = arts_node_info.remote_route_table[i];
  }
#ifdef ARTS_FAM
  /* Whether this shutdown was quiescent, named by contributor so a log's
   * reader knows why a residue went unreported.  Every one is pending work
   * the shutdown left undone: runnable EDTs dropped from a deque, runtime
   * jobs discarded, self-sends never dispatched, and EDTs admitted (every
   * dependence satisfied, acquisition begun) that never finished -- the last
   * covers an EDT parked mid acquisition, which sits in no queue a discard
   * site sees, and it overlaps the first.  Each is read after every runtime
   * thread has joined, so no poster can add to what is counted, and before
   * anything frees it.  A message from another rank still undelivered is
   * not one: a release in flight to a home strands nothing on a cache word,
   * and a grant still owed to a requester leaves that requester's EDT
   * admitted and unfinished. */
  unsigned int ab_queued = __atomic_load_n(&arts_shutdown_abandon.queued_edts,
                                           __ATOMIC_RELAXED);
  unsigned int ab_jobs =
      __atomic_load_n(&arts_shutdown_abandon.jobs, __ATOMIC_RELAXED);
  unsigned int ab_loopback = arts_loopback_pending_count();
  unsigned int ab_admitted = 0;
  for (unsigned int t = 0; t < nt; t++) {
    if (tables[t] == NULL) {
      continue;
    }
    arts_route_table_iterator_t iter;
    arts_reset_route_table_iterator(&iter, tables[t]);
    for (arts_route_item_t *item = arts_route_table_iterate(&iter);
         item != NULL; item = arts_route_table_iterate(&iter)) {
      if (ARTS_GUID_GET_TYPE(item->key) != ARTS_GUID_EDT) {
        continue;
      }
      arts_shared_ptr_t h = arts_atomic_shared_load(&item->value);
      /* Only an object the runtime owns as an EDT is read as one. */
      const struct arts_edt_s *e =
          (h && arts_shared_deleter(h) == arts_edt_get_deleter())
              ? (const struct arts_edt_s *)arts_shared_get(h)
              : NULL;
      if (e != NULL && e->depc_needed == 0u) {
        ab_admitted++;
      }
      if (h) {
        arts_shared_release(&h);
      }
    }
  }
  bool quiescent = (ab_queued | ab_jobs | ab_loopback | ab_admitted) == 0u;
#endif
  for (unsigned int t = 0; t < nt; t++) {
    if (tables[t] == NULL) {
      continue;
    }
    arts_route_table_iterator_t iter;
    arts_reset_route_table_iterator(&iter, tables[t]);
    for (arts_route_item_t *item = arts_route_table_iterate(&iter);
         item != NULL; item = arts_route_table_iterate(&iter)) {
      if (ARTS_GUID_GET_TYPE(item->key) != ARTS_GUID_DB) {
        continue;
      }
      arts_shared_ptr_t h = arts_atomic_shared_load(&item->value);
      struct arts_db_s *db = (struct arts_db_s *)(h ? arts_shared_get(h)
                                                    : NULL);
      if (db != NULL && db->db_type == ARTS_DB) {
#ifdef ARTS_PROTOCOL_FLUSH
        /* FLUSH keeps no wait structure on a descriptor — a blocked release
         * owns a heap semaphore that the home's ACK posts — so a walk has
         * nothing to report. */
#else
        struct arts_db_cache_s *c = &db->cache;
#if (defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)) &&             \
    (!defined(ARTS_WRITE_POLICY_WB) || defined(ARTS_PROTOCOL_INV))
        if (!arts_lf_stack_empty(&c->pub_waiters) ||
            __atomic_load_n(&c->pub_parked, __ATOMIC_ACQUIRE) != NULL) {
          ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu has parked publish "
                     "waiters at teardown",
                     (unsigned long)c->db_guid);
          viol++;
        }
#endif
#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)
        if (!arts_lf_stack_empty(&c->pending_rw)) {
          ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu has parked RW waiters at "
                     "teardown",
                     (unsigned long)c->db_guid);
          viol++;
        }
        if (db->home_initialized) {
          bool baton = atomic_load_explicit(&db->invalidate_in_flight,
                                            memory_order_acquire) != 0;
          bool queued = !arts_home_grantreq_queue_empty(&db->pending_rw);
          if (baton || queued) {
            ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu home directory not "
                       "quiescent (baton=%d queued=%d)",
                       (unsigned long)c->db_guid, baton ? 1 : 0,
                       queued ? 1 : 0);
            viol++;
          }
          /* Directory and word must agree.  Naming this rank as the holder
           * while the word says it possesses nothing is the silent shape of a
           * lost hand-over: every later requester queues behind a server that
           * has nothing to serve.  Home fields only — a cache-only stub does
           * not carry them. */
          unsigned int hw = arts_atomic_read(&c->writer_count);
          if (atomic_load_explicit(&db->rw_holder, memory_order_acquire) ==
                  arts_global_rank_id &&
              !ARTS_GRANT_OWN_OF(hw)) {
            ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu home is named the holder "
                       "but its word holds nothing (word=%u)",
                       (unsigned long)c->db_guid, hw);
            viol++;
          }
#ifdef ARTS_RELEASE_PURGE
          /* Where the right comes back unasked, quiescence means it came
           * back: a directory still naming a remote holder is one that never
           * returned, and nothing will ever ask it to. */
          unsigned int holder =
              atomic_load_explicit(&db->rw_holder, memory_order_acquire);
          if (holder != arts_global_rank_id) {
            ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu rests with rank %u holding "
                       "the write right, which is never asked to give it back",
                       (unsigned long)c->db_guid, holder);
            viol++;
          }
#endif
        }
        /* Every hold has a named releaser, so nothing may rest holding one;
         * an underflowed count is what a release with no matching acquire
         * leaves behind. */
        {
          unsigned int w = arts_atomic_read(&c->writer_count);
          if (ARTS_GRANT_COUNT_OF(w) != 0u) {
            ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu rests with %u unreleased "
                       "hold(s) (word=%u)",
                       (unsigned long)c->db_guid, ARTS_GRANT_COUNT_OF(w), w);
            viol++;
          }
        }
#ifdef ARTS_RELEASE_PURGE
        if (arts_atomic_read_u64(&c->pending_grant_return) != 0u) {
          ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu rests owing a return of the "
                     "write right",
                     (unsigned long)c->db_guid);
          viol++;
        }
#endif
#endif
#if defined(ARTS_PROTOCOL_EXCL) && defined(ARTS_RELEASE_RETAIN)
        /* A read grant marked to return owes exactly one return, payable at
         * the read count's zero edge.  Resting at that edge still marked is
         * the signature of a promise made after the edge it named had already
         * passed: the count covers waiters as well as holders, so nothing is
         * left to reach the edge again, and the directory waits on a return
         * no one will send. */
        {
          uint64_t cw = atomic_load_explicit(&c->cache_state,
                                             memory_order_acquire);
          if (CACHE_RO_ST(cw) == CACHE_ST_GRANT_PURGE &&
              CACHE_RO_CNT(cw) == 0u) {
            ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu rests owing a read-grant "
                       "return with no reader to pay it (word=%llx)",
                       (unsigned long)c->db_guid, (unsigned long long)cw);
            viol++;
          }
        }
#endif
#ifdef ARTS_FAM
        /* The arm's own quiescence, beside the RETAIN block above: what a
         * data block whose storage lives in fabric-attached memory looks
         * like once every runtime thread has joined.  Cache fields need no
         * guard; a home field is read only where db->home_initialized says a
         * home directory exists, exactly like the VAL/INV block above — a
         * cache-only stub ends at lock_state, and reading past it on a
         * non-home rank would be a heap-buffer overflow on every cached
         * block of every non-home rank. */
        {
          uint64_t cw =
              atomic_load_explicit(&c->cache_state, memory_order_acquire);
          uint8_t pending =
              __atomic_load_n(&c->payload_pending, __ATOMIC_ACQUIRE);
          uint64_t addr = arts_db_fam_slot_addr(c);
          /* 1. No slot missing where a working copy exists.  A sentinel-
           * sized block has no slot on either residency and legitimately
           * rests at address 0. */
          if (c->db_size != 0 && pending == 0u && addr == 0) {
            ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu has a working copy but no "
                       "slot for it (size=%llu)",
                       (unsigned long)c->db_guid,
                       (unsigned long long)c->db_size);
            viol++;
          }
          /* 2. Each axis rests only where the single-CAS acquire / claim /
           * release discipline can leave it.  That discipline makes two
           * shapes unreachable on its own: an axis IDLE with its own count
           * still nonzero (going idle happens only at that count's own zero
           * edge), and an axis in GRANT, or, where the arm stages bytes
           * through a claim, FETCH, with nothing counted on EITHER axis --
           * the zero edge that ends a turn returns its grant in the same
           * CAS, so this can only be a lost zero edge.  Both are reported
           * unconditionally.  REQUEST with nothing counted is exempt: it is
           * a request whose requesters all left before its grant came, a
           * legal transient that a late or undispatched grant leaves at
           * teardown.  A non-idle axis WITH a count still on
           * it is a THIRD shape: after every thread has joined nothing can
           * wait on it, and a shutdown that left work undone is the one legal
           * source of it, so it is reported only when the shutdown was
           * quiescent. */
          {
            uint32_t teardown = 0u;
            if (db->home_initialized) {
              teardown = EXCL_STATE_TEARDOWN(atomic_load_explicit(
                  &db->lock_state, memory_order_acquire));
            }
            bool zero_zero =
                (CACHE_RW_CNT(cw) == 0u && CACHE_RO_CNT(cw) == 0u);
            if (CACHE_RW_ST(cw) == CACHE_ST_IDLE) {
              if (CACHE_RW_CNT(cw) != 0u) {
                ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu rw axis rests idle "
                           "with a nonzero count (word=%llx teardown=%u)",
                           (unsigned long)c->db_guid, (unsigned long long)cw,
                           teardown);
                viol++;
              }
            } else if (zero_zero) {
              if (CACHE_RW_ST(cw) != CACHE_ST_REQ) {
                ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu rw axis rests "
                           "non-idle with nothing counted on it (word=%llx "
                           "teardown=%u)",
                           (unsigned long)c->db_guid, (unsigned long long)cw,
                           teardown);
                viol++;
              }
            } else if (quiescent) {
              ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu rw axis rests non-idle "
                         "with a count still on it after a quiescent "
                         "shutdown (word=%llx teardown=%u)",
                         (unsigned long)c->db_guid, (unsigned long long)cw,
                         teardown);
              viol++;
            }
            if (CACHE_RO_ST(cw) == CACHE_ST_IDLE) {
              if (CACHE_RO_CNT(cw) != 0u) {
                ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu ro axis rests idle "
                           "with a nonzero count (word=%llx teardown=%u)",
                           (unsigned long)c->db_guid, (unsigned long long)cw,
                           teardown);
                viol++;
              }
            } else if (zero_zero) {
              if (CACHE_RO_ST(cw) != CACHE_ST_REQ) {
                ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu ro axis rests "
                           "non-idle with nothing counted on it (word=%llx "
                           "teardown=%u)",
                           (unsigned long)c->db_guid, (unsigned long long)cw,
                           teardown);
                viol++;
              }
            } else if (quiescent) {
              ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu ro axis rests non-idle "
                         "with a count still on it after a quiescent "
                         "shutdown (word=%llx teardown=%u)",
                         (unsigned long)c->db_guid, (unsigned long long)cw,
                         teardown);
              viol++;
            }
          }
#ifdef ARTS_FAM_DIRECT
          /* 4. Nothing stands between the store and an EDT: where a holder's
           * working bytes ARE the block's store, a descriptor naming anything
           * else is storage some site materialized instead of adopting the
           * slot.  A home a destroy has marked is left out: a destroy that
           * found holders leaves the block undefined. */
          if (!(db->home_initialized &&
                EXCL_STATE_TEARDOWN(atomic_load_explicit(
                    &db->lock_state, memory_order_acquire)) != 0u)) {
            arts_shared_ptr_t bh = arts_db_buf_acquire(c);
            const struct arts_db_buffer_s *b =
                (const struct arts_db_buffer_s *)(bh ? arts_shared_get(bh)
                                                     : NULL);
            if (b != NULL && !arts_fam_contains(b->data)) {
              ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu rests with a payload "
                         "that is not its slot",
                         (unsigned long)c->db_guid);
              viol++;
            }
            if (bh) {
              arts_db_buf_release(&bh);
            }
          }
#endif
#ifdef ARTS_FAM_BACKEND_SHM
          /* Strict mode's hold registry: a count still nonzero for a line
           * of this block's slot at teardown is a hold whose matching
           * unhold never ran.  Off (and this whole check inert) whenever
           * the run is not under the second coherency domain. */
          if (arts_fam_strict() && addr != 0 && c->db_size != 0 &&
              arts_fam_strict_range_held((const void *)(uintptr_t)addr,
                                         (size_t)c->db_size)) {
            ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu has a strict-mode hold "
                       "outstanding on its slot at teardown",
                       (unsigned long)c->db_guid);
            viol++;
          }
#endif
          /* 5. No waiter is stranded at the home.  A count left on the word
           * with nobody queued behind it is a hold whose fire-and-forget
           * release was still in flight when the network stopped -- a program
           * may shut down while holding -- and nothing can ever wait on it;
           * a count WITH a waiter queued is a release that will never come
           * for someone who asked. */
          if (db->home_initialized) {
            uint64_t ls =
                atomic_load_explicit(&db->lock_state, memory_order_acquire);
            bool rwq = !arts_home_grantreq_queue_empty(&db->rw_waiters);
            bool roq = !arts_lf_stack_empty(&db->ro_waiters);
            if ((EXCL_STATE_W(ls) != 0u || EXCL_STATE_R(ls) != 0u) &&
                (rwq || roq)) {
              ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu home lock_state not "
                         "idle at teardown with a waiter queued "
                         "(w=%u r=%u teardown=%u)",
                         (unsigned long)c->db_guid, EXCL_STATE_W(ls),
                         EXCL_STATE_R(ls), EXCL_STATE_TEARDOWN(ls));
              viol++;
            }
            if (rwq) {
              ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu has queued RW waiters "
                         "at teardown (w=%u r=%u teardown=%u)",
                         (unsigned long)c->db_guid, EXCL_STATE_W(ls),
                         EXCL_STATE_R(ls), EXCL_STATE_TEARDOWN(ls));
              viol++;
            }
            if (roq) {
              ARTS_DEBUG("QUIESCENCE-DEBUG: guid %lu has queued RO waiters "
                         "at teardown (w=%u r=%u teardown=%u)",
                         (unsigned long)c->db_guid, EXCL_STATE_W(ls),
                         EXCL_STATE_R(ls), EXCL_STATE_TEARDOWN(ls));
              viol++;
            }
          }
        }
#endif /* ARTS_FAM */
#endif /* ARTS_PROTOCOL_FLUSH */
      }
      if (h) {
        arts_shared_release(&h);
      }
    }
  }
#ifdef ARTS_FAM
  /* Every contributor is printed, so "nothing was left undone" and "work was
   * left undone, and it is why a residue went unreported" read apart -- the
   * two look identical from the violation count alone. */
  if (viol != 0 || !quiescent) {
    ARTS_DEBUG("QUIESCENCE-DEBUG: %u violation(s) (shutdown left undone: "
               "queued_edts=%u jobs=%u loopback=%u admitted_edts=%u)",
               viol, ab_queued, ab_jobs, ab_loopback, ab_admitted);
  }
#else
  if (viol != 0) {
    ARTS_DEBUG("QUIESCENCE-DEBUG: %u violation(s)", viol);
  }
#endif
#endif /* ARTS_LOG_LEVEL >= 3 */
}

#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV) ||               \
    defined(ARTS_PROTOCOL_EXCL)
void arts_db_cache_common_destroy_post(struct arts_db_cache_s *cache) {
  if (cache == NULL) {
    return;
  }
#if (defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)) &&             \
    (!defined(ARTS_WRITE_POLICY_WB) || defined(ARTS_PROTOCOL_INV))
  /* Teardown backstop; the destroy handlers already abandoned the flight. */
  arts_db_pub_flight_abandon(cache);
#endif
  {
    arts_lf_link_t *n = arts_lf_stack_drain(&cache->pending_snapshot);
    while (n != NULL) {
      arts_lf_link_t *next =
          atomic_load_explicit(&n->next, memory_order_relaxed);
      arts_free(n);
      n = next;
    }
  }
#ifdef ARTS_RO_COMBINING_LIVE
  {
    /* Combining waiters still parked at destroy are freed, not woken —
     * destroying a DB with a pending acquire is undefined per the programming
     * model, same contract as the reorder-buffer drain above. */
    arts_lf_link_t *n = arts_lf_stack_drain(&cache->ro_combine);
    while (n != NULL) {
      arts_lf_link_t *next =
          atomic_load_explicit(&n->next, memory_order_relaxed);
      arts_free(n);
      n = next;
    }
    n = cache->ro_combine_group;
    cache->ro_combine_group = NULL;
    while (n != NULL) {
      arts_lf_link_t *next =
          atomic_load_explicit(&n->next, memory_order_relaxed);
      arts_free(n);
      n = next;
    }
  }
#endif
  {
    struct arts_db_s *db_self = arts_db_of_cache(cache);
    if (db_self->home_initialized) {
      arts_db_home_teardown(db_self);
      db_self->home_initialized = false;
    }
  }
}
#endif /* arms sharing the common cache shape */
