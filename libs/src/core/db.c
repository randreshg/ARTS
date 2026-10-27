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

#ifdef ARTS_DEBUG_DB_QUARANTINE
#include <sanitizer/asan_interface.h>
#endif

#include "arts.h"
#include "arts/coherence/buffer.h"
#include "arts/coherence/census.h"
#include "arts/coherence/coherence.h"
#include "arts/coherence/handlers.h"
#include "arts/counter/Preamble.h"
#include "arts/edt.h"
#include "arts/edt_context.h" /* current_edt + created-DB tracking */
#include "arts/gas/guid.h"
#include "arts/gas/route_table.h"
#include "arts/ooo.h"
#include "arts/runtime_state.h"
#include "arts/runtime_types.h"
#include "arts/system/print.h"
#include "arts/system/threads.h"
#include "arts/transport/protocol.h"
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
     * buf->data is the buffer's payload and stays valid for the single owner that
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
 * For the GPU kinds (GPU_PIN, GPU): there is no DB-level coherence to
 * track; the creator just owns the pointer until it explicitly destroys or
 * hands it off via events.
 *
 * In both cases the descriptor's handle `h` (its ref moves in) is recorded on
 * the creating THREAD's created-DB list, and whoever drains that list drives
 * the matching release: the EDT epilogue for a task, or the thread's own
 * scheduler entry for a startup hook, which runs before any task on that
 * thread.  The list is thread-local and the release chain needs no task
 * context, so a create outside a task is tracked exactly like one inside it —
 * the alternative leaves the hold stamped and nothing owning its release.
 */
static void arts_db_auto_acquire(arts_shared_ptr_t h) {
  arts_track_created_db(h);
}

/* Bytes a descriptor allocation must span.  A coherent DB's payload is its
 * coherence buffer, allocated apart from the descriptor; every other subtype
 * keeps its payload inline after the struct. */
static inline uint64_t db_descriptor_span(arts_db_types_t db_type,
                                          uint64_t len) {
  return sizeof(struct arts_db_s) + ((db_type == ARTS_DB) ? 0 : len);
}

/* The descriptor allocation.  A GPU kind carries its payload inline, lives
 * on the rank that created it and names the memory that payload has to come
 * from, so the subtype also picks the allocator; a coherent descriptor is
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
  if (!ptr) {
    /* The descriptor sits at offset 0 of the allocation and is shared across
     * threads, so it must start on a cache line. */
    ptr = arts_malloc_aligned(span, ARTS_CACHE_LINE_SIZE);
  }
  return ptr;
}

#ifdef ARTS_DEBUG_DB_QUARANTINE
/* A freed coherent descriptor is filled and held back for the next
 * DB_QUARANTINE_SLOTS frees made by the same thread before the allocator gets
 * it, and its fill is checked when it leaves.  A reference that outlives the
 * descriptor and writes through it changes the fill and is reported, instead
 * of landing in whatever object the allocator places there next; a write that
 * comes after the descriptor has left goes unseen, so the window is counted in
 * frees, not in time.  The descriptors still held at teardown are checked once
 * no completion can run any more.  The whole allocation is filled, a home
 * descriptor's directory as much as a stub's cache.  Under AddressSanitizer a
 * held descriptor is poisoned too, so a stray access is reported where it
 * happens, with its stack.  Each thread holds its own frees, so the quarantine
 * orders no thread's free with another's: a thread sanitizer sees no
 * synchronization between them that the program does not have. */
#define DB_QUARANTINE_SLOTS 1024u
#define DB_FREED_FILL 0xDF
#define DB_FREED_WORD ((arts_guid_t)0xDFDFDFDFDFDFDFDFull)

struct db_quarantine_ring_s {
  struct db_quarantine_ring_s *next; /* every thread's ring, for the drain */
  unsigned int head;
  struct arts_db_s *slot[DB_QUARANTINE_SLOTS];
};
static ARTS_THREAD_LOCAL struct db_quarantine_ring_s *tl_db_quarantine;
/* Every thread's ring, pushed once per thread and taken whole by the drain.
 * Only its own value: a ring's contents reach the drain through the joins
 * that precede it. */
static _Atomic(struct db_quarantine_ring_s *) g_db_quarantine_rings;

static void db_quarantine_release(struct arts_db_s *db) {
  size_t span = arts_malloc_usable_size(db);
  ASAN_UNPOISON_MEMORY_REGION(db, span);
  const unsigned char *p = (const unsigned char *)db;
  for (size_t i = 0; i < span; i++) {
    if (p[i] != DB_FREED_FILL) {
      ARTS_ERROR("freed data-block descriptor %p was written at byte %zu",
                 (void *)db, i);
    }
  }
  arts_free(db);
}
#endif

void arts_db_descriptor_free(struct arts_db_s *db) {
  if (db == NULL) {
    return;
  }
#ifdef ARTS_DEBUG_DB_QUARANTINE
  /* A live descriptor's GUID word names a data block, so it is never the fill
   * word; a held one's always is, the fill below being its store.  Two racing
   * frees of one descriptor are a data race a thread sanitizer reports; under
   * AddressSanitizer a later second free is reported by the load of the
   * poisoned word. */
  if (db->cache.db_guid == DB_FREED_WORD) {
    ARTS_ERROR("data-block descriptor %p freed twice", (void *)db);
  }
  size_t span = arts_malloc_usable_size(db);
  memset(db, DB_FREED_FILL, span);
  ASAN_POISON_MEMORY_REGION(db, span);
  struct db_quarantine_ring_s *ring = tl_db_quarantine;
  if (ring == NULL) {
    ring = (struct db_quarantine_ring_s *)arts_calloc(
        1, sizeof(struct db_quarantine_ring_s));
    ring->next =
        atomic_load_explicit(&g_db_quarantine_rings, memory_order_relaxed);
    while (!atomic_compare_exchange_weak_explicit(
        &g_db_quarantine_rings, &ring->next, ring, memory_order_relaxed,
        memory_order_relaxed)) {
    }
    tl_db_quarantine = ring;
  }
  struct arts_db_s *evicted = ring->slot[ring->head];
  ring->slot[ring->head] = db;
  ring->head = (ring->head + 1u) % DB_QUARANTINE_SLOTS;
  if (evicted != NULL) {
    db_quarantine_release(evicted);
  }
#else
  arts_free(db);
#endif
}

#ifdef ARTS_DEBUG_DB_QUARANTINE
void arts_db_descriptor_quarantine_drain(void) {
  /* The caller has joined every other thread that can free a descriptor, so
   * each ring is complete and visible here; when the shutdown gave up joining
   * one, that thread may still free into its ring, so no ring is touched. */
  if (atomic_load_explicit(&arts_shutdown_abandon.unjoined_threads,
                           memory_order_relaxed) != 0) {
    ARTS_WARN("freed descriptors left unchecked: a thread never joined");
    return;
  }
  struct db_quarantine_ring_s *ring = atomic_exchange_explicit(
      &g_db_quarantine_rings, NULL, memory_order_relaxed);
  while (ring != NULL) {
    for (unsigned int i = 0; i < DB_QUARANTINE_SLOTS; i++) {
      if (ring->slot[i] != NULL) {
        db_quarantine_release(ring->slot[i]);
      }
    }
    struct db_quarantine_ring_s *next = ring->next;
    arts_free(ring);
    ring = next;
  }
  tl_db_quarantine = NULL;
}
#endif

void arts_db_free(void *ptr) {
  struct arts_db_s *db = (struct arts_db_s *)ptr;
  /* Chain into coherence cache teardown if this DB has one.  Only ARTS_DB
   * carries coherence state; other subtypes leave the embedded cache zeroed.
   * The cache is embedded by value as the first member of db_s, so the
   * destructor tears down its sub-resources (the buffer slot's shared_ptr ref +
   * home_s) in place, and the storage release reclaims the db_s. */
  if (db->db_type == ARTS_DB) {
    arts_db_cache_destructor(&db->cache);
    arts_db_buf_storage_release(&db->cache);
    return;
  }
#ifdef ARTS_USE_GPU
  if (db->db_type == ARTS_DB_GPU) {
    pthread_rwlock_destroy(db->gpu_lock);
    arts_free(db->gpu_lock);
  }
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

#ifdef ARTS_USE_GPU
/* The staging lock of an ARTS_DB_GPU block.  Writer-preferring: staging and
 * the LC merges must not starve behind CPU EDTs that keep the read side
 * across their whole bodies. */
static pthread_rwlock_t *db_gpu_lock_create(void) {
  pthread_rwlock_t *lock =
      (pthread_rwlock_t *)arts_malloc(sizeof(pthread_rwlock_t));
  pthread_rwlockattr_t attr;
  pthread_rwlockattr_init(&attr);
  pthread_rwlockattr_setkind_np(&attr,
                                PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
  if (pthread_rwlock_init(lock, &attr) != 0) {
    ARTS_ERROR("db: the GPU staging lock could not be initialized");
  }
  pthread_rwlockattr_destroy(&attr);
  return lock;
}
#endif

/*
 * db_create_in_place — Initialize a DB header in pre-allocated memory.
 *
 * Sets up the arts_db_s header fields (type, size, the GPU staging state,
 * db_list) and records metrics.  The caller is responsible for
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
#ifdef ARTS_GPU_LAYOUT
  atomic_store_explicit(&db_res->version, 0u, memory_order_relaxed);
  db_res->time_stamp = 0;
  db_res->gpu_lock = NULL;
#endif
#ifdef ARTS_USE_GPU
  if (db_type == ARTS_DB_GPU) {
    db_res->gpu_lock = db_gpu_lock_create();
  }
#endif
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
  /* Every DB — coherent or not — records its payload length in the cache.
   * Size lives here (cache->db_size), not in a separate per-object header: a
   * DB always carries its own length. */
  struct arts_db_cache_s *cache = &db_res->cache;
  /* The allocation is not necessarily zeroed; zero the embedded cache before
   * in-place init. */
  memset(cache, 0, sizeof(*cache));
  atomic_store_explicit(&cache->db_size, len, memory_order_relaxed);
  cache->db_guid = guid;
  if (db_type == ARTS_DB) {
    arts_db_cache_init(cache, guid, len, ARTS_DB_INIT_CREATOR_HOME,
                       arts_global_rank_id);
#ifdef ARTS_USE_CXL
    /* The block's slot, in place while the cache is still private:
     * the caller's install replays whatever this rank deferred for the GUID
     * and a replayed request can be granted at once, the pointer this create
     * hands out may BE the slot, and the buffer install below adopts the slot
     * on the residency that keeps no copy.  After the cache's init, which is
     * what zeroes the field, and before both.  Inside the coherent branch on
     * purpose — a kind that keeps no coherence state has no funnel to read a
     * slot. */
    (void)arts_db_cxl_slot_create(cache);
#endif
    /* Install a fresh buffer so subsequent coherent acquires
     * (acquire_local / arts_db_resolve_slot_cached) find a non-NULL
     * cache->buffer.  The user pointer returned by arts_db_create
     * points into this buffer's payload, so writes by the creator
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
  /* The GPU kinds (ARTS_DB_GPU_PIN, ARTS_DB_GPU) live on the creator rank
   * and have no DB-level coherence.  The embedded cache stays zeroed and
   * db_list stays NULL. */
#ifdef ARTS_USE_GPU
  if (db_type == ARTS_DB_GPU) {
    /* The shadow descriptor: this subtype keeps no coherence state, so the
     * zero fill is every atomic member's initial value. */
    struct arts_db_s *shadow =
        (struct arts_db_s *)(((char *)addr) + sizeof(struct arts_db_s) + len);
    memset(shadow, 0, sizeof(*shadow));
    shadow->db_type = db_type;
    atomic_store_explicit(&shadow->cache.db_size, len, memory_order_relaxed);
    shadow->cache.db_guid = guid;
  }
#endif
}

/* A coherent block created with ARTS_DB_PROP_NO_ACQUIRE on its home rank:
 * the creator never acquires or releases, so the home is the sole idle
 * owner.  db_create_in_place seeded the creator's hold, but nothing tracks or
 * releases a hold for a create that takes none, so the seed is undone while
 * the descriptor is still private.  Left in place, the unreleased hold blocks
 * every future writer. */
static void db_create_no_acquire_local(struct arts_db_s *db) {
#if defined(ARTS_PROTOCOL_EXCL)
#if defined(ARTS_RELEASE_RETAIN)
  /* RETAIN release policy: data lives with the owner, not the home — with no
   * creator hold there is no owner unless we make one.  This rank (the GUID
   * home, where a local create runs) becomes the IDLE data owner with
   * owner-bit set but rw_st=IDLE, wc=0.  Its buffer does not exist yet: with
   * no creator to place it for, the first user allocates it — a remote writer
   * in the buffer it prepares for the migration, which then carries the size
   * and no bytes, or the acquiring thread when the first user is local.
   * lock_state is the idle directory naming this rank as owner. */
  atomic_store_explicit(&db->cache.cache_state,
                        CACHE_MAKE_FULL(1u, CACHE_ST_IDLE, CACHE_ST_IDLE,
                                        ARTS_EXCL_NO_TARGET, 0u, 0u),
                        memory_order_relaxed);
  atomic_store_explicit(&db->lock_state,
                        LOCK_MAKE(EXCL_PHASE_IDLE, arts_global_rank_id, 0u, 0u),
                        memory_order_relaxed);
#else  /* ARTS_RELEASE_PURGE */
  /* PURGE release policy: the home holds the canonical buffer; undo the
   * create-time creator RW seed → free lock, so the first acquirer is granted
   * rather than blocked behind a hold no EDT will ever release. */
  atomic_store_explicit(&db->cache.cache_state, 0ULL, memory_order_relaxed);
  atomic_store_explicit(&db->lock_state, 0ULL, memory_order_relaxed);
#endif /* ARTS_RELEASE_* */
#elif defined(ARTS_PROTOCOL_FLUSH)
  /* An arm that keeps no permission state has no seed to undo. */
#else
  /* Grant-bearing arms: this rank becomes the idle owner — possession, and no
   * hold under it.  With no creator hold there is nothing to release, so the
   * first foreign request finds an idle grant rather than queueing behind a
   * hold nobody will ever drop. */
  atomic_store_explicit(&db->cache.writer_count, ARTS_GRANT_SEED_IDLE,
                        memory_order_relaxed);
#endif
  /* The creator was given no storage, so any create-time claim to a reader
   * copy of it is false and goes with the write hold. */
  arts_db_create_retract_creator_copy(db);
}

/* Remote creates in progress on this rank, by GUID — deciding, or parked.
 * An acquire that finds this rank's slot for the GUID empty while such a create
 * waits must not install a first-touch cache there: the create would park
 * behind it, and the request that cache sends waits at the home for the
 * announce the create sends only once it installs.  The acquire defers on the
 * slot instead and runs against the create's descriptor when it installs.
 * Parking a remote create is rare, so the table is scanned only while it is
 * non-empty; its segments are only ever appended, and an entry is a GUID
 * claimed from 0 and returned to 0.
 *
 * A registration and the acquire that must see it form a store->load pair
 * across the GUID's route slot: the create registers and then looks the slot
 * up, the acquire looks the slot up and, on a miss, reads the table.  An
 * acquire can miss because a retire of the slot's previous occupant is under
 * way, having synchronised with nothing the create wrote.  A seq_cst fence on
 * each side closes the pair (db_create_remote_creator, acquire_one_dep): the
 * create's lookup read the slot before the retire wrote it, the acquire's
 * lookup read the retire's write, so the create's fence precedes the
 * acquire's and the acquire's table read cannot miss the registration.  The
 * table's own accesses carry nothing else. */
#define DB_PARKED_SEG 64u
struct db_parked_seg_s {
  /* Claimed 0 -> GUID by a registering create and returned to 0 by CAS; only
   * their own value. */
  _Atomic(arts_guid_t) guid[DB_PARKED_SEG];
  /* Publishes a zero-filled segment: CAS acq_rel, loads acquire. */
  _Atomic(struct db_parked_seg_s *) next;
};
static struct db_parked_seg_s db_parked_head;
/* Entries in use: a gate that lets a lookup skip the scan; only its own
 * value. */
static _Atomic(uint64_t) db_parked_count;

static void db_parked_add(arts_guid_t guid) {
  atomic_fetch_add_explicit(&db_parked_count, 1u, memory_order_relaxed);
  for (struct db_parked_seg_s *seg = &db_parked_head;;) {
    for (unsigned int i = 0; i < DB_PARKED_SEG; i++) {
      arts_guid_t expect = NULL_GUID;
      if (atomic_compare_exchange_strong_explicit(&seg->guid[i], &expect, guid,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
        return;
      }
    }
    struct db_parked_seg_s *next =
        atomic_load_explicit(&seg->next, memory_order_acquire);
    if (next == NULL) {
      struct db_parked_seg_s *fresh =
          (struct db_parked_seg_s *)arts_calloc(1, sizeof(*fresh));
      if (atomic_compare_exchange_strong_explicit(&seg->next, &next, fresh,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
        next = fresh;
      } else {
        arts_free(fresh);
      }
    }
    seg = next;
  }
}

static void db_parked_remove(arts_guid_t guid) {
  if (atomic_load_explicit(&db_parked_count, memory_order_relaxed) == 0) {
    return;
  }
  for (struct db_parked_seg_s *seg = &db_parked_head; seg != NULL;
       seg = atomic_load_explicit(&seg->next, memory_order_acquire)) {
    for (unsigned int i = 0; i < DB_PARKED_SEG; i++) {
      arts_guid_t expect = guid;
      if (atomic_compare_exchange_strong_explicit(&seg->guid[i], &expect,
                                                  NULL_GUID,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
        atomic_fetch_sub_explicit(&db_parked_count, 1u, memory_order_relaxed);
        return;
      }
    }
  }
}

static bool db_parked_here(arts_guid_t guid) {
  if (atomic_load_explicit(&db_parked_count, memory_order_relaxed) == 0) {
    return false;
  }
  for (struct db_parked_seg_s *seg = &db_parked_head; seg != NULL;
       seg = atomic_load_explicit(&seg->next, memory_order_acquire)) {
    for (unsigned int i = 0; i < DB_PARKED_SEG; i++) {
      if (atomic_load_explicit(&seg->guid[i], memory_order_relaxed) == guid) {
        return true;
      }
    }
  }
  return false;
}

bool arts_db_create_install_local(arts_shared_ptr_t cb) {
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(cb);
  arts_guid_t guid = db->cache.db_guid;
  /* The install's drain can retire the descriptor (a destroy deferred before
   * the create), so the reads below go through a ref of this call's own. */
  arts_shared_ptr_t pin = arts_shared_copy(cb);
  if (!arts_route_table_install_handle_if_absent(cb, guid)) {
    arts_shared_release(&pin);
    return false;
  }
  unsigned int home = arts_guid_get_rank(guid);
  if (home != arts_global_rank_id && db->db_type == ARTS_DB) {
    /* After the install: an acquire that no longer sees the create parked
     * finds its descriptor. */
    db_parked_remove(guid);
    /* The announce follows the install: whatever the home sends back for the
     * block finds this rank's descriptor. */
    arts_send_db_create_coherent(
        home, guid,
        atomic_load_explicit(&db->cache.db_size, memory_order_relaxed),
        ARTS_DB_PROP_NONE, (uint16_t)ARTS_DB,
        arts_db_cxl_slot_addr(&db->cache), /*owed=*/0u);
  }
  arts_shared_release(&pin);
  return true;
}

/* Hand a descriptor built privately on this rank to the engine.  The creator
 * holds its own reference to it (the created-DB list), so the pointer handed
 * out and every later operation of the creator on its block name this
 * descriptor, whether the create installs now or parks behind an object the
 * GUID still names on this rank. */
static void db_create_enter(struct arts_db_s *db, arts_guid_t guid,
                            bool acquires, void **addr) {
  arts_shared_ptr_t cb = arts_route_table_make_handle(db, guid);
  if (acquires) {
    arts_db_auto_acquire(arts_shared_copy(cb));
  }
  *addr = acquires ? arts_db_user_ptr(db) : NULL;
  struct arts_ooo_args_create_local_s a = {
      .size = ARTS_OOO_CREATE_ADOPT, .guid = guid, .descriptor = (void *)cb};
  arts_ooo_dispatch_or_defer_guid(guid, OOO_DB_CREATE, &a, sizeof(a));
}

/* A cache on this rank that a dependence made before the block's create
 * (ARTS_DB_INIT_STUB) is this rank's view of the block being created, not an
 * occupant: the create claims it, gives it the block's first image and takes
 * its hold in it, unless a right an earlier lifetime kept there refuses the
 * hold. */
static bool db_create_claim_first_touch(struct arts_db_s *found,
                                        uint64_t len, uint8_t *owed) {
  struct arts_db_cache_s *cache = &found->cache;
  if (!arts_db_create_claims_stub(cache)) {
    return false;
  }
  /* This create is the block's declaration of its size.  Before the store,
   * before the image and before the hold — taking the hold admits the
   * waiters an earlier request from this rank parked, an admitted waiter's
   * pointer may BE the store, and the ensure below adopts the store on the
   * residency that keeps no copy.  Within one lifetime whatever the cache
   * learned before the claim was this same size; a cache an earlier lifetime
   * left may carry that block's size, which this store replaces. */
  atomic_store_explicit(&cache->db_size, len, memory_order_relaxed);
#ifdef ARTS_USE_CXL
  (void)arts_db_cxl_slot_create(cache);
#endif
  if (len > 0) {
    (void)arts_db_buf_ensure(cache, len);
  }
  /* On an arm that tracks a durable reader copy, the image this create put
   * here is this rank's — recorded only from an idle reader word, because a
   * fetch already in flight brings the copy with it when it lands. */
  arts_db_create_claim_creator_copy(found);
  /* Within one lifetime the home answers no request for the block before this
   * create's announce, so the cache carries no hold and the hold commits; a
   * cache of an earlier lifetime that kept a right to the block (the write
   * right; under EXCL x RETAIN also its resident bytes or a retained read
   * grant) refuses it.  What the hold serves, it serves from this rank. */
  uint8_t census = arts_db_census_serving(false);
  bool held = arts_db_create_take_hold(cache, owed);
  arts_db_census_served(census);
  return held;
}

/* The creator's cache-only descriptor for a block homed on another rank,
 * built privately: arts_db_cache_stub_size() spans cache + db_type but not the
 * home fields, which this rank never touches.  *cb is the handle the install
 * takes over, *mine the creator's own reference. */
static struct arts_db_s *db_creator_descriptor(arts_guid_t guid, uint64_t len,
                                               arts_shared_ptr_t *cb,
                                               arts_shared_ptr_t *mine) {
  uint64_t stub_sz = arts_db_cache_stub_size();
  struct arts_db_s *stub =
      (struct arts_db_s *)arts_malloc_aligned(stub_sz, ARTS_CACHE_LINE_SIZE);
  memset(stub, 0, stub_sz);
  stub->db_type = ARTS_DB;
  arts_db_cache_init(&stub->cache, guid, len, ARTS_DB_INIT_CREATOR_REMOTE,
                     /*creator_rank=*/0);
#ifdef ARTS_USE_CXL
  /* Still private: the slot is in place before anything can be admitted to
   * the block or handed a pointer into it. */
  (void)arts_db_cxl_slot_create(&stub->cache);
#endif
  /* CREATOR_REMOTE init installs no buffer; the creator's writes land in this
   * one, which its release publishes. */
  if (len > 0) {
    arts_db_buf_install(&stub->cache, /*new_version=*/1,
                        /*data_payload=*/NULL, len);
  }
  *cb = arts_route_table_make_handle(stub, guid);
  *mine = arts_shared_copy(*cb);
  return stub;
}

/* An acquiring create of a coherent block homed on another rank.  The creator
 * side holds a cache-only descriptor carrying the creator's seeded hold,
 * whose buffer the creator writes, published by its release.  The home
 * learns of the block from the announce this rank sends once that descriptor
 * is installed here, so everything the home sends back for the block (the
 * write-through credit, and where the home revokes its first GRANT_INVALIDATE
 * to rw_holder = creator) finds it. */
static void db_create_remote_creator(arts_guid_t guid, uint64_t len,
                                     unsigned int home, bool labeled,
                                     void **addr) {
  arts_shared_ptr_t cb = NULL;
  arts_shared_ptr_t mine = NULL;
  struct arts_db_s *stub = NULL;
  /* Before this rank's slot is examined: an acquire that finds the slot empty
   * from here on defers behind this create instead of installing a
   * first-touch cache the create would have to park behind.  Withdrawn when
   * the create installs (arts_db_create_install_local) or claims a cache.  A
   * freshly minted GUID is named by nothing else yet, so its create cannot
   * park and is not registered. */
  if (labeled) {
    db_parked_add(guid);
    /* Orders the registration above before the slot lookups below; the
     * other side is acquire_one_dep's fence between its lookup miss and its
     * table read (the parked-create table's pair). */
    atomic_thread_fence(memory_order_seq_cst);
  }
  for (;;) {
    arts_shared_ptr_t found_h = arts_route_table_lookup_db(guid);
    struct arts_db_s *found = (struct arts_db_s *)arts_shared_get(found_h);
    if (found != NULL) {
      uint8_t owed = 0u;
      if (found->db_type == ARTS_DB &&
          db_create_claim_first_touch(found, len, &owed)) {
        if (stub != NULL) {
#ifdef ARTS_USE_CXL
          /* Never published: the slot this create minted for its own
           * descriptor is left to the arena. */
          arts_db_cxl_slot_discard(&stub->cache);
#endif
          arts_shared_release(&mine);
          arts_shared_release(&cb);
        }
        if (labeled) {
          db_parked_remove(guid);
        }
        arts_send_db_create_coherent(home, guid, len, ARTS_DB_PROP_NONE,
                                     (uint16_t)ARTS_DB,
                                     arts_db_cxl_slot_addr(&found->cache),
                                     owed);
        *addr = arts_db_user_ptr(found);
        arts_db_auto_acquire(found_h);
        return;
      }
      arts_shared_release(&found_h);
      break;
    }
    if (stub == NULL) {
      stub = db_creator_descriptor(guid, len, &cb, &mine);
    }
    if (arts_db_create_install_local(cb)) {
      *addr = arts_db_user_ptr(stub);
      arts_db_auto_acquire(mine);
      return;
    }
  }
  /* This rank's slot holds a block a create made, or a dependence's cache that
   * refused the create's hold: park behind it. */
  if (stub == NULL) {
    stub = db_creator_descriptor(guid, len, &cb, &mine);
  }
  *addr = arts_db_user_ptr(stub);
  arts_db_auto_acquire(mine);
  struct arts_ooo_args_create_local_s a = {
      .size = ARTS_OOO_CREATE_ADOPT, .guid = guid, .descriptor = (void *)cb};
  arts_ooo_dispatch_or_defer_guid(guid, OOO_DB_CREATE, &a, sizeof(a));
}

/*
 * arts_db_create — Unified DataBlock creation.
 *
 * Handles all DB subtypes (ARTS_DB, ARTS_DB_GPU_PIN, ARTS_DB_GPU).  When
 * hint->rank targets a remote node, only ARTS_DB is supported: the block is
 * announced to its home via DB_CREATE_COHERENT.  A GPU kind returns
 * NULL_GUID with a warning.
 *
 * Every create enters the OoO engine on this rank's slot for the GUID: it
 * installs while the slot is empty and parks while it holds a create's
 * descriptor of another generation or a cache that kept a right to the
 * block, installing at that generation's destroy; a dependence's cache that
 * kept no such right is claimed.  The pointer handed out is the image in the
 * descriptor this create built, in either case; a creator on a rank other
 * than the block's home that claims a dependence's cache hands out the image
 * in that cache.
 */
arts_guid_t arts_db_create(void **addr, uint64_t len, arts_db_types_t db_type,
                           uint16_t flags, const arts_db_hint_t *hint) {
#ifndef ARTS_USE_GPU
  /* The GPU subtype's descriptor carries a shadow image only the GPU
   * allocation path makes room for. */
  if (db_type == ARTS_DB_GPU) {
    ARTS_ERROR("arts_db_create: ARTS_DB_GPU needs a runtime built with the "
               "GPU path (ARTS_USE_GPU)");
  }
#endif
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
  /* A GUID a create minted carries a size hint; only a reserved or labeled
   * one (no hint) may be named before its create, and so be claimed from a
   * dependence's first touch. */
  if (pre_guid != NULL_GUID &&
      ARTS_GUID_DB_GET_SZHINT(pre_guid) != ARTS_GUID_DB_SZHINT_NONE) {
    ARTS_ERROR("arts_db_create: hint->guid %lu was minted by a create (it "
               "carries a size hint); a pre-reserved GUID must come from "
               "arts_guid_reserve or arts_guid_reserve_range",
               (unsigned long)pre_guid);
  }
  unsigned int rank;
  if (pre_guid != NULL_GUID) {
    rank = arts_guid_get_rank(pre_guid);
  } else if (hint == NULL) {
#if ARTS_NOHINT_DB_ROUNDROBIN
    rank = atomic_fetch_add_explicit(&arts_node_info.db_rr_route, 1U,
                                     memory_order_relaxed) %
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
    void *ptr = db_descriptor_alloc(db_type, db_span);
    if (ptr == NULL) {
      /* A silent NULL_GUID here would surface as arbitrary downstream
       * failures instead of the real cause. */
      ARTS_ERROR("arts_db_create: descriptor allocation of %lu bytes failed",
                 (unsigned long)db_span);
    }
    guid = (pre_guid != NULL_GUID)
               ? pre_guid
               : arts_db_guid_stamp_szhint(
                     arts_guid_create_for_rank(arts_global_rank_id,
                                               ARTS_GUID_DB),
                     len);
    db_create_in_place(guid, ptr, len, db_type, /*acquires=*/!no_acquire);
    if (no_acquire && db_type == ARTS_DB) {
      db_create_no_acquire_local((struct arts_db_s *)ptr);
    }
    db_create_enter((struct arts_db_s *)ptr, guid, !no_acquire, addr);
    ARTS_DEBUG("arts_db_create: DB[Guid:%lu, Type:%s, Size:%lu] "
               "created locally",
               guid, GET_DB_TYPE_NAME(db_type), len);
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
      if (no_acquire) {
        /* NO_ACQUIRE: no creator-side descriptor.  The home is the sole idle
         * owner; the first consumer's request acquires ownership.  The
         * block's store is minted here, as for every create, and named on
         * the announce. */
        arts_send_db_create_coherent(rank, guid, len, ARTS_DB_PROP_NO_ACQUIRE,
                                     (uint16_t)db_type,
                                     arts_db_cxl_slot_mint(len), /*owed=*/0u);
        *addr = NULL;
      } else {
        db_create_remote_creator(guid, len, rank, pre_guid != NULL_GUID,
                                 addr);
      }
      ARTS_DEBUG("arts_db_create: DB[Guid:%lu, Type:%s, Size:%lu] "
                 "created remotely on rank %u via DB_CREATE_COHERENT",
                 guid, GET_DB_TYPE_NAME(db_type), len, rank);
    } else {
      /* The GPU kinds (ARTS_DB_GPU_PIN, ARTS_DB_GPU) live on the creator
       * rank.  Creating one on a different rank is a programming error —
       * return NULL_GUID.  Cross-rank distribution must use ARTS_DB. */
      ARTS_WARN("arts_db_create: only ARTS_DB (coherent) DataBlocks support "
                "remote create; %s cannot be homed on rank %u: a GPU kind "
                "lives on its creating rank. Returning NULL_GUID.",
                GET_DB_TYPE_NAME(db_type), rank);
      *addr = NULL;
      guid = NULL_GUID;
    }
  }
  if (guid != NULL_GUID) {
    INCREMENT_NUM_DB_CREATE_BY(1);
    INCREMENT_BYTES_DB_CREATE_BY(len);
    if (no_acquire) {
      INCREMENT_NUM_DB_CREATE_NO_ACQUIRE_BY(1);
    } else {
      /* The creator's hold is a write acquisition, held from the create on. */
      INCREMENT_NUM_DB_ACQUIRE_WRITE_BY(1);
      INCREMENT_NUM_DB_ACQUIRE_LOCAL_HIT_BY(1);
    }
  }
  TIME_DB_CREATE_STOP();
  return guid;
}

/*
 * arts_db_destroy — destroy a DataBlock system-wide, from any rank.
 *
 * An acquire the calling EDT holds on the block (its own create's hold or a
 * dependence slot) is released first.  A coherent block's destroy is then a
 * message to the block's home, which tears the block down and notifies every
 * caching rank; a GPU kind is retired on the creating rank it lives on.
 * Storage goes when the last outstanding reference drops.
 */
void arts_db_destroy(arts_guid_t guid) {
  arts_guid_kind_t type = arts_guid_get_kind(guid);
  if (type != ARTS_GUID_DB) {
    ARTS_WARN("arts_db_destroy called with non-DB type %u (GUID %lu)", type,
              guid);
    return;
  }
  INCREMENT_NUM_DB_DESTROY_BY(1);

  /* Implicit release: if the calling EDT holds an acquire on this DB,
   * release it first (matches OCR ocrDbDestroy semantics).  A created/owned
   * DB releases as RW; a dep release reads the slot mode in Path 2 regardless.
   */
  arts_db_release(guid, DB_MODE_RW);

  arts_shared_ptr_t db_res_h = arts_route_table_lookup_db(guid);
  struct arts_db_s *db_res = (struct arts_db_s *)arts_shared_get(db_res_h);

  /* A coherent block's destroy goes to its home, which is derived from the
   * GUID and fans the teardown out to every caching rank, this one included.
   * The lookup exists only to recognise a GPU kind, which the GUID does not
   * encode: such a block lives on its creating rank alone and has no
   * home protocol, so its descriptor is retired here and, once outstanding
   * refs drop, the cb deleter (arts_db_deleter) runs. */
  if (db_res != NULL && db_res->db_type != ARTS_DB) {
    (void)arts_route_table_set_destroyed_item(guid, db_res_h);
    arts_shared_release(&db_res_h);
    return;
  }
  arts_shared_release(&db_res_h);
  arts_db_destroy_remote(guid);
}

/**********************DB MEMORY MODEL*************************************/

/* One EDT acquisition of a block, counted in the mode its owner slot carries
 * (the strongest any of the EDT's slots names the block in) where the slot is
 * handed to the block's arm, so a deferral counts only when its replay
 * reaches the arm. */
static void db_acquire_census_mode(arts_db_access_mode_t mode) {
  if (mode == DB_MODE_RO) {
    INCREMENT_NUM_DB_ACQUIRE_READ_BY(1);
  } else if (mode == DB_MODE_RW) {
    INCREMENT_NUM_DB_ACQUIRE_WRITE_BY(1);
  }
}

#ifndef NDEBUG
void arts_dep_acq_resolve_once(struct arts_edt_s *edt, uint32_t slot) {
  /* The resolver's own slot is unaccounted, so the zero edge, which needs its
   * account, cannot have been read here: 0 means the EDT is past its acquire
   * phase. */
  if (atomic_load_explicit(&edt->acquire_remaining, memory_order_relaxed) ==
      0u) {
    ARTS_ERROR("db: slot %u of EDT %p resolved after its acquire phase ended",
               slot, (void *)edt);
  }
  if (atomic_exchange_explicit(&edt->dep_acq[slot].resolved, (uint8_t)1,
                               memory_order_relaxed) != 0u) {
    ARTS_ERROR("db: slot %u of EDT[Guid:%lu] resolved twice", slot,
               (unsigned long)edt->guid);
  }
}
#endif

#ifdef ARTS_DEBUG_WAKE_GUID
/* edt->guid has one writer, the create, before the EDT can be made ready, so
 * every wake through a live entry is ordered after it. */
void arts_dep_waiter_check(const struct arts_edt_s *edt, arts_guid_t edt_guid,
                           unsigned int slot) {
  if (edt->guid != edt_guid) {
    ARTS_ERROR("db: a waiter for slot %u of EDT[Guid:%lu] points at %p, "
               "which no longer holds that EDT",
               slot, (unsigned long)edt_guid, (const void *)edt);
  }
}
#endif

/* acquire_one_dep — attempt the single DB dependency depv[i].
 *
 * On a synchronous resolve it writes the slot's acquire record (NULL is a
 * valid payload: a sentinel / version-0 / no-payload DB) and self-accounts
 * via arts_db_acquire_resolved;
 * when the EDT must park (remote ownership/data round, or OoO defer of a
 * not-yet-installed local DB) it leaves the slot to its one resolver (the
 * frame a plane's take hands the waiter to) or to its OoO replay, and
 * does NOT account.  The 3-way route_table dispatch (local
 * entry / remote-home stub install / home==self-but-not-created → OoO push)
 * lives here, inlined from the old single-DB arts_db_acquire API.  The caller
 * (arts_db_acquire_all / rw_fire_from_cursor) has already filtered NULL_GUID /
 * DB_MODE_NULL / pre-filled slots, so depv[i] is a real, not-yet-acquired DB
 * dependency. */
static void acquire_one_dep(struct arts_edt_s *edt, arts_edt_dep_t *depv,
                            uint32_t i) {
  struct arts_dep_acq_s *rec = &edt->dep_acq[i];
  arts_db_access_mode_t access_mode = depv[i].mode;
  unsigned int owner = arts_guid_get_rank(depv[i].guid);
  arts_guid_kind_t guid_type = arts_guid_get_kind(depv[i].guid);

  if (guid_type != ARTS_GUID_DB) {
    return; /* not a DB GUID — nothing to acquire */
  }

  ARTS_INFO("Acquiring DB[Guid:%lu, GuidType:%u, AccessMode:%u, Owner:%u, "
            "Rank:%u] in EDT[Guid:%lu, Slot:%u]",
            depv[i].guid, guid_type, access_mode, owner, arts_global_rank_id,
            edt->guid, i);

  // Look up DB first — subtype dispatch requires the struct.  lookup_db pairs
  // with the release below (every successful lookup => one release).
  arts_shared_ptr_t db_temp_h = arts_route_table_lookup_db(depv[i].guid);
  struct arts_db_s *db_temp = (struct arts_db_s *)arts_shared_get(db_temp_h);

  /* Coherent ARTS_DB path (any protocol, either write policy).  Two entry
   * points:
   *   - Existing local cache_s (db_temp with db_type == ARTS_DB; embedded
   *     cache).
   *   - Remote DB never seen on this rank (db_temp == NULL, owner remote):
   *     stub-install a stub cache_s and dispatch through
   *     arts_handler_db_acquire.
   * The GPU kinds bypass coherence and fall through below. */
  struct arts_db_cache_s *cache = NULL;
  /* When db_temp misses on a remote-owned DB we stub-install a stub and the
   * call returns a SEPARATE pinned handle (stub_h) to the just-installed db_s;
   * it must be released on every path below, mirroring db_temp_h. */
  arts_shared_ptr_t stub_h = NULL;
  /* Which of the two handles keeps `cache`'s descriptor (arts_db_s) alive.
   * It is MOVED into the slot's pin before the handler runs, so the slot pins
   * the descriptor for its whole firing->release span. */
  arts_shared_ptr_t *cache_owner_h = NULL;
  if (db_temp != NULL && db_temp->db_type == ARTS_DB) {
    cache = &db_temp->cache;
    cache_owner_h = &db_temp_h;
  } else if (db_temp == NULL && owner != arts_global_rank_id) {
    /* Orders the lookup miss above before the table read below; the other
     * side is db_create_remote_creator's fence between its registration and
     * its lookups (the parked-create table's pair). */
    atomic_thread_fence(memory_order_seq_cst);
    if (db_parked_here(depv[i].guid)) {
      /* A create of the block on this rank is in progress: the acquire runs
       * against its descriptor once it installs. */
      arts_shared_release(&db_temp_h);
      struct arts_ooo_args_db_acquire_s a = {
          .edt = edt, .db_guid = depv[i].guid, .slot = i};
      arts_ooo_dispatch_or_defer_guid(depv[i].guid, OOO_DB_ACQUIRE, &a,
                                      sizeof(a));
      return;
    }
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
    /* The handler resolves the slot in its own frame, or publishes it to its
     * one resolver and touches neither the slot nor the EDT again.  So the
     * subtype and the descriptor pin go in first: the cache-owning handle is
     * MOVED into the slot's pin, which the resolver reads the descriptor from
     * and release_one_dep consumes, so the release reaches this descriptor
     * even after a destroy has retired its GUID. */
    rec->subtype = ARTS_DB;
    rec->pin = *cache_owner_h;
    *cache_owner_h = NULL;
    struct arts_ooo_args_db_acquire_s a = {
        .edt = edt, .db_guid = depv[i].guid, .slot = i};
    db_acquire_census_mode(access_mode);
    uint8_t census = arts_db_census_deciding();
    arts_handler_db_acquire(arts_db_of_cache(cache), &a);
    arts_db_census_served(census);
    /* The handle that kept the descriptor was moved; drop the other. */
    arts_shared_release(&db_temp_h);
    arts_shared_release(&stub_h);
    return;
  }
  /* cache==NULL fall-throughs below never used stub_h (it is only set on the
   * remote-miss arm, which always has a non-NULL cache here unless the DB was
   * destroyed before install — stub_h NULL then); release defensively. */
  arts_shared_release(&stub_h);

  /* The GPU kinds (ARTS_DB_GPU_PIN, ARTS_DB_GPU) live only on their creator
   * rank, and a coherent DB reaches here only in a mode outside the
   * coherence protocol (device-side sync modes): either way the local payload
   * is handed back, wherever the subtype keeps it. */
  if (db_temp != NULL) {
    if (owner != arts_global_rank_id) {
      ARTS_WARN("arts_db_acquire_all: non-coherent DB[Guid:%lu, Type:%s] "
                "referenced from non-creator rank %u (owner=%u). Only ARTS_DB "
                "is internode relocatable.",
                depv[i].guid, GET_DB_TYPE_NAME(db_temp->db_type),
                arts_global_rank_id, owner);
    }
    rec->subtype = db_temp->db_type;
    if (access_mode == DB_MODE_RO || access_mode == DB_MODE_RW) {
      db_acquire_census_mode(access_mode);
      INCREMENT_NUM_DB_ACQUIRE_LOCAL_HIT_BY(1);
    }
    arts_db_acquire_local_hit(edt, i, arts_db_user_ptr(db_temp));
    arts_shared_release(&db_temp_h);
    arts_db_acquire_resolved(edt, i);
    return;
  }

  /* DB absent locally.  OoO defer keyed on the DB GUID (park): when DB_CREATE
   * installs it (home==self case) the drain re-attempts this dep through
   * arts_handler_db_acquire.  A remote non-coherent (GPU-kind) reference
   * can never resolve locally and waits here.  No resolved — the data and
   * cursor advance arrive on the drain replay. */
  if (arts_guid_is_local(depv[i].guid)) {
    ARTS_DEBUG("DB[Guid:%lu] out of order request slot %u", depv[i].guid, i);
  } else {
    ARTS_WARN("arts_db_acquire_all: cannot resolve remote DB[Guid:%lu] for "
              "non-coherent dep on rank %u — owner=%u. Deferring via "
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
static void resume_enqueue(struct arts_edt_s *edt);

/* OOO_DB_ACQUIRE replay table entry — see db.h.  Re-attempts the single
 * deferred dep through acquire_one_dep's subtype-aware 3-way: for an ARTS_DB
 * this lands in arts_handler_db_acquire (coherent), for a GPU-kind DB in its
 * inline ptr path, and a still-absent DB re-defers.  Mapping OOO_DB_ACQUIRE
 * straight to arts_handler_db_acquire would mishandle non-coherent subtypes
 * (their embedded cache is zeroed — its pending_rw queue is uninitialised, so
 * the coherent RW path would push onto a NULL-headed queue and crash). */
void arts_db_acquire_replay_dep(void *item, void *args) {
  (void)item; /* the 3-way re-looks it up; the dispatcher pins it meanwhile */
  struct arts_ooo_args_db_acquire_s *a =
      (struct arts_ooo_args_db_acquire_s *)args;
  arts_edt_dep_t *depv = (arts_edt_dep_t *)arts_get_depv(a->edt);
  if (arts_dep_is_serialized(depv, a->slot)) {
    /* Serialized dep deferred at the cursor: re-drive the acquire loop from the
     * cursor.  Route through the flat resume trampoline (enqueue + drain) so
     * the re-drive stays top-level even when this replay runs nested inside
     * another EDT's acquire loop (an inline OoO drain) — never an inline
     * rw_fire_from_cursor that would recurse. */
    /* The entry owes the drain an account like any other.  The walk is parked
     * at this unaccounted dep, so the count is above zero and the EDT cannot
     * be scheduled before the drain issues it. */
    unsigned int owed = atomic_fetch_add_explicit(&a->edt->acquire_remaining,
                                                  1u, memory_order_acq_rel);
    assert(owed > 0u); /* the deferred slot is still unpaid */
    (void)owed;
    resume_enqueue(a->edt);
    arts_db_drain_resume_list();
  } else {
    /* Non-serialized (Pass-1) dep: re-attempt just this slot; it self-accounts
     * on a local hit.  The attempt holds no account of its own: the handler
     * touches the slot and the EDT only until it publishes the slot or
     * resolves it, so the slot's account may schedule the EDT while the rest
     * of the handler still runs on the descriptor, which the OoO
     * dispatcher's pin on the installed object keeps alive. */
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
static bool dep_order_after(const arts_edt_dep_t *depv, uint32_t a,
                            uint32_t b) {
  if (depv[a].guid != depv[b].guid) {
    return depv[a].guid > depv[b].guid;
  }
  return dep_mode_strength(depv[a].mode) < dep_mode_strength(depv[b].mode);
}

/* A real DB dep the acquire phase resolves (not NULL / not a raw value / not
 * already carrying a pointer / actually a DB GUID).  A pure function of depv,
 * which no frame writes until the acquire phase ends. */
static bool dep_needs_acquire(const arts_edt_dep_t *depv, uint32_t i) {
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
 * non-DB kind, a slot already carrying a pointer and a device-side internal
 * mode (DB_MODE_LC_SYNC / MEMSET and the like, which the coherence protocol
 * does not acquire and which resolve on their own per-mode terms) are neither
 * owner nor alias and are left exactly as they are.
 *
 * A pure function of depv, which is fixed once the EDT is ready — so the answer
 * is the same for every frame that consumes it and nothing re-derives it. */
void arts_dep_sort_and_classify(const arts_edt_dep_t *depv,
                                struct arts_dep_acq_s *acq, uint32_t depc,
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
    acq[i].alias = (depv[i].guid == group);
    group = depv[i].guid;
  }
}

/* Ownership-serialized (RW cursor) dep? */
bool arts_dep_is_serialized(const arts_edt_dep_t *depv, uint32_t i) {
  return arts_db_acquire_is_serialized(depv[i].mode);
}

/* Decrement-and-maybe-schedule. Caller must not touch the EDT afterward.
 *
 * The decrement that reaches zero has acquired every frame's slot work, so it
 * alone may read the record whole: it hands each slot that resolved to a
 * payload to the public depv the EDT runs on, and leaves every other slot as
 * its satisfy left it (a fired slot's depv pointer is NULL, so a slot that
 * resolved to NULL reads the same either way). */
void arts_db_acquire_account(struct arts_edt_s *edt) {
  if (atomic_fetch_sub_explicit(&edt->acquire_remaining, 1u,
                                memory_order_acq_rel) == 1u) {
    arts_edt_dep_t *depv = (arts_edt_dep_t *)arts_get_depv(edt);
    const struct arts_dep_acq_s *acq = edt->dep_acq;
    for (uint32_t i = 0; i < edt->depc; i++) {
      if (acq[i].ptr != NULL) {
        depv[i].ptr = acq[i].ptr;
      }
    }
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
 * repeated work).  Instead the woken EDT is appended to a thread-local
 * worklist and the resume runs FLAT: when control returns to the top level
 * (no acquire loop in progress, tl_acquiring == NULL) the drain below walks
 * the worklist in a while loop, calling rw_fire_from_cursor once per entry.
 * Re-entrancy-guarded (tl_in_drain) + top-level-guarded (tl_acquiring) so
 * rw_fire_from_cursor can never appear twice on the stack.
 *
 * An entry carries one account the EDT still owes, and the drain issues it
 * only after the walk.  Every walk therefore runs while the EDT is
 * unschedulable, and the account that may schedule it is the frame's last
 * touch.  The same owed account keeps the raw pointer valid: an EDT that
 * cannot be scheduled has not run, so the reference arts_handle_ready_edt took
 * for the runnable phase is still held.  The entry never names the EDT by its
 * GUID, which may name a later EDT once this one completes. */
static ARTS_THREAD_LOCAL struct arts_edt_s **tl_resume_buf = NULL;
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

static void resume_enqueue(struct arts_edt_s *edt) {
  if (tl_resume_len == tl_resume_cap) {
    uint32_t ncap = tl_resume_cap ? tl_resume_cap * 2u : 16u;
    tl_resume_buf = (struct arts_edt_s **)arts_realloc(
        tl_resume_buf, ncap * sizeof(struct arts_edt_s *));
    tl_resume_cap = ncap;
  }
  tl_resume_buf[tl_resume_len++] = edt;
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
    struct arts_edt_s *e = tl_resume_buf[--tl_resume_len];
    rw_fire_from_cursor(e); /* flat: tl_acquiring is NULL here */
    arts_db_acquire_account(e);
  }
  tl_in_drain = false;
}

void arts_db_resume_list_free(void) {
  arts_free(tl_resume_buf);
  tl_resume_buf = NULL;
  tl_resume_len = 0;
  tl_resume_cap = 0;
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
  const struct arts_dep_acq_s *acq = edt->dep_acq;
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
     * request behind the EDT's own unreleased hold and self-deadlock.  Nor is
     * a slot off the walk, whose own wake may be resolving it right now: it is
     * passed before anything about its resolution is asked. */
    if (acq[i].alias || !arts_dep_is_serialized(depv, i) ||
        !dep_needs_acquire(depv, i)) {
      edt->rw_cursor++;
      continue;
    }
    tl_inline_advanced = false;
    /* A dependence fires only for an EDT that still owes an account: once
     * every account is in, the EDT is scheduled and its acquire state belongs
     * to its run, so a walk arriving here past that point would charge a
     * second acquisition to a task that is already running. */
    assert(atomic_load_explicit(&edt->acquire_remaining,
                                memory_order_relaxed) > 0u);
    acquire_one_dep(
        edt, depv,
        i); /* local hit advances the cursor; remote/contended parks */
    if (!tl_inline_advanced) {
      /* Parked (or deferred): the serving side owns the continuation — its
       * arts_db_rw_secure, or for a deferred dep the out-of-order replay,
       * enqueues the flat resume on its own thread.  Do NOT
       * infer "resolved" from cursor movement: a concurrent other-thread serve
       * also advances the cursor, and continuing here would put two threads in
       * the same EDT's loop (double-firing the next dep). */
      break;
    }
    /* resolved in-frame — the loop picks up the next serialized dep */
  }
  tl_acquiring = prev_acquiring;
}

/* Async grant resume — see db.h.  This is the path a slot's one resolver
 * takes when the slot is the serialized dep the walk is parked at (remote
 * grant / release grant / an in-frame serve).  The SYNCHRONOUS local resolve
 * does NOT come through here; it uses arts_db_acquire_resolved (advance only)
 * and the running loop picks up the next dep, so consecutive local resolves
 * never recurse.
 *
 * The frame this runs in grows by nothing: the two outcomes are a thread-local
 * flag and an append to the worklist, neither of which fires a dependence. */
bool arts_db_rw_secure(struct arts_edt_s *edt, unsigned int slot) {
  const uint32_t *sorted = edt->rw_sorted; /* sorted ONCE in acquire_all */
  /* A wake follows an acquisition the walk issued, at the cursor. */
  assert(sorted != NULL);
  assert(edt->rw_cursor < edt->depc && sorted[edt->rw_cursor] == slot);
  (void)sorted;
  (void)slot;
  edt->rw_cursor++;
  /* If this resume is for THIS thread's in-flight EDT (a same-rank grant
   * whose handler ran synchronously inside the running loop), signal the
   * loop to continue — it owns the continuation.  Otherwise it is a PARKED
   * EDT woken by a grant: enqueue it for the flat resume drain (NOT an
   * inline rw_fire_from_cursor, which would nest on the running loop =
   * recursion).  Exactly one side continues the loop, never both. */
  if (edt != tl_acquiring) {
    resume_enqueue(edt);
    return true;
  }
  tl_inline_advanced = true;
  return false;
}

/* Resolve ONE alias slot onto the block's payload: the same bytes at the same
 * address, the slot's own buffer ref (release drops one per slot) and its own
 * descriptor pin.
 *
 * `subtype` is the owner's, because it is the acquisition's: for a coherent
 * block the payload is a buffer's data and carries a ref; for a GPU-kind
 * block it is the descriptor's inline storage and carries none.
 *
 * `pin_src` is a handle on the descriptor the payload belongs to, borrowed from
 * the caller.  INVARIANT: an alias never ends with a buffer ref and no
 * descriptor pin, because a release that cannot derive the buffer from the
 * payload address finds it through the descriptor.  The pin is therefore
 * COPIED from the handle that resolved the payload, never looked up again: a
 * lookup can miss under a concurrent destroy while the payload and its
 * descriptor are still alive under the owner's own pin.
 *
 * The owner's one resolver is the alias's one filler, so every store is
 * plain; the caller owes the slot's account, which publishes them and which
 * is the caller's because it may schedule the EDT. */
static void fill_one_alias(struct arts_edt_s *edt, uint32_t slot, void *payload,
                           arts_db_types_t subtype, arts_shared_ptr_t pin_src) {
  arts_dep_acq_resolve_once(edt, slot);
  struct arts_dep_acq_s *rec = &edt->dep_acq[slot];
  rec->subtype = subtype;
  if (payload == NULL) {
    rec->pin = arts_shared_copy(pin_src);
    return;
  }
  if (subtype != ARTS_DB) {
    /* A non-coherent block's payload is its descriptor's own storage: no
     * buffer, no ref, and a release with nothing to give back. */
    rec->ptr = payload;
    return;
  }
#ifdef ARTS_CXL_DIRECT
  /* Header-relative arithmetic on the payload lands in the block's store, not
   * in a header, so the descriptor comes from the cache the caller has already
   * pinned.  pin_src is non-NULL here by arts_db_fill_aliases's rule — with no
   * descriptor to pin it hands out no bytes — and the owner slot's own ref
   * keeps this descriptor alive across the acquire.  The handle is OWNED: it
   * is the slot's buffer ref, which its release drops through the pin.  A
   * block that no longer names a descriptor takes the same shape here as any
   * other block with no bytes to hand out: NULL, and no ref. */
  struct arts_db_s *pin_db = (struct arts_db_s *)arts_shared_get(pin_src);
  arts_shared_ptr_t buf_h = arts_db_buf_acquire(&pin_db->cache);
  struct arts_db_buffer_s *ob =
      (struct arts_db_buffer_s *)arts_shared_get(buf_h);
  if (ob == NULL) {
    arts_db_buf_release(&buf_h);
    rec->pin = arts_shared_copy(pin_src);
    return;
  }
  if ((void *)ob->data != payload) {
    ARTS_ERROR("db: a block's descriptor names storage other than the payload "
               "an alias was handed");
  }
#else
  /* A ref on the owner's buffer, not on whatever the block's cache holds now,
   * which is what makes the two slots the same address by construction.  The
   * slot's release drops it through the payload's buffer. */
  arts_shared_ptr_t buf_h =
      arts_shared_copy(arts_db_buf_from_data(payload)->cb);
#endif
  (void)buf_h;
  rec->pin = arts_shared_copy(pin_src);
  rec->ptr = payload;
  /* Not counted: an alias is not an acquisition.  One block, one acquisition,
   * one census entry — the owner's. */
}

/* Hand the block's payload to every slot that aliases the acquisition just
 * resolved on `owner_slot`.  This is the one place an alias is ever resolved,
 * on every arm: it runs where an owner's pointer becomes final — the
 * synchronous resolve (arts_db_acquire_resolved) and the owner's one resolver
 * (coherence.c) — and nowhere else.
 *
 * The owner's record pin is the descriptor handle the aliases copy, borrowed
 * for the call.  An owner with no pin (a non-coherent block resolved on the
 * synchronous path) has the block looked up once here — and a MISS means the
 * block was destroyed under a pending acquire, which the model leaves
 * undefined: the aliases then resolve to NULL and take no buffer ref, the
 * value and the shape every other path delivers for a block with no storage.
 * A payload therefore never reaches an alias without the descriptor it
 * belongs to.
 *
 * One pass over depv in this frame, no recursion and no dispatch: an alias
 * needs nothing from the network, and the slots it fills were classified
 * before anything fired, so the walk is decided, not searched.
 *
 * The accounts are issued at the end.  An account may schedule the EDT, and
 * the loop must not read it after that; deferring them is safe because the
 * caller still owes the owner's own account, so the count cannot reach zero
 * while this loop runs. */
void arts_db_fill_aliases(struct arts_edt_s *edt, unsigned int owner_slot) {
  arts_edt_dep_t *depv = (arts_edt_dep_t *)arts_get_depv(edt);
  struct arts_dep_acq_s *acq = edt->dep_acq;
  /* Only a classified owner has aliases: a slot of another mode on the same
   * block (a device-side internal mode) resolves on its own terms, and the
   * group's aliases are its owner's to fill. */
  if (depv[owner_slot].mode != DB_MODE_RO &&
      depv[owner_slot].mode != DB_MODE_RW) {
    return;
  }
  arts_guid_t guid = depv[owner_slot].guid;
  void *payload = acq[owner_slot].ptr;
  arts_db_types_t subtype = acq[owner_slot].subtype;
  arts_shared_ptr_t looked_up = NULL;
  arts_shared_ptr_t pin_src = acq[owner_slot].pin;
  bool pin_known = (pin_src != NULL);
  /* A slot this loop resolves is a slot nobody has accounted, so the EDT
   * cannot have run yet and the owner's payload is alive under its own ref. */
  uint32_t owed = 0;
  for (uint32_t k = 0; k < edt->depc; k++) {
    if (!acq[k].alias || depv[k].guid != guid) {
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
    fill_one_alias(edt, k, payload, subtype, pin_src);
    owed++;
  }
  arts_shared_release(&looked_up);
  while (owed-- > 0) {
    arts_db_acquire_account(edt);
  }
}

/* A SYNCHRONOUS local resolve (the firing frame wrote the slot's record, in
 * the running acquire loop or an off-walk replay).  For a serialized dep,
 * advance the cursor ONLY — the running rw_fire_from_cursor loop picks up the
 * next dep, so this never re-fires recursively (that is the whole point of the
 * loop: acquire_all is the continuation unit, the loop walks deps, and only an
 * ASYNC resume — arts_db_rw_secure / the OoO replay — re-enters the loop).
 * Then account THIS dep, the frame's last touch: it may be the decrement that
 * schedules the EDT. */
void arts_db_acquire_resolved(struct arts_edt_s *edt, unsigned int slot) {
  arts_edt_dep_t *depv = (arts_edt_dep_t *)arts_get_depv(edt);
  if (arts_dep_is_serialized(depv, slot)) {
    /* A serialized slot is fired only by the walk, which runs on this thread
     * and is parked at it; nothing but this flag continues the walk. */
    assert(edt == tl_acquiring);
    assert(edt->rw_cursor < edt->depc &&
           edt->rw_sorted[edt->rw_cursor] == slot);
    edt->rw_cursor++; /* advance only; the loop fires the next dep */
    tl_inline_advanced = true;
  }
  /* The block's payload is final for this EDT: hand it to every slot that
   * aliases this acquisition BEFORE accounting the owner, because the owner's
   * account may be the one that schedules the EDT. */
  arts_db_fill_aliases(edt, slot);
  arts_db_acquire_account(
      edt); /* count THIS dep's data; outermost may schedule */
}

/* Order the deps and classify owner vs alias ONCE for the whole acquire
 * phase, BEFORE anything fires: both are pure functions of depv (fixed once
 * the EDT is ready), so rw_fire_from_cursor / arts_db_rw_secure /
 * arts_db_acquire_resolved / arts_db_fill_aliases all consume the one answer
 * and none re-derives it.  That is one O(depc²) pass per EDT, and it gives
 * every frame that meets a slot the same verdict about which slot owns its
 * block.  The visit order shares the record's allocation, which is freed with
 * the EDT (arts_edt_free). */
void arts_db_acquire_prepare(struct arts_edt_s *edt) {
  uint32_t depc = edt->depc;
  if (depc == 0 || edt->dep_acq != NULL) {
    return;
  }
  struct arts_dep_acq_s *acq = (struct arts_dep_acq_s *)arts_calloc(
      depc, sizeof(struct arts_dep_acq_s) + sizeof(uint32_t));
  edt->dep_acq = acq;
  edt->rw_sorted = (uint32_t *)(acq + depc);
  arts_dep_sort_and_classify((const arts_edt_dep_t *)arts_get_depv(edt), acq,
                             depc, edt->rw_sorted);
}

/* Driver: fire all non-serialized deps (Pass 1), then fire the serialized ones
 * from the cursor (Pass 2; the handler's resolved path self-continues the RW
 * chain). acquire_remaining is +1-biased so data arrivals during the fire
 * cannot schedule before the fire completes. Called once per EDT from
 * arts_handle_ready_edt. */
void arts_db_acquire_all(struct arts_edt_s *edt) {
  arts_edt_dep_t *depv = (arts_edt_dep_t *)arts_get_depv(edt);
  uint32_t depc = edt->depc;
  arts_db_acquire_prepare(edt);
  const uint32_t *sorted = edt->rw_sorted;
  const struct arts_dep_acq_s *acq = edt->dep_acq;

  uint32_t n = 0;
  for (uint32_t k = 0; k < depc; k++) {
    if (dep_needs_acquire(depv, sorted[k])) {
      n++;
    }
  }
  /* now (1 + n) with the bias */
  atomic_fetch_add_explicit(&edt->acquire_remaining, n, memory_order_acq_rel);

  /* Pass 1: every non-serialized real DB dep that OWNS its block's single
   * acquisition, order-independent (the handler self-accounts via
   * arts_db_acquire_resolved on a local hit; remote parks).  An alias asks for
   * nothing — the owner's resolve fills it (arts_db_fill_aliases). */
  for (uint32_t k = 0; k < depc; k++) {
    uint32_t i = sorted[k];
    if (acq[i].alias || !dep_needs_acquire(depv, i) ||
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
 * A coherent block needs nothing here: its protocol did its work at acquire.
 * In GPU builds, a CPU EDT takes the read side of every ARTS_DB_GPU block's
 * staging lock for each slot not in DB_MODE_LC_SYNC and bumps the block's LC
 * version; a DB_MODE_LC_SYNC slot syncs the GPU shadow copies instead.
 *
 * Called from arts_run_edt() after all DB pointers have been resolved.
 */
void prep_dbs(struct arts_edt_s *edt, bool gpu) {
  (void)gpu;
  uint32_t depc = edt->depc;
  arts_edt_dep_t *depv = (arts_edt_dep_t *)arts_get_depv(edt);
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
     * the coherence layer; the GPU kinds (ARTS_DB_GPU_PIN, ARTS_DB_GPU) have
     * no DB-level coherence and fall through to their per-subtype prep (their
     * dep->ptr is db+1, so the recovery below is valid for them). */
    if (arts_dep_subtype(edt, i) == ARTS_DB) {
      continue;
    }
#ifdef ARTS_USE_GPU
    if (!gpu && access_mode != DB_MODE_LC_SYNC) {
      struct arts_db_s *db = ((struct arts_db_s *)depv[i].ptr) - 1;
      if (db->db_type == ARTS_DB_GPU) {
        pthread_rwlock_rdlock(db->gpu_lock);
        atomic_fetch_add_explicit(&db->version, 1u, memory_order_acq_rel);
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

/* What a release needs of one slot: the public view the EDT ran on plus the
 * storage kind and the alias verdict its acquire recorded. */
struct arts_dep_release_s {
  arts_guid_t guid;
  void *ptr;
  arts_db_access_mode_t mode;
  arts_db_types_t subtype;
  bool alias;
};

/* The release view of depv[i]: the run thread owns both depv and the record
 * once the EDT is scheduled. */
static struct arts_dep_release_s dep_release_of(const struct arts_edt_s *edt,
                                                const arts_edt_dep_t *depv,
                                                uint32_t i) {
  return (struct arts_dep_release_s){.guid = depv[i].guid,
                                     .ptr = depv[i].ptr,
                                     .mode = depv[i].mode,
                                     .subtype = edt->dep_acq[i].subtype,
                                     .alias = edt->dep_acq[i].alias};
}

/* The descriptor pin slot i holds, for its release to consume. */
static arts_shared_ptr_t dep_release_pin(const struct arts_edt_s *edt,
                                         uint32_t i) {
  return edt->dep_acq[i].pin;
}

/*
 * release_one_dep — Single source of truth for "release one dep slot".
 *
 * Used by:
 *   - release_dbs (EDT epilogue, all dep slots)
 *   - arts_db_release Path 2 (mid-EDT release of one depv slot)
 *
 * Per access mode:
 *   - DB_MODE_RO / DB_MODE_RW: only ARTS_DB needs DB-level coherence
 *     work; route through the coherent release entry points
 *     (arts_db_release_ro / arts_db_release_rw).  The GPU kinds have no
 *     DB-level coherence — release is a no-op.
 *   - ARTS_DB_GPU subtype (GPU build, a CPU EDT's non-LC_SYNC slot): drop
 *     the staging lock's read side prep_dbs took.
 *
 * `db_pin` is the descriptor pin the slot holds (NULL for none) and is
 * consumed.  Does NOT nullify caller-visible state (guid/ptr/mode).  Callers
 * that need to mark the slot as released (mid-EDT release) do that
 * themselves.
 */
static void release_one_dep(const struct arts_dep_release_s *dep,
                            arts_shared_ptr_t db_pin, bool gpu) {
  arts_db_access_mode_t access_mode = dep->mode;
  /* A slot that names no data block (NULL_GUID or another kind) was never
   * acquired, whatever its mode: dep_needs_acquire skips it. */
  if (access_mode == DB_MODE_NULL ||
      arts_guid_get_kind(dep->guid) != ARTS_GUID_DB) {
    arts_shared_release(&db_pin); /* NULL for such a slot */
    return;
  }

  /* Coherent release path for ARTS_DB.  Routed by dep->subtype (recorded at
   * acquire), NOT by recovering the subtype from dep->ptr: for a coherent DB
   * dep->ptr is cache->buffer->data (NOT (db+1)), so the GPU-kind
   * pointer arithmetic below would read a wild address.  The EDT's
   * per-acquire buffer ref (taken by the slot's one resolver: acquire_local,
   * the resolver's cache-slot read, or the landed private copy) is dropped at
   * the tail of this block via the buffer's own cb: the EDT's ref kept the
   * buffer (hence buf->cb) alive up to there, so the deref is never
   * use-after-free.  The arm's release runs against the descriptor the slot's
   * pin names; a destroy is ordered after this release, so the block is still
   * installed here, and the pin keeps the descriptor valid even for a program
   * that breaks that order. */
  if (dep->subtype == ARTS_DB &&
      (access_mode == DB_MODE_RO || access_mode == DB_MODE_RW)) {
    /* Alias slot (a slot naming a DB whose single acquisition another slot of
     * the same EDT owns): it took a buffer ref when that acquisition's payload
     * was handed to it (so the tail drop balances it) but never a coherence
     * hold.  Skip release_rw/ro so the hold is dropped exactly once per
     * distinct DB.  The alias bit is decided before the acquire phase fires
     * (arts_dep_sort_and_classify), NOT re-derived here, so a mid-EDT release
     * that nulls the owning slot's GUID cannot make an alias masquerade as the
     * owner.  Still drop the alias's own pin. */
    if (!dep->alias) {
      /* A DB-kind RO/RW owner whose subtype is ARTS_DB went through
       * acquire_one_dep's coherent branch, which stored the pin before the
       * handler ran. */
      assert(db_pin != NULL);
      struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(db_pin);
      if (access_mode == DB_MODE_RW) {
        arts_db_release_rw(&db->cache, dep->ptr);
      } else {
        arts_db_release_ro(&db->cache);
      }
    }
    /* Drop the EDT's per-acquire buffer ref AFTER the arm's release: an arm
     * whose release moves the EDT's own bytes needs them alive until its
     * transfer has completed.  An alias slot takes this path too — it holds
     * a ref of its own, it just never held the coherence hold. */
    if (dep->ptr != NULL) {
#ifdef ARTS_CXL_DIRECT
      /* Header-relative arithmetic on dep->ptr lands in the shared store, not
       * in a header, so the descriptor is the one this slot's own pin names:
       * every site that takes this ref installs that pin in the same frame, so
       * the two name one object.  The pin defers the descriptor's destructor,
       * the only thing that empties a published cache's slot, and a block has
       * one descriptor for its whole life on this residency, so the lookup
       * answers the descriptor this ref was taken on. */
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
    /* The descriptor pin last: the CXL-direct branch above resolves the
     * buffer through it. */
    arts_shared_release(&db_pin);
    return;
  }

  /* Reaching this point means the dep is for a GPU kind (ARTS_DB_GPU_PIN,
   * ARTS_DB_GPU) or a special access mode (LC_*, MEMSET) — none of which
   * carry DB-level coherence.  The subtype is
   * the one the acquire recorded, as prep_dbs reads it: a coherent block's
   * dep->ptr is buffer data, with no descriptor before it. */
  arts_db_types_t db_subtype = dep->subtype;

  ARTS_DEBUG("Releasing DB[Guid:%lu] [AccessMode:%s, DbSubtype:%s]", dep->guid,
             GET_DB_MODE_NAME(access_mode), GET_DB_TYPE_NAME(db_subtype));

#ifdef ARTS_USE_GPU
  /* The read side prep_dbs took, under exactly the condition it took it. */
  if (!gpu && access_mode != DB_MODE_LC_SYNC && db_subtype == ARTS_DB_GPU &&
      dep->guid != NULL_GUID && dep->ptr != NULL) {
    struct arts_db_s *db = ((struct arts_db_s *)dep->ptr) - 1;
    pthread_rwlock_unlock(db->gpu_lock);
  }
#else
  (void)gpu;
#endif
  /* GPU_PIN, or RW / RO on a GPU kind: nothing to release at the
   * DB-coherence level.  Hardware coherence and
   * application-level event ordering handle the rest. */
  arts_shared_release(&db_pin);
}

/*
 * release_dbs — Release DB dependencies after EDT execution completes.
 * Thin loop over depv calling the single-source-of-truth release_one_dep.
 */
void release_dbs(struct arts_edt_s *edt, bool gpu) {
  uint32_t depc = edt->depc;
  arts_edt_dep_t *depv = (arts_edt_dep_t *)arts_get_depv(edt);
  for (uint32_t i = 0; i < depc; i++) {
    /* A NULL-mode slot holds no pin, or one the body's release consumed. */
    if (depv[i].mode == DB_MODE_NULL) {
      continue;
    }
    /* Alias-vs-owner is decided before the acquire phase fires
     * (arts_dep_sort_and_classify): an alias slot drops only its buffer ref,
     * the owner also releases the single coherence hold.  See
     * struct arts_dep_acq_s / release_one_dep. */
    struct arts_dep_release_s dep = dep_release_of(edt, depv, i);
    release_one_dep(&dep, dep_release_pin(edt, i), gpu);
  }
}

/*
 * release_one_created — Release a single created DB through the descriptor
 * handle its create recorded, and drop that handle's ref.
 *
 * No route lookup: the creator's hold is on the descriptor its create built,
 * which the route slot may not hold yet (a parked create).  For ARTS_DB the
 * creator's hold
 * is the arm's own state, seeded at ARTS_DB_INIT_CREATOR_HOME /
 * ARTS_DB_INIT_CREATOR_REMOTE and released through the arm's own entry point
 * with NO buffer ref to drop (auto_acquire never called acquire_buf).  The
 * creator of a GPU kind (ARTS_DB_GPU_PIN, ARTS_DB_GPU) holds nothing to
 * release: no coherence hold, and no side of a GPU staging lock, which only
 * prep_dbs takes.
 */
static void release_one_created(arts_shared_ptr_t h) {
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(h);
  if (db->db_type == ARTS_DB) {
    /* Coherent creator release.  No buffer ref to drop (the hold is the seed
     * arts_db_cache_init stamped on the cache).  The arm's create-hold entry
     * point runs it: the creating EDT's bytes are wherever that arm put them
     * when the block was made, which is not something a dependence's pointer
     * could name.  The coherent creator hold is always RW. */
    arts_db_release_created(&db->cache);
  }
  arts_shared_release(&h);
}

/*
 * arts_db_release — Release access to a single DB mid-EDT.
 *
 * Two search paths:
 *   1. created_db_list — DBs the current EDT created (held RW from the create).
 *   2. depv — DBs received as dependencies (RW or RO mode).
 *
 * Both paths funnel through release_one_dep / release_one_created so the
 * per-mode, per-subtype and route-table-ref rules live in exactly one
 * place.  The slot/entry is marked released after the unwind so the
 * epilogue (release_dbs / arts_release_created_dbs) skips it cleanly.
 */
void arts_db_release(arts_guid_t guid, arts_db_access_mode_t mode) {
  /* The block, not the mode, decides what either path releases. */
  (void)mode;
  /* Path 1: created_db_list (DBs this EDT created).  Scanned from the back
   * because the common shape is create-then-release, and REMOVED on a match
   * rather than blanked: the list has to track what is still HELD, not what was
   * ever created, or a release that finds nothing walks every entry the EDT
   * ever made and d releases cost d^2. */
  arts_vector_t *list = arts_get_created_db_list();
  uint64_t count = arts_vector_count(list);
  for (uint64_t i = count; i > 0; i--) {
    arts_shared_ptr_t h = *(arts_shared_ptr_t *)arts_vector_at(list, i - 1);
    if (((struct arts_db_s *)arts_shared_get(h))->cache.db_guid == guid) {
      arts_vector_swap_remove(list, i - 1);
      release_one_created(h);
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
  struct arts_edt_s *edt = current_edt;
  arts_edt_dep_t *depv = (arts_edt_dep_t *)arts_get_depv(edt);
  uint32_t depc = edt->depc;
  uint32_t owner = depc;
  uint32_t unclassified = depc;
  for (uint32_t i = 0; i < depc; i++) {
    if (depv[i].guid != guid || depv[i].mode == DB_MODE_NULL ||
        edt->dep_acq[i].alias) {
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
  /* The flag prep_dbs ran this EDT's slots with. */
  bool gpu = edt->edt_type == ARTS_EDT_GPU;
  struct arts_dep_release_s dep = dep_release_of(edt, depv, owner);
  release_one_dep(&dep, dep_release_pin(edt, owner), gpu);
  depv[owner].guid = NULL_GUID;
  depv[owner].ptr = NULL;
  depv[owner].mode = DB_MODE_NULL;
  for (uint32_t i = 0; i < depc; i++) {
    if (i == owner || depv[i].guid != guid || depv[i].mode == DB_MODE_NULL ||
        !edt->dep_acq[i].alias) {
      continue;
    }
    /* buffer ref + descriptor pin only */
    dep = dep_release_of(edt, depv, i);
    release_one_dep(&dep, dep_release_pin(edt, i), gpu);
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
    arts_shared_ptr_t h = *(arts_shared_ptr_t *)arts_vector_at(list, last);
    arts_vector_swap_remove(list, last);
    release_one_created(h);
  }
}

/*
 * arts_wait_release_dbs / arts_wait_reacquire_dbs -- Pre-/post-yield
 * hooks invoked around arts_event_wait.
 *
 * No-op under the OCR model: multi-EDT same-rank concurrent acquire is
 * allowed (writer_count CAS-loop), so the creator's hold persists across the
 * yield and is dropped exactly once at EDT epilogue via
 * arts_release_created_dbs.  The GPU kinds have no DB-level coherence to
 * drop either.  Kept as stable hooks for future per-EDT release semantics.
 */
void arts_wait_release_dbs(void) {}
void arts_wait_reacquire_dbs(void) {}
