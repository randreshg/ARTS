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

#include "arts/db.h"

#include <assert.h>
#include <string.h>

#include "arts.h"
#include "arts/cxl/wrapper.h"
#ifdef ARTS_USE_CXL
#include "arts/cxl/deque.h"
#endif
#include "arts/coherence/buffer.h"
#include "arts/coherence/coherence.h"
#include "arts/coherence/handlers.h"
#include "arts/counter/Preamble.h"
#include "arts/edt.h"
#include "arts/edt_context.h" /* current_edt + created-DB tracking */
#include "arts/fam/pool.h" /* arts_fam_strict_hold (inert without a pool) */
#include "arts/gas/guid.h"
#include "arts/gas/route_table.h"
#include "arts/ooo.h"
#include "arts/runtime_state.h"
#include "arts/runtime_types.h"
#include "arts/system/print.h"
#include "arts/system/schedfuzz.h"
#include "arts/system/threads.h"
#include "arts/transport/protocol.h"
#include "arts/utils/atomics.h"
#include "arts/utils/malloc.h"
#include "arts/utils/shared.h" /* arts_shared_ptr_t, get/release */

#ifdef ARTS_USE_GPU
#include "arts/gpu/gpu_internal.h"
#endif

/* No-hint DB home policy: CREATOR (default) keeps the home on the creating
 * rank — first-touch, so a block inherits whatever distribution the placement
 * of its creating task achieved, and create/destroy directory traffic stays
 * local.  ROUNDROBIN distributes homes across all ranks regardless of the
 * creation site.  An explicit hint rank or a pre-reserved GUID always wins.
 * CMake sets this for every libarts compile; the fallback covers any TU that
 * pulls in db.c outside the normal build (e.g. direct inclusion). */
#ifndef ARTS_NOHINT_DB_ROUNDROBIN
#define ARTS_NOHINT_DB_ROUNDROBIN 0
#endif

ARTS_TYPE_NAME;
ARTS_DB_TYPE_NAME;
DB_MODE_NAME;

/*
 * arts_db_user_ptr — the user-visible payload pointer for a DB.
 *
 * A coherent DB's payload is its installed coherence buffer; every other
 * subtype's payload is inline after the struct.  NULL when `db` is NULL, or
 * when a coherent DB has no buffer installed yet — which a caller must read
 * as "no payload here", not as an error.
 */
void *arts_db_user_ptr(struct arts_db_s *db) {
  if (db == NULL) {
    return NULL;
  }
  if (db->db_type == ARTS_DB) {
    /* Deliberately NOT a first-use point, although asking for the pointer
     * looks like one: a create that takes no hold is handed no pointer at
     * all, so no caller here can be a no-hold block's first user — and
     * "this is the home" is not the right to materialize a block on either
     * arm whose payload lives with its owner.  The arms' acquire paths are
     * where that right is established, and they are the only points that
     * materialize. */
    /* Single-owner context: acquire a ref, read the payload pointer, release.
     * buf->data is the buffer's FAM and stays valid for the single owner that
     * consumes the returned pointer. */
    arts_shared_ptr_t buf_h = arts_db_buf_acquire(&db->cache);
    struct arts_db_buffer_s *buf =
        (struct arts_db_buffer_s *)arts_shared_get(buf_h);
    void *ptr = buf ? (void *)buf->data : NULL;
    arts_db_buf_release(&buf_h);
    return ptr;
  }
  return (void *)(db + 1);
}

/*
 * arts_db_auto_acquire — Track the creator's hold on a DB.
 *
 * For ARTS_DB: the coherence cache_s was seeded with possession plus the
 * creator's own hold via ARTS_DB_INIT_CREATOR_HOME or
 * ARTS_DB_INIT_CREATOR_REMOTE, so that hold is already counted in the
 * coherence state machine and release_rw is what drops it.
 *
 * For pinned subtypes (PIN, GPU_PIN, GPU, CXL): there is no
 * DB-level coherence to track; the creator just owns the pointer
 * until it explicitly destroys or hands it off via events.
 *
 * In both cases the GUID is recorded on the creating THREAD's created-DB
 * list, and whoever drains that list drives the matching release: the EDT
 * epilogue for a task, or the thread's own scheduler entry for a startup
 * hook, which runs before any task on that thread.  The GUID is enough
 * because a label names one object for its lifetime: the create that
 * installs it is its only creator, and a create that finds it already there
 * creates nothing and registers nothing.  The list is thread-local and the
 * release chain needs no task context, so a create outside a task is tracked
 * exactly like one inside it — the alternative leaves the hold stamped and
 * nothing owning its release.
 */
static void arts_db_auto_acquire(struct arts_db_s *db, uint64_t bytes) {
  (void)bytes;
#ifdef ARTS_FAM
  /* A create's write turn begins here, and it ends at the zero edge its
   * release drives.  What a second coherency domain may write back under the
   * turn is registered over the block's slot for exactly that span: one hold
   * per turn, one unhold at the edge.  The span is the one the slot was
   * allocated for, which the caller knows and a published cache only agrees
   * with.  A create that takes no turn registers nothing — it never reaches
   * this call. */
  uint64_t slot = arts_db_fam_slot_addr(&db->cache);
  if (slot != 0) {
    arts_fam_strict_hold((const void *)(uintptr_t)slot, (size_t)bytes);
  }
#endif
  arts_track_created_db(db->cache.db_guid);
}

/* arts_db_creator_skip_hold — should the coherent (ARTS_DB) creator EDT be kept
 * OFF created_db_list?  Under the EXCL protocol the creator takes no implicit
 * lock: the home rank is the sole arbiter and zero-inits the buffer at create
 * time, and a writer only ever holds the lock via a granted EXCL_REQUEST (which
 * bumps the per-rank cache_state rw_count + sets rw_state=GRANT).  A
 * create-time stub has neither, so registering the creator on created_db_list
 * would make the EDT epilogue (arts_release_created_dbs -> release_one_created
 * -> release_rw) run a release on a hold that was never granted.  When a
 * same-rank worker has concurrently JOINed (raising local_count), that bogus
 * release steals the worker's count, drives the 0-edge, and ships a stale
 * publish to home as a spurious RW_REL — corrupting home's lock_state w
 * counter and overwriting the worker's update (the cross-rank lost-update). The
 * creator's stub buffer is still installed (so the user pointer is writable);
 * it just is not tracked for an auto-release.  A creator that must publish
 * initial data does so through a normal RW dependency, like any other writer.
 * Pinned subtypes still need created_db_list (destroy bookkeeping); the
 * single-owner protocols keep the legitimate creator-owns-until-release hold
 * (writer_count pre-stamped to 2).
 */
static inline bool arts_db_creator_skip_hold(arts_db_types_t db_type) {
  /* No protocol skips the creator hold any more: arts_db_create defaults to an
   * RW acquire for every coherent DB, and each protocol seeds that hold at
   * create time (single-owner: writer_count=2; EXCL: cache_state RW-GRANT +
   * lock_state w=1).  The matching release (explicit or EDT-epilogue
   * auto-release) drives it back, so the creator is tracked like any holder. */
  (void)db_type;
  return false;
}

/* Bytes a descriptor allocation must span.  A coherent DB's payload is its
 * coherence buffer, allocated apart from the descriptor; every other subtype
 * keeps its payload inline after the struct. */
static inline uint64_t db_descriptor_span(arts_db_types_t db_type,
                                          uint64_t len) {
  return sizeof(struct arts_db_s) + ((db_type == ARTS_DB) ? 0 : len);
}

/* The descriptor allocation.  A subtype that carries its payload inline is
 * pinned to the rank that created it and names the memory that payload has to
 * come from, so the subtype also picks the allocator; a coherent descriptor is
 * plain shared storage. */
static void *db_descriptor_alloc(arts_db_types_t db_type, uint64_t span) {
  (void)db_type;
  void *ptr = NULL;
#ifdef ARTS_USE_GPU
  if (arts_node_info.gpu) {
    /* The GPU subtype carries a shadow image of itself for version
     * reconciliation, so its allocation is twice the descriptor span. */
    if (db_type == ARTS_DB_GPU)
      ptr = arts_cuda_malloc_host(span * 2);
    else if (db_type == ARTS_DB_GPU_PIN)
      ptr = arts_cuda_malloc_host(span);
  }
#endif
#ifdef ARTS_USE_CXL
  if (db_type == ARTS_DB_CXL) {
    unsigned int dev_idx;
    if (arts_node_info.cxl_db_dev_count > 1) {
      /* Round-robin: atomically advance the index and wrap around. */
      dev_idx = arts_atomic_fetch_add(&arts_node_info.cxl_db_rr_idx, 1U) %
                arts_node_info.cxl_db_dev_count;
    } else {
      /* Static: use the configured device. */
      dev_idx = arts_node_info.cxl_db_static_device;
    }
    ptr = arts_cxl_deque_db_malloc_dev(arts_node_info.cxl_deque,
                                       &arts_node_info.cxl_local_lock, span,
                                       dev_idx);
    assert(ptr && "arts_cxl_deque_db_malloc_dev ptr is valid\n");
    /* A CXL descriptor names an offset into the shared window, never a
     * DRAM address; on exhaustion the caller must see NULL and report it
     * loudly rather than have this fall to the DRAM allocator below. */
    return ptr;
  }
#endif
  if (!ptr) {
    /* The descriptor sits at offset 0 of the allocation and is shared across
     * threads, so it must start on a cache line. */
    ptr = arts_malloc_aligned(span, ARTS_CACHE_LINE_SIZE);
  }
  return ptr;
}

void arts_db_free(void *ptr) {
  struct arts_db_s *db = (struct arts_db_s *)ptr;
  /* Chain into coherence cache teardown if this DB has one.  Only ARTS_DB
   * carries coherence state; other subtypes leave the embedded cache zeroed.
   * The cache is embedded by value as the first member of db_s, so the
   * destructor tears down its sub-resources (the buffer slot's shared_ptr ref +
   * home_s) in place — we do NOT free it separately; the db_s free below
   * reclaims its storage. */
  if (db->db_type == ARTS_DB) {
    arts_db_cache_destructor(&db->cache);
  }
#ifdef ARTS_USE_GPU
  if (arts_node_info.gpu &&
      (db->db_type == ARTS_DB_GPU_PIN || db->db_type == ARTS_DB_GPU)) {
    arts_cuda_free_host(ptr);
    ptr = NULL;
  }
#endif
  if (ptr) {
    arts_free(ptr);
  }
}

/*
 * arts_db_deleter — shared_t deleter (called by route_table.c free_item once
 * the slot's lock count hits 0 with DELETE set).  Entry point that
 * consolidates DB teardown through the route_table generic dispatch.  The
 * cleanup logic itself lives in arts_db_free (hierarchical: cache_destructor
 * → cache_s → GPU-host free → struct).
 */
/* cb deleter (route_table deleter-by-kind for ARTS_GUID_DB).  External
 * linkage so route_table.c references it directly.  `self` is the cb object
 * pointer = &db->cache (cache is the first member), which aliases arts_db_s. */
void arts_db_deleter(void *self) { arts_db_free(self); }

/* Publish the DB cb deleter into the route_table's per-kind table at startup
 * (decoupled registration — see arts_route_table_register_deleter). */
__attribute__((constructor)) static void arts_db_register_cb_deleter(void) {
  arts_route_table_register_deleter(ARTS_GUID_DB, arts_db_deleter);
}

/*
 * db_create_in_place — Initialize a DB header in pre-allocated memory.
 *
 * Sets up the arts_db_s header fields (type, size, version, reader/writer
 * counts, db_list) and records metrics.  The caller is responsible for
 * route-table registration.
 *
 * `acquires` says whether the create takes the creator's hold, which is also
 * what decides who allocates the payload: a create that acquires has its
 * first user right here and allocates for it, while one that does not leaves
 * the buffer to the first acquirer (arts_db_buf_ensure).
 */
static void db_create_in_place(arts_guid_t guid, void *addr, uint64_t len,
                               arts_db_types_t db_type, bool acquires) {
  struct arts_db_s *db_res = (struct arts_db_s *)addr;
  /* lifecycle/deleter handled by the route_table cb (deleter-by-kind) on
   * install — no per-object shared field to initialize. */
  db_res->version = 0;
  db_res->reader = 0;
  db_res->writer = 0;
  db_res->db_type = db_type;
  /* ARTS_DB enters the coherence protocol at create time.  Initialize the
   * embedded cache (first member of db_s) with CREATOR_HOME init —
   * arts_db_create only routes here when the local rank is the creator
   * (route == arts_global_rank_id), which for round-robin home is also the
   * home rank.  Non-coherent subtypes leave the embedded cache zeroed.
   *
   * Note: db_create_in_place is called only on the local-create
   * branch of arts_db_create; the remote-create branch builds its own
   * stub directly and does NOT invoke this routine. */
  /* Every DB — coherent or pinned — records its payload length in the cache.
   * Size lives here (cache->db_size), not in a separate per-object header: a
   * DB always carries its own length. */
  struct arts_db_cache_s *cache = &db_res->cache;
  /* The allocation is not necessarily zeroed; zero the embedded cache before
   * in-place init. */
  memset(cache, 0, sizeof(*cache));
  cache->db_size = len;
  cache->db_guid = guid;
  if (db_type == ARTS_DB) {
    arts_db_cache_init(cache, guid, len, ARTS_DB_INIT_CREATOR_HOME,
                       arts_global_rank_id);
#ifdef ARTS_FAM
    /* The block's canonical store, in place while the cache is still private:
     * the caller's install replays whatever this rank deferred for the GUID
     * and a replayed request can be granted at once, the pointer this create
     * hands out may BE the slot, and the buffer install below adopts the slot
     * on the residency that keeps no copy.  After the cache's init, which is
     * what zeroes the field, and before both.  Inside the coherent branch on
     * purpose — a kind that keeps no coherence state has no funnel to read a
     * slot and no teardown to free one.
     *
     * A create that acquires hands its caller an uninitialized payload, so
     * its store owes no value; a create that does not hands out no pointer,
     * and its block's first acquire reads zero, which only this call can
     * establish. */
    (void)arts_db_fam_slot_create(cache, /*zero_first=*/!acquires);
#endif
    /* Install a fresh buffer so subsequent coherent acquires
     * (acquire_local / mark_edt_ready_by_guid) find a non-NULL
     * cache->buffer.  The user pointer returned by arts_db_create
     * points into this buffer's data[] FAM, so writes by the creator
     * EDT land in buf->data and are published when release_rw bumps
     * the version.  No initial value: the payload an acquiring create
     * hands out is uninitialized by contract.
     *
     * Only when the create acquires: then the creating thread IS the
     * payload's first user and allocating here places it on that thread's
     * node.  A create that acquires nothing has no first user yet, so the
     * buffer waits for one (arts_db_buf_ensure at every point that can be
     * it) rather than being placed for a thread that may never read it. */
    if (len > 0 && acquires) {
      arts_db_buf_install(cache, /*new_version=*/1,
                          /*data_payload=*/NULL, len);
    }
  }
  /* Non-coherent subtypes (ARTS_DB_PIN, ARTS_DB_GPU_PIN, ARTS_DB_GPU,
   * ARTS_DB_CXL) are pinned to the creator rank and have no DB-level
   * coherence.  The embedded cache stays zeroed and db_list stays NULL. */
  if (db_type == ARTS_DB_GPU) {
    void *shadow_copy =
        (void *)(((char *)addr) + sizeof(struct arts_db_s) + len);
    memcpy(shadow_copy, addr, sizeof(struct arts_db_s));
  }
  INCREMENT_NUM_DB_CREATE_BY(1);
  INCREMENT_BYTES_DB_CREATE_BY(len);
}

/*
 * arts_db_create — Unified DataBlock creation.
 *
 * Handles all DB subtypes (ARTS_DB, ARTS_DB_PIN, ARTS_DB_GPU_PIN, ARTS_DB_GPU,
 * ARTS_DB_CXL).  When hint->rank targets a remote node, only ARTS_DB is
 * supported: a coherent home stub is installed via DB_CREATE_COHERENT.
 * Pinned subtypes return NULL_GUID with a warning.
 */
arts_guid_t arts_db_create(void **addr, uint64_t len, arts_db_types_t db_type,
                           uint16_t flags, const arts_db_hint_t *hint) {
  TIME_DB_CREATE_START();
  /* Route resolution:
   *   hint == NULL                          -> policy-selected home
   *                                             (ARTS_NOHINT_DB_ROUNDROBIN;
   *                                             creator-local by default).
   *   hint->rank == ARTS_HINT_CURRENT_RANK -> caller explicitly requested
   *                                             current node.
   *   hint->rank == specific rank          -> caller-specified rank.
   */
  /* If the caller supplies a pre-reserved GUID its encoded rank is
   * authoritative and overrides hint->rank. */
  arts_guid_t pre_guid = (hint != NULL) ? hint->guid : NULL_GUID;
  /* CHECK / rendezvous: fail (keep existing) instead of overwriting a live
   * labeled GUID.  Default false = unconditional replace on reuse. */
  bool check = (hint != NULL) ? hint->check : false;
  unsigned int rank;
  if (pre_guid != NULL_GUID) {
    rank = arts_guid_get_rank(pre_guid);
  } else if (hint == NULL) {
#if ARTS_NOHINT_DB_ROUNDROBIN
    rank = arts_atomic_fetch_add(&arts_node_info.db_rr_route, 1U) %
           arts_global_rank_count;
#else
    rank = arts_global_rank_id;
#endif
  } else if (hint->rank == ARTS_HINT_CURRENT_RANK) {
    rank = arts_global_rank_id;
  } else {
    rank = hint->rank;
  }
  bool no_acquire = (flags & ARTS_DB_PROP_NO_ACQUIRE) != 0;
  arts_guid_t guid = NULL_GUID;

  if (rank == arts_global_rank_id) {
    uint64_t db_span = db_descriptor_span(db_type, len);
#ifdef ARTS_USE_CXL
    if (db_type == ARTS_DB_CXL) {
      db_span = ALIGN_UP(db_span, CACHELINE_SIZE);
      void *ptr = db_descriptor_alloc(ARTS_DB_CXL, db_span);
      if (ptr) {
        guid = arts_cxl_make_guid(ptr);
        db_create_in_place(guid, ptr, len, ARTS_DB_CXL,
                           /*acquires=*/!no_acquire);
        /* No route table entry — GUID encodes CXL pointer directly */
        // FLUSH_FENCE_PRODUCER(ptr, db_span);
        FLUSH_FENCE_PRODUCER(ptr, sizeof(struct arts_db_s));
        *addr = no_acquire ? NULL : (void *)((struct arts_db_s *)ptr + 1);
        ARTS_DEBUG("arts_db_create: CXL DB[Guid:%lu, Size:%lu] created", guid,
                   len);
      } else {
        /* A silent NULL_GUID here would surface as arbitrary downstream
         * failures instead of the real cause. */
        ARTS_ERROR("arts_db_create: ARTS_DB_CXL arena exhausted for a "
                   "%lu-byte block",
                   (unsigned long)len);
      }
    } else
#endif
    {
      void *ptr = db_descriptor_alloc(db_type, db_span);
      if (ptr) {
        if (pre_guid != NULL_GUID) {
          /* Pre-reserved labeled GUID. */
          guid = pre_guid;
          db_create_in_place(guid, ptr, len, db_type,
                             /*acquires=*/!no_acquire);
          if (no_acquire && db_type == ARTS_DB) {
            /* NO_ACQUIRE coherent: the creator never acquires or releases, so
             * home is the sole idle owner.  db_create_in_place pre-stamped the
             * coherent writer_count to 2 (sentinel + creator-hold), but no
             * EDT tracks or releases that hold (nothing is registered for a
             * create that takes none), so drop it to the sentinel (1) before
             * the DB becomes visible at install.  Without this the
             * unreleased creator-hold blocks every future writer under a
             * single-writer protocol — the same reason the remote DB_CREATE
             * handler stamps writer_count = 1 for NO_ACQUIRE. */
#if defined(ARTS_PROTOCOL_EXCL)
#if defined(ARTS_RELEASE_RETAIN)
            /* RETAIN release policy: data lives with the owner, not the home — with no creator
             * hold there is no owner unless we make one.  This rank (the GUID
             * home, where a local create runs) becomes the IDLE data owner
             * with owner-bit set but rw_st=IDLE, wc=0.  Its buffer does not
             * exist yet: with no creator to place it for, the first user
             * allocates it — a remote writer in the buffer it prepares for
             * the migration, which then carries the size and no bytes, or
             * the acquiring thread when the first user is local.  lock_state
             * is the idle directory naming this rank as owner. */
            atomic_store_explicit(&((struct arts_db_s *)ptr)->cache.cache_state,
                                  CACHE_MAKE_FULL(1u, CACHE_ST_IDLE,
                                                  CACHE_ST_IDLE,
                                                  ARTS_EXCL_NO_TARGET, 0u, 0u),
                                  memory_order_relaxed);
            atomic_store_explicit(
                &((struct arts_db_s *)ptr)->lock_state,
                LOCK_MAKE(EXCL_PHASE_IDLE, arts_global_rank_id, 0u, 0u),
                memory_order_relaxed);
#else  /* ARTS_RELEASE_PURGE */
            /* PURGE release policy: the home holds the canonical buffer; undo the create-time
             * creator RW seed → free lock, so the first acquirer is granted
             * rather than blocked behind a hold no EDT will ever release. */
            atomic_store_explicit(&((struct arts_db_s *)ptr)->cache.cache_state,
                                  0ULL, memory_order_relaxed);
            atomic_store_explicit(&((struct arts_db_s *)ptr)->lock_state, 0ULL,
                                  memory_order_relaxed);
#endif /* ARTS_RELEASE_* */
#elif defined(ARTS_PROTOCOL_FLUSH)
            /* An arm that keeps no permission state has no seed to undo. */
#else
            /* Grant-bearing arms: this rank becomes the idle owner —
             * possession, and no hold under it.  With no creator hold there
             * is nothing to release, so the first foreign request finds an
             * idle grant rather than queueing behind a hold nobody will ever
             * drop. */
            ((struct arts_db_s *)ptr)->cache.writer_count =
                ARTS_GRANT_SEED_IDLE;
#endif
            /* The creator was given no storage, so any create-time claim to a
             * reader copy of it is false and goes with the write hold. */
            arts_db_create_retract_creator_copy((struct arts_db_s *)ptr);
          }
          /* First-wins, for every labeled create: one label names one object
           * for its lifetime, so the install decides whether this create
           * created anything at all.  The `check` hint is accepted and
           * ignored — a replacing install would leave two directories for
           * one GUID, and every message that finds the block by label would
           * land on whichever one the slot holds now.  The create's own hold
           * is the seed db_create_in_place stamped on the descriptor above,
           * so it is registered only once this create knows its descriptor
           * is the installed one; registering it first would make the
           * epilogue drop a hold on an object this create does not own. */
          bool installed = arts_route_table_install_if_absent(
              ptr, guid, arts_global_rank_id, /*used=*/true);
          if (!installed) {
            /* This create created nothing: the block under that label is
             * somebody else's, this rank holds no right to it, and a pointer
             * is only ever handed back through a hold this create took.  The
             * descriptor is torn down rather than left as an orphan whose
             * writes nothing would publish. */
#ifdef ARTS_FAM
            /* A losing labeled creator never received a pointer, and the slot
             * it minted is its own slice's. */
            arts_db_fam_slot_discard(&((struct arts_db_s *)ptr)->cache);
#endif
            arts_db_free(ptr);
            ptr = NULL;
            *addr = NULL;
          } else if (!no_acquire && !arts_db_creator_skip_hold(db_type)) {
            arts_db_auto_acquire((struct arts_db_s *)ptr, len);
          }
        } else {
          guid = arts_db_guid_stamp_szhint(
              arts_guid_create_for_rank(arts_global_rank_id, ARTS_GUID_DB),
              len);
          db_create_in_place(guid, ptr, len, db_type,
                             /*acquires=*/!no_acquire);
          if (no_acquire && db_type == ARTS_DB) {
            /* NO_ACQUIRE coherent: the creator never acquires or releases, so
             * home is the sole idle owner.  db_create_in_place pre-stamped the
             * coherent writer_count to 2 (sentinel + creator-hold), but no
             * EDT tracks or releases that hold (nothing is registered for a
             * create that takes none), so drop it to the sentinel (1) before
             * the DB becomes visible at install.  Without this the
             * unreleased creator-hold blocks every future writer under a
             * single-writer protocol — the same reason the remote DB_CREATE
             * handler stamps writer_count = 1 for NO_ACQUIRE. */
#if defined(ARTS_PROTOCOL_EXCL)
#if defined(ARTS_RELEASE_RETAIN)
            /* RETAIN release policy: data lives with the owner, not the home — with no creator
             * hold there is no owner unless we make one.  This rank (the GUID
             * home, where a local create runs) becomes the IDLE data owner
             * with owner-bit set but rw_st=IDLE, wc=0.  Its buffer does not
             * exist yet: with no creator to place it for, the first user
             * allocates it — a remote writer in the buffer it prepares for
             * the migration, which then carries the size and no bytes, or
             * the acquiring thread when the first user is local.  lock_state
             * is the idle directory naming this rank as owner. */
            atomic_store_explicit(&((struct arts_db_s *)ptr)->cache.cache_state,
                                  CACHE_MAKE_FULL(1u, CACHE_ST_IDLE,
                                                  CACHE_ST_IDLE,
                                                  ARTS_EXCL_NO_TARGET, 0u, 0u),
                                  memory_order_relaxed);
            atomic_store_explicit(
                &((struct arts_db_s *)ptr)->lock_state,
                LOCK_MAKE(EXCL_PHASE_IDLE, arts_global_rank_id, 0u, 0u),
                memory_order_relaxed);
#else  /* ARTS_RELEASE_PURGE */
            /* PURGE release policy: the home holds the canonical buffer; undo the create-time
             * creator RW seed → free lock, so the first acquirer is granted
             * rather than blocked behind a hold no EDT will ever release. */
            atomic_store_explicit(&((struct arts_db_s *)ptr)->cache.cache_state,
                                  0ULL, memory_order_relaxed);
            atomic_store_explicit(&((struct arts_db_s *)ptr)->lock_state, 0ULL,
                                  memory_order_relaxed);
#endif /* ARTS_RELEASE_* */
#elif defined(ARTS_PROTOCOL_FLUSH)
            /* An arm that keeps no permission state has no seed to undo. */
#else
            /* Grant-bearing arms: this rank becomes the idle owner —
             * possession, and no hold under it.  With no creator hold there
             * is nothing to release, so the first foreign request finds an
             * idle grant rather than queueing behind a hold nobody will ever
             * drop. */
            ((struct arts_db_s *)ptr)->cache.writer_count =
                ARTS_GRANT_SEED_IDLE;
#endif
            /* The creator was given no storage, so any create-time claim to a
             * reader copy of it is false and goes with the write hold. */
            arts_db_create_retract_creator_copy((struct arts_db_s *)ptr);
          }
          /* A fresh auto-GUID is this rank's alone, so the install lands by
           * construction and the hold is this create's. */
          arts_route_table_install(ptr, guid, arts_global_rank_id, true);
          if (!no_acquire && !arts_db_creator_skip_hold(db_type)) {
            arts_db_auto_acquire((struct arts_db_s *)ptr, len);
          }
        }
        /* A pointer is handed out only under a hold, and a NO_ACQUIRE create
         * takes none.  A create that installed nothing was answered above. */
        if (ptr != NULL) {
          *addr = no_acquire ? NULL : arts_db_user_ptr((struct arts_db_s *)ptr);
        }
        ARTS_DEBUG("arts_db_create: DB[Guid:%lu, Type:%s, Size:%lu] "
                   "created locally",
                   guid, GET_DB_TYPE_NAME(db_type), len);
      } else {
        /* A silent NULL_GUID here would surface as arbitrary downstream
         * failures instead of the real cause. */
        ARTS_ERROR("arts_db_create: descriptor allocation of %lu bytes failed",
                   (unsigned long)db_span);
      }
    }
  } else {
    /* Pre-reserved (labeled) GUID with remote home: use it verbatim so the
     * remote install lands at the application-visible GUID.  Otherwise
     * (NULL hint round-robin / explicit-rank hint) generate a fresh
     * auto-GUID on the home rank's key counter. */
    guid = (pre_guid != NULL_GUID)
               ? pre_guid
               : arts_db_guid_stamp_szhint(
                     arts_guid_create_for_rank(rank, ARTS_GUID_DB), len);
    if (db_type == ARTS_DB) {
      /* For ARTS_DB, ask the home rank to install a coherent cache_s
       * via DB_CREATE_COHERENT.  The home handler
       * (arts_handler_db_create) allocates its own stub +
       * cache_s with ARTS_DB_INIT_HOME_RECV.
       *
       * Also stub-install a creator-side cache_s on this (non-home)
       * rank via arts_db_cache_stub_install.  This is necessary so
       * that home's first GRANT_INVALIDATE (sent to
       * rw_holder = creator_rank when a foreign GRANT_REQUEST arrives)
       * finds a cache_s on this rank to drop the sentinel and trigger
       * invalidate_transfer.  Without it, home's invalidation goes to a
       * phantom holder and the first foreign acquirer stalls forever. */
      /* Use ARTS_DB_INIT_CREATOR_REMOTE (writer_count = 2: sentinel +
       * creator EDT) so that the home-side GRANT_INVALIDATE round-trip
       * works correctly.  When a foreign GRANT_REQUEST arrives at home,
       * home sends GRANT_INVALIDATE to rw_holder = creator; creator's
       * fetch_sub takes wc 2 -> 1 (no transfer yet -- creator EDT may
       * still be using the buffer).  Creator EDT release_rw drops wc
       * 1 -> 0, triggering R4 PUBLISH_AND_TRANSFER with the creator's
       * data.  wc = 1 (the implementer's earlier choice) was wrong: it
       * would trigger the transfer immediately on GRANT_INVALIDATE
       * while the creator EDT was still writing.
       *
       * The cache is built privately (CREATOR_REMOTE init -> wc = 2) and
       * only then published via add_item_race, so the visibility
       * transition (NULL -> cache) already shows wc > 0.
       *
       * Buffer install: CREATOR_REMOTE init does NOT install a buffer
       * (alloc_cache_s contract).  We install one explicitly via
       * arts_db_buf_install so the user's `*addr = ...` write
       * lands in cache->buffer->data, and the buffer is captured by the
       * subsequent PUBLISH_AND_TRANSFER. */
      if (no_acquire) {
        /* NO_ACQUIRE: do NOT stub-install a creator-side cache_s.  The
         * home is the sole idle owner; first consumer EDT triggers a
         * normal GRANT_REQUEST to acquire ownership.  Wire only carries
         * metadata (no payload bytes). */
        arts_send_db_create_coherent(rank, guid, len, ARTS_DB_PROP_NO_ACQUIRE,
                                     (uint16_t)db_type, 0);
        *addr = NULL;
      } else {
        /* Creator-remote (home != self): cache-only stub — no home directory.
         * arts_db_cache_stub_size() spans cache + db_type but not the home
         * fields, which this rank never touches (home lives on the GUID home).
         * Init the cache in place, then install the buffer. */
        uint64_t stub_sz = arts_db_cache_stub_size();
        struct arts_db_s *creator_stub = (struct arts_db_s *)arts_malloc_aligned(
            stub_sz, ARTS_CACHE_LINE_SIZE);
        memset(creator_stub, 0, stub_sz);
        creator_stub->db_type = ARTS_DB;
        struct arts_db_cache_s *creator_cache = &creator_stub->cache;
        /* A create makes the block: the object at its home, which every
         * operation on the label is ordered against, and on this rank the
         * cache that holds the creator's own hold on it.  A cache is not the
         * block though — any rank makes one when it first touches a DB,
         * before or without a create — so a create that finds one here uses
         * it and records its hold in it.  What decides whether there is
         * anything to record is the cache's create mark, which one rank's
         * create takes once and never gives back: a create that finds it
         * taken creates nothing at all. */
        bool took_hold = true;
        /* The bytes this create's turn covers: its own length, or the length
         * the cache it found already declares. */
        uint64_t turn_bytes = len;
        arts_db_cache_init(creator_cache, guid, len,
                           ARTS_DB_INIT_CREATOR_REMOTE, /*creator_rank=*/0);
#ifdef ARTS_FAM
        /* Still private: the stub is not installed and *addr is unwritten, so
         * the slot is in place before anything can be admitted to the block,
         * handed a pointer into it, or adopt it as this block's descriptor. */
        (void)arts_db_fam_slot_create(creator_cache, /*zero_first=*/false);
#endif
        if (len > 0) {
          arts_db_buf_install(creator_cache, /*new_version=*/1,
                              /*data_payload=*/NULL, len);
        }
        if (!arts_route_table_install_if_absent(creator_stub, guid,
                                                arts_global_rank_id,
                                                /*used=*/true)) {
#ifdef ARTS_FAM
          /* This create installed nothing, so the slot it minted a moment ago
           * — out of this rank's own slice — belongs to no block. */
          arts_db_fam_slot_discard(creator_cache);
#endif
          arts_db_free(creator_stub);
          creator_cache = NULL;
          took_hold = false;
          arts_shared_ptr_t found_h = arts_route_table_lookup_db(guid);
          struct arts_db_s *found =
              (struct arts_db_s *)arts_shared_get(found_h);
          if (found != NULL && found->db_type == ARTS_DB) {
            struct arts_db_cache_s *cache = &found->cache;
            /* The mark is asked before anything is made for this create, so
             * a create that loses it leaves nothing behind: no image in the
             * slot, no claim to one, no hold. */
            if (arts_db_create_hold_once(cache)) {
#ifdef ARTS_FAM
              /* A slot already known here was delivered with the block's
               * bytes, so the block exists elsewhere and this create makes
               * nothing: no store, no image, no hold, no announce, a NULL
               * pointer and success — one label names one object for its
               * lifetime. */
              if (arts_db_fam_slot_addr(cache) == 0) {
#endif
              /* The block's first image, into a cache a first touch left
               * without one.  Size from whatever this rank has been taught,
               * else this create's own length. */
              uint64_t size = (cache->db_size != 0) ? cache->db_size : len;
              turn_bytes = size;
#ifdef ARTS_FAM
              /* A first touch's cache declares no size, and the store is
               * allocated for the size the cache declares: this create is the
               * block's declaration of one.  Before the store, before the
               * image and before the hold — taking the hold admits the
               * waiters an earlier request from this rank parked, an admitted
               * waiter's pointer may BE the slot, and the ensure below adopts
               * the slot on the residency that keeps no copy. */
              if (cache->db_size == 0) {
                cache->db_size = size;
              }
              (void)arts_db_fam_slot_create(cache, /*zero_first=*/false);
#endif
              if (size > 0) {
                (void)arts_db_buf_ensure(cache, size);
              }
              /* On an arm that tracks a durable reader copy, the image this
               * create put here is this rank's — recorded only from an idle
               * reader word, because a fetch already in flight brings the
               * copy with it when it lands. */
              arts_db_create_claim_creator_copy(found);
              /* The create's own hold, in its arm's own state. */
              if (arts_db_create_take_hold(cache)) {
                creator_cache = cache;
                took_hold = true;
              }
#ifdef ARTS_FAM
              /* A refused hold hands nothing back.  The refusal means another
               * party on this rank already holds or is fetching the block, and
               * that party is served out of the store this cache names — so the
               * store belongs to the block, not to this create, and it goes
               * back with the block's teardown.  Handing it back here would
               * free a granule under a live holder and re-issue it to another
               * block. */
              }
#endif
            }
          }
          arts_shared_release(&found_h);
        }
        /* Only a create that created something announces the block: the one
         * that found it already there was preceded by the create that did,
         * and that create's own announce is the home's. */
        if (took_hold) {
          arts_send_db_create_coherent(rank, guid, len, ARTS_DB_PROP_NONE,
                                       (uint16_t)db_type,
                                       arts_db_fam_slot_addr(creator_cache));
        }
        if (took_hold && !arts_db_creator_skip_hold(ARTS_DB)) {
          /* Register the DB on the creating thread's created-DB list so the
           * release that drops this hold runs at the end of the creating EDT
           * (or, for a startup hook, at that thread's scheduler entry).
           * Before the pointer below: the turn must be registered before the
           * storage it covers can be written through it. */
          arts_db_auto_acquire(arts_db_of_cache(creator_cache), turn_bytes);
        }
        /* The creator-side buffer pointer, so the user can write the local
         * copy its release publishes — handed out only through the hold this
         * create took. */
        arts_shared_ptr_t creator_buf_h =
            took_hold ? arts_db_buf_acquire(creator_cache) : NULL;
        struct arts_db_buffer_s *creator_buf =
            (struct arts_db_buffer_s *)arts_shared_get(creator_buf_h);
        *addr = creator_buf ? (void *)creator_buf->data : NULL;
        arts_db_buf_release(&creator_buf_h);
      }
      ARTS_DEBUG("arts_db_create: DB[Guid:%lu, Type:%s, Size:%lu] "
                 "created remotely on rank %u via DB_CREATE_COHERENT",
                 guid, GET_DB_TYPE_NAME(db_type), len, rank);
    } else {
      /* Non-coherent subtypes (ARTS_DB_PIN, ARTS_DB_GPU_PIN, ARTS_DB_GPU,
       * ARTS_DB_CXL) are pinned to the creator rank.  Creating one on a
       * different rank is a programming error — return NULL_GUID.
       * Cross-rank distribution for these subtypes must use ARTS_DB instead. */
      ARTS_WARN("arts_db_create: only ARTS_DB (coherent) DataBlocks support "
                "remote create; %s on rank %u is pinned to the creator rank. "
                "Returning NULL_GUID.",
                GET_DB_TYPE_NAME(db_type), rank);
      *addr = NULL;
      guid = NULL_GUID;
    }
  }
  TIME_DB_CREATE_STOP();
  return guid;
}

/*
 * arts_db_destroy — Mark a DataBlock for deferred destruction.
 *
 * If the calling EDT currently holds an acquire on this DB (either via
 * the auto-acquired created_db_list or via a dependency slot), the
 * acquire is implicitly released first.  This matches OCR's ocrDbDestroy
 * semantics: "If the EDT has acquired this DB, this call implicitly
 * releases the DB."
 *
 * After the implicit release, the route-table entry is marked for
 * deletion.  New acquire attempts (inc_item) will fail once DELETE_ITEM
 * is set.  The actual memory is freed when the last outstanding
 * route-table reference is returned (deferred deletion).
 */
void arts_db_destroy(arts_guid_t guid) {
  INCREMENT_NUM_DB_DESTROY_BY(1);
  arts_guid_kind_t type = arts_guid_get_kind(guid);
  if (type != ARTS_GUID_DB) {
    ARTS_WARN("arts_db_destroy called with non-DB type %u (GUID %lu)", type,
              guid);
    return;
  }

#ifdef ARTS_USE_CXL
  if (arts_guid_is_cxl(guid)) {
    return;
  }
#endif

  /* Implicit release: if the calling EDT holds an acquire on this DB,
   * release it first (matches OCR ocrDbDestroy semantics).  A created/owned
   * DB releases as RW; a dep release reads the slot mode in Path 2 regardless.
   */
  arts_db_release(guid, DB_MODE_RW);

  arts_shared_ptr_t db_res_h = arts_route_table_lookup_db(guid);
  struct arts_db_s *db_res = (struct arts_db_s *)arts_shared_get(db_res_h);

  /* Coherent ARTS_DB path: hand off to the coherence-layer destroy entry,
   * which sends DESTROY_REQ to home and runs the fan-out / finalize there. */
  if (db_res != NULL && db_res->db_type == ARTS_DB) {
    arts_shared_release(&db_res_h);
    arts_db_destroy_remote(guid);
    return;
  }

  /* Non-coherent pinned subtypes (ARTS_DB_PIN, ARTS_DB_GPU_PIN, ARTS_DB_GPU,
   * ARTS_DB_CXL): the DB lives only on the creator rank.  Route through
   * arts_route_table_set_destroyed — once outstanding refs drop, the cb
   * deleter (arts_db_deleter) runs. */
  if (db_res != NULL) {
    arts_shared_release(&db_res_h);
    arts_route_table_set_destroyed(guid);
  }
}

/**********************DB MEMORY MODEL*************************************/

/* acquire_one_dep — attempt the single DB dependency depv[i].
 *
 * On a synchronous resolve it writes depv[i].ptr (NULL is valid: a sentinel /
 * version-0 / no-payload DB) and self-accounts via arts_db_acquire_resolved;
 * when the EDT must park (remote ownership/data round, or OoO defer of a
 * not-yet-installed local DB) it leaves the slot for the protocol wake / OoO
 * drain replay and does NOT account.  The 3-way route_table dispatch (local
 * entry / remote-home stub install / home==self-but-not-created → OoO push)
 * lives here, inlined from the old single-DB arts_db_acquire API.  The caller
 * (arts_db_acquire_all / rw_fire_from_cursor) has already filtered NULL_GUID /
 * DB_MODE_NULL / pre-filled slots, so depv[i] is a real, not-yet-acquired DB
 * dependency. */
static void acquire_one_dep(struct arts_edt_s *edt, arts_edt_dep_t *depv,
                            uint32_t i) {
  arts_db_access_mode_t access_mode = depv[i].mode;
  unsigned int owner = arts_guid_get_rank(depv[i].guid);
  arts_guid_kind_t guid_type = arts_guid_get_kind(depv[i].guid);

  if (guid_type != ARTS_GUID_DB) {
    return; /* not a DB GUID — nothing to acquire */
  }

  // Update access-mode counters
  if (access_mode == DB_MODE_RO) {
    INCREMENT_NUM_DB_ACQUIRE_READ_BY(1);
  } else if (access_mode == DB_MODE_RW) {
    INCREMENT_NUM_DB_ACQUIRE_WRITE_BY(1);
  }

  ARTS_INFO("Acquiring DB[Guid:%lu, GuidType:%u, AccessMode:%u, Owner:%u, "
            "Rank:%u] in EDT[Guid:%lu, Slot:%u]",
            depv[i].guid, guid_type, access_mode, owner, arts_global_rank_id,
            edt->guid, i);

#ifdef ARTS_USE_CXL
  if (arts_guid_is_cxl(depv[i].guid)) {
    struct arts_db_s *cxl_db =
        (struct arts_db_s *)arts_cxl_get_ptr(depv[i].guid);
    /* Consumer flush deferred to prep_dbs (just before user func) to avoid
     * stale reads after deque wait. */
    if (cxl_db) {
      depv[i].ptr = cxl_db + 1;
      depv[i].subtype = ARTS_DB_CXL;
      arts_db_acquire_resolved(edt, i);
      return;
    }
    /* Not yet allocated in the shared segment — OoO defer (park); the CXL deque
     * ordering makes the producer's allocation visible before the consumer
     * runs.  Data/cursor arrive on the drain replay; no resolved here. */
    {
      struct arts_ooo_args_db_acquire_s a = {
          .edt = edt, .db_guid = depv[i].guid, .slot = i};
      arts_ooo_dispatch_or_defer_guid(depv[i].guid, OOO_DB_ACQUIRE, &a,
                                      sizeof(a));
    }
    return;
  }
#endif

  // Look up DB first — subtype dispatch requires the struct.  lookup_db pairs
  // with the release below (every successful lookup => one release).
  arts_shared_ptr_t db_temp_h = arts_route_table_lookup_db(depv[i].guid);
  struct arts_db_s *db_temp = (struct arts_db_s *)arts_shared_get(db_temp_h);

  /* Coherent ARTS_DB path (either placement, any protocol).  Two entry
   * points:
   *   - Existing local cache_s (db_temp with db_type == ARTS_DB; embedded
   *     cache).
   *   - Remote DB never seen on this rank (db_temp == NULL, owner remote):
   *     stub-install a stub cache_s and dispatch through
   *     arts_handler_db_acquire.
   * Other (pinned) subtypes bypass coherence and fall through below. */
  struct arts_db_cache_s *cache = NULL;
  /* When db_temp misses on a remote-owned DB we stub-install a stub and the
   * call returns a SEPARATE pinned handle (stub_h) to the just-installed db_s;
   * it must be released on every path below, mirroring db_temp_h. */
  arts_shared_ptr_t stub_h = NULL;
  /* B1: which of the two handles keeps `cache`'s descriptor (arts_db_s) alive.
   * If the handler resolves locally (takes the EDT's buffer ref), this handle
   * is MOVED into depv[i].db_pin to pin the descriptor — and the buffer slot +
   * recycle pool embedded in it — for the slot's whole acquire->release span.
   */
  arts_shared_ptr_t *cache_owner_h = NULL;
  if (db_temp != NULL && db_temp->db_type == ARTS_DB) {
    cache = &db_temp->cache;
    cache_owner_h = &db_temp_h;
  } else if (db_temp == NULL && owner != arts_global_rank_id) {
    /* db_size=0 means "size learned on first GRANT/SNAPSHOT_RESPONSE
     * install_buffer".  The home is encoded in the GUID, so all ranks
     * agree. */
    stub_h = arts_db_cache_stub_install(depv[i].guid, /*db_size=*/0);
    struct arts_db_s *stub_db = (struct arts_db_s *)arts_shared_get(stub_h);
    if (stub_db != NULL) {
      cache = &stub_db->cache;
      cache_owner_h = &stub_h;
    }
  }
  if (cache != NULL &&
      (access_mode == DB_MODE_RO || access_mode == DB_MODE_RW)) {
    /* The handler self-resolves (writes depv[i].ptr + arts_db_acquire_resolved)
     * on a local hit, or parks on a remote ownership/data round; no return.
     * Record the coherent subtype now (before any park) so release routes by
     * dep->subtype regardless of whether the handler resolves locally or the
     * async data-response fills depv[i].ptr later. */
    depv[i].subtype = ARTS_DB;
    struct arts_ooo_args_db_acquire_s a = {
        .edt = edt, .db_guid = depv[i].guid, .slot = i};
    arts_handler_db_acquire(arts_db_of_cache(cache), &a);
    /* B1: a local hit set depv[i].ptr and took the EDT's buffer ref.  Pin the
     * descriptor by MOVING the still-alive cache-owning handle into db_pin
     * (released last in release_one_dep), so a concurrent destroy cannot free
     * the cache out from under the outstanding buffer ref / its
     * recycle-on-drop. A parked handler leaves ptr NULL — the resume site
     * (mark_edt_ready_by_guid) pins instead.  The db_pin==NULL guard keeps the
     * pin balanced across OoO replays. */
    if (depv[i].ptr != NULL &&
        __atomic_load_n(&depv[i].db_pin, __ATOMIC_ACQUIRE) == NULL &&
        cache_owner_h != NULL) {
      __atomic_store_n(&depv[i].db_pin, (void *)*cache_owner_h,
                       __ATOMIC_RELEASE);
      *cache_owner_h = NULL;
    }
    arts_shared_release(&db_temp_h);
    arts_shared_release(&stub_h);
    return;
  }
  /* cache==NULL fall-throughs below never used stub_h (it is only set on the
   * remote-miss arm, which always has a non-NULL cache here unless the DB was
   * destroyed before install — stub_h NULL then); release defensively. */
  arts_shared_release(&stub_h);

  /* Non-coherent pinned subtypes (ARTS_DB_PIN, ARTS_DB_GPU_PIN, ARTS_DB_GPU,
   * ARTS_DB_CXL) live only on their creator rank, and a coherent DB reaches
   * here only in a mode outside the coherence protocol (device-side sync
   * modes): either way the local payload is handed back, wherever the subtype
   * keeps it. */
  if (db_temp != NULL) {
    if (owner != arts_global_rank_id) {
      ARTS_WARN(
          "arts_db_acquire_all: pinned DB[Guid:%lu, Type:%s] referenced from "
          "non-creator rank %u (owner=%u). Only ARTS_DB is internode "
          "relocatable.",
          depv[i].guid, GET_DB_TYPE_NAME(db_temp->db_type), arts_global_rank_id,
          owner);
    }
#ifdef ARTS_FAM_DIRECT
    if (db_temp->db_type == ARTS_DB) {
      ARTS_ERROR("db: access mode %u takes no hold, so it cannot address a "
                 "shared store — use DB_MODE_RO or DB_MODE_RW",
                 (unsigned)access_mode);
    }
#endif
    depv[i].ptr = arts_db_user_ptr(db_temp);
    depv[i].subtype = db_temp->db_type;
    arts_shared_release(&db_temp_h);
    arts_db_acquire_resolved(edt, i);
    return;
  }

  /* DB absent locally.  OoO defer keyed on the DB GUID (park): when DB_CREATE
   * installs it (home==self case) the drain re-attempts this dep through
   * arts_handler_db_acquire.  A remote non-coherent (pinned-subtype) reference
   * can never resolve locally and waits here.  No resolved — the data and
   * cursor advance arrive on the drain replay. */
  if (arts_guid_is_local(depv[i].guid)) {
    ARTS_DEBUG("DB[Guid:%lu] out of order request slot %u", depv[i].guid, i);
  } else {
    ARTS_WARN("arts_db_acquire_all: cannot resolve remote DB[Guid:%lu] for "
              "non-coherent (pinned) dep on rank %u — owner=%u. Deferring via "
              "OoO.",
              depv[i].guid, arts_global_rank_id, owner);
  }
  {
    struct arts_ooo_args_db_acquire_s a = {
        .edt = edt, .db_guid = depv[i].guid, .slot = i};
    arts_ooo_dispatch_or_defer_guid(depv[i].guid, OOO_DB_ACQUIRE, &a,
                                    sizeof(a));
  }
}

static void rw_fire_from_cursor(struct arts_edt_s *edt);
static void resume_enqueue(arts_guid_t edt_guid);

/* OOO_DB_ACQUIRE replay table entry — see db.h.  Re-attempts the single
 * deferred dep through acquire_one_dep's subtype-aware 3-way: for an ARTS_DB
 * this lands in arts_handler_db_acquire (coherent), for a PIN/GPU/CXL DB in the
 * pinned ptr path, and a still-absent DB re-defers.  Mapping OOO_DB_ACQUIRE
 * straight to arts_handler_db_acquire would mishandle non-coherent subtypes
 * (their embedded cache is zeroed — its pending_rw queue is uninitialised, so
 * the coherent RW path would push onto a NULL-headed queue and crash). */
void arts_db_acquire_replay_dep(void *item, void *args) {
  (void)item; /* the 3-way re-looks-up the installed db_s; the drain pins it */
  struct arts_ooo_args_db_acquire_s *a =
      (struct arts_ooo_args_db_acquire_s *)args;
  arts_edt_dep_t *depv = (arts_edt_dep_t *)arts_get_depv(a->edt);
  if (arts_dep_is_serialized(depv, a->slot)) {
    /* Serialized dep deferred at the cursor: re-drive the acquire loop from the
     * cursor.  Route through the flat resume trampoline (enqueue + drain) so
     * the re-drive stays top-level even when this replay runs nested inside
     * another EDT's acquire loop (an inline OoO drain) — never an inline
     * rw_fire_from_cursor that would recurse. */
    resume_enqueue(a->edt->guid);
    arts_db_drain_resume_list();
  } else {
    /* Non-serialized (Pass-1) dep: re-attempt just this slot; it self-accounts
     * on a local hit and the EDT schedules when the last dep's data lands. */
    acquire_one_dep(a->edt, depv, a->slot);
  }
}

/* How much of a block a dep asks for: a write acquisition subsumes a read one,
 * and a slot that acquires nothing asks for neither. */
static unsigned int dep_mode_strength(arts_db_access_mode_t mode) {
  if (mode == DB_MODE_RW) {
    return 2u;
  }
  return mode == DB_MODE_RO ? 1u : 0u;
}

/* True when dep `a` must be visited AFTER dep `b`. */
static bool dep_order_after(arts_edt_dep_t *depv, uint32_t a, uint32_t b) {
  if (depv[a].guid != depv[b].guid) {
    return depv[a].guid > depv[b].guid;
  }
  return dep_mode_strength(depv[a].mode) < dep_mode_strength(depv[b].mode);
}

/* A real DB dep still needing acquisition (not NULL / not a raw value / not
 * pre-filled / actually a DB GUID). */
static bool dep_needs_acquire(arts_edt_dep_t *depv, uint32_t i) {
  return depv[i].guid != NULL_GUID && depv[i].mode != DB_MODE_NULL &&
         depv[i].ptr == NULL &&
         arts_guid_get_kind(depv[i].guid) == ARTS_GUID_DB;
}

/* Visit order AND owner/alias classification for one EDT's whole dependence
 * vector — computed once, consumed by every later frame.
 *
 * Order: by GUID, so same-block deps are adjacent and a serialized walk takes
 * blocks in one global order; within a block, STRONGEST MODE FIRST; ties keep
 * slot order (insertion sort with a strict comparison is stable).
 *
 * Classification: an EDT acquires each distinct block ONCE.  The first slot of
 * a block's group OWNS that acquisition and is the only slot that asks an arm
 * for the block; every later slot naming it is an ALIAS, which shares the
 * owner's payload pointer, requests nothing and skips the arm's release,
 * holding only its own buffer ref and descriptor pin.  Being first in the
 * order, the owner carries the strongest mode any slot asks for: a write
 * released as a read is not exclusive and, on an arm whose release is what
 * carries the bytes home, is dropped outright.  Two copies of one block inside
 * one EDT would also be written back over each other, and the copy the EDT
 * never wrote through can be the one that lands last.
 *
 * Only the slots that share a block's coherence acquisition are classified —
 * DB_MODE_RO and DB_MODE_RW.  A NULL GUID, a value slot (DB_MODE_NULL), a
 * non-DB kind, an already-resolved slot, a CXL block and a device-side internal
 * mode (DB_MODE_LC_SYNC / MEMSET and the like, which the coherence protocol
 * does not acquire and which resolve on their own per-mode terms) are neither
 * owner nor alias and are left exactly as they are.
 *
 * A pure function of depv, which is fixed once the EDT is ready — so the answer
 * is the same for every frame that consumes it and nothing re-derives it. */
void arts_dep_sort_and_classify(arts_edt_dep_t *depv, uint32_t depc,
                                uint32_t *sorted) {
  for (uint32_t k = 0; k < depc; k++) {
    sorted[k] = k;
  }
  for (uint32_t k = 1; k < depc; k++) {
    uint32_t val = sorted[k];
    int j = (int)k - 1;
    while (j >= 0 && dep_order_after(depv, sorted[j], val)) {
      sorted[j + 1] = sorted[j];
      j--;
    }
    sorted[j + 1] = val;
  }
  /* An eligible slot never names NULL_GUID, so the seed can only start a
   * group, never join one. */
  arts_guid_t group = NULL_GUID;
  for (uint32_t k = 0; k < depc; k++) {
    uint32_t i = sorted[k];
    if (!dep_needs_acquire(depv, i) ||
        (depv[i].mode != DB_MODE_RO && depv[i].mode != DB_MODE_RW)) {
      continue;
    }
#ifdef ARTS_USE_CXL
    /* A CXL block has no descriptor to pin and no coherence acquisition to
     * share: every slot naming it resolves on its own from the segment. */
    if (arts_guid_is_cxl(depv[i].guid)) {
      continue;
    }
#endif
    depv[i].alias = (depv[i].guid == group);
    group = depv[i].guid;
  }
}

/* Ownership-serialized (RW cursor) dep? CXL DBs bypass DB coherence (no
 * ownership round), so they are never serialized — they fire in Pass 1. */
bool arts_dep_is_serialized(arts_edt_dep_t *depv, uint32_t i) {
#ifdef ARTS_USE_CXL
  if (arts_guid_is_cxl(depv[i].guid)) {
    return false;
  }
#endif
  return arts_db_acquire_is_serialized(depv[i].mode);
}

/* Decrement-and-maybe-schedule. Caller must not touch the EDT afterward. */
void arts_db_acquire_account(struct arts_edt_s *edt) {
  if (arts_atomic_sub(&edt->acquire_remaining, 1) == 0) {
    arts_schedule_ready_edt(edt);
  }
}

/* The EDT whose serialized-acquire loop is in progress in THIS execution
 * context right now (the resume_k loop below records it for its own duration).
 * When a dep is resolved INTRA-RANK and synchronously — reached from inside
 * that running loop (a same-rank grant whose handler runs directly on this
 * rank, no wire) — the running loop already advances to the next dep, so it
 * must NOT re-enter (re-firing would recurse once per dep).  A resume that is
 * NOT for the acquire in progress here (a parked EDT woken by an inter-rank
 * grant, a release grant, or an OoO replay) sees a different (or NULL) value
 * and DOES re-enter to drive that EDT.  Execution-context state, not per-EDT
 * state. */
static ARTS_THREAD_LOCAL struct arts_edt_s *tl_acquiring = NULL;

/* ===== Flat resume trampoline ==========================================
 * A grant/drain that secures a dep for a PARKED EDT (one that is not the EDT
 * whose acquire loop is currently running, i.e. edt != tl_acquiring) must
 * resume that EDT's serialized acquire — but it MUST NOT call
 * rw_fire_from_cursor inline: that EDT's loop would then nest on top of the
 * currently-running loop (continuation recursion, O(depth) stack and O(depc²)
 * repeated work).  Instead the woken EDT's GUID is appended to a thread-local
 * worklist and the resume runs FLAT: when control returns to the top level
 * (no acquire loop in progress, tl_acquiring == NULL) the drain below walks
 * the worklist in a while loop, calling rw_fire_from_cursor once per entry.
 * Re-entrancy-guarded (tl_in_drain) + top-level-guarded (tl_acquiring) so
 * rw_fire_from_cursor can never appear twice on the stack. */
static ARTS_THREAD_LOCAL arts_guid_t *tl_resume_buf = NULL;
static ARTS_THREAD_LOCAL uint32_t tl_resume_len = 0;
static ARTS_THREAD_LOCAL uint32_t tl_resume_cap = 0;
static ARTS_THREAD_LOCAL bool tl_in_drain = false;

/* Continuation-ownership signal for the resume_k loop: set when the dep the
 * loop just fired was resolved IN-FRAME (a synchronous local resolve or a
 * same-thread nested serve — arts_db_rw_secure / arts_db_acquire_resolved with
 * edt == tl_acquiring).  The loop continues ONLY on this signal; otherwise it
 * breaks and the serving side owns the continuation (its arts_db_rw_secure
 * enqueued the flat resume on its own thread).  A cursor comparison cannot
 * make this call: a CONCURRENT other-thread serve also advances the cursor, and
 * reading "advanced" as "resolved in-frame" lets two threads drive the same
 * EDT's loop at once — double-firing the next dep (double count, one release,
 * and a double acquire_remaining account).  Being thread-local, this flag is
 * untouchable by other threads' serves, so ownership is race-free. */
static ARTS_THREAD_LOCAL bool tl_inline_advanced = false;

static void resume_enqueue(arts_guid_t edt_guid) {
  if (tl_resume_len == tl_resume_cap) {
    uint32_t ncap = tl_resume_cap ? tl_resume_cap * 2u : 16u;
    tl_resume_buf =
        (arts_guid_t *)arts_realloc(tl_resume_buf, ncap * sizeof(arts_guid_t));
    tl_resume_cap = ncap;
  }
  tl_resume_buf[tl_resume_len++] = edt_guid;
}

/* Drain the resume worklist as a flat loop — see db.h.  No-op when called from
 * inside an acquire loop (tl_acquiring != NULL) or an in-progress drain — the
 * outermost caller owns it, so a wake enqueued deep in the nest is picked up by
 * that single top-level while loop, never by a nested rw_fire_from_cursor. */
static void rw_fire_from_cursor(struct arts_edt_s *edt);
void arts_db_drain_resume_list(void) {
  if (tl_acquiring != NULL || tl_in_drain) {
    return;
  }
  tl_in_drain = true;
  while (tl_resume_len > 0) {
    arts_guid_t g = tl_resume_buf[--tl_resume_len];
    arts_shared_ptr_t h = arts_route_table_lookup_edt(g);
    struct arts_edt_s *e = (struct arts_edt_s *)arts_shared_get(h);
    if (e != NULL) {
      rw_fire_from_cursor(e); /* flat: tl_acquiring is NULL here */
    }
    arts_shared_release(&h);
  }
  tl_in_drain = false;
}

/* resume_k loop: walk the serialized deps from the cursor, firing each.  A dep
 * that resolves LOCALLY advances the cursor (arts_db_acquire_resolved — advance
 * only, no re-fire) and the loop picks up the next one; a dep that PARKS (its
 * grant is not local) leaves the cursor put and we return — the matching async
 * grant re-enters here (arts_db_rw_secure enqueues, the drain calls) to
 * continue.  Driving the
 * walk as a LOOP (not the handler re-firing recursively) keeps the stack O(1)
 * however many serialized deps resolve in a row — required for EXCL, where RW
 * AND RO are both serialized so an EDT can have very many serialized deps. */
static void rw_fire_from_cursor(struct arts_edt_s *edt) {
  arts_edt_dep_t *depv = (arts_edt_dep_t *)arts_get_depv(edt);
  uint32_t depc = edt->depc;
  const uint32_t *sorted = edt->rw_sorted; /* sorted ONCE in acquire_all */
  struct arts_edt_s *prev_acquiring = tl_acquiring;
  tl_acquiring = edt;
  while (edt->rw_cursor < depc) {
    uint32_t i = sorted[edt->rw_cursor];
    /* An alias is not on this walk.  It holds no acquisition of its own, so it
     * can neither wait on another EDT's release nor be waited on, and there is
     * nothing about it to order: the owner's resolve hands it the block's
     * payload (arts_db_fill_aliases).  Issuing an acquisition here instead
     * would, under a protocol that lets one holder at a time write, queue the
     * request behind the EDT's own unreleased hold and self-deadlock. */
    if (depv[i].alias || !dep_needs_acquire(depv, i) ||
        !arts_dep_is_serialized(depv, i)) {
      edt->rw_cursor++;
      continue;
    }
    tl_inline_advanced = false;
    /* A dependence fires only for an EDT that still owes an account: once
     * every account is in, the EDT is scheduled and its acquire state belongs
     * to its run, so a walk arriving here past that point would charge a
     * second acquisition to a task that is already running. */
    assert(edt->acquire_remaining > 0);
    acquire_one_dep(
        edt, depv,
        i); /* local hit advances the cursor; remote/contended parks */
    if (!tl_inline_advanced) {
      /* Parked (or deferred): the serving side owns the continuation — its
       * arts_db_rw_secure enqueues the flat resume on its own thread.  Do NOT
       * infer "resolved" from cursor movement: a concurrent other-thread serve
       * also advances the cursor, and continuing here would put two threads in
       * the same EDT's loop (double-firing the next dep). */
      break;
    }
    /* resolved in-frame — the loop picks up the next serialized dep */
  }
  tl_acquiring = prev_acquiring;
}

/* Async grant resume — see db.h.  Position-idempotent: only advances when the
 * cursor still points at `slot`, so a redundant secure for an already-passed
 * slot is a no-op (it neither re-fires the in-flight dep nor double-accounts).
 * This is the path a grant for a PARKED EDT takes (remote grant / release
 * grant).  The SYNCHRONOUS local resolve does NOT come through here; it uses
 * arts_db_acquire_resolved (advance only) and the running loop picks up the
 * next dep, so consecutive local resolves never recurse.
 *
 * The frame this runs in grows by nothing: the two outcomes are a thread-local
 * flag and an append to the worklist, neither of which fires a dependence. */
void arts_db_rw_secure(struct arts_edt_s *edt, unsigned int slot) {
  uint32_t depc = edt->depc;
  const uint32_t *sorted = edt->rw_sorted; /* sorted ONCE in acquire_all */
  /* A wake follows an acquisition the walk issued, so the walk has begun. */
  assert(sorted != NULL);
  if (edt->rw_cursor < depc && sorted[edt->rw_cursor] == slot) {
    edt->rw_cursor++;
    /* If this resume is for THIS thread's in-flight EDT (a same-rank grant
     * whose handler ran synchronously inside the running loop), signal the
     * loop to continue — it owns the continuation.  Otherwise it is a PARKED
     * EDT woken by a grant: enqueue it for the flat resume drain (NOT an
     * inline rw_fire_from_cursor, which would nest on the running loop =
     * recursion).  Exactly one side continues the loop, never both. */
    if (edt != tl_acquiring) {
      resume_enqueue(edt->guid);
    } else {
      tl_inline_advanced = true;
    }
  }
}

/* Resolve ONE alias slot onto the block's payload: the same bytes at the same
 * address, the slot's own buffer ref (release drops one per slot) and its own
 * descriptor pin.
 *
 * `subtype` is the owner's, because it is the acquisition's: for a coherent
 * block the payload is a buffer's data and carries a ref; for a non-coherent
 * (pinned) block it is the descriptor's inline storage and carries none.
 *
 * `pin_src` is a handle on the descriptor the payload belongs to, borrowed from
 * the caller.  INVARIANT: an alias never ends with a buffer ref and no
 * descriptor pin.  A buffer's last drop recycles it into its owning cache's
 * free-list, that cache lives inside the descriptor, and the slots release in
 * index order — so an unpinned alias above the owner would recycle into a cache
 * the owner's pin drop had already freed.  The pin is therefore COPIED from the
 * handle that resolved the payload, never looked up again: a lookup can miss
 * under a concurrent destroy while the payload and its descriptor are still
 * alive under the owner's own pin.
 *
 * The payload claim is the idempotent CAS a parked resume uses, so a slot
 * resolves exactly once however many frames meet it; the loser drops the ref it
 * took.  A NULL payload cannot be claimed that way — NULL is the resolved value
 * — so it is arbitrated on the descriptor pin instead, which exactly one frame
 * installs.  With no pin to install either (the block is gone) nothing can
 * arbitrate, and a destroyed block woken twice is outside the programming
 * model.
 *
 * Returns whether THIS call resolved the slot, i.e. whether the caller now
 * owes it an account.  Accounting is the caller's because it may schedule the
 * EDT, after which nothing may read its acquire state. */
static bool fill_one_alias(arts_edt_dep_t *depv, uint32_t slot, void *payload,
                           arts_db_types_t subtype,
                           arts_shared_ptr_t pin_src) {
  depv[slot].subtype = subtype;
  if (payload == NULL) {
    if (pin_src == NULL) {
      return true;
    }
    arts_shared_ptr_t pin_h = arts_shared_copy(pin_src);
    void *null_expected = NULL;
    if (!__atomic_compare_exchange_n(&depv[slot].db_pin, &null_expected,
                                     (void *)pin_h, false, __ATOMIC_RELEASE,
                                     __ATOMIC_RELAXED)) {
      arts_shared_release(&pin_h); /* another frame resolved the slot */
      return false;
    }
    return true;
  }
  if (subtype != ARTS_DB) {
    /* A non-coherent block's payload is its descriptor's own storage: no
     * buffer, no ref, and a release with nothing to give back. */
    void *pinned_expected = NULL;
    return __atomic_compare_exchange_n(&depv[slot].ptr, &pinned_expected,
                                       payload, false, __ATOMIC_ACQ_REL,
                                       __ATOMIC_ACQUIRE);
  }
#ifdef ARTS_FAM_DIRECT
  /* Header-relative arithmetic on the payload lands in the block's store, not
   * in a header, so the descriptor comes from the cache the caller has already
   * pinned.  pin_src is non-NULL here by arts_db_fill_aliases's rule — with no
   * descriptor to pin it hands out no bytes — and the owner slot's own ref
   * keeps this descriptor alive across the acquire.  The handle is OWNED, as
   * the copy below is, and it is taken before this frame touches the slot, so
   * a block that no longer names a descriptor takes the same shape here as any
   * other block with no bytes to hand out: NULL, and no ref. */
  struct arts_db_s *pin_db = (struct arts_db_s *)arts_shared_get(pin_src);
  arts_shared_ptr_t buf_h = arts_db_buf_acquire(&pin_db->cache);
  struct arts_db_buffer_s *ob =
      (struct arts_db_buffer_s *)arts_shared_get(buf_h);
  if (ob == NULL) {
    arts_db_buf_release(&buf_h);
    return fill_one_alias(depv, slot, NULL, subtype, pin_src);
  }
  if ((void *)ob->data != payload) {
    ARTS_ERROR("db: a block's descriptor names storage other than the payload "
               "an alias was handed");
  }
#endif
  /* The pin goes in BEFORE the payload: a slot takes a pin only while it
   * carries none, so publishing the pin first leaves exactly one pin however
   * two frames interleave.  Publish the other way round and both can read
   * db_pin == NULL with ptr already set, and one handle leaks. */
  arts_shared_ptr_t db_h = arts_shared_copy(pin_src);
  void *pin_expected = NULL;
  if (__atomic_compare_exchange_n(&depv[slot].db_pin, &pin_expected,
                                  (void *)db_h, false, __ATOMIC_RELEASE,
                                  __ATOMIC_RELAXED)) {
    db_h = NULL; /* the slot owns it now; release_one_dep drops it last */
  }
  arts_shared_release(&db_h); /* no-op when the slot took it */
#ifndef ARTS_FAM_DIRECT
  /* The ref is taken BEFORE the claim so the losing side's drop is symmetric.
   * It is a ref on the owner's buffer, not on whatever the block's cache holds
   * now, which is what makes the two slots the same address by construction. */
  arts_shared_ptr_t buf_h =
      arts_shared_copy(arts_db_buf_from_data(payload)->cb);
#endif
  void *expected = NULL;
  if (!__atomic_compare_exchange_n(&depv[slot].ptr, &expected, payload, false,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    arts_db_buf_release(&buf_h); /* another frame claimed the slot */
    return false;
  }
  /* Not counted: an alias is not an acquisition.  One block, one acquisition,
   * one census entry — the owner's. */
  return true;
}

/* Hand the block's payload to every slot that aliases the acquisition just
 * resolved on `owner_slot`.  This is the one place an alias is ever resolved,
 * on every arm: it runs where an owner's pointer becomes final — the
 * synchronous resolve (arts_db_acquire_resolved) and the parked resume
 * (resume_parked in coherence.c, behind arts_db_resume_parked and
 * mark_edt_ready_by_guid) — and nowhere else.
 *
 * `owner_db_h` is the descriptor handle the resolving frame holds for the
 * block, borrowed for the call, or NULL when the frame has none (the
 * synchronous resolve pins the owner's slot only after its handler returns).
 * With none, the block is looked up once here — and a MISS means the block was
 * destroyed under a pending acquire, which the model leaves undefined: the
 * aliases then resolve to NULL and take no buffer ref, the value and the shape
 * every other path delivers for a block with no storage.  A payload therefore
 * never reaches an alias without the descriptor it belongs to.
 *
 * One pass over depv in this frame, no recursion and no dispatch: an alias
 * needs nothing from the network, and the slots it fills were classified
 * before anything fired, so the walk is decided, not searched.
 *
 * The accounts are issued at the end.  An account may schedule the EDT, and
 * the loop must not read it after that; deferring them is safe because the
 * caller still owes the owner's own account, so the count cannot reach zero
 * while this loop runs. */
void arts_db_fill_aliases(struct arts_edt_s *edt, unsigned int owner_slot,
                          arts_shared_ptr_t owner_db_h) {
  arts_edt_dep_t *depv = (arts_edt_dep_t *)arts_get_depv(edt);
  arts_guid_t guid = depv[owner_slot].guid;
  if (guid == NULL_GUID) {
    /* The EDT released the block from inside its own body, which retires every
     * slot naming it — there is nothing left to alias. */
    return;
  }
  void *payload = __atomic_load_n(&depv[owner_slot].ptr, __ATOMIC_ACQUIRE);
  arts_db_types_t subtype = depv[owner_slot].subtype;
  arts_shared_ptr_t looked_up = NULL;
  arts_shared_ptr_t pin_src = owner_db_h;
  bool pin_known = (pin_src != NULL);
  /* A slot this loop resolves is a slot nobody has accounted, so the EDT
   * cannot have run yet and the owner's payload is alive under its own ref. */
  uint32_t owed = 0;
  for (uint32_t k = 0; k < edt->depc; k++) {
    if (!depv[k].alias || depv[k].guid != guid ||
        __atomic_load_n(&depv[k].ptr, __ATOMIC_ACQUIRE) != NULL) {
      continue;
    }
    if (!pin_known) {
      /* Paid once, and only by an EDT that has an alias to fill: the common
       * alias-free resolve never touches the route table here. */
      looked_up = arts_route_table_lookup_db(guid);
      pin_src = looked_up;
      if (pin_src == NULL) {
        payload = NULL; /* no descriptor to pin: hand out no bytes either */
      }
      pin_known = true;
    }
    if (fill_one_alias(depv, k, payload, subtype, pin_src)) {
      owed++;
    }
  }
  arts_shared_release(&looked_up);
  while (owed-- > 0) {
    arts_db_acquire_account(edt);
  }
}

/* A SYNCHRONOUS local resolve (handler set dep->ptr, in the running acquire
 * loop).  For a serialized dep, advance the cursor ONLY — the running
 * rw_fire_from_cursor loop picks up the next dep, so this never re-fires
 * recursively (that is the whole point of the loop: acquire_all is the
 * continuation unit, the loop walks deps, and only an ASYNC resume —
 * arts_db_rw_secure / the OoO replay — re-enters the loop).  Then account THIS
 * dep; the +1 bias on acquire_remaining keeps the count above zero until the
 * loop completes, so accounting order is free.  The upstream caller holds an
 * EDT ref across the loop, so a schedule from the final account cannot free the
 * EDT mid-loop. */
void arts_db_acquire_resolved(struct arts_edt_s *edt, unsigned int slot) {
  arts_edt_dep_t *depv = (arts_edt_dep_t *)arts_get_depv(edt);
  if (arts_dep_is_serialized(depv, slot)) {
    uint32_t depc = edt->depc;
    const uint32_t *sorted = edt->rw_sorted; /* sorted ONCE in acquire_all */
    if (edt->rw_cursor < depc && sorted[edt->rw_cursor] == slot) {
      edt->rw_cursor++; /* advance only; the loop fires the next dep */
      if (edt == tl_acquiring) {
        tl_inline_advanced = true; /* in-frame resolve: the loop continues */
      }
    }
  }
  /* The block's payload is final for this EDT: hand it to every slot that
   * aliases this acquisition BEFORE accounting the owner, because the owner's
   * account may be the one that schedules the EDT. */
  arts_db_fill_aliases(edt, slot, NULL);
  arts_db_acquire_account(
      edt); /* count THIS dep's data; outermost may schedule */
}

/* Driver: fire all non-serialized deps (Pass 1), then fire the serialized ones
 * from the cursor (Pass 2; the handler's resolved path self-continues the RW
 * chain). acquire_remaining is +1-biased so data arrivals during the fire
 * cannot schedule before the fire completes. Called once per EDT from
 * arts_handle_ready_edt. */
void arts_db_acquire_all(struct arts_edt_s *edt) {
  arts_edt_dep_t *depv = (arts_edt_dep_t *)arts_get_depv(edt);
  uint32_t depc = edt->depc;
  /* Order the deps and classify owner vs alias ONCE for the whole acquire
   * phase, BEFORE anything fires: both are pure functions of depv (fixed once
   * the EDT is ready), so rw_fire_from_cursor / arts_db_rw_secure /
   * arts_db_acquire_resolved / arts_db_fill_aliases all consume the one answer
   * and none re-derives it — turning the per-dep O(depc²) re-sort into a single
   * O(depc²), and giving every frame that meets a slot the same verdict about
   * which slot owns its block.  Freed with the EDT (arts_edt_free). */
  if (depc > 0 && edt->rw_sorted == NULL) {
    edt->rw_sorted = (uint32_t *)arts_malloc(depc * sizeof(uint32_t));
    arts_dep_sort_and_classify(depv, depc, edt->rw_sorted);
  }
  const uint32_t *sorted = edt->rw_sorted;

  uint32_t n = 0;
  for (uint32_t k = 0; k < depc; k++) {
    if (dep_needs_acquire(depv, sorted[k])) {
      n++;
    }
  }
  arts_atomic_add(&edt->acquire_remaining, n); /* now (1 + n) with the bias */

  /* Pass 1: every non-serialized real DB dep that OWNS its block's single
   * acquisition, order-independent (the handler self-accounts via
   * arts_db_acquire_resolved on a local hit; remote parks).  An alias asks for
   * nothing — the owner's resolve fills it (arts_db_fill_aliases). */
  for (uint32_t k = 0; k < depc; k++) {
    uint32_t i = sorted[k];
    if (depv[i].alias || !dep_needs_acquire(depv, i) ||
        arts_dep_is_serialized(depv, i)) {
      continue;
    }
    acquire_one_dep(edt, depv, i);
  }

  /* Pass 2: fire the serialized owners from the cursor. */
  rw_fire_from_cursor(edt);

  /* Remove the +1 bias; this decrement may be the one that reaches 0. */
  arts_db_acquire_account(edt);

  /* Drain any cross-EDT resumes enqueued while this EDT's loop ran (flat). */
  arts_db_drain_resume_list();
}

/*
 * prep_dbs — Prepare DB dependencies just before EDT execution.
 *
 * For each WRITE-mode dependency, invalidates remote route table entries
 * (marks other caches stale).  In GPU builds, for every LC (locally-coherent)
 * DB (regardless of access mode), acquires a reader lock and increments the
 * DB version counter; for DB_MODE_LC_SYNC deps specifically, syncs GPU
 * shadow copies.
 *
 * Called from arts_run_edt() after all DB pointers have been resolved.
 */
void prep_dbs(unsigned int depc, arts_edt_dep_t *depv, bool gpu) {
  (void)gpu;
  for (unsigned int i = 0; i < depc; i++) {
    arts_db_access_mode_t access_mode = depv[i].mode;
    if (depv[i].guid == NULL_GUID || depv[i].ptr == NULL) {
      continue;
    }
    /* For coherent ARTS_DB, dep->ptr is buf->data and pointer arithmetic to
     * recover db_s would not land on a db_s.  Skip via the subtype recorded
     * at acquire — NOT a borrowed (non-refcounted) route pointer, which would
     * use-after-free if a concurrent destroy freed the arts_db_s while we read
     * db->db_type through it.  Coherent ARTS_DB drives invalidation inside
     * the coherence layer; non-coherent pinned subtypes (ARTS_DB_PIN,
     * ARTS_DB_GPU_PIN, ARTS_DB_GPU, ARTS_DB_CXL) have no DB-level coherence
     * and fall through to their per-subtype prep (their dep->ptr is db+1, so
     * the recovery below is valid for them). */
    if (depv[i].subtype == ARTS_DB) {
      continue;
    }
#ifdef ARTS_USE_CXL
    {
      struct arts_db_s *db_cxl = ((struct arts_db_s *)depv[i].ptr) - 1;
      if (db_cxl->db_type == ARTS_DB_CXL) {
        arts_cxl_consumer_flush(db_cxl->cache.db_guid);
      }
    }
#endif
#ifdef ARTS_USE_GPU
    if (!gpu && access_mode != DB_MODE_LC_SYNC) {
      struct arts_db_s *db = ((struct arts_db_s *)depv[i].ptr) - 1;
      if (db->db_type == ARTS_DB_GPU) {
        arts_reader_lock(&db->reader, &db->writer);
        arts_atomic_add(&db->version, 1);
      }
    }

    if (!gpu && access_mode == DB_MODE_LC_SYNC) {
      struct arts_db_s *db = ((struct arts_db_s *)depv[i].ptr) - 1;
      ARTS_DEBUG("internalLCSync %lu %p", depv[i].guid, db);
      internal_lc_sync_gpu(depv[i].guid, db);
    }
#endif
  }
}

/*
 * release_one_dep — Single source of truth for "release one dep slot".
 *
 * Used by:
 *   - release_dbs (EDT epilogue, all dep slots)
 *   - arts_db_release Path 2 (mid-EDT release of one depv slot)
 *   - arts_db_release Path 1 + arts_release_created_dbs (via a synthetic
 *     dep built from a created_db_list entry)
 *
 * Per access mode:
 *   - DB_MODE_RO / DB_MODE_RW: only ARTS_DB needs DB-level coherence
 *     work; route through the coherent release entry points
 *     (arts_db_release_ro / arts_db_release_rw).  Non-coherent pinned
 *     subtypes have no DB-level coherence — release is a no-op.
 *   - ARTS_DB_GPU subtype (GPU build, non-LC_SYNC mode): release the GPU-LC
 *     reader lock — pure intra-rank multi-device coordination.
 *   - ARTS_DB_CXL subtype: producer-flush and return.
 *
 * Does NOT nullify caller-visible state (guid/ptr/mode).  Callers that
 * need to mark the slot as released (mid-EDT release) do that themselves.
 */
static void release_one_dep(arts_edt_dep_t *dep, bool gpu) {
  arts_db_access_mode_t access_mode = dep->mode;
  if (access_mode == DB_MODE_NULL) {
    return;
  }

  /* Coherent release path for ARTS_DB.  Routed by dep->subtype (recorded at
   * acquire), NOT by recovering the subtype from dep->ptr: for a coherent DB
   * dep->ptr is cache->buffer->data (NOT (db+1)), so the pinned-subtype
   * pointer arithmetic below would read a wild address — fatal once a
   * concurrent destroy has removed the route entry (cache lookup then misses).
   * The EDT's per-acquire buffer ref (taken at acquire time: acquire_local /
   * the parked-EDT resume each do arts_db_buf_acquire) is dropped at the tail
   * of this block via the buffer's own cb, on every path on which the block
   * still names that buffer: the EDT's ref kept the buffer (hence buf->cb)
   * alive up to there, so the deref is never use-after-free even under a
   * racing destroy.  Dispatch release_rw
   * / release_ro only while the cache is still installed; once destroyed there
   * is no publish / version work left to do (that ref drop is the only cleanup
   * needed). */
  if (dep->subtype == ARTS_DB &&
      (access_mode == DB_MODE_RO || access_mode == DB_MODE_RW)) {
    /* B1: take the stashed descriptor pin (set when this slot's buffer ref was
     * secured at acquire).  It is released LAST — after the arm's release and
     * after the EDT's buffer ref — because the pin outlives the buffer ref by
     * contract: a buffer's last drop RECYCLES it into its owning cache's
     * free-list, so dropping the pin first could free that cache (and its
     * free-list) and turn the recycle into a write-after-free. */
    /* Consume the descriptor pin published (with release) at the acquire/wake
     * site; acquire-load matches that release across the work-stealing handoff
     * (mirrors the sibling ptr field's atomic discipline — TSan-clean). */
    arts_shared_ptr_t db_pin =
        (arts_shared_ptr_t)__atomic_load_n(&dep->db_pin, __ATOMIC_ACQUIRE);
    __atomic_store_n(&dep->db_pin, NULL, __ATOMIC_RELAXED);
    /* The re-lookup handle of the no-pin path, released at the same tail and
     * under the same rule. */
    arts_shared_ptr_t db_fallback = NULL;
    /* Alias slot (a slot naming a DB whose single acquisition another slot of
     * the same EDT owns): it took a buffer ref when that acquisition's payload
     * was handed to it (so the tail drop balances it) but never a coherence
     * hold.  Skip release_rw/ro so the hold is dropped exactly once per
     * distinct DB.  The alias bit is decided before the acquire phase fires
     * (arts_dep_sort_and_classify), NOT re-derived here, so a mid-EDT release
     * that nulls the owning slot's GUID cannot make an alias masquerade as the
     * owner.  Still drop the alias's own pin. */
    if (!dep->alias) {
      struct arts_db_s *db = NULL;
      if (db_pin != NULL) {
        /* Resolve the descriptor via the B1 pin — guaranteed alive (it kept
         * the cache pinned across the whole span), so it is immune to the
         * destroyed-but-lingering-DB re-lookup miss the old path risked. */
        db = (struct arts_db_s *)arts_shared_get(db_pin);
      } else if (dep->guid != NULL_GUID) {
        /* No pin stashed, so this slot took no buffer ref either: its resolved
         * value is NULL, or the block was destroyed before a ref was taken.  A
         * pin-less slot that DOES carry a ref is a runtime-invariant break,
         * since every site that takes the ref installs the pin in the same
         * frame.  What is left to do here is the arm's release, for which the
         * ref-counted route re-lookup is correct for the held writer_count; it
         * only loses the destroy-race robustness the pin provides. */
        db_fallback = arts_route_table_lookup_db(dep->guid);
        db = (struct arts_db_s *)arts_shared_get(db_fallback);
      }
      if (db != NULL && db->db_type == ARTS_DB) {
        if (access_mode == DB_MODE_RW) {
          arts_db_release_rw(&db->cache, dep->ptr);
        } else {
          arts_db_release_ro(&db->cache);
        }
      }
    }
    /* Drop the EDT's per-acquire buffer ref AFTER the arm's release: an arm
     * whose release moves the EDT's own bytes needs them alive until its
     * transfer has completed.  An alias slot takes this path too — it holds
     * a ref of its own, it just never held the coherence hold. */
    if (dep->ptr != NULL) {
#ifdef ARTS_FAM_DIRECT
      /* Header-relative arithmetic on dep->ptr lands in the shared store, not
       * in a header, so the descriptor is the one this slot's own pin names:
       * every site that takes this ref installs that pin in the same frame, so
       * the two name one object.  A cache whose descriptor slot has already
       * been cleared answers NULL and this ref is then not dropped — one
       * descriptor left behind until teardown, on the path where a block is
       * destroyed under a live holder, which the model leaves undefined. */
      if (db_pin == NULL) {
        ARTS_ERROR("db: a resolved dep carries a buffer ref with no "
                   "descriptor pin");
      }
      struct arts_db_s *pin_db = (struct arts_db_s *)arts_shared_get(db_pin);
      struct arts_db_buffer_s *buf =
          arts_db_buf_for_payload(&pin_db->cache, dep->ptr);
#else
      struct arts_db_buffer_s *buf = arts_db_buf_from_data(dep->ptr);
#endif
      if (buf != NULL) {
        arts_shared_ptr_t buf_cb = buf->cb;
        arts_db_buf_release(&buf_cb);
      }
    }
    /* The descriptor pin LAST, on every path: the drop above may have been the
     * buffer's last, and a buffer's deleter recycles it into its owning
     * cache's free-list — which lives inside the descriptor this pin holds. */
    arts_shared_release(&db_pin);
    arts_shared_release(&db_fallback);
    return;
  }

  /* Get DB subtype from struct when ptr is available.  Guard with
   * guid != NULL_GUID because arts_db_release may have already nulled
   * the guid while leaving ptr non-NULL (caller responsibility).
   *
   * Reaching this point means the dep is for a non-coherent pinned subtype
   * (ARTS_DB_PIN, ARTS_DB_GPU_PIN, ARTS_DB_GPU, ARTS_DB_CXL) or a special
   * access mode (LC_*, MEMSET) — none of which carry DB-level
   * coherence. */
  arts_db_types_t db_subtype = ARTS_DB;
  if (dep->guid != NULL_GUID && dep->ptr) {
    struct arts_db_s *db_hdr = ((struct arts_db_s *)dep->ptr) - 1;
    db_subtype = db_hdr->db_type;
  }

  ARTS_DEBUG("Releasing DB[Guid:%lu] [AccessMode:%s, DbSubtype:%s]", dep->guid,
             GET_DB_MODE_NAME(access_mode), GET_DB_TYPE_NAME(db_subtype));

#ifdef ARTS_USE_CXL
  if (db_subtype == ARTS_DB_CXL) {
    if (dep->guid != NULL_GUID && dep->ptr &&
        (access_mode == DB_MODE_RW
#ifdef ARTS_USE_GPU
         || access_mode == DB_MODE_MEMSET
#endif
        )) {
      arts_cxl_producer_flush(dep->guid);
    }
    return; /* CXL: no route table, HW MESI handles intra-node coherence */
  }
#endif

  if (!gpu && db_subtype == ARTS_DB_GPU) {
    if (dep->ptr) {
      struct arts_db_s *db = ((struct arts_db_s *)dep->ptr) - 1;
      arts_reader_unlock(&db->reader);
    }
  }
  /* PIN / GPU_PIN / regular RW or RO on non-coherent subtypes: nothing to
   * release at the DB-coherence level.  Hardware coherence and
   * application-level event ordering handle the rest. */
}

/*
 * release_dbs — Release DB dependencies after EDT execution completes.
 * Thin loop over depv calling the single-source-of-truth release_one_dep.
 */
void release_dbs(unsigned int depc, arts_edt_dep_t *depv, bool gpu) {
  for (uint32_t i = 0; i < depc; i++) {
    /* Alias-vs-owner is decided before the acquire phase fires
     * (arts_dep_sort_and_classify): an alias slot drops only its buffer ref,
     * the owner also releases the single coherence hold.  See
     * arts_edt_dep_t.alias / release_one_dep. */
    release_one_dep(&depv[i], gpu);
  }
}

/*
 * release_one_created — Release a single created DB by GUID.
 *
 * Looks up the DB struct via the route table; for ARTS_DB the creator's hold
 * is the arm's own state, seeded at ARTS_DB_INIT_CREATOR_HOME /
 * ARTS_DB_INIT_CREATOR_REMOTE and released through the arm's own entry point
 * with NO buffer ref to drop (auto_acquire never called acquire_buf).  For
 * non-coherent pinned subtypes (ARTS_DB_PIN, ARTS_DB_GPU_PIN, ARTS_DB_GPU,
 * ARTS_DB_CXL) the creator EDT has no DB-level coherence hold to drop;
 * building a synthetic RW-mode dep and dispatching through release_one_dep
 * handles only the per-mode non-coherence work (GPU-LC reader unlock, CXL
 * producer flush).
 */
static void release_one_created(arts_guid_t guid, arts_db_access_mode_t mode) {
  arts_shared_ptr_t db_h = arts_route_table_lookup_db(guid);
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(db_h);
  if (!db) {
    return;
  }
  if (db->db_type == ARTS_DB) {
    /* Coherent creator release.  No buffer ref to drop (auto_acquire is a
     * no-op for ARTS_DB — the hold is the seed arts_db_cache_init stamped on
     * the cache).  The arm's create-hold entry point runs it: the creating
     * EDT's bytes are wherever that arm put them when the block was made,
     * which is not something a dependence's pointer could name.  The coherent
     * creator hold is always RW, so `mode` only steers the pinned-subtype
     * synthetic dep below. */
    arts_db_release_created(&db->cache);
    arts_shared_release(&db_h);
    return;
  }
  arts_edt_dep_t synthetic = {
      .guid = guid,
      .ptr = (void *)(db + 1),
      .mode = mode,
      .subtype =
          db->db_type, /* pinned subtype (coherent ARTS_DB returned above) */
      .alias = false,
  };
  release_one_dep(&synthetic, false);
  arts_shared_release(&db_h);
}

/*
 * arts_db_release — Release access to a single DB mid-EDT.
 *
 * Two search paths:
 *   1. created_db_list — DBs the current EDT created (auto-acquired EW).
 *   2. depv — DBs received as dependencies (EW or RO mode).
 *
 * Both paths funnel through release_one_dep / release_one_created so the
 * EW / RO / LOCAL / LC / CXL / route-table-ref rules live in exactly one
 * place.  The slot/entry is marked released after the unwind so the
 * epilogue (release_dbs / arts_release_created_dbs) skips it cleanly.
 */
void arts_db_release(arts_guid_t guid, arts_db_access_mode_t mode) {
  /* Path 1: created_db_list (DBs this EDT created).  Scanned from the back
   * because the common shape is create-then-release, and REMOVED on a match
   * rather than blanked: the list has to track what is still HELD, not what was
   * ever created, or a release that finds nothing walks every entry the EDT
   * ever made and d releases cost d^2. */
  arts_vector_t *list = arts_get_created_db_list();
  uint64_t count = arts_vector_count(list);
  for (uint64_t i = count; i > 0; i--) {
    arts_guid_t *g = (arts_guid_t *)arts_vector_at(list, i - 1);
    if (*g == guid) {
      arts_vector_swap_remove(list, i - 1);
      release_one_created(guid, mode);
      return;
    }
  }

  /* Path 2: depv (dependency-acquired DBs).
   *
   * An EDT acquires each distinct block once and every other slot naming it is
   * an alias on that one acquisition, so a block has exactly ONE non-alias slot
   * in a dependence vector and that slot is what this call releases.  Releasing
   * an alias instead would release nothing — an alias carries no coherence hold
   * — and leave the acquisition held until the epilogue, exposing the block (a
   * satisfy later in the same body) before the release that carries the bytes
   * home has run.  The aliases retire with the owner: each still drops its own
   * buffer ref and pin exactly once, and the epilogue then skips every slot
   * naming the block.
   *
   * `mode` therefore selects nothing among the slots that SHARE the block's
   * acquisition (DB_MODE_RO / DB_MODE_RW): the owner's acquisition is the
   * strongest mode the EDT asked for and is the only one there is.  A
   * device-side internal mode is not part of that acquisition and is not
   * classified, so it is never the owner while a coherent slot names the same
   * block: the search takes the first RO/RW slot and falls back to an
   * unclassified one only when the block has no coherent acquisition here.
   * Releasing the internal-mode slot first would retire the acquisition's
   * aliases while the acquisition itself stayed held, so the bytes would reach
   * the home later than the program asked. */
  if (!current_edt) {
    return;
  }
  arts_edt_dep_t *depv = (arts_edt_dep_t *)arts_get_depv(current_edt);
  uint32_t depc = current_edt->depc;
  uint32_t owner = depc;
  uint32_t unclassified = depc;
  for (uint32_t i = 0; i < depc; i++) {
    if (depv[i].guid != guid || depv[i].mode == DB_MODE_NULL ||
        depv[i].alias) {
      continue;
    }
    if (depv[i].mode == DB_MODE_RO || depv[i].mode == DB_MODE_RW) {
      owner = i;
      break;
    }
    if (unclassified == depc) {
      unclassified = i;
    }
  }
  if (owner == depc) {
    owner = unclassified;
  }
  if (owner == depc) {
    return;
  }
  release_one_dep(&depv[owner], false);
  depv[owner].guid = NULL_GUID;
  depv[owner].ptr = NULL;
  depv[owner].mode = DB_MODE_NULL;
  for (uint32_t i = 0; i < depc; i++) {
    if (i == owner || depv[i].guid != guid || depv[i].mode == DB_MODE_NULL ||
        !depv[i].alias) {
      continue;
    }
    release_one_dep(&depv[i], false); /* buffer ref + descriptor pin only */
    depv[i].guid = NULL_GUID;
    depv[i].ptr = NULL;
    depv[i].mode = DB_MODE_NULL;
  }
}

/*
 * arts_release_created_dbs — EDT epilogue helper: release every entry in
 * the thread-local created_db_list that hasn't already been explicitly
 * released by arts_db_release.
 */
void arts_release_created_dbs(void) {
  /* Epilogue: whatever is still on the list was never released.  Drain from
   * the back so each removal is the cheap case and the walk stays linear.
   * The trip count is taken ONCE: a release must not grow this list, and a
   * bounded loop turns that contract violation into a leftover entry the
   * next EDT's start reports, rather than a livelock here. */
  arts_vector_t *list = arts_get_created_db_list();
  for (uint64_t remaining = arts_vector_count(list); remaining > 0;
       remaining--) {
    uint64_t last = arts_vector_count(list) - 1;
    arts_guid_t g = *(arts_guid_t *)arts_vector_at(list, last);
    arts_vector_swap_remove(list, last);
    release_one_created(g, DB_MODE_RW);
  }
}

/*
 * arts_wait_release_dbs / arts_wait_reacquire_dbs -- Pre-/post-yield
 * hooks invoked around arts_event_wait.
 *
 * No-op under the OCR model: multi-EDT same-rank concurrent acquire is
 * allowed (writer_count CAS-loop), so the creator's hold persists across the
 * yield and is dropped exactly once at EDT epilogue via
 * arts_release_created_dbs.  Pinned subtypes have no DB-level coherence to
 * drop either.  Kept as stable hooks for future per-EDT release semantics.
 */
void arts_wait_release_dbs(void) {}
void arts_wait_reacquire_dbs(void) {}

/* ── CXL cache-flush helpers ────────────────────────────────────────────────
 */

#ifdef ARTS_USE_CXL
void arts_cxl_producer_flush(arts_guid_t guid) {
  struct arts_db_s *db = (struct arts_db_s *)arts_cxl_get_ptr(guid);
  FLUSH_FENCE_PRODUCER(db, ALIGN_UP(arts_db_total_size(db), CACHELINE_SIZE));
}

void arts_cxl_consumer_flush(arts_guid_t guid) {
  struct arts_db_s *db = (struct arts_db_s *)arts_cxl_get_ptr(guid);
  /* First flush the struct to read the actual size (db_size in the cache). */
  FLUSH_FENCE_CONSUMER(db, ALIGN_UP(sizeof(struct arts_db_s), CACHELINE_SIZE));
  /* Then flush the full DB (struct + payload). */
  if (db->cache.db_size > 0) {
    FLUSH_FENCE_CONSUMER(db, ALIGN_UP(arts_db_total_size(db), CACHELINE_SIZE));
  }
}
#endif /* ARTS_USE_CXL */
