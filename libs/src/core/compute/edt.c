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
#include "arts/compute/edt.h"
#include "arts/utils/malloc.h"

#include <string.h>

#include "arts/gas/guid.h"
#include "arts/gas/out_of_order.h"
#include "arts/gas/route_table.h"
#include "arts/memory/db.h"
#include "arts/remote/handler.h"
#include "arts/runtime_state.h"
#include "arts/sync/termination.h"
#include "arts/system/print.h"
#include "arts/system/threads.h"
#include "arts/utils/array_list.h"
#include "arts/utils/atomics.h"

#ifdef ARTS_USE_GPU
#include "arts/gpu/gpu_internal.h"
#endif

#define MAX_EPOCH_ARRAY_LIST 32

ARTS_THREAD_LOCAL arts_array_list_t *epoch_list = NULL;
ARTS_THREAD_LOCAL struct arts_edt_s *current_edt = NULL;
ARTS_THREAD_LOCAL arts_array_list_t *created_db_list = NULL;
extern unsigned int num_numa_domains;

/* =========================================================================
 * Thread-local EDT memory pool.
 *
 * EDT allocations are a hot path: every task creation does an arts_calloc_align
 * and every completion does an arts_free.  At high thread counts this causes
 * severe allocator contention (lock + syscall overhead).
 *
 * Design:
 *   - 4 size-class buckets: <=128, <=256, <=512, <=1024 bytes.
 *   - Each bucket is a singly-linked free-list (the freed EDT memory is
 *     reused as the link pointer — no extra allocation needed).
 *   - Oversized EDTs (>1024 bytes) fall through to arts_malloc_align/arts_free.
 *   - The pool is purely thread-local: no locks, no atomics.
 *   - Because work-stealing means the freeing thread differs from the
 *     allocating thread, the pool naturally rebalances: threads that execute
 *     many stolen EDTs accumulate pool entries they can reuse for creation.
 *   - A per-bucket depth cap (EDT_POOL_MAX_PER_BUCKET) prevents unbounded
 *     growth if one thread only frees but never allocates.
 *   - arts_cleanup_edt_pool() drains all buckets at thread shutdown.
 *
 * Memory layout requirement: EDT allocations are 16-byte aligned and use
 * arts_malloc_align, which prepends a header_t.  Pool entries store the
 * *user pointer* (the value returned by arts_malloc_align), not the base.
 * When returning to the OS we call arts_free(user_ptr) which recovers the
 * base pointer via hdr->base.
 * ========================================================================= */

#define EDT_POOL_NUM_BUCKETS    4
#define EDT_POOL_BUCKET_0_MAX   128
#define EDT_POOL_BUCKET_1_MAX   256
#define EDT_POOL_BUCKET_2_MAX   512
#define EDT_POOL_BUCKET_3_MAX   1024
#define EDT_POOL_MAX_PER_BUCKET 64

/** A free-list node overlaid on a freed EDT allocation. */
typedef struct edt_pool_entry_s {
  struct edt_pool_entry_s *next;
  unsigned int alloc_size; /**< usable size of this slot (bucket max). */
} edt_pool_entry_t;

/** Per-bucket state. */
typedef struct {
  edt_pool_entry_t *head;
  unsigned int count;
} edt_pool_bucket_t;

static ARTS_THREAD_LOCAL edt_pool_bucket_t edt_pool[EDT_POOL_NUM_BUCKETS];
static ARTS_THREAD_LOCAL int edt_pool_initialized = 0;

static const unsigned int edt_pool_bucket_sizes[EDT_POOL_NUM_BUCKETS] = {
    EDT_POOL_BUCKET_0_MAX,
    EDT_POOL_BUCKET_1_MAX,
    EDT_POOL_BUCKET_2_MAX,
    EDT_POOL_BUCKET_3_MAX,
};

/** Map a requested size to a bucket index, or -1 if too large. */
static inline int edt_pool_bucket_index(unsigned int size) {
  if (size <= EDT_POOL_BUCKET_0_MAX) return 0;
  if (size <= EDT_POOL_BUCKET_1_MAX) return 1;
  if (size <= EDT_POOL_BUCKET_2_MAX) return 2;
  if (size <= EDT_POOL_BUCKET_3_MAX) return 3;
  return -1;
}

static inline void edt_pool_ensure_init(void) {
  if (!edt_pool_initialized) {
    for (int i = 0; i < EDT_POOL_NUM_BUCKETS; i++) {
      edt_pool[i].head = NULL;
      edt_pool[i].count = 0;
    }
    edt_pool_initialized = 1;
  }
}

/**
 * Try to allocate from the thread-local pool.
 * Returns NULL if the pool has no entry for this size class.
 * The returned memory is zeroed (memset) to match arts_calloc_align behavior.
 */
static inline void *edt_pool_alloc(unsigned int size) {
  edt_pool_ensure_init();
  int idx = edt_pool_bucket_index(size);
  if (idx < 0) return NULL;

  edt_pool_bucket_t *bucket = &edt_pool[idx];
  if (bucket->head) {
    edt_pool_entry_t *entry = bucket->head;
    bucket->head = entry->next;
    bucket->count--;
    /* Zero the memory to match calloc semantics. */
    memset(entry, 0, edt_pool_bucket_sizes[idx]);
    return (void *)entry;
  }
  return NULL;
}

/**
 * Return an EDT allocation to the thread-local pool.
 * If the pool bucket is full, falls through to arts_free.
 * @param ptr   The user pointer (as returned by arts_malloc_align).
 * @param size  The allocation size (from edt->header.size).
 */
static inline void edt_pool_free(void *ptr, unsigned int size) {
  edt_pool_ensure_init();
  int idx = edt_pool_bucket_index(size);
  if (idx < 0) {
    /* Oversized — cannot pool. */
    arts_free(ptr);
    return;
  }

  edt_pool_bucket_t *bucket = &edt_pool[idx];
  if (bucket->count >= EDT_POOL_MAX_PER_BUCKET) {
    /* Bucket full — release to the allocator. */
    arts_free(ptr);
    return;
  }

  edt_pool_entry_t *entry = (edt_pool_entry_t *)ptr;
  entry->next = bucket->head;
  entry->alloc_size = edt_pool_bucket_sizes[idx];
  bucket->head = entry;
  bucket->count++;
}

/** Drain all pool buckets, releasing memory to the system allocator. */
void arts_cleanup_edt_pool(void) {
  if (!edt_pool_initialized) return;
  for (int i = 0; i < EDT_POOL_NUM_BUCKETS; i++) {
    edt_pool_entry_t *entry = edt_pool[i].head;
    while (entry) {
      edt_pool_entry_t *next = entry->next;
      arts_free(entry);
      entry = next;
    }
    edt_pool[i].head = NULL;
    edt_pool[i].count = 0;
  }
  edt_pool_initialized = 0;
}

bool arts_set_current_epoch_guid(arts_guid_t epoch_guid) {
  if (epoch_guid) {
    if (!epoch_list) {
      epoch_list = arts_new_array_list(sizeof(arts_guid_t), 8);
    }
    arts_push_to_array_list(epoch_list, &epoch_guid);
    if (current_edt) {
      current_edt->epoch_guid = epoch_guid;
      return true;
    }
  }
  return false;
}

arts_guid_t arts_get_current_epoch_guid() {
  if (epoch_list) {
    uint64_t length = arts_length_array_list(epoch_list);
    if (length) {
      arts_guid_t *guid =
          (arts_guid_t *)arts_get_from_array_list(epoch_list, length - 1);
      return *guid;
    }
  }
  return NULL_GUID;
}

arts_guid_t arts_get_edt_epoch_guid() {
  return current_edt ? current_edt->epoch_guid : NULL_GUID;
}

arts_guid_t *arts_check_epoch_is_root(arts_guid_t to_check) {
  if (epoch_list) {
    uint64_t length = arts_length_array_list(epoch_list);
    for (uint64_t i = 0; i < length; i++) {
      arts_guid_t *guid =
          (arts_guid_t *)arts_get_from_array_list(epoch_list, i);
      if (*guid == to_check) {
        return guid;
      }
    }
  }
  ARTS_INFO("ERROR %lu is not a valid epoch", to_check);
  return NULL;
}

void arts_track_created_db(arts_guid_t guid) {
  if (!created_db_list) {
    created_db_list = arts_new_array_list(sizeof(arts_guid_t), 65536);
  }
  arts_push_to_array_list(created_db_list, &guid);
}

arts_array_list_t *arts_get_created_db_list(void) { return created_db_list; }

void arts_set_thread_local_edt_info(struct arts_edt_s *edt) {
  arts_thread_info.current_edt_guid = edt->current_edt;
  current_edt = edt;

  if (epoch_list) {
    arts_reset_array_list(epoch_list);
  }

  if (created_db_list) {
    arts_reset_array_list(created_db_list);
  }

  arts_set_current_epoch_guid(current_edt->epoch_guid);
}

void arts_save_thread_local(thread_local_t *tl) {
  TIME_CONTEXT_SWITCH_START();
  tl->current_edt_guid = arts_thread_info.current_edt_guid;
  tl->current_edt = current_edt;
  tl->epoch_list = (void *)epoch_list;
  tl->created_db_list = (void *)created_db_list;

  arts_thread_info.current_edt_guid = NULL_GUID;
  current_edt = NULL;
  epoch_list = NULL;
  created_db_list = NULL;
  TIME_CONTEXT_SWITCH_STOP();
}

void arts_restore_thread_local(thread_local_t *tl) {
  TIME_CONTEXT_SWITCH_START();
  arts_thread_info.current_edt_guid = tl->current_edt_guid;
  current_edt = tl->current_edt;
  if (epoch_list) {
    arts_delete_array_list(epoch_list);
  }
  epoch_list = (arts_array_list_t *)tl->epoch_list;
  if (created_db_list) {
    arts_delete_array_list(created_db_list);
  }
  created_db_list = (arts_array_list_t *)tl->created_db_list;
  TIME_CONTEXT_SWITCH_STOP();
}

void arts_cleanup_edt_tls() {
  if (epoch_list) {
    arts_delete_array_list(epoch_list);
    epoch_list = NULL;
  }
  if (created_db_list) {
    arts_delete_array_list(created_db_list);
    created_db_list = NULL;
  }
  /* Drain the thread-local EDT memory pool. */
  arts_cleanup_edt_pool();
}

void arts_increment_finished_epoch_list() {
  if (epoch_list) {

    unsigned int epoch_array_length = arts_length_array_list(epoch_list);
    for (unsigned int i = 0; i < epoch_array_length; i++) {
      arts_guid_t *guid =
          (arts_guid_t *)arts_get_from_array_list(epoch_list, i);
#if ARTS_LOG_LEVEL >= 2
      uint64_t current_id = current_edt ? current_edt->arts_id : 0;
      ARTS_INFO("Current EDT[Id:%lu, Guid:%lu] - Unsetting Epoch [Guid:%lu]",
                current_id, arts_thread_info.current_edt_guid, *guid);
#endif
      if (*guid) {
        increment_finished_epoch(*guid);
      }
    }

    if (epoch_array_length > MAX_EPOCH_ARRAY_LIST) {
      arts_delete_array_list(epoch_list);
      epoch_list = NULL;
    } else {
      arts_reset_array_list(epoch_list);
    }
  }
  arts_shutdown_epoch_inc_finished();
}

void arts_unset_thread_local_edt_info() {
  arts_increment_finished_epoch_list();
  arts_thread_info.current_edt_guid = NULL_GUID;
  current_edt = NULL;
}

/*
 * arts_edt_create_internal — Core EDT allocation and registration.
 *
 * Allocates the EDT struct (header + paramv + depv + modes), assigns its GUID,
 * copies parameters, registers with the epoch system, and places the EDT into
 * the route table so that incoming signals can find it.
 *
 * Two paths exist depending on whether a GUID was pre-reserved:
 *   1. New GUID (created_guid == true):
 *        - arts_route_table_add_item (no race — nobody else knows the GUID
 * yet).
 *        - If depc == 0, the EDT is immediately ready.
 *   2. Pre-reserved GUID (created_guid == false):
 *        - The GUID may already have received out-of-order signals while it was
 *          in RESERVED state. A sentinel (+1 on depc_needed) prevents premature
 *          firing during the race window between route-table insertion and
 *          OOO replay.  See the inline comments for the full protocol.
 *
 * Concurrency notes:
 *   - depc_needed is the primary synchronisation counter.  Every satisfied
 *     dependency atomically decrements it; exactly one thread observes 0
 *     and calls arts_handle_ready_edt.
 *   - The EDT must NOT be visible (in the route table) while its fields
 *     are still being written.
 */
bool arts_edt_create_internal(struct arts_edt_s *edt, arts_type_t mode,
                              arts_guid_t *guid, unsigned int route,
                              unsigned int numa_domain, unsigned int edt_space,
                              arts_guid_t output_buffer, arts_edt_t func_ptr,
                              uint32_t paramc, const uint64_t *paramv,
                              uint32_t depc, bool use_epoch,
                              arts_guid_t epoch_guid, bool has_depv,
                              const arts_edt_dep_t *initial_depv,
                              uint64_t arts_id) {
  if (!edt) {
    /* Determine the bucket for this size class.  All allocations are
     * rounded up to the bucket ceiling so that any pooled entry is
     * reusable for any request within the same bucket. */
    int bucket_idx = edt_pool_bucket_index(edt_space);
    unsigned int alloc_size =
        (bucket_idx >= 0) ? edt_pool_bucket_sizes[bucket_idx] : edt_space;

    /* Try the thread-local pool first to avoid allocator contention. */
    edt = (struct arts_edt_s *)edt_pool_alloc(edt_space);
    if (!edt) {
      edt = (struct arts_edt_s *)arts_calloc_align(1, alloc_size, 16);
    }
    /* Record the allocation capacity (bucket ceiling) rather than the
     * requested size.  This ensures edt_pool_free places the entry in
     * the correct bucket regardless of which thread frees it.  The
     * extra bytes are always zero and harmless for remote transfer. */
    edt_space = alloc_size;
  }
  if (!edt) {
    ARTS_ERROR("EDT allocation failed (size=%u)", edt_space);
  }

  edt->header.type = mode;
  edt->header.size = edt_space;
  edt->arts_id = arts_id;

  bool created_guid = false;
  if (*guid == NULL_GUID) {
    created_guid = true;
    edt->current_edt = *guid = arts_guid_create_for_rank(route, mode);
  } else {
    edt->current_edt = *guid;
  }

  edt->func_ptr = func_ptr;
  edt->depc = (has_depv) ? depc : 0;
  edt->paramc = paramc;
  edt->output_buffer = output_buffer;
  edt->epoch_guid = NULL_GUID;
  edt->numa_domain = numa_domain;
  edt->depc_needed = depc;

  if (use_epoch) {
    arts_guid_t current_epoch_guid = NULL_GUID;
    if (epoch_guid && arts_check_epoch_is_root(epoch_guid)) {
      current_epoch_guid = epoch_guid;
    } else {
      current_epoch_guid = arts_get_current_epoch_guid();
    }

    if (current_epoch_guid) {
      edt->epoch_guid = current_epoch_guid;
      increment_active_epoch(current_epoch_guid);
    }
  }
  arts_shutdown_epoch_inc_active();

  /* Copy inline parameter values into the EDT's trailing storage.
   * Layout: [arts_edt_s | paramv[paramc] | depv[depc]]
   * paramv starts immediately after the struct header. */
  if (paramc) {
    unsigned int offset = sizeof(struct arts_edt_s);
    ARTS_DEBUG("EDT paramv copy: edt=%p offset=%u paramc=%u depc=%u "
               "edt_space=%u dep_size=%zu",
               (void *)edt, offset, paramc, depc, edt_space,
               depc * sizeof(arts_edt_dep_t));
    char *tmp = (char *)edt + offset;
    memcpy(tmp, paramv, sizeof(uint64_t) * paramc);
  }

  if (has_depv && depc) {
    arts_edt_dep_t *edt_dep = (arts_edt_dep_t *)arts_get_depv(edt);
    unsigned int dep_space = depc * sizeof(arts_edt_dep_t);
    if (initial_depv) {
      memcpy(edt_dep, initial_depv, dep_space);
    } else {
      memset(edt_dep, 0, dep_space);
    }
  }

  ARTS_INFO("EDT create [Guid:%lu, Id:%lu, Depc:%u, Route:%u, "
            "PreReserved:%s, Epoch:%lu, FuncPtr:%p]",
            *guid, edt->arts_id, edt->depc, route, created_guid ? "no" : "yes",
            edt->epoch_guid, (void *)func_ptr);

  if (route != arts_global_rank_id) {
    /* Remote EDT: serialise and send to the target node. */
    ARTS_INFO("EDT[Guid:%lu] remote move to rank %u", *guid, route);
    arts_remote_memory_move(route, *guid, (void *)edt,
                            (unsigned int)edt->header.size,
                            ARTS_REMOTE_EDT_MOVE_MSG, arts_free);
  } else {
    /* Local EDT: register in the route table and check readiness. */
    INC_OUTSTANDING_EDTS(1);
    if (created_guid) {
      /* New GUID path — no race, safe non-atomic insert. */
      arts_route_table_add_item(edt, *guid, arts_global_rank_id, false);
      if (edt->depc_needed == 0) {
        ARTS_INFO("EDT[Guid:%lu] immediately ready (depc=0)", *guid);
        arts_handle_ready_edt(edt);
      } else {
        ARTS_DEBUG("EDT[Guid:%lu] waiting for %u deps", *guid,
                   edt->depc_needed);
      }
    } else {
      /*
       * Pre-reserved GUID path — other threads may already hold this GUID
       * and could have queued out-of-order (OOO) signals.
       *
       * Protocol:
       *   1. Set depc_needed = depc + 1  (sentinel prevents premature 0)
       *   2. Insert into route table (EDT is now globally visible)
       *   3. Replay any queued OOO signals (they decrement depc_needed)
       *   4. Atomically remove sentinel (-1); if result is 0, all deps
       *      were already satisfied and we fire the EDT.
       *
       * Exactly one thread (either the OOO replay callback or us at step 4)
       * will observe depc_needed == 0 and call arts_handle_ready_edt.
       */
      edt->depc_needed = depc + 1;
      ARTS_INFO("EDT[Guid:%lu] pre-reserved path: sentinel depc_needed=%u",
                *guid, edt->depc_needed);
      arts_route_table_add_item_race(edt, *guid, arts_global_rank_id, false);
      arts_route_table_fire_oo(*guid, arts_out_of_order_handler);
      unsigned int remaining = arts_atomic_sub(&edt->depc_needed, 1U);
      ARTS_INFO("EDT[Guid:%lu] sentinel removed: depc_needed=%u", *guid,
                remaining);
      if (remaining == 0) {
        arts_handle_ready_edt(edt);
      }
    }
  }

  INCREMENT_NUM_EDT_CREATE_BY(1);
  return true;
}

static inline uint64_t arts_mix_u64(uint64_t value) {
  value ^= value >> 30;
  value *= 0xbf58476d1ce4e5b9ULL;
  value ^= value >> 27;
  value *= 0x94d049bb133111ebULL;
  value ^= value >> 31;
  return value;
}

static unsigned int arts_choose_ready_local_numa(uint32_t paramc,
                                                 const uint64_t *paramv,
                                                 uint32_t depc,
                                                 const arts_edt_dep_t *depv,
                                                 uint64_t arts_id) {
  if (num_numa_domains <= 1)
    return arts_thread_info.numa_domain_id;

  if (depv) {
    for (uint32_t i = 0; i < depc; ++i) {
      arts_guid_t guid = depv[i].guid;
      if (guid && arts_guid_get_type(guid) == ARTS_DB)
        return (unsigned int)(arts_guid_get_key(guid) % num_numa_domains);
    }
  }

  if (paramv && paramc != 0) {
    uint64_t hash = arts_mix_u64(arts_id);
    for (uint32_t i = 0; i < paramc; ++i) {
      hash ^= arts_mix_u64(paramv[i] + 0x9e3779b97f4a7c15ULL +
                           ((uint64_t)i << 32));
    }
    return (unsigned int)(hash % num_numa_domains);
  }

  return arts_thread_info.numa_domain_id;
}

arts_guid_t arts_edt_create_dep(arts_edt_t func_ptr, uint32_t paramc,
                                const uint64_t *paramv, uint32_t depc,
                                bool has_depv, const arts_hint_t *hint) {
  TIME_EDT_CREATE_START();
  unsigned int route = (hint && hint->route != ARTS_HINT_CURRENT_NODE)
                           ? hint->route
                           : arts_global_rank_id;
  uint64_t arts_id = hint ? hint->id : 0;
  unsigned int dep_space = (has_depv) ? depc * sizeof(arts_edt_dep_t) : 0;
  unsigned int edt_space =
      sizeof(struct arts_edt_s) + (paramc * sizeof(uint64_t)) + dep_space;
  arts_guid_t guid = NULL_GUID;
  arts_guid_t *guid_ptr = &guid;
  bool created = arts_edt_create_internal(
      NULL, ARTS_EDT, guid_ptr, route, arts_thread_info.numa_domain_id,
      edt_space, NULL_GUID, func_ptr, paramc, paramv, depc, true, NULL_GUID,
      has_depv, NULL, arts_id);
  TIME_EDT_CREATE_STOP();
  return guid;
}

arts_guid_t arts_edt_create_with_guid_dep(arts_edt_t func_ptr, arts_guid_t guid,
                                          uint32_t paramc,
                                          const uint64_t *paramv, uint32_t depc,
                                          bool has_depv) {
  TIME_EDT_CREATE_START();
  unsigned int route = arts_guid_get_rank(guid);
  unsigned int dep_space = (has_depv) ? depc * sizeof(arts_edt_dep_t) : 0;
  unsigned int edt_space =
      sizeof(struct arts_edt_s) + (paramc * sizeof(uint64_t)) + dep_space;
  bool ret = arts_edt_create_internal(
      NULL, ARTS_EDT, &guid, route, arts_thread_info.numa_domain_id, edt_space,
      NULL_GUID, func_ptr, paramc, paramv, depc, true, NULL_GUID, has_depv,
      NULL, 0);
  TIME_EDT_CREATE_STOP();
  return (ret) ? guid : NULL_GUID;
}

arts_guid_t arts_edt_create_with_epoch_dep(
    arts_edt_t func_ptr, uint32_t paramc, const uint64_t *paramv, uint32_t depc,
    arts_guid_t epoch_guid, bool has_depv, const arts_hint_t *hint) {
  TIME_EDT_CREATE_START();
  unsigned int route = (hint && hint->route != ARTS_HINT_CURRENT_NODE)
                           ? hint->route
                           : arts_global_rank_id;
  uint64_t arts_id = hint ? hint->id : 0;
  unsigned int dep_space = (has_depv) ? depc * sizeof(arts_edt_dep_t) : 0;
  unsigned int edt_space =
      sizeof(struct arts_edt_s) + (paramc * sizeof(uint64_t)) + dep_space;
  arts_guid_t guid = NULL_GUID;
  bool created = arts_edt_create_internal(
      NULL, ARTS_EDT, &guid, route, arts_thread_info.numa_domain_id, edt_space,
      NULL_GUID, func_ptr, paramc, paramv, depc, true, epoch_guid, has_depv,
      NULL, arts_id);
  TIME_EDT_CREATE_STOP();
  return guid;
}

arts_guid_t arts_edt_create(arts_edt_t func_ptr, uint32_t paramc,
                            const uint64_t *paramv, uint32_t depc,
                            const arts_hint_t *hint) {
  return arts_edt_create_dep(func_ptr, paramc, paramv, depc, true, hint);
}

arts_guid_t arts_edt_create_with_guid(arts_edt_t func_ptr, arts_guid_t guid,
                                      uint32_t paramc, const uint64_t *paramv,
                                      uint32_t depc) {
  return arts_edt_create_with_guid_dep(func_ptr, guid, paramc, paramv, depc,
                                       true);
}

arts_guid_t arts_edt_create_with_epoch(arts_edt_t func_ptr, uint32_t paramc,
                                       const uint64_t *paramv, uint32_t depc,
                                       arts_guid_t epoch_guid,
                                       const arts_hint_t *hint) {
  return arts_edt_create_with_epoch_dep(func_ptr, paramc, paramv, depc,
                                        epoch_guid, true, hint);
}

arts_guid_t arts_edt_create_ready_local_with_epoch(
    arts_edt_t func_ptr, uint32_t paramc, const uint64_t *paramv,
    uint32_t depc, const arts_edt_dep_t *depv, arts_guid_t epoch_guid,
    const arts_hint_t *hint) {
  if (depc == 0) {
    return arts_edt_create_with_epoch_dep(func_ptr, paramc, paramv, depc,
                                          epoch_guid, true, hint);
  }

  unsigned int route = (hint && hint->route != ARTS_HINT_CURRENT_NODE)
                           ? hint->route
                           : arts_global_rank_id;

  if (route != arts_global_rank_id) {
    arts_guid_t guid = arts_edt_create_with_epoch_dep(
        func_ptr, paramc, paramv, depc, epoch_guid, true, hint);
    for (uint32_t i = 0; i < depc; ++i) {
      const arts_edt_dep_t *dep = depv ? &depv[i] : NULL;
      arts_guid_t source = dep ? dep->guid : NULL_GUID;
      arts_db_access_mode_t mode = dep ? dep->mode : DB_MODE_NULL;
      uint32_t flags = dep ? dep->flags : 0;
      uint64_t slice_offset = dep ? dep->slice_offset : 0;
      uint64_t slice_size = dep ? dep->slice_size : 0;
      if (slice_offset != 0 || slice_size != 0) {
        arts_add_dependence_at_ex(source, guid, i, mode, slice_offset,
                                  slice_size, flags);
      } else {
        arts_add_dependence_ex(source, guid, i, mode, flags);
      }
    }
    return guid;
  }

  TIME_EDT_CREATE_START();
  uint64_t arts_id = hint ? hint->id : 0;
  unsigned int preferred_numa =
      arts_choose_ready_local_numa(paramc, paramv, depc, depv, arts_id);
  unsigned int dep_space = depc * sizeof(arts_edt_dep_t);
  unsigned int edt_space =
      sizeof(struct arts_edt_s) + (paramc * sizeof(uint64_t)) + dep_space;
  arts_guid_t guid = NULL_GUID;
  bool created = arts_edt_create_internal(
      NULL, ARTS_EDT, &guid, route, preferred_numa, edt_space,
      NULL_GUID, func_ptr, paramc, paramv, depc, true, epoch_guid, true,
      depv, arts_id);
  if (created) {
    struct arts_edt_s *edt =
        (struct arts_edt_s *)arts_route_table_lookup_item(guid);
    if (!edt) {
      ARTS_ERROR("Ready-local EDT[Guid:%lu] missing from route table", guid);
    } else {
      arts_handle_ready_edt(edt);
    }
  }
  TIME_EDT_CREATE_STOP();
  return guid;
}

void arts_edt_free(struct arts_edt_s *edt) {
  arts_thread_info.edt_free = 1;
  /* Return to thread-local pool if the size class fits; else arts_free. */
  edt_pool_free(edt, (unsigned int)edt->header.size);
  arts_thread_info.edt_free = 0;
}

void arts_edt_delete(struct arts_edt_s *edt) {
  if (!edt) {
    ARTS_INFO("EDT delete called with NULL edt on rank %u",
              arts_global_rank_id);
    return;
  }
  ARTS_INFO("EDT delete [Guid:%lu, Id:%lu, Depc:%u, DepcNeeded:%u] on rank %u",
            edt->current_edt, edt->arts_id, edt->depc, edt->depc_needed,
            arts_global_rank_id);
  arts_route_table_remove_item(edt->current_edt);
  arts_edt_free(edt);
}

void arts_edt_destroy(arts_guid_t guid) {
  struct arts_edt_s *edt =
      (struct arts_edt_s *)arts_route_table_lookup_item(guid);
  if (!edt) {
    ARTS_INFO("EDT destroy missing [Guid:%lu] on rank %u", guid,
              arts_global_rank_id);
    return;
  }
  ARTS_INFO("EDT destroy [Guid:%lu, Id:%lu, Depc:%u, DepcNeeded:%u] on rank %u",
            edt->current_edt, edt->arts_id, edt->depc, edt->depc_needed,
            arts_global_rank_id);
  arts_route_table_remove_item(guid);
  arts_edt_free(edt);
}

void *arts_get_depv(void *edt_ptr) {
  struct arts_edt_s *edt = (struct arts_edt_s *)edt_ptr;
  unsigned int paramc = edt->paramc;
  if (edt->edt_type == ARTS_EDT_GPU) {
#ifdef ARTS_USE_GPU
    arts_gpu_edt_t *edtGpu = (arts_gpu_edt_t *)edt_ptr;
    return (void *)((uint64_t *)(edtGpu + 1) + paramc);
#else
    return NULL;
#endif
  }
  return (void *)((uint64_t *)(edt + 1) + paramc);
}

/* arts_get_dep_modes removed — mode now lives in arts_edt_dep_t.mode */

/*
 * arts_set_dep_metadata — Write dep metadata to an EDT slot without signaling.
 *
 * This is the metadata half of the two-message add_dependence pattern.
 * It sets depv[slot].mode / depv[slot].flags on the target EDT. It does NOT
 * decrement depc_needed and does NOT deliver data.
 *
 * Local EDT: direct write.  Remote EDT: forward via network message.
 * Not-yet-created EDT: queue a dedicated OOO metadata write that replays once
 * the EDT becomes available.
 */
void arts_set_dep_metadata_ext(arts_guid_t edt_guid, uint32_t slot,
                               arts_db_access_mode_t mode, uint32_t flags,
                               uint64_t slice_offset,
                               uint64_t slice_size) {
  unsigned int rank = arts_guid_get_rank(edt_guid);
  if (rank == arts_global_rank_id) {
    struct arts_edt_s *edt =
        (struct arts_edt_s *)arts_route_table_lookup_item(edt_guid);
    if (edt) {
      arts_edt_dep_t *edt_dep = (arts_edt_dep_t *)arts_get_depv(edt);
      if (slot < edt->depc) {
        edt_dep[slot].mode = mode;
        edt_dep[slot].flags = flags;
        edt_dep[slot].slice_offset = slice_offset;
        edt_dep[slot].slice_size = slice_size;
      }
    } else {
      arts_out_of_order_set_dep_metadata_ext(edt_guid, slot, mode, flags,
                                             slice_offset, slice_size);
    }
  } else {
    /* Remote EDT — forward dep metadata to the owning rank. */
    arts_remote_set_dep_metadata_ext(edt_guid, slot, mode, flags, slice_offset,
                                     slice_size);
  }
}

void arts_set_dep_metadata(arts_guid_t edt_guid, uint32_t slot,
                           arts_db_access_mode_t mode, uint32_t flags) {
  arts_set_dep_metadata_ext(edt_guid, slot, mode, flags, 0, 0);
}

void arts_set_dep_mode(arts_guid_t edt_guid, uint32_t slot,
                       arts_db_access_mode_t mode) {
  arts_set_dep_metadata(edt_guid, slot, mode, 0);
}

/*
 * internal_signal_edt — Satisfy one dependency slot on an EDT.
 *
 * Four dispatch paths:
 *   1. CDAG invalidation (current EDT has pending invalidations) →
 *      route through OOO to preserve ordering.
 *   2. Local EDT found in route table → write the dep slot and
 *      atomically decrement depc_needed.  If this was the last
 *      dependency (depc_needed hits 0), call arts_handle_ready_edt.
 *   3. Local EDT NOT found (still RESERVED or not yet created) →
 *      enqueue in the OOO list; will be replayed when the EDT
 *      transitions to AVAILABLE via arts_route_table_fire_oo.
 *   4. Remote EDT → forward the signal over the network.
 */
void internal_signal_edt_ex(arts_guid_t edt_packet, uint32_t slot,
                            arts_guid_t data_guid,
                            arts_db_access_mode_t mode, uint32_t flags,
                            void *ptr, unsigned int size) {
  TIME_EDT_SIGNAL_START();
  INCREMENT_NUM_EDT_SIGNAL_BY(1);

  if (current_edt && current_edt->invalidate_count > 0) {
    /* CDAG path: defer signal to maintain write-ordering invariants. */
    ARTS_DEBUG("Signal EDT[Guid:%lu] Slot:%u deferred (CDAG invalidation)",
               edt_packet, slot);
    if (mode == DB_MODE_PTR) {
      arts_out_of_order_signal_edt_with_ptr(edt_packet, data_guid, ptr, size,
                                            slot);
    } else {
      arts_out_of_order_signal_edt(current_edt->current_edt, edt_packet,
                                   data_guid, slot, mode, flags, true);
    }
  } else {
    unsigned int rank = arts_guid_get_rank(edt_packet);
    if (rank == arts_global_rank_id) {
      /* Local signal path. */
      struct arts_edt_s *edt =
          (struct arts_edt_s *)arts_route_table_lookup_item(edt_packet);
      if (edt) {
        /* EDT exists in route table — write dep slot. */
        arts_edt_dep_t *edt_dep = (arts_edt_dep_t *)arts_get_depv(edt);
        if (slot < edt->depc) {
          edt_dep[slot].guid = data_guid;
          if (mode == DB_MODE_PTR && size > 0) {
            void *copy = arts_malloc(size);
            memcpy(copy, ptr, size);
            edt_dep[slot].ptr = copy;
          } else {
            edt_dep[slot].ptr = ptr;
          }
          if (mode != DB_MODE_NULL) {
            edt_dep[slot].mode = mode;
            edt_dep[slot].slice_offset = 0;
            edt_dep[slot].slice_size = 0;
          }
          if (flags) {
            edt_dep[slot].flags = flags;
          }
        }
        unsigned int res = arts_atomic_sub(&edt->depc_needed, 1U);
        ARTS_INFO("Signal EDT[Guid:%lu, Slot:%u] DB[Guid:%lu] "
                  "depc_needed=%u→%u",
                  edt->current_edt, slot, data_guid, res + 1, res);
        if (res == 0) {
          ARTS_INFO("EDT[Guid:%lu] all deps satisfied — firing",
                    edt->current_edt);
          arts_handle_ready_edt(edt);
        }
      } else {
        /* EDT not yet in route table — queue as OOO. */
        ARTS_DEBUG("Signal EDT[Guid:%lu, Slot:%u] OOO (not in route table yet)",
                   edt_packet, slot);
        if (mode == DB_MODE_PTR) {
          arts_out_of_order_signal_edt_with_ptr(edt_packet, data_guid, ptr,
                                                size, slot);
        } else {
          arts_out_of_order_signal_edt(edt_packet, edt_packet, data_guid, slot,
                                       mode, flags, false);
        }
      }
    } else {
      /* Remote signal — forward over the network. */
      ARTS_DEBUG("Signal EDT[Guid:%lu, Slot:%u] remote to rank %u", edt_packet,
                 slot, rank);
      if (mode == DB_MODE_PTR) {
        arts_remote_signal_edt_with_ptr(edt_packet, data_guid, ptr, size, slot);
      } else {
        arts_remote_signal_edt(edt_packet, data_guid, slot, mode, flags);
      }
    }
  }
  TIME_EDT_SIGNAL_STOP();
}

void internal_signal_edt(arts_guid_t edt_packet, uint32_t slot,
                         arts_guid_t data_guid, arts_db_access_mode_t mode,
                         void *ptr, unsigned int size) {
  internal_signal_edt_ex(edt_packet, slot, data_guid, mode, 0, ptr, size);
}

void arts_signal_edt_with_flags(arts_guid_t edt_guid, uint32_t slot,
                                arts_guid_t data_guid,
                                arts_db_access_mode_t mode, uint32_t flags) {
  internal_signal_edt_ex(edt_guid, slot, data_guid, mode, flags, NULL, 0);
}

void arts_signal_edt(arts_guid_t edt_guid, uint32_t slot, arts_guid_t data_guid,
                     arts_db_access_mode_t mode) {
  ARTS_DEBUG("arts_signal_edt [EDT:%lu, Slot:%u, DB:%lu, Mode:%u]", edt_guid,
             slot, data_guid, mode);
  internal_signal_edt_ex(edt_guid, slot, data_guid, mode, 0, NULL, 0);
}

// Internal function to signal EDT with explicit access mode
void internal_signal_edt_with_mode(arts_guid_t edt_packet, uint32_t slot,
                                   arts_guid_t data_guid,
                                   arts_db_access_mode_t mode) {
  internal_signal_edt_ex(edt_packet, slot, data_guid, mode, 0, NULL, 0);
}

void arts_signal_edt_value(arts_guid_t edt_guid, uint32_t slot,
                           uint64_t value) {
  ARTS_DEBUG("arts_signal_edt_value [EDT:%lu, Slot:%u, Value:%lu]", edt_guid,
             slot, value);
  internal_signal_edt(edt_guid, slot, (arts_guid_t)value, DB_MODE_VALUE, NULL,
                      0);
}

void arts_signal_edt_ptr(arts_guid_t edt_guid, uint32_t slot, void *ptr,
                         unsigned int size) {
  internal_signal_edt(edt_guid, slot, NULL_GUID, DB_MODE_PTR, ptr, size);
}

void arts_signal_edt_ptr_with_guid(arts_guid_t edt_guid, uint32_t slot,
                                   arts_guid_t db_guid, void *ptr,
                                   unsigned int size) {
  internal_signal_edt(edt_guid, slot, db_guid, DB_MODE_PTR, ptr, size);
}

void arts_signal_edt_null(arts_guid_t edt_guid, uint32_t slot) {
  internal_signal_edt(edt_guid, slot, NULL_GUID, DB_MODE_NULL, NULL, 0);
}

arts_guid_t arts_allocate_local_buffer(void **buffer, unsigned int size,
                                       unsigned int uses,
                                       arts_guid_t epoch_guid) {
  if (epoch_guid) {
    increment_active_epoch(epoch_guid);
  }
  arts_shutdown_epoch_inc_active();

  // unsigned int alloc = 0;
  if (size) {
    if (*buffer == NULL) {
      *buffer = (char *)arts_malloc(sizeof(char) * size);
      // alloc = 1;
    }
  }

  arts_buffer_t *stub = (arts_buffer_t *)arts_malloc(sizeof(arts_buffer_t));
  stub->buffer = (buffer) ? *buffer : NULL;
  stub->size_to_write = NULL;
  stub->size = size;
  stub->uses = uses;
  stub->epoch_guid = epoch_guid;

  arts_guid_t guid =
      arts_guid_create_for_rank(arts_global_rank_id, ARTS_BUFFER);
  arts_route_table_add_item(stub, guid, arts_global_rank_id, false);
  return guid;
}

void *arts_set_buffer(arts_guid_t buffer_guid, void *buffer,
                      unsigned int size) {
  void *ret = NULL;
  unsigned int rank = arts_guid_get_rank(buffer_guid);
  if (rank == arts_global_rank_id) {
    arts_buffer_t *stub =
        (arts_buffer_t *)arts_route_table_lookup_item(buffer_guid);
    if (stub) {
      arts_guid_t epoch_guid = stub->epoch_guid;
      if (epoch_guid) {
        increment_queue_epoch(epoch_guid);
      }
      arts_shutdown_epoch_inc_queue();

      if (size > stub->size) {
        if (stub->size) {
          ARTS_INFO("Truncating buffer data buffer size: %u stub size: %u",
                    size, stub->size);
        } else if (stub->buffer == NULL) {
          stub->buffer = (char *)arts_malloc(sizeof(char) * size);
          stub->size = size;
        } else {
          stub->size = size;
        }
      }

      if (stub->size_to_write) {
        *stub->size_to_write = (uint32_t)size;
      }

      if (stub->buffer) {
        memcpy(stub->buffer, buffer, stub->size);
        ARTS_DEBUG("Set buffer [Ptr:%p, Size:%u, Uses: %u]", stub->buffer,
                   *((unsigned int *)stub->buffer), stub->size);
        ret = stub->buffer;
      } else {
        ret = NULL;
      }

      if (!arts_atomic_sub(&stub->uses, 1)) {
        arts_route_table_remove_item(buffer_guid);
        arts_free(stub);
      }

      if (epoch_guid) {
        increment_finished_epoch(epoch_guid);
      }
      arts_shutdown_epoch_inc_finished();
    } else {
      ARTS_INFO("Out-of-order buffers not supported");
    }
  } else {
    arts_remote_memory_move(rank, buffer_guid, buffer, size,
                            ARTS_REMOTE_BUFFER_SEND_MSG, arts_free);
  }
  return ret;
}

void *arts_get_buffer(arts_guid_t buffer_guid) {
  void *buffer = NULL;
  if (arts_guid_is_local(buffer_guid)) {
    arts_buffer_t *stub =
        (arts_buffer_t *)arts_route_table_lookup_item(buffer_guid);
    if (stub == NULL) {
      return NULL;
    }
    buffer = stub->buffer;
    if (!arts_atomic_sub(&stub->uses, 1)) {
      arts_route_table_remove_item(buffer_guid);
      arts_free(stub);
    }
  }
  return buffer;
}

void *arts_block_for_buffer(arts_guid_t buffer_guid) {
  void *buffer = NULL;
  if (arts_guid_is_local(buffer_guid)) {
    arts_buffer_t *stub =
        (arts_buffer_t *)arts_route_table_lookup_item(buffer_guid);
    if (stub == NULL) {
      return NULL;
    }
    while (stub->uses > 1) {
      ARTS_DEBUG("Yield: [Uses: %u]", stub->uses);
      arts_yield();
    }
    buffer = stub->buffer;
    if (!arts_atomic_sub(&stub->uses, 1)) {
      arts_route_table_remove_item(buffer_guid);
      arts_free(stub);
    }
  }
  return buffer;
}

volatile uint64_t outstanding_edts __attribute__((aligned(64))) = 0;
void check_out_edts(uint64_t threshold) {
  static volatile uint64_t count __attribute__((aligned(64))) = 0;
  if (arts_atomic_fetch_add_u64(&count, 1) + 1 == threshold) {
    arts_atomic_fetch_sub_u64(&count, threshold);
  }
}

void arts_lc_sync(arts_guid_t edt_guid, uint32_t slot, arts_guid_t data_guid) {
  arts_type_t type = arts_guid_get_type(data_guid);
  (void)type;
  internal_signal_edt(edt_guid, slot, data_guid, DB_MODE_LC_SYNC, NULL, 0);
}

void arts_gpu_signal_edt_memset(arts_guid_t edt_guid, uint32_t slot,
                                arts_guid_t data_guid) {
  arts_db_access_mode_t mode = DB_MODE_MEMSET;
  struct arts_db_s *db =
      (struct arts_db_s *)arts_route_table_lookup_db(data_guid, NULL, false);
  if (db && db->db_type == ARTS_DB_LC) {
    mode = DB_MODE_LC_NO_COPY;
  }
  if (db) {
    arts_route_table_return_db(data_guid, false);
  }
  internal_signal_edt(edt_guid, slot, data_guid, mode, NULL, 0);
}
